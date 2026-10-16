// GameGBuffer offline test (development tool).
//
//   tmvs_gbuftest <gameshaders folder> [--device] [--list]
//
// The game's pixel shaders as the mod dumped them (TMVS\gameshaders, see gameshaders.h) are
// compiled again with the game's own compiler (d3dx9_30.dll, the 32-bit copy) from the dumped
// source, defines and include files, through gbufpatch::compile exactly as the compile hook calls
// it: once with the patch off (must give the game's bytecode, the hash in the dump) and once on.
// --device then draws every patched shader next to the game's on a real D3D9 device with random
// inputs and textures: the game's image (COLOR0) must be bit-identical, COLOR1 a unit normal and
// the eye distance; plus how the driver treats a second render target (fog, alpha test, Clear).
#include "gbufpatch.h"
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace tmshaders;

namespace {

int g_failures = 0;
#define CHECK(c, ...)                                                                                                                      \
    do {                                                                                                                                   \
        printf((c) ? "ok:   " : "FAIL: ");                                                                                                 \
        printf(__VA_ARGS__);                                                                                                               \
        printf("\n");                                                                                                                      \
        if (!(c)) g_failures++;                                                                                                            \
    } while (0)

using DisassembleFn = HRESULT(WINAPI*)(const DWORD* shader, BOOL color, const char* comments, ID3DBlob** out);
gbufpatch::CompileFn g_compile = nullptr;
DisassembleFn g_disassemble = nullptr;

uint64_t bytecodeHash(const void* code, size_t bytes) {
    // gameshaders::bytecodeHash: FNV-1a without the comment tokens.
    const DWORD* t = static_cast<const DWORD*>(code);
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < bytes / 4;) {
        if (i > 0 && (t[i] & 0xFFFF) == 0xFFFE) {
            i += 1 + ((t[i] >> 16) & 0x7FFF);
            continue;
        }
        for (int b = 0; b < 4; b++) h = (h ^ ((t[i] >> (8 * b)) & 0xFF)) * 1099511628211ull;
        i++;
    }
    return h;
}

struct Dump {
    std::string file, profile, entry, source;
    std::vector<std::pair<std::string, std::string>> defines;
    std::vector<std::pair<std::string, std::string>> includes;
    uint64_t bytecode = 0;
    bool hasBytecode = false;
    bool patchedInGame = false; // "// gbuffer patched: ...": the hash is the patched shader's
};

std::string readFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return std::string();
    std::string s;
    char buffer[65536];
    for (size_t n; (n = fread(buffer, 1, sizeof(buffer), f)) > 0;) s.append(buffer, n);
    fclose(f);
    return s;
}

bool readDump(const std::string& path, Dump& d) {
    const std::string text = readFile(path);
    size_t pos = text.find('\n');
    if (pos == std::string::npos) return false;
    const std::string head = text.substr(0, pos);
    const size_t prof = head.find("profile "), entry = head.find(", entry ");
    if (prof == std::string::npos || entry == std::string::npos) return false;
    d.profile = head.substr(prof + 8, head.find(',', prof) - prof - 8);
    d.entry = head.substr(entry + 8);
    pos++;
    if (text.compare(pos, 12, "// bytecode ") == 0) {
        d.bytecode = strtoull(text.c_str() + pos + 12, nullptr, 16);
        d.hasBytecode = true;
        pos = text.find('\n', pos) + 1;
    }
    if (text.compare(pos, 11, "// gbuffer ") == 0) {
        d.patchedInGame = text.compare(pos, 19, "// gbuffer patched:") == 0;
        pos = text.find('\n', pos) + 1;
    }
    while (text.compare(pos, 11, "// #define ") == 0) {
        // A value may span lines (the game's shadow code: each line but the last ends in '\').
        size_t end = text.find('\n', pos);
        while (end != std::string::npos && end > 0 && (text[end - 1] == '\\' || (text[end - 1] == '\r' && end > 1 && text[end - 2] == '\\'))) {
            end = text.find('\n', end + 1);
        }
        const std::string line = text.substr(pos + 11, end - pos - 11);
        const size_t space = line.find(' ');
        d.defines.emplace_back(line.substr(0, space), space == std::string::npos ? std::string() : line.substr(space + 1));
        pos = end + 1;
    }
    const std::string markInclude = "\n// ==== #include \"", markDis = "\n\n// ==== disassembly\n";
    const size_t dis = std::min(text.find(markDis, pos), text.size());
    size_t inc = text.find(markInclude, pos);
    d.source = text.substr(pos, std::min(inc, dis) - pos);
    while (inc != std::string::npos && inc < dis) {
        const size_t nameStart = inc + markInclude.size();
        const size_t nameEnd = text.find("\"\n", nameStart);
        const size_t next = std::min(text.find(markInclude, nameEnd), dis);
        d.includes.emplace_back(text.substr(nameStart, nameEnd - nameStart), text.substr(nameEnd + 2, next - nameEnd - 2));
        inc = next < dis ? next : std::string::npos;
    }
    return true;
}

