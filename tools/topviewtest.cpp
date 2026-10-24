// The minimap's picture from above (misc/topview.cpp) on a real Direct3D 9 device, without the game:
// two textured quads drawn the way the game draws a block (its kind of vertex shader with
// GbxVisualPrCamera, a perspective camera looking down, the texture on stage 0) go through
// topview::onDraw and topview::render exactly as plugin.cpp and mapview.cpp call them. Checks
// that each draw is taken once, lands at its place in the picture (row 0 at the area's z0), with
// its texture, the higher quad over the lower one, nothing elsewhere, and that the device state
// is given back (development tool; it can't say which of the game's draws are blocks or which
// texture stage holds a block's colour: that needs the game).
#include "misc/internal.h"
#include "engine.h"
#include "gfx.h"
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <array>
#include <algorithm>
#include <vector>

// casters.cpp lives in the plugin; these are the only parts of hook.cpp it needs.
namespace tmshaders {
namespace hook {
void logMemory(const char*) {}
void beginInternal() {}
void endInternal() {}
} // namespace hook
} // namespace tmshaders

// What the engine side would say: the material being drawn.
namespace tmshaders {
namespace engine {
Material currentMaterial() { return Material::Other; }
const void* currentShader() { return nullptr; }
bool materialFile(const void*, wchar_t* out, size_t size) {
    wcsncpy_s(out, size, L"TestMaterial.Shader.Gbx", _TRUNCATE);
    return true;
}
} // namespace engine
} // namespace tmshaders

using namespace tmshaders;
namespace topview = tmshaders::misc::topview;

namespace {

int g_failed = 0;

void check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failed++;
}

void mul(const float a[16], const float b[16], float out[16]) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
    }
}

const char* kGameVS = R"(
float4x4 GbxVisualPrCamera;
struct Out { float4 pos : POSITION; float2 tc : TEXCOORD0; };
Out main(float4 pos : POSITION, float3 normal : NORMAL, float2 tc : TEXCOORD0) {
    Out o;
    o.pos = mul(pos, GbxVisualPrCamera) + float4(normal * 0.0001, 0);
    o.tc = tc;
    return o;
}
)";

// The game's kind of pixel shader: the colour from a sampler named for it, not on stage 0 (a
// lightmap there, as in the game's lightmapped blocks).
const char* kGamePS = R"(
sampler2D LightMap : register(s0);
sampler2D MapDiffuse : register(s1);
float4 main(float2 tc : TEXCOORD0) : COLOR0 { return tex2D(MapDiffuse, tc) * tex2D(LightMap, tc) * 2; }
)";

struct Vertex {
    float x, y, z, nx, ny, nz, u, v;
};

IDirect3DTexture9* solidTexture(IDirect3DDevice9* device, D3DCOLOR colour) {
    IDirect3DTexture9* t = nullptr;
    if (FAILED(device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) return nullptr;
    D3DLOCKED_RECT r{};
    t->LockRect(0, &r, nullptr, 0);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) reinterpret_cast<D3DCOLOR*>(static_cast<BYTE*>(r.pBits) + y * r.Pitch)[x] = colour;
    }
    t->UnlockRect(0);
    return t;
}


double milliseconds() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

// Waits until the GPU has done everything submitted so far.
void finishGpu(IDirect3DDevice9* device) {
    IDirect3DQuery9* event = nullptr;
    if (FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT, &event))) return;
    event->Issue(D3DISSUE_END);
    while (event->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
    }
    event->Release();
}

