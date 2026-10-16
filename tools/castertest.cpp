// CasterShadows CPU test: a synthetic track of static meshes (a few hundred buffers placed
// thousands of times, like a Stadium map) drawn by a camera that drives along it, with every
// draw handed to casters::recordDraw exactly as plugin.cpp's onDrawIndexed does. Prints the
// CPU time the recording takes per frame and per draw, and what was kept (development tool).
//
//   tmvs_castertest [--instances n] [--visible n] [--frames n] [--update] [--alpha f] [--rebuild]
//   tmvs_castertest --verify [--alpha-off]
//
// --update also draws the maps (casters::update) and measures their GPU time per frame.
// --rebuild: the light turns a little every frame, so every frame draws the maps' full budget
// (as after a light change or on a new map): the cost per mesh drawn into the maps.
// --alpha f: that fraction of the instances is alpha-tested (CasterAlphaTest): drawn by a
// game-like shader pair with its own constants, sampler and texture, kept with that state.
// --verify: a lattice (an alpha-tested quad with stripes of alpha 1 and 0) and a solid quad
// lit from straight above: the near map must hold the stripes and the solid quad, and the
// state the alpha-tested draws change must be given back. --alpha-off: as with the setting
// off (the lattice isn't handed over: no shadow from it).
//
// Needs a plain (non-Ex) Direct3D 9 device: the game's meshes are in the managed pool.
#include "casters.h"
#include "gfx.h"
#include "log.h"
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

// casters.cpp lives in the plugin; these are the only parts of hook.cpp it needs.
namespace tmshaders {
namespace hook {
void logMemory(const char*) {}
void beginInternal() {}
void endInternal() {}
} // namespace hook
} // namespace tmshaders

using namespace tmshaders;

namespace {

// Row-vector 4x4 multiply: out = a * b.
void mul(const float a[16], const float b[16], float out[16]) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
    }
}

// world * view * projection, as the shader constants' columns (GbxVisualPrCamera).
void cameraColumns(const float world[16], const float view[16], const float proj[16], float columns[16]) {
    float wv[16], wvp[16];
    mul(world, view, wv);
    mul(wv, proj, wvp);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) columns[c * 4 + r] = wvp[r * 4 + c];
    }
}

struct Instance {
    int mesh;
    float world[16];
    float x;
    bool alpha;
};

const char* kGameVS = R"(
float4x4 GbxVisualPrCamera;
float4 main(float4 pos : POSITION, float3 normal : NORMAL) : POSITION {
    return mul(pos, GbxVisualPrCamera) + float4(normal * 0.0001, 0);
}
)";

// An alpha-tested material as the game's: the camera's matrix not at c0, a texture coordinate
// transform (Tech3 projects some of its coordinates), the alpha from a texture on stage 1
// times a constant.
const char* kAlphaVS = R"(
float4x4 GbxVisualPrCamera : register(c4);
float4 TexScale : register(c9);
struct VSOut { float4 pos : POSITION; float2 tc : TEXCOORD0; };
VSOut main(float4 pos : POSITION, float3 normal : NORMAL, float2 tc : TEXCOORD0) {
    VSOut o;
    o.pos = mul(pos, GbxVisualPrCamera) + float4(normal * 0.0001, 0);
    o.tc = tc * TexScale.xy + TexScale.zw;
    return o;
}
)";

const char* kAlphaPS = R"(
sampler2D MapDiffuse : register(s1);
float4 Tint : register(c2);
float4 main(float2 tc : TEXCOORD0) : COLOR0 {
    float4 d = tex2D(MapDiffuse, tc);
    return float4(d.rgb * Tint.rgb, d.a * Tint.a);
}
)";

// The map's depth into a float target (INTZ reads as depth in .r).
const char* kCopyPS = R"(
sampler2D Map : register(s0);
float4 main(float2 uv : TEXCOORD0) : COLOR0 { return tex2D(Map, uv).r; }
)";

struct Device {
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    HWND window = nullptr;
};

bool createDevice(Device& d) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TMVSCasterTest";
    RegisterClassW(&wc);
    d.window = CreateWindowW(wc.lpszClassName, L"castertest", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    d.d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = d.window;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    // As the game: mixed vertex processing (its CreateDevice flags 0x80).
    return d.d3d && SUCCEEDED(d.d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, d.window,
                                                  D3DCREATE_MIXED_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &d.device));
}