std::string normalizeName(std::string s) {
    for (char& c : s) c = c == '/' ? '\\' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (size_t at; (at = s.find("\\\\")) != std::string::npos;) s.erase(at, 1);
    return s;
}

// Hands out the dumped include files, as the game's include handler did.
struct DumpInclude : gbufpatch::Include {
    const Dump* dump = nullptr;
    int missing = 0;
    HRESULT __stdcall Open(int, const char* name, const void*, const void** data, UINT* bytes) override {
        for (const auto& inc : dump->includes) {
            if (inc.first == name || normalizeName(inc.first) == normalizeName(name)) {
                if (inc.second == "// (not found)\n") break;
                *data = inc.second.data();
                *bytes = static_cast<UINT>(inc.second.size());
                return S_OK;
            }
        }
        missing++;
        return E_FAIL;
    }
    HRESULT __stdcall Close(const void*) override { return S_OK; }
};

struct Compiled {
    HRESULT hr = E_FAIL;
    std::vector<unsigned char> code;
    std::string errors;
    gbufpatch::Outcome outcome;
    double ms = 0.0;
};

Compiled compileDump(const Dump& d, bool patch) {
    std::vector<gbufpatch::Macro> macros;
    for (const auto& m : d.defines) macros.push_back({m.first.c_str(), m.second.c_str()});
    macros.push_back({nullptr, nullptr});
    DumpInclude include;
    include.dump = &d;
    ID3DBlob* shader = nullptr;
    ID3DBlob* errors = nullptr;
    void* constants = nullptr;
    LARGE_INTEGER a, b, f;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    Compiled c;
    c.hr = gbufpatch::compile(g_compile, patch, d.source.data(), static_cast<UINT>(d.source.size()), macros.data(), &include, d.entry.c_str(),
                              d.profile.c_str(), 0, &shader, &errors, &constants, &c.outcome);
    QueryPerformanceCounter(&b);
    c.ms = 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart);
    if (shader) {
        const unsigned char* p = static_cast<const unsigned char*>(shader->GetBufferPointer());
        c.code.assign(p, p + shader->GetBufferSize());
        shader->Release();
    }
    if (errors) {
        c.errors.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        errors->Release();
    }
    if (constants) static_cast<IUnknown*>(constants)->Release();
    return c;
}

int slots(const std::vector<unsigned char>& code) {
    if (!g_disassemble || code.empty()) return -1;
    ID3DBlob* text = nullptr;
    if (FAILED(g_disassemble(reinterpret_cast<const DWORD*>(code.data()), FALSE, nullptr, &text)) || !text) return -1;
    const std::string s(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize());
    text->Release();
    const size_t at = s.find("approximately ");
    return at == std::string::npos ? -1 : atoi(s.c_str() + at + 14);
}

// --- Device -----------------------------------------------------------------

IDirect3DDevice9* g_device = nullptr;

