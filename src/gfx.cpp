#include "gfx.h"
#include "log.h"
#include <d3dcompiler.h>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>

#pragma comment(lib, "d3dcompiler.lib")

namespace tmshaders {
namespace gfx {
namespace {

ID3DBlob* compileNow(const std::string& source, const char* entry, const char* profile, const char* name, std::string* errors) {
    ID3DBlob* code = nullptr;
    ID3DBlob* messages = nullptr;
    HRESULT hr = D3DCompile(source.data(), source.size(), name, nullptr, nullptr, entry, profile,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);
    if (FAILED(hr) || !code) {
        const char* text = messages ? static_cast<const char*>(messages->GetBufferPointer()) : "unknown error";
        TMVS_LOG("shader %s (%s) failed to compile:\n%s", name, entry, text);
        if (errors) *errors += text;
        release(messages);
        release(code);
        return nullptr;
    }
    release(messages);
    return code;
}

// Compiled shaders, kept for the whole session (see precompile in gfx.h). Keyed by a hash of
// source, entry point and profile. `pending`: the worker is compiling it right now.
struct CacheEntry {
    ID3DBlob* code = nullptr;
    bool pending = false;
};
std::mutex g_cacheMutex;
std::condition_variable g_cacheDone;
std::map<uint64_t, CacheEntry> g_cache;

uint64_t cacheKey(const std::string& source, const char* entry, const char* profile) {
    uint64_t h = 14695981039346656037ull;
    auto add = [&h](const char* p, size_t n) {
        for (size_t i = 0; i < n; i++) h = (h ^ static_cast<unsigned char>(p[i])) * 1099511628211ull;
        h = (h ^ 0xFF) * 1099511628211ull; // a separator
    };
    add(source.data(), source.size());
    add(entry, strlen(entry));
    add(profile, strlen(profile));
    return h;
}

// The compiled code (a reference for the caller): from the cache, or compiled now and kept.
ID3DBlob* compile(const std::string& source, const char* entry, const char* profile, const char* name, std::string* errors) {
    const uint64_t key = cacheKey(source, entry, profile);
    {
        std::unique_lock<std::mutex> lock(g_cacheMutex);
        // The worker is on it: wait instead of compiling it twice.
        g_cacheDone.wait(lock, [&] { return !g_cache[key].pending; });
        CacheEntry& e = g_cache[key];
        if (e.code) {
            e.code->AddRef();
            return e.code;
        }
        e.pending = true; // new (or it failed before: again, for the error text)
    }
    ID3DBlob* code = compileNow(source, entry, profile, name, errors);
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        CacheEntry& e = g_cache[key];
        e.code = code;
        if (code) code->AddRef();
        e.pending = false;
    }
    g_cacheDone.notify_all();
    return code;
}

const char* kQuadVS = R"(
float4 u_HalfPixel : register(c0); // (-1/w, 1/h, 0, 0): D3D9 texel-to-pixel alignment
struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VSOut main(float2 pos : POSITION, float2 uv : TEXCOORD0) {
    VSOut o;
    o.pos = float4(pos + u_HalfPixel.xy, 0.0, 1.0);
    o.uv = uv;
    return o;
}
)";

struct QuadVertex {
    float x, y, u, v;
};

} // namespace

void precompile(const std::vector<ShaderSource>& shaders) {
    for (const ShaderSource& s : shaders) {
        const uint64_t key = cacheKey(s.source, s.entry.c_str(), s.profile.c_str());
        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            if (g_cache.count(key)) continue; // compiled already (or being compiled where it was needed)
            g_cache[key].pending = true;
        }
        ID3DBlob* code = compileNow(s.source, s.entry.c_str(), s.profile.c_str(), s.name.c_str(), nullptr);
        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            CacheEntry& e = g_cache[key];
            e.code = code;
            e.pending = false;
        }
        g_cacheDone.notify_all();
    }
}

void quadShaders(std::vector<ShaderSource>& out) {
    out.push_back({kQuadVS, "main", "vs_3_0", "QuadVS"});
}

IDirect3DPixelShader9* compilePixelShader(IDirect3DDevice9* device, const std::string& source, const char* entry, const char* name,
                                          std::string* errors) {
    ID3DBlob* code = compile(source, entry, "ps_3_0", name, errors);
    if (!code) return nullptr;
    IDirect3DPixelShader9* shader = nullptr;
    device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &shader);
    code->Release();
    return shader;
}

IDirect3DVertexShader9* compileVertexShader(IDirect3DDevice9* device, const std::string& source, const char* entry,
                                            const char* name, std::string* errors, const char* profile) {
    ID3DBlob* code = compile(source, entry, profile, name, errors);
    if (!code) return nullptr;
    IDirect3DVertexShader9* shader = nullptr;
    device->CreateVertexShader(static_cast<const DWORD*>(code->GetBufferPointer()), &shader);
    code->Release();
    return shader;
}

bool Target::create(IDirect3DDevice9* device, UINT w, UINT h, D3DFORMAT fmt) {
    if (valid() && width == w && height == h && format == fmt) return true;
    destroy();
    if (FAILED(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &texture, nullptr)) || !texture) {
        TMVS_LOG("gfx: failed to create %ux%u render target (format %d)", w, h, static_cast<int>(fmt));
        texture = nullptr;
        return false;
    }
    texture->GetSurfaceLevel(0, &surface);
    width = w;
    height = h;
    format = fmt;
    return true;
}

void Target::destroy() {
    release(surface);
    release(texture);
    width = height = 0;
}

bool Quad::init(IDirect3DDevice9* device) {
    if (m_vs) return true;
    m_vs = compileVertexShader(device, kQuadVS, "main", "QuadVS");
    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };
    device->CreateVertexDeclaration(elements, &m_decl);
    return m_vs && m_decl;
}

void Quad::destroy() {
    release(m_vs);
    release(m_decl);
}

void Quad::draw(IDirect3DDevice9* device, UINT targetWidth, UINT targetHeight) {
    const QuadVertex vertices[4] = {
        {-1.0f, 1.0f, 0.0f, 0.0f},
        {1.0f, 1.0f, 1.0f, 0.0f},
        {-1.0f, -1.0f, 0.0f, 1.0f},
        {1.0f, -1.0f, 1.0f, 1.0f},
    };
    const float halfPixel[4] = {-1.0f / static_cast<float>(targetWidth), 1.0f / static_cast<float>(targetHeight), 0.0f, 0.0f};
    D3DVIEWPORT9 viewport = {0, 0, targetWidth, targetHeight, 0.0f, 1.0f};
    device->SetViewport(&viewport);
    device->SetVertexShader(m_vs);
    device->SetVertexDeclaration(m_decl);
    device->SetVertexShaderConstantF(0, halfPixel, 1);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(QuadVertex));
}

} // namespace gfx
} // namespace tmshaders