// --bench: 3000 meshes of 512 triangles (a 16 x 16 grid of quads over a 32 m cell) at random places
// and turns, as the game's draws of a big map, into a 1024 and a 4096 pixel picture: the CPU time of
// the checks per draw (new, known, after the build-up), and of the redraws per frame with the GPU's.
void bench(IDirect3DDevice9* device, IDirect3DVertexShader9* vs, IDirect3DPixelShader9* ps, IDirect3DVertexDeclaration9* decl, IDirect3DTexture9* texture,
           const float view[16], const float projection[16]) {
    constexpr int kGrid = 16, kMeshes = 3000;
    std::vector<Vertex> vertices;
    std::vector<WORD> indices;
    for (int z = 0; z <= kGrid; z++) {
        for (int x = 0; x <= kGrid; x++) {
            const float fx = x * 2.0f, fz = z * 2.0f;
            vertices.push_back({fx, 0.3f * sinf(fx * 0.5f) * cosf(fz * 0.4f), fz, 0, 1, 0, fx / 8.0f, fz / 8.0f});
        }
    }
    for (int z = 0; z < kGrid; z++) {
        for (int x = 0; x < kGrid; x++) {
            const WORD a = static_cast<WORD>(z * (kGrid + 1) + x), b = static_cast<WORD>(a + 1), c = static_cast<WORD>(a + kGrid + 1), d = static_cast<WORD>(c + 1);
            const WORD quad[6] = {a, b, d, a, d, c};
            indices.insert(indices.end(), quad, quad + 6);
        }
    }
    IDirect3DVertexBuffer9* vb = nullptr;
    IDirect3DIndexBuffer9* ib = nullptr;
    device->CreateVertexBuffer(static_cast<UINT>(vertices.size() * sizeof(Vertex)), 0, 0, D3DPOOL_MANAGED, &vb, nullptr);
    device->CreateIndexBuffer(static_cast<UINT>(indices.size() * sizeof(WORD)), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr);
    void* p = nullptr;
    vb->Lock(0, 0, &p, 0);
    memcpy(p, vertices.data(), vertices.size() * sizeof(Vertex));
    vb->Unlock();
    ib->Lock(0, 0, &p, 0);
    memcpy(p, indices.data(), indices.size() * sizeof(WORD));
    ib->Unlock();
    const UINT nv = static_cast<UINT>(vertices.size()), triangles = static_cast<UINT>(indices.size() / 3);
    for (int cells : {32, 128}) {
        const float side = cells * 32.0f;
        topview::setRegion(reinterpret_cast<const void*>(static_cast<uintptr_t>(0x5000 + cells)), 0.0f, 0.0f, side, side, 200.0f, cells, cells);
        unsigned seed = 12345;
        const auto random = [&seed]() {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(seed >> 8) / 16777216.0f;
        };
        std::vector<std::array<float, 16>> columns(kMeshes);
        for (int i = 0; i < kMeshes; i++) {
            const float x = random() * (side - 48.0f) + 8.0f, z = random() * (side - 48.0f) + 8.0f, y = 8.0f + random() * 80.0f;
            const int turn = static_cast<int>(random() * 4.0f) & 3;
            const float c = turn == 0 ? 1.0f : turn == 2 ? -1.0f : 0.0f, s = turn == 1 ? 1.0f : turn == 3 ? -1.0f : 0.0f;
            const float world[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, x + 16.0f, y, z + 16.0f, 1};
            float wv[16], wvp[16];
            mul(world, view, wv);
            mul(wv, projection, wvp);
            for (int r = 0; r < 4; r++) {
                for (int k = 0; k < 4; k++) columns[i][k * 4 + r] = wvp[r * 4 + k];
            }
        }
        device->BeginScene();
        device->SetVertexDeclaration(decl);
        device->SetStreamSource(0, vb, 0, sizeof(Vertex));
        device->SetIndices(ib);
        device->SetVertexShader(vs);
        device->SetPixelShader(ps);
        device->SetTexture(1, texture);
        const auto frameOfDraws = [&]() {
            double ms = 0.0;
            for (int i = 0; i < kMeshes; i++) {
                device->SetVertexShaderConstantF(0, columns[i].data(), 4);
                const double t0 = milliseconds();
                topview::onDraw(device, D3DPT_TRIANGLELIST, 0, 0, nv, 0, triangles, view, projection);
                ms += milliseconds() - t0;
            }
            return ms;
        };
        device->EndScene();
        finishGpu(device);
        // The build-up: each frame the game's 3000 draws (checks), then the redraws, until all are in.
        int frames = 0;
        double checkSum = 0.0, checkMax = 0.0, cpuSum = 0.0, cpuMax = 0.0, wallSum = 0.0, wallMax = 0.0;
        int last = -1, still = 0;
        while (topview::meshes() < kMeshes && frames < 400 && still < 20) {
            still = topview::meshes() == last ? still + 1 : 0;
            last = topview::meshes();
            device->BeginScene();
            const double checks = frameOfDraws();
            device->EndScene();
            finishGpu(device);
            const double t0 = milliseconds();
            topview::render(device);
            const double t1 = milliseconds();
            finishGpu(device);
            const double t2 = milliseconds();
            frames++;
            checkSum += checks;
            checkMax = std::max(checkMax, checks);
            cpuSum += t1 - t0;
            cpuMax = std::max(cpuMax, t1 - t0);
            wallSum += t2 - t0;
            wallMax = std::max(wallMax, t2 - t0);
        }
        device->BeginScene();
        const double knownMs = frameOfDraws();
        device->EndScene();
        // After the build-up (nothing new for 5 s): one draw in 8 looked at per frame.
        Sleep(5200);
        topview::noteFrame(0.0);
        topview::render(device);
        device->BeginScene();
        const double settledMs = frameOfDraws();
        device->EndScene();
        printf("bench %d x %d px, %d meshes of %u triangles: %d meshes drawn in %d frames\n", cells * 32, cells * 32, kMeshes, triangles,
               topview::meshes(), frames);
        printf("      build-up frames: checks of %d draws %.2f ms a frame (most %.2f); redraws CPU %.2f ms (most %.2f), CPU + GPU until done %.2f ms (most %.2f)\n",
               kMeshes, checkSum / std::max(frames, 1), checkMax, cpuSum / std::max(frames, 1), cpuMax, wallSum / std::max(frames, 1), wallMax);
        printf("      after it: checks of %d known draws %.2f ms a frame while still looking at all, %.2f ms once settled (one in 16)\n", kMeshes, knownMs,
               settledMs);
    }
    vb->Release();
    ib->Release();
}

} // namespace