// The alpha material's texture: 8x8, alpha 1 in the left half of each tile, 0 in the right.
IDirect3DTexture9* stripeTexture(IDirect3DDevice9* device) {
    IDirect3DTexture9* t = nullptr;
    if (FAILED(device->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) return nullptr;
    D3DLOCKED_RECT r{};
    t->LockRect(0, &r, nullptr, 0);
    for (int y = 0; y < 8; y++) {
        DWORD* row = reinterpret_cast<DWORD*>(static_cast<unsigned char*>(r.pBits) + y * r.Pitch);
        for (int x = 0; x < 8; x++) row[x] = (x < 4 ? 0xFF000000u : 0u) | 0x808080u;
    }
    t->UnlockRect(0);
    return t;
}

struct AlphaMaterial {
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DTexture9* texture = nullptr;

    bool create(IDirect3DDevice9* device) {
        vs = gfx::compileVertexShader(device, kAlphaVS, "main", "AlphaVS");
        ps = gfx::compilePixelShader(device, kAlphaPS, "main", "AlphaPS");
        texture = stripeTexture(device);
        return vs && ps && texture;
    }
    // The game's state for one alpha-tested draw (the camera's matrix at c4).
    void bind(IDirect3DDevice9* device, const float columns[16]) const {
        device->SetVertexShader(vs);
        device->SetPixelShader(ps);
        device->SetVertexShaderConstantF(4, columns, 4);
        const float scale[4] = {1.0f, 1.0f, 0.0f, 0.0f}, tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        device->SetVertexShaderConstantF(9, scale, 1);
        device->SetPixelShaderConstantF(2, tint, 1);
        device->SetTexture(1, texture);
        device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        device->SetRenderState(D3DRS_ALPHAREF, 128);
        device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    }
    // What the game binds after it (or the pipeline before the maps): none of the above. The
    // maps' draws must bring their own.
    static void unbind(IDirect3DDevice9* device) {
        const float zero[4] = {}, clear[4] = {1.0f, 1.0f, 1.0f, 0.0f};
        device->SetVertexShaderConstantF(9, zero, 1);
        device->SetPixelShaderConstantF(2, clear, 1);
        device->SetTexture(1, nullptr);
        device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHAREF, 0);
        device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_ALWAYS);
    }
    void release() {
        gfx::release(vs);
        gfx::release(ps);
        gfx::release(texture);
    }
};

// GPU time of what's between begin() and end(), read at the end of the frame (waits).
struct GpuTimer {
    IDirect3DQuery9* disjoint = nullptr;
    IDirect3DQuery9* frequency = nullptr;
    IDirect3DQuery9* from = nullptr;
    IDirect3DQuery9* to = nullptr;
    bool create(IDirect3DDevice9* device) {
        return SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &disjoint)) &&
               SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &frequency)) &&
               SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &from)) && SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &to));
    }
    void begin() {
        disjoint->Issue(D3DISSUE_BEGIN);
        from->Issue(D3DISSUE_END);
    }
    void end() {
        to->Issue(D3DISSUE_END);
        disjoint->Issue(D3DISSUE_END);
        frequency->Issue(D3DISSUE_END);
    }
    // Milliseconds, -1 when the counter was unreliable.
    double read() {
        UINT64 a = 0, b = 0, f = 0;
        BOOL bad = TRUE;
        while (to->GetData(&b, sizeof(b), D3DGETDATA_FLUSH) == S_FALSE) {
        }
        while (from->GetData(&a, sizeof(a), D3DGETDATA_FLUSH) == S_FALSE) {
        }
        while (frequency->GetData(&f, sizeof(f), D3DGETDATA_FLUSH) == S_FALSE) {
        }
        while (disjoint->GetData(&bad, sizeof(bad), D3DGETDATA_FLUSH) == S_FALSE) {
        }
        return bad || f == 0 ? -1.0 : 1000.0 * static_cast<double>(b - a) / static_cast<double>(f);
    }
    void release() {
        gfx::release(disjoint);
        gfx::release(frequency);
        gfx::release(from);
        gfx::release(to);
    }
};

