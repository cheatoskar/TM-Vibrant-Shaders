#include "gameshaders.h"
#include "config.h"
#include "gbufpatch.h"
#include "hook.h"
#include "log.h"
#include <windows.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>

namespace tmshaders {
namespace gameshaders {
namespace {

using gbufpatch::Include;
using gbufpatch::Macro;
using AssembleFn = HRESULT(WINAPI*)(const char* source, UINT length, const Macro* defines, Include* include, DWORD flags,
                                    ID3DBlob** shader, ID3DBlob** errors);

gbufpatch::CompileFn g_compile = nullptr;
AssembleFn g_assemble = nullptr;
std::wstring g_dir;
bool g_dump = false;  // the gameshaders folder exists: write every shader
bool g_patch = false; // GameGBuffer was on at the start: patch the pixel shaders
std::mutex g_mutex;
std::set<uint64_t> g_seen;
int g_written = 0;

// GameGBuffer bookkeeping. Compiles may come from a loading thread, shaders are created and
// drawn on the render thread: both under g_mutex.
std::unordered_set<uint64_t> g_patchedHashes;               // bytecode of the patched shaders handed to the game
std::unordered_set<const IDirect3DPixelShader9*> g_normalShaders; // created from one of them
int g_patched = 0, g_failed = 0;      // compiles (a shader compiled again counts again)
std::map<std::string, int> g_skipped; // reason -> shaders
int g_logged = -1;                    // shaders counted at the last summary
DWORD g_lastCompile = 0;

// Passes the game's include handler through and keeps what it hands out (each file once: with
// GameGBuffer the compiler reads them twice).
struct IncludeTap : Include {
    Include* inner = nullptr;
    std::string text;
    std::set<std::string> names;
    HRESULT __stdcall Open(int type, const char* name, const void* parent, const void** data, UINT* bytes) override {
        HRESULT hr = inner->Open(type, name, parent, data, bytes);
        if (!names.insert(name ? name : "?").second) return hr;
        text += "\n// ==== #include \"";
        text += name ? name : "?";
        text += "\"\n";
        if (SUCCEEDED(hr) && data && *data && bytes) text.append(static_cast<const char*>(*data), *bytes);
        else text += "// (not found)\n";
        return hr;
    }
    HRESULT __stdcall Close(const void* data) override { return inner->Close(data); }
};

uint64_t fnv(uint64_t h, const char* data, size_t length) {
    for (size_t i = 0; i < length; i++) h = (h ^ static_cast<unsigned char>(data[i])) * 1099511628211ull;
    return h;
}

std::string describeDefines(const Macro* defines) {
    std::string out;
    for (const Macro* m = defines; m && m->name; m++) {
        out += "// #define ";
        out += m->name;
        out += " ";
        out += m->definition ? m->definition : "";
        out += "\n";
    }
    return out;
}

// `patch`: GameGBuffer's note on this shader ("patched: normal nNormal, eye EyeToPos"), written
// after the bytecode line (whose hash is then the patched shader's, as F11 traces show it).
void dump(const char* kind, const char* source, UINT length, const Macro* defines, const char* function, const char* profile,
          const std::string& includes, ID3DBlob* shader, const std::string& patch) {
    if (!source) return;
    if (!length) length = static_cast<UINT>(strlen(source));
    const std::string defs = describeDefines(defines);
    uint64_t h = 14695981039346656037ull;
    h = fnv(h, source, length);
    h = fnv(h, defs.data(), defs.size());
    if (function) h = fnv(h, function, strlen(function));
    if (profile) h = fnv(h, profile, strlen(profile));

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_seen.insert(h).second) return;
    wchar_t name[96];
    swprintf(name, 96, L"\\%hs_%016llx.txt", profile ? profile : kind, static_cast<unsigned long long>(h));
    FILE* f = _wfopen((g_dir + name).c_str(), L"wb");
    if (!f) return;
    fprintf(f, "// %s, profile %s, entry %s\n", kind, profile ? profile : "(asm)", function ? function : "-");
    if (shader) {
        fprintf(f, "// bytecode %016llx\n",
                static_cast<unsigned long long>(bytecodeHash(shader->GetBufferPointer(), shader->GetBufferSize())));
    }
    if (!patch.empty()) fprintf(f, "// gbuffer %s\n", patch.c_str());
    fputs(defs.c_str(), f);
    fwrite(source, 1, length, f);
    fputs(includes.c_str(), f);
    if (shader) {
        ID3DBlob* text = nullptr;
        if (SUCCEEDED(D3DDisassemble(shader->GetBufferPointer(), shader->GetBufferSize(), 0, nullptr, &text)) && text) {
            fputs("\n\n// ==== disassembly\n", f);
            fwrite(text->GetBufferPointer(), 1, text->GetBufferSize() ? text->GetBufferSize() - 1 : 0, f);
            text->Release();
        }
    }
    fclose(f);
    if (g_written++ == 0) TMVS_LOG("gameshaders: writing the game's shaders to %ls", g_dir.c_str());
    else if (g_written % 100 == 0) TMVS_LOG("gameshaders: %d written", g_written);
}

// What the patch did with one shader: the patched ones are known by their bytecode, a failure
// (a patch that should have worked) is logged by itself, the skipped ones go into the summary.
std::string notePatch(const gbufpatch::Outcome& outcome, ID3DBlob* shader, const char* profile) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_lastCompile = GetTickCount();
    switch (outcome.kind) {
        case gbufpatch::Outcome::Patched:
            g_patched++;
            if (shader) g_patchedHashes.insert(bytecodeHash(shader->GetBufferPointer(), shader->GetBufferSize()));
            return "patched: " + outcome.detail;
        case gbufpatch::Outcome::Failed:
            if (g_failed++ < 20) TMVS_LOG("gbuffer: a %s shader is kept as the game's: %s", profile ? profile : "?", outcome.detail.c_str());
            return "kept: " + outcome.detail;
        case gbufpatch::Outcome::Skipped:
            g_skipped[outcome.detail]++;
            return std::string();
        default:
            return std::string();
    }
}

HRESULT WINAPI compileDetour(const char* source, UINT length, const Macro* defines, Include* include, const char* function,
                             const char* profile, DWORD flags, ID3DBlob** shader, ID3DBlob** errors, void** constants) {
    IncludeTap tap;
    tap.inner = include;
    gbufpatch::Outcome outcome;
    // The game's own compile; with GameGBuffer then the patched source (only a sure patch
    // replaces the game's result). Without it, exactly the game's call.
    const HRESULT hr = gbufpatch::compile(g_compile, g_patch, source, length, defines, include ? &tap : nullptr, function, profile, flags, shader,
                                          errors, constants, &outcome);
    ID3DBlob* code = SUCCEEDED(hr) && shader ? *shader : nullptr;
    const std::string note = g_patch ? notePatch(outcome, code, profile) : std::string();
    if (g_dump) dump("hlsl", source, length, defines, function, profile, tap.text, code, note);
    return hr;
}

HRESULT WINAPI assembleDetour(const char* source, UINT length, const Macro* defines, Include* include, DWORD flags, ID3DBlob** shader,
                              ID3DBlob** errors) {
    IncludeTap tap;
    tap.inner = include;
    HRESULT hr = g_assemble(source, length, defines, include ? &tap : nullptr, flags, shader, errors);
    dump("asm", source, length, defines, nullptr, nullptr, tap.text, SUCCEEDED(hr) && shader ? *shader : nullptr, std::string());
    return hr;
}

} // namespace