float halfToFloat(uint16_t h) {
    const unsigned s = h >> 15, e = (h >> 10) & 31, m = h & 1023;
    const float v = e == 0 ? ldexpf(static_cast<float>(m), -24) : (e == 31 ? (m ? NAN : INFINITY) : ldexpf(static_cast<float>(m | 1024), static_cast<int>(e) - 25));
    return s ? -v : v;
}

struct Target {
    IDirect3DTexture9* texture = nullptr;
    IDirect3DSurface9* surface = nullptr;
    void release() {
        if (surface) surface->Release();
        if (texture) texture->Release();
        surface = nullptr;
        texture = nullptr;
    }
};

constexpr UINT kSize = 64;

Target makeTarget() {
    Target t;
    if (SUCCEEDED(g_device->CreateTexture(kSize, kSize, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &t.texture, nullptr))) {
        t.texture->GetSurfaceLevel(0, &t.surface);
    }
    return t;
}

std::vector<uint16_t> readTarget(IDirect3DSurface9* surface) {
    std::vector<uint16_t> out(kSize * kSize * 4);
    IDirect3DSurface9* sys = nullptr;
    if (FAILED(g_device->CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &sys, nullptr))) return out;
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(g_device->GetRenderTargetData(surface, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        for (UINT y = 0; y < kSize; y++) memcpy(&out[y * kSize * 4], static_cast<const char*>(lr.pBits) + y * lr.Pitch, kSize * 8);
        sys->UnlockRect();
    }
    sys->Release();
    return out;
}

// Position + TEXCOORD0..7 + COLOR0..1, each float4: the generic vertex for any ps_2_x input.
struct Vertex {
    float v[11][4];
};

IDirect3DVertexShader9* g_vs = nullptr;
IDirect3DVertexDeclaration9* g_decl = nullptr;

bool makePassThrough() {
    const char* src =
        "struct V { float4 p : POSITION; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;"
        " float4 t4 : TEXCOORD4; float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6; float4 t7 : TEXCOORD7; float4 c0 : COLOR0; float4 c1 : COLOR1; };"
        "V main(V v) { return v; }";
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    if (FAILED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "main", "vs_2_0", 0, 0, &code, &errors))) {
        printf("vs: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return false;
    }
    g_device->CreateVertexShader(static_cast<const DWORD*>(code->GetBufferPointer()), &g_vs);
    code->Release();
    D3DVERTEXELEMENT9 elements[12];
    elements[0] = {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0};
    for (BYTE i = 0; i < 8; i++) elements[1 + i] = {0, static_cast<WORD>(16 * (1 + i)), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, i};
    elements[9] = {0, 16 * 9, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0};
    elements[10] = {0, 16 * 10, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 1};
    elements[11] = D3DDECL_END();
    return SUCCEEDED(g_device->CreateVertexDeclaration(elements, &g_decl)) && g_vs;
}

// Samplers of a shader from its constant table: register and texture type (2D, cube, volume).
struct SamplerUse {
    int reg;
    int type; // D3DXPT_SAMPLER2D 12, SAMPLER3D 13, SAMPLERCUBE 14
};

std::vector<SamplerUse> samplers(const std::vector<unsigned char>& code) {
    std::vector<SamplerUse> out;
    const DWORD* t = reinterpret_cast<const DWORD*>(code.data());
    const size_t n = code.size() / 4;
    for (size_t i = 1; i < n;) {
        if ((t[i] & 0xFFFF) != 0xFFFE) break;
        const size_t words = (t[i] >> 16) & 0x7FFF;
        if (t[i + 1] == 0x42415443u) {
            const char* base = reinterpret_cast<const char*>(t + i + 2);
            const DWORD* header = reinterpret_cast<const DWORD*>(base);
            for (DWORD c = 0; c < header[3]; c++) {
                const char* e = base + header[4] + c * 20;
                WORD set, index, count;
                DWORD typeInfo;
                memcpy(&set, e + 4, 2);
                memcpy(&index, e + 6, 2);
                memcpy(&count, e + 8, 2);
                memcpy(&typeInfo, e + 12, 4);
                WORD type;
                memcpy(&type, base + typeInfo + 2, 2);
                if (set == 3) {
                    for (WORD k = 0; k < count; k++) out.push_back({index + k, type});
                }
            }
        }
        i += 1 + words;
    }
    return out;
}