// The projection: perspective, left-handed (P03 = P13 = 0, P23 = 1, P33 = 0), near 0.5.
void projection(float out[16]) {
    const float zn = 0.5f, zf = 6000.0f, f = 1.0f / tanf(0.5f), aspect = 16.0f / 9.0f;
    const float p[16] = {f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
    memcpy(out, p, sizeof(p));
}

// View: world -> camera, the camera's axes x = -z world, y = up, z = +x world (looks along +x).
void viewAlongX(float camX, float camY, float camZ, float out[16]) {
    const float v[16] = {0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0, camZ, -camY, -camX, 1};
    memcpy(out, v, sizeof(v));
}

int verify(Device& d, IDirect3DVertexShader9* gameVS, IDirect3DVertexDeclaration9* decl, bool alphaOn) {
    IDirect3DDevice9* device = d.device;
    AlphaMaterial material;
    if (!material.create(device)) {
        fprintf(stderr, "alpha material failed\n");
        return 1;
    }
    // A 20 x 20 m quad (the mesh), texture coordinates 0..10: a stripe tile every 2 m along x.
    float v[4][8] = {};
    for (int i = 0; i < 4; i++) {
        v[i][0] = (i & 1) ? 20.0f : 0.0f;
        v[i][2] = (i & 2) ? 20.0f : 0.0f;
        v[i][4] = 1.0f;
        v[i][6] = v[i][0] * 0.5f;
        v[i][7] = v[i][2] * 0.5f;
    }
    const WORD idx[6] = {0, 1, 2, 2, 1, 3};
    IDirect3DVertexBuffer9* vb = nullptr;
    IDirect3DIndexBuffer9* ib = nullptr;
    device->CreateVertexBuffer(sizeof(v), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, nullptr);
    device->CreateIndexBuffer(sizeof(idx), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr);
    void* p = nullptr;
    vb->Lock(0, 0, &p, 0);
    memcpy(p, v, sizeof(v));
    vb->Unlock();
    ib->Lock(0, 0, &p, 0);
    memcpy(p, idx, sizeof(idx));
    ib->Unlock();
    const float lattice[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 100, 10, 100, 1};
    const float solid[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 140, 10, 100, 1};
    float proj[16], view[16];
    projection(proj);
    viewAlongX(90.0f, 30.0f, 110.0f, view);
    const float sun[3] = {0.0f, 1.0f, 0.0f}; // straight above: map texel = world x, z
    const float camera[3] = {90.0f, 30.0f, 110.0f};

    // Sentinels the maps' draws must give back.
    const float vsSentinel[4] = {1, 2, 3, 4}, psSentinel[4] = {5, 6, 7, 8};
    int restoreErrors = 0;
    casters::Maps maps;
    const DWORD start = GetTickCount();
    for (int frame = 0; GetTickCount() - start < 2500; frame++) {
        device->BeginScene();
        float columns[16];
        device->SetVertexDeclaration(decl);
        device->SetStreamSource(0, vb, 0, 32);
        device->SetIndices(ib);
        // The lattice: handed over only with the setting on (plugin.cpp's onDrawIndexed).
        cameraColumns(lattice, view, proj, columns);
        material.bind(device, columns);
        if (casters::takeTurn() && alphaOn) casters::recordDraw(device, D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2, view, proj, true);
        AlphaMaterial::unbind(device);
        // The solid quad, an opaque draw of the same mesh.
        device->SetVertexShader(gameVS);
        cameraColumns(solid, view, proj, columns);
        device->SetVertexShaderConstantF(0, columns, 4);
        if (casters::takeTurn()) casters::recordDraw(device, D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2, view, proj, false);
        device->SetVertexShaderConstantF(20, vsSentinel, 1);
        device->SetPixelShaderConstantF(40, psSentinel, 1);
        device->SetStreamSource(1, nullptr, 0, 0);
        casters::update(device, sun, camera, maps);
        float vsBack[4], psBack[4], tint[4];
        device->GetVertexShaderConstantF(20, vsBack, 1);
        device->GetPixelShaderConstantF(40, psBack, 1);
        device->GetPixelShaderConstantF(2, tint, 1);
        IDirect3DBaseTexture9* stage1 = nullptr;
        device->GetTexture(1, &stage1);
        DWORD addressU = 0, alphaTest = 0, alphaRef = 0;
        device->GetSamplerState(1, D3DSAMP_ADDRESSU, &addressU);
        device->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
        device->GetRenderState(D3DRS_ALPHAREF, &alphaRef);
        if (memcmp(vsBack, vsSentinel, 16) || memcmp(psBack, psSentinel, 16) || tint[3] != 0.0f || stage1 || addressU != D3DTADDRESS_CLAMP ||
            alphaTest || alphaRef != 0) {
            restoreErrors++;
        }
        gfx::release(stage1);
        casters::frameDone();
        device->EndScene();
        Sleep(16);
    }
    const unsigned kept = casters::keptMeshes(), keptAlpha = casters::keptAlphaMeshes();
    int result = 0;
    if (!maps.nearMap.texture) {
        printf("verify: no near map (%u meshes kept)\n", kept);
        result = 1;
    } else {
        // The map's depth, texel for texel, into memory.
        const UINT size = 4096;
        gfx::Target copy;
        gfx::Quad quad;
        IDirect3DPixelShader9* copyPS = gfx::compilePixelShader(device, kCopyPS, "main", "CopyPS");
        IDirect3DSurface9* memory = nullptr;
        if (!copy.create(device, size, size, D3DFMT_R32F) || !quad.init(device) || !copyPS ||
            FAILED(device->CreateOffscreenPlainSurface(size, size, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &memory, nullptr))) {
            printf("verify: readback resources failed\n");
            return 1;
        }
        device->BeginScene();
        device->SetRenderTarget(0, copy.surface);
        device->SetDepthStencilSurface(nullptr);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        device->SetPixelShader(copyPS);
        device->SetTexture(0, maps.nearMap.texture);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        quad.draw(device, size, size);
        device->EndScene();
        device->SetTexture(0, nullptr);
        device->GetRenderTargetData(copy.surface, memory);
        D3DLOCKED_RECT r{};
        memory->LockRect(&r, nullptr, D3DLOCK_READONLY);
        const casters::MapInfo& m = maps.nearMap;
        auto covered = [&](float x, float y, float z) {
            const float u = m.rows[0][0] * x + m.rows[0][1] * y + m.rows[0][2] * z + m.rows[0][3];
            const float w = m.rows[1][0] * x + m.rows[1][1] * y + m.rows[1][2] * z + m.rows[1][3];
            const int tx = static_cast<int>(floorf(u * size)), ty = static_cast<int>(floorf(w * size));
            if (tx < 0 || ty < 0 || tx >= static_cast<int>(size) || ty >= static_cast<int>(size)) return false;
            const float depth = reinterpret_cast<const float*>(static_cast<const unsigned char*>(r.pBits) + ty * r.Pitch)[tx];
            return depth < 0.9f;
        };
        // The lattice: away from the stripes' edges (a texel is 8 cm), alpha 1 where frac(x / 2) < 0.5.
        unsigned agree = 0, total = 0, coveredLattice = 0, coveredSolid = 0, totalSolid = 0, coveredOutside = 0, totalOutside = 0;
        for (float z = 0.25f; z < 19.8f; z += 0.5f) {
            for (float x = 0.05f; x < 20.0f; x += 0.1f) {
                const float t = x * 0.5f - floorf(x * 0.5f);
                if (fabsf(t - 0.5f) < 0.08f || t < 0.08f || t > 0.92f) continue;
                const bool want = alphaOn && t < 0.5f;
                const bool got = covered(100.0f + x, 10.0f, 100.0f + z);
                total++;
                coveredLattice += got;
                agree += got == want;
                totalSolid++;
                coveredSolid += covered(140.0f + x, 10.0f, 100.0f + z);
                // Between the two quads and beside them: nothing.
                totalOutside++;
                coveredOutside += covered(100.0f + x, 10.0f, 125.0f + z);
            }
        }
        memory->UnlockRect();
        printf("verify (%s): %u meshes kept (%u alpha-tested); lattice: %u of %u points as expected (%.1f %% covered, want %s); "
               "solid quad %.1f %% covered; outside %.1f %% covered; state given back %s\n",
               alphaOn ? "CasterAlphaTest on" : "CasterAlphaTest off", kept, keptAlpha, agree, total, 100.0 * coveredLattice / total,
               alphaOn ? "50 %" : "0 %", 100.0 * coveredSolid / totalSolid, 100.0 * coveredOutside / totalOutside,
               restoreErrors ? "NOT always" : "every frame");
        const bool ok = agree >= total * 99 / 100 && coveredSolid == totalSolid && coveredOutside == 0 && restoreErrors == 0 &&
                        keptAlpha == (alphaOn ? 1u : 0u);
        printf("verify: %s\n", ok ? "PASS" : "FAIL");
        result = ok ? 0 : 1;
        memory->Release();
        copyPS->Release();
        copy.destroy();
        quad.destroy();
    }
    casters::reset("test end");
    casters::releaseDevice();
    material.release();
    vb->Release();
    ib->Release();
    return result;
}

} // namespace