uint64_t bytecodeHash(const void* code, size_t bytes) {
    const DWORD* t = static_cast<const DWORD*>(code);
    const size_t n = bytes / 4;
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n;) {
        if (i > 0 && (t[i] & 0xFFFF) == 0xFFFE) {
            i += 1 + ((t[i] >> 16) & 0x7FFF);
            continue;
        }
        h = fnv(h, reinterpret_cast<const char*>(&t[i]), 4);
        i++;
    }
    return h;
}

void install() {
    g_dir = log::dataDir() + L"\\gameshaders";
    const DWORD attributes = GetFileAttributesW(g_dir.c_str());
    g_dump = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    const Settings& s = Config::get().settings;
    const bool wantPatch = s.gameGBuffer; // F7 at the start doesn't matter: a patched shader draws the same image
    TMVS_LOG(wantPatch ? "gbuffer: on, the game's pixel shaders are patched as it compiles them"
                       : "gbuffer: off (GameGBuffer), the game's shaders are compiled as they are");
    if (!g_dump && !wantPatch) return;
    const bool compile = hook::patchGameImport("d3dx9_30.dll", "D3DXCompileShader", reinterpret_cast<void*>(&compileDetour),
                                               reinterpret_cast<void**>(&g_compile));
    g_patch = wantPatch && compile;
    const bool assemble = g_dump && hook::patchGameImport("d3dx9_30.dll", "D3DXAssembleShader", reinterpret_cast<void*>(&assembleDetour),
                                                          reinterpret_cast<void**>(&g_assemble));
    TMVS_LOG("gameshaders: D3DXCompileShader %s, D3DXAssembleShader %s", compile ? "hooked" : "not imported",
             assemble ? "hooked" : (g_dump ? "not imported" : "left alone"));
    if (wantPatch && !compile) TMVS_LOG("gbuffer: the game doesn't import D3DXCompileShader: no normals from its shaders");
}

bool patching() {
    return g_patch;
}

void notePixelShader(IDirect3DPixelShader9* shader, const DWORD* code) {
    if (!g_patch || !shader) return;
    const size_t bytes = gbufpatch::bytecodeSize(code);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (bytes && g_patchedHashes.count(bytecodeHash(code, bytes))) g_normalShaders.insert(shader);
    else g_normalShaders.erase(shader);
}

bool writesNormal(IDirect3DPixelShader9* shader) {
    if (!g_patch || !shader) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_normalShaders.count(shader) != 0;
}

int patchedCount() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<int>(g_patchedHashes.size());
}

void tick() {
    if (!g_patch) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    int skipped = 0;
    for (const auto& r : g_skipped) skipped += r.second;
    const int total = g_patched + g_failed + skipped;
    if (total == g_logged || GetTickCount() - g_lastCompile < 3000) return;
    g_logged = total;
    std::string why;
    for (const auto& r : g_skipped) why += (why.empty() ? "" : ", ") + std::to_string(r.second) + " " + r.first;
    // Distinct shaders patched (the game may compile one again); the others counted per compile.
    TMVS_LOG("gbuffer: %d pixel shaders patched, %d left as they are (%s), %d kept after a failed patch",
             static_cast<int>(g_patchedHashes.size()), skipped, why.empty() ? "-" : why.c_str(), g_failed);
}

} // namespace gameshaders
} // namespace tmshaders