int main(int argc, char** argv) {
    const bool benchmark = argc > 1 && !strcmp(argv[1], "--bench");
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TMVSTopViewTest";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"topviewtest", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    IDirect3DDevice9* device = nullptr;
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_MIXED_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device))) {
        printf("no Direct3D 9 device\n");
        return 2;
    }

    // Two quads in mesh space (0..32 m in x and z), placed by their world matrices: A at (0, 10, 0),
    // red; B at (16, 20, 16), green, higher, over A's corner.
    const Vertex quad[4] = {{0, 0, 0, 0, 1, 0, 0, 0}, {32, 0, 0, 0, 1, 0, 1, 0}, {32, 0, 32, 0, 1, 0, 1, 1}, {0, 0, 32, 0, 1, 0, 0, 1}};
    const WORD indices[6] = {0, 1, 2, 0, 2, 3};
    IDirect3DVertexBuffer9* vb = nullptr;
    IDirect3DIndexBuffer9* ib = nullptr;
    device->CreateVertexBuffer(sizeof(quad), 0, 0, D3DPOOL_MANAGED, &vb, nullptr);
    device->CreateIndexBuffer(sizeof(indices), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr);
    void* p = nullptr;
    vb->Lock(0, 0, &p, 0);
    memcpy(p, quad, sizeof(quad));
    vb->Unlock();
    ib->Lock(0, 0, &p, 0);
    memcpy(p, indices, sizeof(indices));
    ib->Unlock();
    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };
    IDirect3DVertexDeclaration9* decl = nullptr;
    device->CreateVertexDeclaration(elements, &decl);
    IDirect3DVertexShader9* vs = gfx::compileVertexShader(device, kGameVS, "main", "GameVS");
    IDirect3DPixelShader9* ps = gfx::compilePixelShader(device, kGamePS, "main", "GamePS");
    IDirect3DTexture9* red = solidTexture(device, D3DCOLOR_ARGB(255, 255, 0, 0));
    IDirect3DTexture9* green = solidTexture(device, D3DCOLOR_ARGB(255, 0, 255, 0));
    IDirect3DTexture9* lightmap = solidTexture(device, D3DCOLOR_ARGB(255, 128, 128, 128));
    if (!vb || !ib || !decl || !vs || !ps || !red || !green || !lightmap) {
        printf("setup failed\n");
        return 2;
    }

    // The camera: 300 m above (32, 0, 32) looking straight down, north (z) up on screen.
    // View (row vectors): right = x, up = z, forward = -y.
    const float eye[3] = {32.0f, 300.0f, 32.0f};
    const float view[16] = {1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, -eye[0], -eye[2], eye[1], 1};
    const float n = 1.0f, f = 1000.0f, s = 1.0f / tanf(0.5f);
    const float projection[16] = {s, 0, 0, 0, 0, s, 0, 0, 0, 0, f / (f - n), 1, 0, 0, -n * f / (f - n), 0};

    topview::setWanted(true);
    topview::setRegion(reinterpret_cast<const void*>(0x1234), 0.0f, 0.0f, 64.0f, 64.0f, 100.0f, 2, 2); // 64 x 64 px, up to 100 m

    device->BeginScene();
    device->SetVertexDeclaration(decl);
    device->SetStreamSource(0, vb, 0, sizeof(Vertex));
    device->SetIndices(ib);
    device->SetVertexShader(vs);
    device->SetPixelShader(ps);
    device->SetTexture(0, lightmap);
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    const auto drawQuad = [&](float x, float y, float z, IDirect3DTexture9* texture) {
        const float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1};
        float wv[16], wvp[16], columns[16];
        mul(world, view, wv);
        mul(wv, projection, wvp);
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) columns[c * 4 + r] = wvp[r * 4 + c];
        }
        device->SetVertexShaderConstantF(0, columns, 4);
        device->SetTexture(1, texture);
        topview::onDraw(device, D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2, view, projection);
        device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
    };
    drawQuad(0.0f, 10.0f, 0.0f, red);
    drawQuad(16.0f, 20.0f, 16.0f, green);
    drawQuad(0.0f, 10.0f, 0.0f, red);     // the same draw at the same place again: taken once
    drawQuad(0.6f, 10.4f, -0.7f, red);    // and again, moved by the camera's jitter (under a metre): taken once
    drawQuad(15.3f, 20.8f, 16.9f, green);
    drawQuad(200.0f, 10.0f, 0.0f, red);   // outside the map's area: left out
    drawQuad(0.0f, 500.0f, 0.0f, red);    // above the highest track: left out
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    drawQuad(30.0f, 30.0f, 30.0f, red);   // blended: left out
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->EndScene();

    // The state render() must give back.
    IDirect3DSurface9* backBuffer = nullptr;
    device->GetRenderTarget(0, &backBuffer);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
    const D3DVIEWPORT9 viewport = {0, 0, 64, 64, 0.0f, 1.0f};
    device->SetViewport(&viewport);
    topview::render(device);
    DWORD cull = 0;
    device->GetRenderState(D3DRS_CULLMODE, &cull);
    IDirect3DSurface9* target = nullptr;
    device->GetRenderTarget(0, &target);
    IDirect3DVertexShader9* boundVS = nullptr;
    device->GetVertexShader(&boundVS);
    IDirect3DBaseTexture9* boundTexture = nullptr;
    device->GetTexture(1, &boundTexture);
    check(cull == D3DCULL_CW, "a render state is given back");
    check(target == backBuffer, "the render target is given back");
    check(boundVS == vs, "the vertex shader is given back");
    check(boundTexture == red, "the texture on stage 1 is given back");
    gfx::release(target);
    gfx::release(boundVS);
    gfx::release(boundTexture);
    gfx::release(backBuffer);

    check(topview::meshes() == 2, "two draws in the picture (each seen again, also moved by jitter; one outside the area, one too high, one blended left out)");
    IDirect3DTexture9* picture = topview::picture();
    check(picture != nullptr, "the picture exists");
    if (picture) {
        IDirect3DSurface9* level = nullptr;
        IDirect3DSurface9* copy = nullptr;
        picture->GetSurfaceLevel(0, &level);
        D3DSURFACE_DESC d{};
        level->GetDesc(&d);
        check(d.Width == 64 && d.Height == 64, "64 x 64 px for 2 x 2 cells");
        device->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr);
        device->GetRenderTargetData(level, copy);
        D3DLOCKED_RECT r{};
        copy->LockRect(&r, nullptr, D3DLOCK_READONLY);
        // World (x, z) -> pixel (x, z): 1 px per metre, row 0 at z = 0.
        const auto at = [&](int x, int z) { return reinterpret_cast<const D3DCOLOR*>(static_cast<const BYTE*>(r.pBits) + z * r.Pitch)[x]; };
        const auto describe = [](D3DCOLOR c) {
            char text[64];
            snprintf(text, sizeof(text), "a %u r %u g %u b %u", (c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255, c & 255);
            return std::string(text);
        };
        const D3DCOLOR a = at(8, 8), b = at(40, 40), both = at(24, 24), none = at(56, 8), corner = at(8, 56);
        printf("      A only %s; B only %s; both %s; nothing %s / %s\n", describe(a).c_str(), describe(b).c_str(), describe(both).c_str(), describe(none).c_str(),
               describe(corner).c_str());
        const auto red8 = [](D3DCOLOR c) { return (c >> 16) & 255; };
        const auto green8 = [](D3DCOLOR c) { return (c >> 8) & 255; };
        check((a >> 24) == 255 && red8(a) > 150 && green8(a) < 40, "the lower quad: red, at its place");
        check((b >> 24) == 255 && green8(b) > 150 && red8(b) < 40, "the higher quad: green, at its place");
        check(green8(both) > 150 && red8(both) < 40, "where both are: the higher one");
        check((none >> 24) == 0 && (corner >> 24) == 0, "nothing elsewhere (transparent)");
        // The edges: A covers x, z 0..32 (pixels 0..31), so pixel 31 is A's and 33 (outside B) isn't.
        check((at(31, 4) >> 24) == 255 && (at(33, 4) >> 24) == 0, "the edge at 32 m falls between pixels 31 and 32");
        copy->UnlockRect();
        gfx::release(copy);
        gfx::release(level);
    }
    if (benchmark) bench(device, vs, ps, decl, green, view, projection);
    topview::releaseDevice();
    printf(g_failed ? "%d check(s) failed\n" : "all passed\n", g_failed);
    return g_failed ? 1 : 0;
}
