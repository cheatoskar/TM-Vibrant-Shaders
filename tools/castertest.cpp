// CasterShadows CPU test: a synthetic track of static meshes (a few hundred buffers placed
// thousands of times, like a Stadium map) drawn by a camera that drives along it, with every
// draw handed to casters::recordDraw exactly as plugin.cpp's onDrawIndexed does. Prints the
// CPU time the recording takes per frame and per draw, and what was kept (development tool).
//
//   tmvs_castertest [--instances n] [--visible n] [--frames n] [--update]
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

struct Instance {
    int mesh;
    float world[16];
    float x;
};

const char* kGameVS = R"(
float4x4 GbxVisualPrCamera;
float4 main(float4 pos : POSITION, float3 normal : NORMAL) : POSITION {
    return mul(pos, GbxVisualPrCamera) + float4(normal * 0.0001, 0);
}
)";

} // namespace

int main(int argc, char** argv) {
    int instances = 25000, visible = 8000, frames = 400;
    bool update = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--instances") && i + 1 < argc) instances = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--visible") && i + 1 < argc) visible = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--update")) update = true;
    }
    log::setFileName(L"castertest.log");

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TMVSCasterTest";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"castertest", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    IDirect3DDevice9* device = nullptr;
    // As the game: mixed vertex processing (its CreateDevice flags 0x80).
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_MIXED_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp,
                                         &device))) {
        fprintf(stderr, "cannot create Direct3D 9 device\n");
        return 1;
    }
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
    }
    std::sort(track.begin(), track.end(), [](const Instance& a, const Instance& b) { return a.x < b.x; });

    // The projection: perspective, left-handed (P03 = P13 = 0, P23 = 1, P33 = 0), near 0.5.
    const float zn = 0.5f, zf = 6000.0f, f = 1.0f / tanf(0.5f), aspect = 16.0f / 9.0f;
    const float proj[16] = {f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const double toMs = 1000.0 / static_cast<double>(freq.QuadPart);
    double recordMs = 0.0, worstMs = 0.0, updateMs = 0.0;
    unsigned measured = 0, draws = 0;
    const DWORD start = GetTickCount();
    const float sun[3] = {0.4f, 0.8f, 0.3f};
    for (int frame = 0;; frame++) {
        // Two seconds to settle (the meshes are kept half a second after they were first
        // seen), then `frames` measured.
        const bool settling = GetTickCount() - start < 2000;
        if (!settling && measured >= static_cast<unsigned>(frames)) break;
        // The camera drives along x at 60 m/s (1 m a frame at 60 FPS), looking along +x.
        const float camX = 600.0f + static_cast<float>(frame % 2400), camY = 20.0f, camZ = 1500.0f;
        // View: world -> camera, the camera's axes x = -z world, y = up, z = +x world.
        const float view[16] = {0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0, camZ, -camY, -camX, 1};
        // The visible draws: the instances nearest the camera along x.
        auto first = std::lower_bound(track.begin(), track.end(), camX, [](const Instance& a, float x) { return a.x < x; });
        size_t from = static_cast<size_t>(first - track.begin());
        from = from > static_cast<size_t>(visible / 3) ? from - visible / 3 : 0;
        const size_t to = std::min(track.size(), from + static_cast<size_t>(visible));
        double frameMs = 0.0;
        device->BeginScene();
        device->SetVertexShader(vs);
        device->SetVertexDeclaration(decl);
        for (size_t i = from; i < to; i++) {
            const Instance& in = track[i];
            float wv[16], wvp[16], columns[16];
            mul(in.world, view, wv);
            mul(wv, proj, wvp);
            for (int r = 0; r < 4; r++) {
                for (int c = 0; c < 4; c++) columns[c * 4 + r] = wvp[r * 4 + c];
            }
            device->SetVertexShaderConstantF(0, columns, 4);
            device->SetStreamSource(0, vbs[in.mesh], 0, 32);
            device->SetIndices(ibs[in.mesh]);
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            if (casters::takeTurn()) casters::recordDraw(device, D3DPT_TRIANGLELIST, 0, 0, 24, 0, 12, view, proj);
            QueryPerformanceCounter(&b);
            frameMs += static_cast<double>(b.QuadPart - a.QuadPart) * toMs;
        }
        if (update) {
            float camera[3] = {camX, camY, camZ};
            casters::Maps maps;
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            casters::update(device, sun, camera, maps);
            QueryPerformanceCounter(&b);
            if (!settling) updateMs += static_cast<double>(b.QuadPart - a.QuadPart) * toMs;
        }
        casters::frameDone();
        device->EndScene();
        if (!settling) {
            measured++;
            draws += static_cast<unsigned>(to - from);
            recordMs += frameMs;
            worstMs = std::max(worstMs, frameMs);
        }
    }
    printf("%u frames, %.0f draws a frame: recording %.3f ms a frame (worst %.3f), %.3f us a draw; update %.3f ms a frame; %u meshes kept\n",
           measured, static_cast<double>(draws) / measured, recordMs / measured, worstMs, 1000.0 * recordMs / draws, updateMs / measured,
           casters::keptMeshes());
    casters::reset("test end");
    casters::releaseDevice();
    for (auto* b : vbs) b->Release();
    for (auto* b : ibs) b->Release();
    decl->Release();
    vs->Release();
    device->Release();
    d3d->Release();
    DestroyWindow(window);
    return 0;
}