int main(int argc, char** argv) {
    int instances = 25000, visible = 8000, frames = 400;
    bool update = false, verifyOnly = false, alphaOff = false, rebuild = false;
    float alphaShare = 0.0f;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--instances") && i + 1 < argc) instances = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--visible") && i + 1 < argc) visible = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--alpha") && i + 1 < argc) alphaShare = static_cast<float>(atof(argv[++i]));
        else if (!strcmp(argv[i], "--update")) update = true;
        else if (!strcmp(argv[i], "--verify")) verifyOnly = true;
        else if (!strcmp(argv[i], "--alpha-off")) alphaOff = true;
        else if (!strcmp(argv[i], "--rebuild")) update = rebuild = true;
    }
    log::setFileName(L"castertest.log");

    Device d;
    if (!createDevice(d)) {
        fprintf(stderr, "cannot create Direct3D 9 device\n");
        return 1;
    }
    IDirect3DDevice9* device = d.device;
    IDirect3DVertexShader9* vs = gfx::compileVertexShader(device, kGameVS, "main", "GameVS");
    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };
    IDirect3DVertexDeclaration9* decl = nullptr;
    device->CreateVertexDeclaration(elements, &decl);
    if (!vs || !decl) {
        fprintf(stderr, "shader or declaration failed\n");
        return 1;
    }
    if (verifyOnly) {
        const int result = verify(d, vs, decl, !alphaOff);
        decl->Release();
        vs->Release();
        device->Release();
        d.d3d->Release();
        DestroyWindow(d.window);
        return result;
    }
    AlphaMaterial material;
    if (alphaShare > 0.0f && !material.create(device)) {
        fprintf(stderr, "alpha material failed\n");
        return 1;
    }
    GpuTimer timer;
    const bool timed = update && timer.create(device);

    // 300 meshes (boxes of 24 vertices, 12 triangles), several draws each like the game's parts.
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const int kMeshes = 300;
    std::vector<IDirect3DVertexBuffer9*> vbs(kMeshes);
    std::vector<IDirect3DIndexBuffer9*> ibs(kMeshes);
    for (int m = 0; m < kMeshes; m++) {
        const float sx = 2.0f + 30.0f * unit(rng), sy = 1.0f + 20.0f * unit(rng), sz = 2.0f + 30.0f * unit(rng);
        float v[24][8] = {};
        for (int i = 0; i < 24; i++) {
            v[i][0] = (i & 1) ? sx : -sx;
            v[i][1] = (i & 2) ? sy : 0.0f;
            v[i][2] = (i & 4) ? sz : -sz;
            v[i][6] = v[i][0] * 0.25f; // texture coordinates (the alpha-tested ones' stripes)
            v[i][7] = (v[i][1] + v[i][2]) * 0.25f;
        }
        WORD idx[36];
        for (int i = 0; i < 36; i++) idx[i] = static_cast<WORD>((i * 7) % 24);
        device->CreateVertexBuffer(sizeof(v), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vbs[m], nullptr);
        device->CreateIndexBuffer(sizeof(idx), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ibs[m], nullptr);
        void* p = nullptr;
        vbs[m]->Lock(0, 0, &p, 0);
        memcpy(p, v, sizeof(v));
        vbs[m]->Unlock();
        ibs[m]->Lock(0, 0, &p, 0);
        memcpy(p, idx, sizeof(idx));
        ibs[m]->Unlock();
    }
    // The track: 4 km along x, 3 km across, the instances sorted along x.
    std::vector<Instance> track(static_cast<size_t>(instances));
    for (Instance& in : track) {
        in.mesh = static_cast<int>(unit(rng) * kMeshes) % kMeshes;
        const float a = 1.5707963f * static_cast<int>(unit(rng) * 4.0f);
        const float c = cosf(a), s = sinf(a);
        const float x = 4000.0f * unit(rng), y = 9.0f + 8.0f * static_cast<int>(unit(rng) * 4.0f), z = 3000.0f * unit(rng);
        const float w[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, x, y, z, 1};
        memcpy(in.world, w, sizeof(w));
        in.x = x;
        in.alpha = unit(rng) < alphaShare;
    }
    std::sort(track.begin(), track.end(), [](const Instance& a, const Instance& b) { return a.x < b.x; });

    float proj[16];
    projection(proj);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const double toMs = 1000.0 / static_cast<double>(freq.QuadPart);
    double recordMs = 0.0, worstMs = 0.0, updateMs = 0.0, updateWorst = 0.0, gpuMs = 0.0, gpuWorst = 0.0;
    unsigned measured = 0, draws = 0, alphaDraws = 0, gpuFrames = 0;
    const DWORD start = GetTickCount();
    for (int frame = 0;; frame++) {
        const float sun[3] = {rebuild && (frame & 1) ? 0.41f : 0.4f, 0.8f, 0.3f};
        // Two seconds to settle (the meshes are kept half a second after they were first
        // seen), then `frames` measured.
        const bool settling = GetTickCount() - start < 2000;
        if (!settling && measured >= static_cast<unsigned>(frames)) break;
        // The camera drives along x at 60 m/s (1 m a frame at 60 FPS), looking along +x.
        const float camX = 600.0f + static_cast<float>(frame % 2400), camY = 20.0f, camZ = 1500.0f;
        float view[16];
        viewAlongX(camX, camY, camZ, view);
        // The visible draws: the instances nearest the camera along x.
        auto first = std::lower_bound(track.begin(), track.end(), camX, [](const Instance& a, float x) { return a.x < x; });
        size_t from = static_cast<size_t>(first - track.begin());
        from = from > static_cast<size_t>(visible / 3) ? from - visible / 3 : 0;
        const size_t to = std::min(track.size(), from + static_cast<size_t>(visible));
        double frameMs = 0.0;
        device->BeginScene();
        device->SetVertexShader(vs);
        device->SetVertexDeclaration(decl);
        bool alphaBound = false;
        for (size_t i = from; i < to; i++) {
            const Instance& in = track[i];
            float columns[16];
            cameraColumns(in.world, view, proj, columns);
            if (in.alpha) {
                material.bind(device, columns);
                alphaBound = true;
                alphaDraws += !settling;
            } else {
                if (alphaBound) {
                    device->SetVertexShader(vs);
                    device->SetPixelShader(nullptr);
                    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                    alphaBound = false;
                }
                device->SetVertexShaderConstantF(0, columns, 4);
            }
            device->SetStreamSource(0, vbs[in.mesh], 0, 32);
            device->SetIndices(ibs[in.mesh]);
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            if (casters::takeTurn()) casters::recordDraw(device, D3DPT_TRIANGLELIST, 0, 0, 24, 0, 12, view, proj, in.alpha);
            QueryPerformanceCounter(&b);
            frameMs += static_cast<double>(b.QuadPart - a.QuadPart) * toMs;
        }
        if (alphaBound) AlphaMaterial::unbind(device);
        if (update) {
            float camera[3] = {camX, camY, camZ};
            casters::Maps maps;
            LARGE_INTEGER a, b;
            if (timed) timer.begin();
            QueryPerformanceCounter(&a);
            casters::update(device, sun, camera, maps);
            QueryPerformanceCounter(&b);
            if (timed) timer.end();
            if (!settling) updateWorst = std::max(updateWorst, static_cast<double>(b.QuadPart - a.QuadPart) * toMs);
            if (!settling) updateMs += static_cast<double>(b.QuadPart - a.QuadPart) * toMs;
        }
        casters::frameDone();
        device->EndScene();
        if (timed) {
            const double ms = timer.read();
            if (!settling && ms >= 0.0) {
                gpuMs += ms;
                gpuWorst = std::max(gpuWorst, ms);
                gpuFrames++;
            }
        }
        if (!settling) {
            measured++;
            draws += static_cast<unsigned>(to - from);
            recordMs += frameMs;
            worstMs = std::max(worstMs, frameMs);
        }
    }
    printf("%u frames, %.0f draws a frame (%.0f alpha-tested): recording %.3f ms a frame (worst %.3f), %.3f us a draw; update %.3f ms a "
           "frame (worst %.3f); %u meshes kept (%u alpha-tested)\n",
           measured, static_cast<double>(draws) / measured, static_cast<double>(alphaDraws) / measured, recordMs / measured, worstMs,
           1000.0 * recordMs / draws, updateMs / measured, updateWorst, casters::keptMeshes(), casters::keptAlphaMeshes());
    if (timed && gpuFrames) printf("GPU: the maps' draws %.3f ms a frame (worst %.3f), %u frames\n", gpuMs / gpuFrames, gpuWorst, gpuFrames);
    casters::reset("test end");
    casters::releaseDevice();
    timer.release();
    material.release();
    for (auto* b : vbs) b->Release();
    for (auto* b : ibs) b->Release();
    decl->Release();
    vs->Release();
    device->Release();
    d.d3d->Release();
    DestroyWindow(d.window);
    return 0;
}