std::mt19937 g_random(1234);

float uniform(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(g_random);
}

IDirect3DBaseTexture9* randomTexture(int type) {
    if (type == 14) {
        IDirect3DCubeTexture9* cube = nullptr;
        if (FAILED(g_device->CreateCubeTexture(16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, nullptr))) return nullptr;
        for (int f = 0; f < 6; f++) {
            D3DLOCKED_RECT lr{};
            cube->LockRect(static_cast<D3DCUBEMAP_FACES>(f), 0, &lr, nullptr, 0);
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) reinterpret_cast<DWORD*>(static_cast<char*>(lr.pBits) + y * lr.Pitch)[x] = g_random();
            cube->UnlockRect(static_cast<D3DCUBEMAP_FACES>(f), 0);
        }
        return cube;
    }
    if (type == 13) {
        IDirect3DVolumeTexture9* volume = nullptr;
        if (FAILED(g_device->CreateVolumeTexture(8, 8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &volume, nullptr))) return nullptr;
        D3DLOCKED_BOX lb{};
        volume->LockBox(0, &lb, nullptr, 0);
        for (int z = 0; z < 8; z++)
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) reinterpret_cast<DWORD*>(static_cast<char*>(lb.pBits) + z * lb.SlicePitch + y * lb.RowPitch)[x] = g_random();
        volume->UnlockBox(0);
        return volume;
    }
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(g_device->CreateTexture(32, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))) return nullptr;
    D3DLOCKED_RECT lr{};
    tex->LockRect(0, &lr, nullptr, 0);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) reinterpret_cast<DWORD*>(static_cast<char*>(lr.pBits) + y * lr.Pitch)[x] = g_random();
    tex->UnlockRect(0);
    return tex;
}

// A grid of quads, each with its own random inputs (or one quad with `fixed` everywhere).
std::vector<Vertex> makeGeometry(const Vertex* fixed) {
    std::vector<Vertex> out;
    const int cells = fixed ? 1 : 8;
    for (int cy = 0; cy < cells; cy++) {
        for (int cx = 0; cx < cells; cx++) {
            Vertex corner[4];
            for (int k = 0; k < 4; k++) {
                if (fixed) {
                    corner[k] = *fixed;
                } else {
                    for (int a = 1; a < 11; a++)
                        for (int c = 0; c < 4; c++) corner[k].v[a][c] = a >= 9 ? uniform(0.0f, 1.0f) : uniform(-1.5f, 1.5f);
                }
                const float x0 = -1.0f + 2.0f * (cx + (k & 1)) / cells, y0 = -1.0f + 2.0f * (cy + (k >> 1)) / cells;
                corner[k].v[0][0] = x0;
                corner[k].v[0][1] = y0;
                corner[k].v[0][2] = 0.5f;
                corner[k].v[0][3] = 1.0f;
            }
            const int order[6] = {0, 1, 2, 2, 1, 3};
            for (int k : order) out.push_back(corner[k]);
        }
    }
    return out;
}

void draw(IDirect3DPixelShader9* ps, IDirect3DSurface9* rt0, IDirect3DSurface9* rt1, const std::vector<Vertex>& geometry) {
    g_device->SetRenderTarget(0, rt0);
    g_device->SetRenderTarget(1, nullptr);
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
    if (rt1) {
        g_device->SetRenderTarget(0, rt1);
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
        g_device->SetRenderTarget(0, rt0);
        g_device->SetRenderTarget(1, rt1);
    }
    g_device->SetPixelShader(ps);
    g_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(geometry.size() / 3), geometry.data(), sizeof(Vertex));
    g_device->SetRenderTarget(1, nullptr);
}

int semanticIndex(const std::string& semantic, const char* kind) {
    const size_t n = strlen(kind);
    if (semantic.compare(0, n, kind) != 0) return -1;
    return semantic.size() == n ? 0 : atoi(semantic.c_str() + n);
}

