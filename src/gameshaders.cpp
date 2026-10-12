#include "gameshaders.h"
#include "hook.h"
#include "log.h"
#include <windows.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>

namespace tmshaders {
namespace gameshaders {
namespace {

// d3dx9 types, opaque: the exe's import is replaced, d3dx9's headers aren't needed.
struct Macro {
    const char* name;
    const char* definition;
};

// ID3DXInclude: two __stdcall virtual methods, no IUnknown.
struct Include {
    virtual HRESULT __stdcall Open(int type, const char* name, const void* parent, const void** data, UINT* bytes) = 0;
    virtual HRESULT __stdcall Close(const void* data) = 0;
};

using CompileFn = HRESULT(WINAPI*)(const char* source, UINT length, const Macro* defines, Include* include, const char* function,
                                   const char* profile, DWORD flags, ID3DBlob** shader, ID3DBlob** errors, void** constants);
using AssembleFn = HRESULT(WINAPI*)(const char* source, UINT length, const Macro* defines, Include* include, DWORD flags,
                                    ID3DBlob** shader, ID3DBlob** errors);

CompileFn g_compile = nullptr;
AssembleFn g_assemble = nullptr;
std::wstring g_dir;
std::mutex g_mutex;
std::set<uint64_t> g_seen;
int g_written = 0;

// Passes the game's include handler through and keeps what it hands out.
struct IncludeTap : Include {
    Include* inner = nullptr;
    std::string text;
    HRESULT __stdcall Open(int type, const char* name, const void* parent, const void** data, UINT* bytes) override {
        HRESULT hr = inner->Open(type, name, parent, data, bytes);
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

void dump(const char* kind, const char* source, UINT length, const Macro* defines, const char* function, const char* profile,
          const std::string& includes, ID3DBlob* shader) {
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

HRESULT WINAPI compileDetour(const char* source, UINT length, const Macro* defines, Include* include, const char* function,
                             const char* profile, DWORD flags, ID3DBlob** shader, ID3DBlob** errors, void** constants) {
    IncludeTap tap;
    tap.inner = include;
    HRESULT hr = g_compile(source, length, defines, include ? &tap : nullptr, function, profile, flags, shader, errors, constants);
    dump("hlsl", source, length, defines, function, profile, tap.text, SUCCEEDED(hr) && shader ? *shader : nullptr);
    return hr;
}

HRESULT WINAPI assembleDetour(const char* source, UINT length, const Macro* defines, Include* include, DWORD flags, ID3DBlob** shader,
                              ID3DBlob** errors) {
    IncludeTap tap;
    tap.inner = include;
    HRESULT hr = g_assemble(source, length, defines, include ? &tap : nullptr, flags, shader, errors);
    dump("asm", source, length, defines, nullptr, nullptr, tap.text, SUCCEEDED(hr) && shader ? *shader : nullptr);
    return hr;
}

} // namespace

void install() {
    g_dir = log::dataDir() + L"\\gameshaders";
    const DWORD attributes = GetFileAttributesW(g_dir.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return;
    const bool compile = hook::patchGameImport("d3dx9_30.dll", "D3DXCompileShader", reinterpret_cast<void*>(&compileDetour),
                                               reinterpret_cast<void**>(&g_compile));
    const bool assemble = hook::patchGameImport("d3dx9_30.dll", "D3DXAssembleShader", reinterpret_cast<void*>(&assembleDetour),
                                                reinterpret_cast<void**>(&g_assemble));
    TMVS_LOG("gameshaders: D3DXCompileShader %s, D3DXAssembleShader %s", compile ? "hooked" : "not imported",
             assemble ? "hooked" : "not imported");
}

} // namespace gameshaders
} // namespace tmshaders