// One patched shader against the game's.
void compareOnDevice(const std::string& name, const std::vector<unsigned char>& original, const std::vector<unsigned char>& patched,
                     const gbufpatch::Patch& patch) {
    IDirect3DPixelShader9* a = nullptr;
    IDirect3DPixelShader9* b = nullptr;
    if (FAILED(g_device->CreatePixelShader(reinterpret_cast<const DWORD*>(original.data()), &a)) ||
        FAILED(g_device->CreatePixelShader(reinterpret_cast<const DWORD*>(patched.data()), &b))) {
        CHECK(false, "%s: CreatePixelShader", name.c_str());
        if (a) a->Release();
        return;
    }
    std::vector<IDirect3DBaseTexture9*> textures;
    for (const SamplerUse& s : samplers(original)) {
        IDirect3DBaseTexture9* tex = randomTexture(s.type);
        textures.push_back(tex);
        g_device->SetTexture(s.reg, tex);
        g_device->SetSamplerState(s.reg, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        g_device->SetSamplerState(s.reg, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        g_device->SetSamplerState(s.reg, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        g_device->SetSamplerState(s.reg, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    }
    float constants[32][4];
    for (auto& c : constants)
        for (float& x : c) x = uniform(-1.0f, 1.0f);
    g_device->SetPixelShaderConstantF(0, &constants[0][0], 32);

    Target rt0a = makeTarget(), rt0b = makeTarget(), rt1 = makeTarget();
    const std::vector<Vertex> grid = makeGeometry(nullptr);
    draw(a, rt0a.surface, nullptr, grid);
    draw(b, rt0b.surface, rt1.surface, grid);
    const std::vector<uint16_t> ia = readTarget(rt0a.surface), ib = readTarget(rt0b.surface), n = readTarget(rt1.surface);
    int differ = 0, unit = 0, nonUnit = 0, unwritten = 0;
    for (size_t p = 0; p < ia.size() / 4; p++) {
        if (memcmp(&ia[p * 4], &ib[p * 4], 8) != 0) differ++;
        const float x = halfToFloat(n[p * 4]), y = halfToFloat(n[p * 4 + 1]), z = halfToFloat(n[p * 4 + 2]), w = halfToFloat(n[p * 4 + 3]);
        const float len = sqrtf(x * x + y * y + z * z);
        if (w <= 0.0f || !(len == len)) unwritten++;
        else if (fabsf(len - 1.0f) < 0.01f) unit++;
        else nonUnit++;
    }
    CHECK(differ == 0, "%s: the game's image is the same with the patch (%d of %u pixels differ)", name.c_str(), differ, kSize * kSize);
    CHECK(unit > 0 && nonUnit == 0, "%s: COLOR1 holds unit normals (%d unit, %d not, %d without (degenerate inputs))", name.c_str(), unit, nonUnit,
          unwritten);

    // The same inputs everywhere: the distance is |eye vector| exactly, an input normal comes out normalised.
    Vertex fixed{};
    for (int k = 1; k < 11; k++)
        for (int c = 0; c < 4; c++) fixed.v[k][c] = k >= 9 ? uniform(0.0f, 1.0f) : uniform(-1.5f, 1.5f);
    const int eye = semanticIndex(patch.eyeSemantic, "TEXCOORD");
    draw(b, rt0b.surface, rt1.surface, makeGeometry(&fixed));
    const std::vector<uint16_t> f = readTarget(rt1.surface);
    const size_t centre = (kSize / 2 * kSize + kSize / 2) * 4;
    if (eye >= 0) {
        const float* e = fixed.v[1 + eye];
        const float expected = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        const float got = halfToFloat(f[centre + 3]);
        CHECK(fabsf(got - expected) < 2e-3f * expected + 1e-3f, "%s: COLOR1.a = |%s| (%.4f, expected %.4f)", name.c_str(), patch.eyeSemantic.c_str(), got,
              expected);
    }
    const int normalIn = semanticIndex(patch.normalSemantic, "TEXCOORD");
    if (normalIn >= 0) {
        const float* v = fixed.v[1 + normalIn];
        const float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        float worst = 0.0f;
        for (int c = 0; c < 3; c++) worst = std::max(worst, fabsf(halfToFloat(f[centre + c]) - v[c] / len));
        CHECK(worst < 4e-3f, "%s: COLOR1.xyz = normalize(%s) (off by %.4f)", name.c_str(), patch.normalSemantic.c_str(), worst);
    }
    for (size_t s = 0; s < textures.size(); s++) {
        if (textures[s]) textures[s]->Release();
    }
    for (int s = 0; s < 16; s++) g_device->SetTexture(s, nullptr);
    rt0a.release();
    rt0b.release();
    rt1.release();
    a->Release();
    b->Release();
}

// How the driver treats a second target (what the plugin's binding relies on).
void driverSemantics() {
    const char* vsSrc = "struct O { float4 p : POSITION; float f : FOG; }; O main(float4 p : POSITION) { O o; o.p = p; o.f = 0.25; return o; }";
    const char* psSrc = "struct O { float4 c0 : COLOR0; float4 c1 : COLOR1; }; O main() { O o; o.c0 = float4(0.2, 0.4, 0.6, 0.5); o.c1 = float4(0.25, -0.5, 0.75, 123.0); return o; }";
    ID3DBlob* vsCode = nullptr;
    ID3DBlob* psCode = nullptr;
    D3DCompile(vsSrc, strlen(vsSrc), nullptr, nullptr, nullptr, "main", "vs_2_0", 0, 0, &vsCode, nullptr);
    D3DCompile(psSrc, strlen(psSrc), nullptr, nullptr, nullptr, "main", "ps_2_0", 0, 0, &psCode, nullptr);
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    g_device->CreateVertexShader(static_cast<const DWORD*>(vsCode->GetBufferPointer()), &vs);
    g_device->CreatePixelShader(static_cast<const DWORD*>(psCode->GetBufferPointer()), &ps);
    vsCode->Release();
    psCode->Release();
    g_device->SetVertexShader(vs);
    g_device->SetVertexDeclaration(nullptr);
    g_device->SetFVF(D3DFVF_XYZW);
    g_device->SetPixelShader(ps);
    Target rt0 = makeTarget(), rt1 = makeTarget();
    struct P {
        float x, y, z, w;
    } quad[4] = {{-1, -1, 0.5f, 1}, {-1, 1, 0.5f, 1}, {1, -1, 0.5f, 1}, {1, 1, 0.5f, 1}};
    const auto clearBoth = [&](D3DCOLOR c0, D3DCOLOR c1) {
        g_device->SetRenderTarget(1, nullptr);
        g_device->SetRenderTarget(0, rt1.surface);
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, c1, 1, 0);
        g_device->SetRenderTarget(0, rt0.surface);
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, c0, 1, 0);
    };
    const auto at = [](const std::vector<uint16_t>& v, int c) { return halfToFloat(v[(kSize / 2 * kSize + kSize / 2) * 4 + c]); };
    D3DCAPS9 caps{};
    g_device->GetDeviceCaps(&caps);
    printf("info: %lu simultaneous render targets, post-shader blending on all %d, independent write masks %d\n", caps.NumSimultaneousRTs,
           !!(caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING), !!(caps.PrimitiveMiscCaps & D3DPMISCCAPS_INDEPENDENTWRITEMASKS));

    clearBoth(0, 0);
    HRESULT hr = g_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(P));
    std::vector<uint16_t> a = readTarget(rt0.surface), b = readTarget(rt1.surface);
    CHECK(SUCCEEDED(hr) && fabsf(at(a, 0) - 0.2f) < 1e-3f && at(b, 3) == 0.0f, "a shader writing COLOR1 with no second target: draws, COLOR0 as usual");

    clearBoth(0, 0);
    g_device->SetRenderTarget(1, rt1.surface);
    g_device->SetRenderState(D3DRS_FOGENABLE, TRUE);
    g_device->SetRenderState(D3DRS_FOGCOLOR, 0xFFFF0000);
    g_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(P));
    g_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    g_device->SetRenderTarget(1, nullptr);
    a = readTarget(rt0.surface);
    b = readTarget(rt1.surface);
    CHECK(fabsf(at(a, 0) - 0.2f) > 0.1f && fabsf(at(b, 0) - 0.25f) < 1e-3f && at(b, 3) == 123.0f,
          "fog: the game's target fogged (%.3f), the second not (%.3f %.3f %.3f %.1f)", at(a, 0), at(b, 0), at(b, 1), at(b, 2), at(b, 3));

    clearBoth(0, 0);
    g_device->SetRenderTarget(1, rt1.surface);
    g_device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    g_device->SetRenderState(D3DRS_ALPHAREF, 200);
    g_device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    g_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(P));
    g_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_device->SetRenderTarget(1, nullptr);
    b = readTarget(rt1.surface);
    CHECK(at(b, 3) == 0.0f, "alpha test: a discarded pixel writes no normal either");

    clearBoth(0, 0);
    g_device->SetRenderTarget(1, rt1.surface);
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFFFFFFFF, 1, 0);
    g_device->SetRenderTarget(1, nullptr);
    b = readTarget(rt1.surface);
    CHECK(at(b, 0) == 1.0f, "Clear with the second target bound clears it too (the plugin unbinds it before the game's clears)");

    D3DVIEWPORT9 vp = {2, 3, 5, 7, 0.0f, 1.0f}, got{};
    g_device->SetRenderTarget(0, rt0.surface);
    g_device->SetViewport(&vp);
    g_device->SetRenderTarget(1, rt1.surface);
    g_device->GetViewport(&got);
    g_device->SetRenderTarget(1, nullptr);
    CHECK(got.X == 2 && got.Width == 5 && got.Height == 7, "binding target 1 keeps the game's viewport");
    vp = {0, 0, kSize, kSize, 0.0f, 1.0f};
    g_device->SetViewport(&vp);

    g_device->SetPixelShader(nullptr);
    g_device->SetVertexShader(nullptr);
    ps->Release();
    vs->Release();
    rt0.release();
    rt1.release();
}

bool createDevice() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"tmvs_gbuftest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return false;
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = kSize;
    pp.BackBufferHeight = kSize;
    const HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_device);
    d3d->Release();
    if (FAILED(hr)) return false;
    g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_device->SetRenderState(D3DRS_LIGHTING, FALSE);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: tmvs_gbuftest <gameshaders folder> [--device] [--list]\n");
        return 2;
    }
    const std::string dir = argv[1];
    bool device = false, list = false;
    for (int i = 2; i < argc; i++) {
        device |= !strcmp(argv[i], "--device");
        list |= !strcmp(argv[i], "--list");
    }
    HMODULE d3dx = LoadLibraryA("d3dx9_30.dll");
    if (!d3dx) {
        printf("d3dx9_30.dll not found (DirectX end-user runtime)\n");
        return 2;
    }
    g_compile = reinterpret_cast<gbufpatch::CompileFn>(GetProcAddress(d3dx, "D3DXCompileShader"));
    g_disassemble = reinterpret_cast<DisassembleFn>(GetProcAddress(d3dx, "D3DXDisassembleShader"));
    if (!g_compile) return 2;
    if (device && (!createDevice() || !makePassThrough())) {
        printf("no D3D9 device\n");
        return 2;
    }
    if (device) {
        g_device->BeginScene();
        driverSemantics();
        g_device->SetVertexShader(g_vs);
        g_device->SetVertexDeclaration(g_decl);
    }

    WIN32_FIND_DATAA fd{};
    HANDLE find = FindFirstFileA((dir + "\\ps_*.txt").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        printf("no ps_*.txt in %s\n", dir.c_str());
        return 2;
    }
    std::vector<std::string> files;
    do files.push_back(fd.cFileName);
    while (FindNextFileA(find, &fd));
    FindClose(find);
    std::sort(files.begin(), files.end());

    int same = 0, withHash = 0, patched = 0, skipped = 0, failed = 0;
    double plainMs = 0.0, patchMs = 0.0;
    std::map<std::string, int> reasons;
    for (const std::string& file : files) {
        Dump d;
        if (!readDump(dir + "\\" + file, d)) {
            CHECK(false, "%s: not readable", file.c_str());
            continue;
        }
        // Off: the wrapper is the game's own call.
        const Compiled off = compileDump(d, false);
        if (FAILED(off.hr)) {
            CHECK(false, "%s: the dumped source doesn't compile again: %.200s", file.c_str(), off.errors.c_str());
            continue;
        }
        CHECK(off.outcome.kind == gbufpatch::Outcome::Untouched, "%s: off: nothing patched", file.c_str());
        plainMs += off.ms;
        const Compiled on = compileDump(d, true);
        if (d.hasBytecode) {
            // Dumped with GameGBuffer on in game: the hash is of what the game got, the patched shader.
            const Compiled& mine = d.patchedInGame ? on : off;
            withHash++;
            const bool identical = bytecodeHash(mine.code.data(), mine.code.size()) == d.bytecode;
            same += identical;
            if (!identical) CHECK(false, "%s: recompiled bytecode differs from the game's (dump hash)", file.c_str());
        }
        patchMs += on.ms;
        const gbufpatch::Patch p = gbufpatch::patchSource(d.source.data(), d.source.size(), d.profile.c_str());
        const char* kind = on.outcome.kind == gbufpatch::Outcome::Patched ? "PATCHED" : on.outcome.kind == gbufpatch::Outcome::Failed ? "FAILED" : "skipped";
        if (on.outcome.kind == gbufpatch::Outcome::Patched) {
            patched++;
            CHECK(gbufpatch::writesColorOutput(on.code.data(), on.code.size(), 1) && !gbufpatch::writesColorOutput(off.code.data(), off.code.size(), 1),
                  "%s: patched writes COLOR1, the game's doesn't", file.c_str());
            CHECK(gbufpatch::sameConstants(off.code.data(), off.code.size(), on.code.data(), on.code.size(), nullptr), "%s: constants in place",
                  file.c_str());
            // CreatePixelShader gets no size: the patched shader is found again by its tokens.
            CHECK(gbufpatch::bytecodeSize(reinterpret_cast<const DWORD*>(on.code.data())) == on.code.size(),
                  "%s: the size walked from the tokens is the compiler's (%zu)", file.c_str(), on.code.size());
            printf("      %-28s %s %-7s %s; slots %d -> %d, instructions %d -> %d\n", file.c_str(), d.profile.c_str(), kind, on.outcome.detail.c_str(),
                   slots(off.code), slots(on.code), gbufpatch::instructionCount(off.code.data(), off.code.size()),
                   gbufpatch::instructionCount(on.code.data(), on.code.size()));
            if (device) compareOnDevice(file, off.code, on.code, p);
        } else {
            (on.outcome.kind == gbufpatch::Outcome::Failed ? failed : skipped)++;
            reasons[on.outcome.detail]++;
            // A failure keeps the game's shader: the bytes must be the game's.
            CHECK(on.code == off.code, "%s: %s keeps the game's bytecode", file.c_str(), kind);
            if (list || on.outcome.kind == gbufpatch::Outcome::Failed)
                printf("      %-28s %s %-7s %s\n", file.c_str(), d.profile.c_str(), kind, on.outcome.detail.c_str());
        }
    }
    if (device) g_device->EndScene();
    printf("\n%d pixel shaders: %d patched, %d skipped, %d failed (kept the game's)\n", static_cast<int>(files.size()), patched, skipped, failed);
    for (const auto& r : reasons) printf("  %3d x %s\n", r.second, r.first.c_str());
    printf("recompiled from the dumps: %d of %d identical to the game's bytecode\n", same, withHash);
    printf("compile time: %.0f ms as the game compiles, %.0f ms with the patch (both compiles)\n", plainMs, patchMs);
    printf("%d failures\n", g_failures);
    return g_failures ? 1 : 0;
}
