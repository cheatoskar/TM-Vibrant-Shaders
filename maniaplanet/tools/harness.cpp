// Offline test of the ManiaPlanet proxy without the game.
//
//   mp_harness <build>/d3d11.dll <tonemap.dxbc> <ui.dxbc> <scene.dxbc> [--srgb]
//
// The .dxbc files are the engine's own pixel shaders from ManiaPlanet's GpuCache
// (tools/gpucache.py --dump), so the proxy recognises them exactly as in the game. Each frame
// draws like the engine: a scene draw with the depth buffer bound, the tone map into the back
// buffer, an interface draw, then a red "HUD" box. The engine shaders draw into a 1x1 viewport (their
// inputs aren't set up); the back buffer is filled with a known grey by a clear instead.
//
// Checks: the mod's pass lands between the tone map and the interface (grey changed, red box
// untouched), the depth buffer is found, the Present fallback works when there is no interface, and a
// capture writes its files. Set TMVS_MP_DATA to a scratch folder first (settings, log, captures).
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) g_failures++;
}

std::vector<char> readFile(const char* path) {
    std::vector<char> data;
    FILE* f = fopen(path, "rb");
    if (!f) return data;
    fseek(f, 0, SEEK_END);
    data.resize(static_cast<size_t>(ftell(f)));
    fseek(f, 0, SEEK_SET);
    fread(data.data(), 1, data.size(), f);
    fclose(f);
    return data;
}

const char* kShaders = R"(
struct VSOut { float4 pos : SV_Position; };
VSOut VS(uint id : SV_VertexID) {
    VSOut o; float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1); return o;
}
float4 PS_Red(VSOut i) : SV_Target { return float4(1, 0, 0, 0.75); } // plain red is an engine shader too
// What "Tech3 Block DefReadP1 SelfIllum" reads from its vertex shader: a vertex colour of 0.5 (its
// ambient), an identity tangent frame, the view along +z, the textures' middle.
struct SIOut {
    float4 pos : SV_Position; float3 colour : COLOR0; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; float4 t4 : TEXCOORD4; float2 t5 : TEXCOORD5;
};
SIOut VS_SI(uint id : SV_VertexID) {
    SIOut o; float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
    o.colour = 0.5; o.t0 = float4(1, 0, 0, 0); o.t1 = float4(0, 1, 0, 0); o.t2 = float4(0, 0, 1, 1);
    o.t3 = float4(0.5, 0.5, 0, 0); o.t4 = 0; o.t5 = 0.5;
    return o;
}
)";

float halfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
    float v = exponent == 0 ? mantissa / 1024.0f * std::ldexp(1.0f, -14)
                            : (1.0f + mantissa / 1024.0f) * std::ldexp(1.0f, static_cast<int>(exponent) - 15);
    return sign ? -v : v;
}

// The first texel (rgb) of a capture's RGBA16F DDS.
bool firstTexel(const std::string& path, float rgb[3]) {
    const std::vector<char> dds = readFile(path.c_str());
    const size_t at = dds.size() > 88 && !memcmp(&dds[84], "DX10", 4) ? 148 : 128;
    if (dds.size() < at + 8) return false;
    for (int i = 0; i < 3; i++) {
        uint16_t h = 0;
        memcpy(&h, &dds[at + 2 * i], 2);
        rgb[i] = halfToFloat(h);
    }
    return true;
}

ID3DBlob* compile(const char* entry, const char* profile) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    D3DCompile(kShaders, strlen(kShaders), "harness", nullptr, nullptr, entry, profile, 0, 0, &code, &errors);
    if (errors) {
        printf("%s\n", static_cast<const char*>(errors->GetBufferPointer()));
        errors->Release();
    }
    return code;
}

// RGBA rows, top first.
bool readBmp(const char* path, std::vector<unsigned char>& rgba, UINT& w, UINT& h) {
    const std::vector<char> f = readFile(path);
    if (f.size() < 54 || f[0] != 'B' || f[1] != 'M') return false;
    int32_t width = 0, height = 0;
    uint16_t bits = 0;
    uint32_t offset = 0;
    memcpy(&offset, &f[10], 4);
    memcpy(&width, &f[18], 4);
    memcpy(&height, &f[22], 4);
    memcpy(&bits, &f[28], 2);
    if ((bits != 24 && bits != 32) || width <= 0 || height == 0) return false;
    const bool bottomUp = height > 0;
    w = static_cast<UINT>(width);
    h = static_cast<UINT>(bottomUp ? height : -height);
    const UINT bpp = bits / 8, row = (w * bpp + 3) & ~3u;
    if (offset + static_cast<size_t>(row) * h > f.size()) return false;
    rgba.assign(static_cast<size_t>(w) * h * 4, 255);
    for (UINT y = 0; y < h; y++) {
        const char* src = &f[offset + static_cast<size_t>(row) * (bottomUp ? h - 1 - y : y)];
        for (UINT x = 0; x < w; x++) {
            unsigned char* d = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            d[0] = static_cast<unsigned char>(src[x * bpp + 2]);
            d[1] = static_cast<unsigned char>(src[x * bpp + 1]);
            d[2] = static_cast<unsigned char>(src[x * bpp]);
        }
    }
    return true;
}

bool writeBmp(const char* path, const std::vector<unsigned char>& rgba, UINT w, UINT h) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
    unsigned char hd[54] = {'B', 'M'};
    const uint32_t offset = 54, info = 40, planesBits = 1 | (24 << 16), image = row * h;
    const int32_t iw = static_cast<int32_t>(w), ih = static_cast<int32_t>(h);
    memcpy(hd + 2, &size, 4);
    memcpy(hd + 10, &offset, 4);
    memcpy(hd + 14, &info, 4);
    memcpy(hd + 18, &iw, 4);
    memcpy(hd + 22, &ih, 4);
    memcpy(hd + 26, &planesBits, 4);
    memcpy(hd + 34, &image, 4);
    fwrite(hd, 1, 54, f);
    std::vector<unsigned char> line(row, 0);
    for (int y = static_cast<int>(h) - 1; y >= 0; y--) {
        for (UINT x = 0; x < w; x++) {
            const unsigned char* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            line[x * 3] = p[2];
            line[x * 3 + 1] = p[1];
            line[x * 3 + 2] = p[0];
        }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
    return true;
}

void viewport(ID3D11DeviceContext* ctx, float x, float y, float w, float h) {
    D3D11_VIEWPORT vp = {x, y, w, h, 0, 1};
    ctx->RSSetViewports(1, &vp);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 6) {
        printf("usage: mp_harness <d3d11.dll> <tonemap.dxbc> <ui.dxbc> <scene.dxbc> <defread.dxbc> "
               "[--srgb | --grade in.bmp out.bmp preset | --decomp decomp.dxbc] [--shadow pssm.dxbc]\n");
        return 2;
    }
    const bool srgb = argc > 6 && strcmp(argv[6], "--srgb") == 0;
    // --grade: run one picture through a preset instead of the checks (to look at the presets).
    const bool grade = argc > 9 && strcmp(argv[6], "--grade") == 0;
    // --decomp: Tech3/DeferredDeCompFaceNormal_p, for the check that the light mask follows the
    // engine's new targets after a map change (that pass reads the G-buffer before the light mask).
    const char* decompPath = argc > 7 && strcmp(argv[6], "--decomp") == 0 ? argv[7] : nullptr;
    // --shadow: Tech3/DeferredShadowPssm_p with 5 splits (constants 576 bytes, like in a race), for
    // the soft shadow checks (SoftShadows: the mod's shader draws the engine's sun shadow pass).
    const char* shadowPath = nullptr;
    for (int i = 6; i + 1 < argc; i++)
        if (strcmp(argv[i], "--shadow") == 0) shadowPath = argv[i + 1];
    // --selfillum: "Tech3 Block DefReadP1 SelfIllum" (gpucache.py --dump), for the check of
    // EngineLightDraws: its own light goes into the light buffer, its lit surface doesn't, and the
    // engine's image stays as it was.
    const char* selfIllumPath = argc > 7 && strcmp(argv[6], "--selfillum") == 0 ? argv[7] : nullptr;
    std::vector<unsigned char> picture;
    UINT W = 640, H = 360;
    if (grade && !readBmp(argv[7], picture, W, H)) {
        printf("cannot read %s (24 or 32-bit BMP)\n", argv[7]);
        return 2;
    }
    // Settings, log and captures go to a scratch folder; the test sets its values after start.
    char data[MAX_PATH] = {};
    if (!GetEnvironmentVariableA("TMVS_MP_DATA", data, MAX_PATH)) {
        printf("set TMVS_MP_DATA to a scratch folder first\n");
        return 2;
    }
    CreateDirectoryA(data, nullptr);
    HMODULE proxy = LoadLibraryA(argv[1]);
    if (!proxy) {
        printf("cannot load %s (%lu)\n", argv[1], GetLastError());
        return 2;
    }
    auto createDevice = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(proxy, "D3D11CreateDevice"));
    auto stats = reinterpret_cast<void(WINAPI*)(int*)>(GetProcAddress(proxy, "TmvsMpStats"));
    auto requestCapture = reinterpret_cast<void(WINAPI*)()>(GetProcAddress(proxy, "TmvsMpCapture"));
    auto set = reinterpret_cast<int(WINAPI*)(const char*, const char*)>(GetProcAddress(proxy, "TmvsMpSet"));
    if (!createDevice || !stats || !requestCapture || !set) {
        printf("exports missing\n");
        return 2;
    }

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "mp_harness";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("mp_harness", "mp_harness", WS_OVERLAPPEDWINDOW, 0, 0, W, H, nullptr, nullptr, wc.hInstance, nullptr);

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    // No debug layer: it wraps the context in an object of its own, unlike the game's device.
    HRESULT hr = createDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &ctx);
    if (FAILED(hr)) {
        printf("device failed 0x%08lx\n", static_cast<unsigned long>(hr));
        return 2;
    }
    // Like ManiaPlanet: multithread protection switched on after the device exists. The runtime
    // rewrites the context's vtable then (this once removed every hook of the mod).
    ID3D10Multithread* multithread = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
        multithread->Release();
    }

    // The look for the checks: the engine's tone map, exposure +1 stop on the finished image and
    // nothing else (the TMNF settings at their neutral values). --grade: a preset instead.
    const char* neutral[][2] = {{"Enabled", "1"},     {"HdrStage", "0"},    {"Placement", "0"},  {"FallbackAtEnd", "1"},
                                {"ToneCurve", "0"},   {"Exposure", "1"},    {"Contrast", "1"},   {"Saturation", "1"},
                                {"Vibrance", "0"},    {"Temperature", "0"}, {"Tint", "0"},       {"Lift", "0"},
                                {"Gamma", "1"},       {"Gain", "1"},        {"SplitToning", "0"}, {"Vignette", "0"},
                                {"FilmGrain", "0"},   {"ChromaticAberration", "0"},              {"Sharpen", "0"},
                                {"Bloom", "0"},       {"Lut", ""}};
    if (grade) {
        set("HdrStage", "0");
        if (!set("Preset", argv[9])) {
            printf("unknown preset %s\n", argv[9]);
            return 2;
        }
    } else {
        for (const auto& k : neutral) {
            if (!set(k[0], k[1])) {
                printf("unknown setting %s\n", k[0]);
                return 2;
            }
        }
    }

    // Like the game: the swap chain comes from a DXGI factory, not from the device call.
    IDXGIFactory1* factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = W;
    sd.BufferDesc.Height = H;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* swapChain = nullptr;
    hr = factory->CreateSwapChain(device, &sd, &swapChain);
    if (FAILED(hr)) {
        printf("swap chain failed 0x%08lx\n", static_cast<unsigned long>(hr));
        return 2;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    ID3D11RenderTargetView* bbRtv = nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC rvd = {};
    rvd.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(backBuffer, &rvd, &bbRtv);

    // The engine's HDR target and depth buffer.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W;
    td.Height = H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* hdr = nullptr;
    ID3D11RenderTargetView* hdrRtv = nullptr;
    ID3D11ShaderResourceView* hdrSrv = nullptr;
    device->CreateTexture2D(&td, nullptr, &hdr);
    device->CreateRenderTargetView(hdr, nullptr, &hdrRtv);
    device->CreateShaderResourceView(hdr, nullptr, &hdrSrv);
    td.Format = DXGI_FORMAT_R24G8_TYPELESS;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* depth = nullptr;
    device->CreateTexture2D(&td, nullptr, &depth);
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {};
    dvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ID3D11DepthStencilView* dsv = nullptr;
    device->CreateDepthStencilView(depth, &dvd, &dsv);

    ID3DBlob* vsCode = compile("VS", "vs_4_0");
    ID3DBlob* redCode = compile("PS_Red", "ps_4_0");
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* red = nullptr;
    device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs);
    device->CreatePixelShader(redCode->GetBufferPointer(), redCode->GetBufferSize(), nullptr, &red);
    ID3D11PixelShader* engine[4] = {};
    for (int i = 0; i < 4; i++) {
        const std::vector<char> code = readFile(argv[2 + i]);
        if (code.empty() || FAILED(device->CreatePixelShader(code.data(), code.size(), nullptr, &engine[i]))) {
            printf("cannot create engine shader %s\n", argv[2 + i]);
            return 2;
        }
    }
    ID3D11PixelShader *toneMap = engine[0], *ui = engine[1], *scene = engine[2], *lightRead = engine[3];
    ID3D11PixelShader* decomp = nullptr;
    if (decompPath) {
        const std::vector<char> code = readFile(decompPath);
        if (code.empty() || FAILED(device->CreatePixelShader(code.data(), code.size(), nullptr, &decomp))) {
            printf("cannot create engine shader %s\n", decompPath);
            return 2;
        }
    }
    bool withDecomp = false; // the frames draw DeferredDeCompFaceNormal_p before the light-read pass
    ID3D11PixelShader* selfIllum = nullptr;
    if (selfIllumPath) {
        const std::vector<char> code = readFile(selfIllumPath);
        if (code.empty() || FAILED(device->CreatePixelShader(code.data(), code.size(), nullptr, &selfIllum))) {
            printf("cannot create engine shader %s\n", selfIllumPath);
            return 2;
        }
    }
    bool withSelfIllum = false; // the frames draw it after the light-read pass

    // The engine's sun shadow pass (--shadow) into a light mask of its own: a plane facing the light
    // 10 m from the camera, and in every split the left half of it under a caster 20 m nearer the
    // light (the map mirrors x: the shadow falls on the right half of the screen).
    const UINT SW = 128, SH = 64, tile = 64;
    ID3D11PixelShader* shadowPs = nullptr;
    ID3D11Texture2D* shadowMask = nullptr;
    ID3D11Texture2D* shadowStaging = nullptr;
    ID3D11RenderTargetView* shadowMaskRtv = nullptr;
    ID3D11ShaderResourceView* shadowSrv[5] = {}; // depth, static maps, split atlas, face normal
    ID3D11Buffer* shadowCb[2] = {};               // ShaderP (b0), SceneP (b13)
    ID3D11SamplerState* shadowSampler = nullptr;
    ID3D11Buffer* gameCb5 = nullptr;              // the game's own at slot 5 (where the mod binds its)
    ID3D11SamplerState* gameSampler5 = nullptr;
    std::vector<unsigned char> shadowPixels(SW * SH * 4);
    bool shadowRestored = false; // after the draw: the engine's shader and the game's slot 5 again
    if (shadowPath) {
        const std::vector<char> code = readFile(shadowPath);
        if (code.empty() || FAILED(device->CreatePixelShader(code.data(), code.size(), nullptr, &shadowPs))) {
            printf("cannot create engine shader %s\n", shadowPath);
            return 2;
        }
        set("SoftShadows", "0");
        auto texture = [&](UINT w, UINT h, DXGI_FORMAT format, DXGI_FORMAT view, const void* texels, UINT pitch,
                           ID3D11ShaderResourceView** srv) {
            D3D11_TEXTURE2D_DESC d = {};
            d.Width = w;
            d.Height = h;
            d.MipLevels = d.ArraySize = 1;
            d.Format = format;
            d.SampleDesc.Count = 1;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init = {texels, pitch, 0};
            ID3D11Texture2D* t = nullptr;
            device->CreateTexture2D(&d, &init, &t);
            D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
            v.Format = view;
            v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(t, &v, srv);
            t->Release();
        };
        const std::vector<float> linearDepth(SW * SH, 0.1f); // of the 100 m far plane below: 10 m
        texture(SW, SH, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, linearDepth.data(), SW * 4, &shadowSrv[0]);
        const uint16_t empty[16] = {}; // static maps: nothing in them
        texture(4, 4, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, empty, 8, &shadowSrv[1]);
        texture(4, 4, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, empty, 8, &shadowSrv[2]);
        // Five 64-texel tiles of 20 x 20 m; larger = nearer the light, the plane at 0.5 drawn a step
        // further away (like the engine's depth bias), the caster at 0.7.
        std::vector<uint16_t> atlas(5 * tile * tile);
        for (UINT y = 0; y < tile; y++)
            for (UINT x = 0; x < 5 * tile; x++) atlas[y * 5 * tile + x] = x % tile < tile / 2 ? 45874 : 32766;
        texture(5 * tile, tile, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, atlas.data(), 5 * tile * 2, &shadowSrv[3]);
        const std::vector<uint32_t> facing(SW * SH, 0xFF808000u); // BGRA: the normal (0, 0, -1), to the camera
        texture(SW, SH, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, facing.data(), SW * 4, &shadowSrv[4]);
        // ShaderP: the camera ray from the pixel (c0..c2, far 100 m), the static maps (c5..c10), the
        // splits (c11 on, all alike: x and y across, z along the light from the camera) and their
        // rectangles in the atlas.
        float c[36][4] = {{1, 0.5f, 1, 100}, {-2, 0, 0, 0}, {0, -1, 0, 0}, {0, 0.0078125f, 3, 1.0f / SW}};
        for (int m = 0; m < 2; m++) {
            const float rows[3][4] = {{0.01f, 0, 0, 0.5f}, {0, 0.05f, 0, 0.5f}, {0, 0, -0.01f, 0.6f}};
            memcpy(c[5 + 3 * m], rows, sizeof(rows));
        }
        for (int i = 0; i < 5; i++) {
            const float rows[3][4] = {{0.01f, 0, 0, 0.1f + 0.2f * i}, {0, 0.05f, 0, 0.5f}, {0, 0, -0.01f, 0.6f}};
            memcpy(c[11 + 3 * i], rows, sizeof(rows));
            const float halfU = 0.5f / (5 * tile), halfV = 0.5f / tile;
            const float mins[4] = {0.2f * i + halfU, halfV, 0, 0}, maxs[4] = {0.2f * (i + 1) - halfU, 1 - halfV, 1, 0};
            memcpy(c[26 + i], mins, sizeof(mins));
            memcpy(c[31 + i], maxs, sizeof(maxs));
        }
        float sceneP[168] = {};
        sceneP[24] = 1.0f / SW; // SceneP[6].xy: pixel to 0..1
        sceneP[25] = 1.0f / SH;
        const void* contents[2] = {c, sceneP};
        const UINT sizes[2] = {sizeof(c), sizeof(sceneP)};
        for (int i = 0; i < 2; i++) {
            D3D11_BUFFER_DESC b = {};
            b.ByteWidth = sizes[i];
            b.Usage = D3D11_USAGE_DEFAULT;
            b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA init = {contents[i], 0, 0};
            device->CreateBuffer(&b, &init, &shadowCb[i]);
        }
        const float marker[4] = {1, 2, 3, 4};
        D3D11_BUFFER_DESC b = {};
        b.ByteWidth = sizeof(marker);
        b.Usage = D3D11_USAGE_DEFAULT;
        b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init = {marker, 0, 0};
        device->CreateBuffer(&b, &init, &gameCb5);
        // The engine's samplers: comparison, linear, border, greater-equal (lit where the map is not nearer).
        D3D11_SAMPLER_DESC smp = {};
        smp.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        smp.ComparisonFunc = D3D11_COMPARISON_GREATER_EQUAL;
        smp.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&smp, &shadowSampler);
        smp.Filter = D3D11_FILTER_ANISOTROPIC;
        smp.MaxAnisotropy = 4;
        smp.ComparisonFunc = D3D11_COMPARISON_NEVER;
        device->CreateSamplerState(&smp, &gameSampler5);
        D3D11_TEXTURE2D_DESC md = {};
        md.Width = SW;
        md.Height = SH;
        md.MipLevels = md.ArraySize = 1;
        md.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        md.SampleDesc.Count = 1;
        md.BindFlags = D3D11_BIND_RENDER_TARGET;
        device->CreateTexture2D(&md, nullptr, &shadowMask);
        device->CreateRenderTargetView(shadowMask, nullptr, &shadowMaskRtv);
        md.BindFlags = 0;
        md.Usage = D3D11_USAGE_STAGING;
        md.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        device->CreateTexture2D(&md, nullptr, &shadowStaging);
    }
    auto drawShadow = [&] {
        ctx->OMSetRenderTargets(1, &shadowMaskRtv, nullptr);
        ctx->PSSetShader(shadowPs, nullptr, 0);
        ctx->PSSetShaderResources(0, 5, shadowSrv);
        ctx->PSSetConstantBuffers(0, 1, &shadowCb[0]);
        ctx->PSSetConstantBuffers(13, 1, &shadowCb[1]);
        ctx->PSSetConstantBuffers(5, 1, &gameCb5);
        ID3D11SamplerState* samplers[2] = {shadowSampler, shadowSampler};
        ctx->PSSetSamplers(0, 2, samplers);
        ctx->PSSetSamplers(5, 1, &gameSampler5);
        viewport(ctx, 0, 0, static_cast<float>(SW), static_cast<float>(SH));
        ctx->Draw(3, 0);
        ID3D11PixelShader* ps = nullptr;
        ID3D11Buffer* cb5 = nullptr;
        ID3D11SamplerState* s5 = nullptr;
        ctx->PSGetShader(&ps, nullptr, nullptr);
        ctx->PSGetConstantBuffers(5, 1, &cb5);
        ctx->PSGetSamplers(5, 1, &s5);
        shadowRestored = ps == shadowPs && cb5 == gameCb5 && s5 == gameSampler5;
        if (ps) ps->Release();
        if (cb5) cb5->Release();
        if (s5) s5->Release();
        ID3D11ShaderResourceView* none[5] = {};
        ctx->PSSetShaderResources(0, 5, none);
        viewport(ctx, 0, 0, 1, 1);
        ctx->CopyResource(shadowStaging, shadowMask);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(shadowStaging, 0, D3D11_MAP_READ, 0, &m))) {
            for (UINT y = 0; y < SH; y++) memcpy(&shadowPixels[y * SW * 4], static_cast<unsigned char*>(m.pData) + y * m.RowPitch, SW * 4);
            ctx->Unmap(shadowStaging, 0);
        }
    };

    // The engine's G-buffer and light buffers (contents don't matter) and its motion target.
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* gbuffer[4] = {};
    ID3D11ShaderResourceView* gbufferSrv[4] = {};
    for (int i = 0; i < 4; i++) {
        device->CreateTexture2D(&td, nullptr, &gbuffer[i]);
        device->CreateShaderResourceView(gbuffer[i], nullptr, &gbufferSrv[i]);
    }
    td.Format = DXGI_FORMAT_R16G16_FLOAT;
    ID3D11Texture2D* motion = nullptr;
    ID3D11RenderTargetView* motionRtv = nullptr;
    device->CreateTexture2D(&td, nullptr, &motion);
    device->CreateRenderTargetView(motion, nullptr, &motionRtv);

    // SceneV like the engine uploads it: a dynamic 704-byte constant buffer at vertex slot 13, with
    // an eye at (10, 20, 30) looking along +z (reversed infinite projection, near 0.1).
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 704;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* sceneV = nullptr;
    device->CreateBuffer(&cbd, nullptr, &sceneV);
    const float eye[3] = {10, 20, 30};
    const float viewProj[16] = {1, 0, 0, -eye[0], 0, 1, 0, -eye[1], 0, 0, 0, 0.1f, 0, 0, 1, -eye[2]}; // rows

    // The engine's tone-map inputs: a 2x2 exposure texture (z = exposure 1) and its constants, a
    // Hable curve with the classic Uncharted 2 values.
    const float hable[7] = {0.15f, 0.50f, 0.10f, 0.20f, 0.02f, 0.30f, 11.2f}; // A B C D E F white
    auto hableF = [&](float x) {
        const float A = hable[0], B = hable[1], C = hable[2], D = hable[3], E = hable[4], F = hable[5];
        return (x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F) - E / F;
    };
    const float toneCb[12] = {hable[0], hable[1], hable[1] * hable[2], 1.0f / hableF(hable[6]),
                              hable[3] * hable[4], hable[3] * hable[5], hable[4] / hable[5], 0.0f,
                              0.25f, 0.25f, 0.0f, 0.0f};
    D3D11_BUFFER_DESC tbd = {};
    tbd.ByteWidth = sizeof(toneCb);
    tbd.Usage = D3D11_USAGE_DEFAULT;
    tbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA tbData = {toneCb, 0, 0};
    ID3D11Buffer* toneConstants = nullptr;
    device->CreateBuffer(&tbd, &tbData, &toneConstants);
    const uint16_t half1 = 0x3C00; // 1.0 in half floats
    const uint16_t exposureTexels[16] = {half1, half1, half1, half1, half1, half1, half1, half1,
                                         half1, half1, half1, half1, half1, half1, half1, half1};
    D3D11_TEXTURE2D_DESC ed = {};
    ed.Width = ed.Height = 2;
    ed.MipLevels = ed.ArraySize = 1;
    ed.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ed.SampleDesc.Count = 1;
    ed.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA edData = {exposureTexels, 16, 0};
    ID3D11Texture2D* exposureTex = nullptr;
    ID3D11ShaderResourceView* exposureSrv = nullptr;
    device->CreateTexture2D(&ed, &edData, &exposureTex);
    device->CreateShaderResourceView(exposureTex, nullptr, &exposureSrv);
    float hdrClear = 0.0f; // the HDR scene's brightness (0 for the grading checks)

    D3D11_TEXTURE2D_DESC st = {};
    backBuffer->GetDesc(&st);
    st.Usage = D3D11_USAGE_STAGING;
    st.BindFlags = 0;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    device->CreateTexture2D(&st, nullptr, &staging);

    // The game's image after its tone map: plain grey (sRGB 128).
    const float grey = srgb ? 0.2158605f : 128.0f / 255.0f;
    const float clear[4] = {grey, grey, grey, 1};

    // --selfillum: its own light texture (1x1: 0.25, 0.125, 0.0625), SelfIllumScale 2 (ShaderP),
    // and SceneP with HDR scale 1, cloud factor 1 (the engine lights with 2 x it), identity
    // camera rows, the sun from above (no sun or specular on a surface facing +z): its pixel is
    // 2 x the light texture (0.5, 0.25, 0.125) on top of 2 x the ambient part (mask.w 0.5 x vertex
    // colour 0.5 x diffuse 0.5^2 = 0.0625). Depth like the engine's: reversed, greater or equal.
    ID3D11VertexShader* vsSelfIllum = nullptr;
    ID3D11ShaderResourceView* selfIllumSrv = nullptr;
    ID3D11Buffer* selfIllumCb[2] = {};
    ID3D11DepthStencilState* greaterEqual = nullptr;
    if (selfIllum) {
        ID3DBlob* code = compile("VS_SI", "vs_4_0");
        if (code) {
            device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vsSelfIllum);
            code->Release();
        }
        const uint16_t texel[4] = {0x3400, 0x3000, 0x2C00, 0x3C00}; // 0.25, 0.125, 0.0625, 1
        D3D11_TEXTURE2D_DESC sd1 = ed;
        sd1.Width = sd1.Height = 1;
        D3D11_SUBRESOURCE_DATA sdata = {texel, 8, 0};
        ID3D11Texture2D* siTex = nullptr;
        device->CreateTexture2D(&sd1, &sdata, &siTex);
        device->CreateShaderResourceView(siTex, nullptr, &selfIllumSrv);
        siTex->Release();
        float shaderP[4] = {2.0f, 0, 0, 0};
        float sceneP[168] = {}; // 672 bytes
        for (int r = 0; r < 3; r++) sceneP[(1 + r) * 4 + r] = 1.0f; // WorldToCamera rows
        sceneP[10 * 4 + 1] = 1.0f;                                   // LightDirDirInWorld0: from above
        sceneP[18 * 4 + 3] = 1.0f;                                   // T3HdrScales.w (blocks)
        for (int i = 0; i < 4; i++) sceneP[19 * 4 + i] = 1.0f;       // CloudsX2minRGB_Half
        for (int i = 0; i < 3; i++) sceneP[20 * 4 + i] = 1.0f;       // CloudsX2maxRGB
        D3D11_BUFFER_DESC b = {};
        b.Usage = D3D11_USAGE_DEFAULT;
        b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        b.ByteWidth = sizeof(shaderP);
        D3D11_SUBRESOURCE_DATA d0 = {shaderP, 0, 0};
        device->CreateBuffer(&b, &d0, &selfIllumCb[0]);
        b.ByteWidth = sizeof(sceneP);
        D3D11_SUBRESOURCE_DATA d1 = {sceneP, 0, 0};
        device->CreateBuffer(&b, &d1, &selfIllumCb[1]);
        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        device->CreateDepthStencilState(&dd, &greaterEqual);
    }

    auto frame = [&](bool withInterface, unsigned char* centre, unsigned char* hud) {
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0);
        // Scene: the engine's G-buffer shader with the depth buffer bound.
        const float hdrColour[4] = {hdrClear, hdrClear, hdrClear, 1};
        ctx->ClearRenderTargetView(hdrRtv, hdrColour);
        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 0.25f, 0);
        ctx->OMSetRenderTargets(1, &hdrRtv, dsv);
        ctx->PSSetShader(scene, nullptr, 0);
        viewport(ctx, 0, 0, 1, 1);
        ctx->Draw(3, 0);
        if (shadowPs) drawShadow();
        // Light-read pass: camera at vertex slot 13, the buffers at the shader's own slots
        // (Block_DefReadP1: light mask t1, diffuse t2, specular t3, normals t4), motion as target 1.
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(ctx->Map(sceneV, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memset(mapped.pData, 0, 704);
            memcpy(static_cast<char*>(mapped.pData) + 560, viewProj, sizeof(viewProj));
            memcpy(static_cast<char*>(mapped.pData) + 624, eye, sizeof(eye));
            ctx->Unmap(sceneV, 0);
        }
        ctx->VSSetConstantBuffers(13, 1, &sceneV);
        if (withDecomp) {
            // Like the engine: the face normals decompressed first, from the G-buffer (diffuse t0,
            // specular t1, normals t2), into a screen-sized target; no light mask there.
            ctx->OMSetRenderTargets(1, &hdrRtv, dsv);
            ctx->PSSetShaderResources(0, 3, &gbufferSrv[1]);
            ctx->PSSetShader(decomp, nullptr, 0);
            ctx->Draw(3, 0);
            ID3D11ShaderResourceView* none[3] = {};
            ctx->PSSetShaderResources(0, 3, none);
        }
        ID3D11RenderTargetView* lightTargets[2] = {hdrRtv, motionRtv};
        ctx->OMSetRenderTargets(2, lightTargets, dsv);
        ctx->PSSetShaderResources(1, 4, gbufferSrv);
        ctx->PSSetShader(lightRead, nullptr, 0);
        ctx->Draw(3, 0);
        if (withSelfIllum) {
            // A self-illuminated block of the lighting pass (G-buffer t1-t4, its light texture t5).
            ctx->PSSetShaderResources(5, 1, &selfIllumSrv);
            ctx->PSSetConstantBuffers(0, 1, &selfIllumCb[0]);
            ctx->PSSetConstantBuffers(13, 1, &selfIllumCb[1]);
            ctx->OMSetDepthStencilState(greaterEqual, 0);
            ctx->VSSetShader(vsSelfIllum, nullptr, 0);
            ctx->PSSetShader(selfIllum, nullptr, 0);
            ctx->Draw(3, 0);
            ctx->VSSetShader(vs, nullptr, 0);
            ctx->OMSetDepthStencilState(nullptr, 0);
            ID3D11ShaderResourceView* noSi = nullptr;
            ctx->PSSetShaderResources(5, 1, &noSi);
            ID3D11Buffer* noCb = nullptr;
            ctx->PSSetConstantBuffers(13, 1, &noCb);
        }
        ID3D11ShaderResourceView* noGbuffer[4] = {};
        ctx->PSSetShaderResources(1, 4, noGbuffer);
        // Tone map into the back buffer.
        if (grade) ctx->UpdateSubresource(backBuffer, 0, nullptr, picture.data(), W * 4, 0);
        else ctx->ClearRenderTargetView(bbRtv, clear);
        ctx->OMSetRenderTargets(1, &bbRtv, nullptr);
        ctx->PSSetShader(toneMap, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &hdrSrv);
        ctx->PSSetShaderResources(1, 1, &exposureSrv);
        ctx->PSSetConstantBuffers(0, 1, &toneConstants);
        viewport(ctx, 0, 0, 1, 1);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* noSrv = nullptr;
        ctx->PSSetShaderResources(0, 1, &noSrv);
        if (withInterface) {
            // Interface: the engine's 2D shader, then a red box.
            ctx->PSSetShader(ui, nullptr, 0);
            viewport(ctx, 1, 0, 1, 1);
            ctx->Draw(3, 0);
            ctx->PSSetShader(red, nullptr, 0);
            viewport(ctx, 20, 20, 100, 50);
            ctx->Draw(3, 0);
        }
        ctx->CopyResource(staging, backBuffer);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            memcpy(centre, static_cast<unsigned char*>(m.pData) + (H / 2) * m.RowPitch + (W / 2) * 4, 4);
            memcpy(hud, static_cast<unsigned char*>(m.pData) + 40 * m.RowPitch + 60 * 4, 4);
            if (grade) {
                for (UINT y = 0; y < H; y++) memcpy(&picture[y * W * 4], static_cast<unsigned char*>(m.pData) + y * m.RowPitch, W * 4);
            }
            ctx->Unmap(staging, 0);
        }
        swapChain->Present(0, 0);
    };

    int s[13] = {};
    unsigned char centre[4] = {}, hud[4] = {};
    if (grade) {
        for (int i = 0; i < 3; i++) frame(false, centre, hud);
        return writeBmp(argv[8], picture, W, H) ? 0 : 1;
    }
    printf("harness: %s back buffer view\n", srgb ? "sRGB" : "UNORM");
    for (int i = 0; i < 3; i++) frame(true, centre, hud);
    stats(s);
    printf("  stats: placed %d, tone map %d, shaders %d/%d, depth %d, draws %d, camera %d, textures 0x%x; centre %u %u %u, "
           "hud %u %u %u\n", s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], centre[0], centre[1], centre[2], hud[0], hud[1], hud[2]);
    check(s[2] == 4 + (decomp ? 1 : 0) + (selfIllum ? 1 : 0) + (shadowPs ? 1 : 0), "the engine shaders are recognised (and only those)");
    check(s[6] == 1, "the engine camera is read from its SceneV upload");
    check(s[8] == 1000 && s[9] == 2000 && s[10] == 3000, "the camera's eye is (10, 20, 30)");
    const int wanted = (1 << 0) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7);
    check((s[7] & wanted) == wanted, "HDR, depth, normals, light buffers, light mask and motion are found by name");
    check(s[1] == 1, "the tone map is found");
    check(s[0] == 1, "the effects go in before the interface");
    check(s[4] == 1, "the scene depth is found and readable");
    // The test settings: exposure +1 stop and nothing else, so grey 128 (0.216 linear) becomes
    // 0.432 linear = sRGB 175.
    check(std::abs(static_cast<int>(centre[0]) - 175) <= 3 && centre[0] == centre[1] && centre[1] == centre[2],
          "the image is graded (grey 128 -> 175 with +1 stop)");
    check(hud[0] == 255 && hud[1] == 0 && hud[2] == 0, "the interface drawn after it is untouched (pure red)");

    check(s[11] == 0, "with the HDR stage off, the engine's tone map runs");
    for (int i = 0; i < 2; i++) frame(false, centre, hud);
    stats(s);
    check(s[0] == 3, "without an interface draw the effects run at Present (fallback)");

    // The HDR stage: the TMVS curve in place of the engine's, on a grey HDR scene (0.18 at exposure
    // 1). It is matched to the engine's mid grey, so the grey comes out as the engine's curve would
    // draw it.
    set("Exposure", "0");
    set("HdrStage", "1");
    set("ToneCurve", "1");
    // The curve alone: no scene effects (they relight the grey).
    static const char* const kSceneKeys[] = {"AOStrength", "ShadowStrength", "GlobalIllumination", "NeonLight", "SunLight", "AmbientTint",
                                             "FogDensity", "SkyEnhance", "GodRays", "LensFlare", "VolumetricClouds", "SkyMode",
                                             "Wetness", "Rain", "Snow", "Puddles", "Reflections", "Lightning", "DepthOfField",
                                             "MotionBlur", "TAA", "VolumetricLight"};
    for (const char* key : kSceneKeys) set(key, "0");
    hdrClear = 0.18f;
    for (int i = 0; i < 3; i++) frame(true, centre, hud);
    stats(s);
    const float engineGrey = hableF(0.18f) / hableF(hable[6]);
    const float stored = srgb ? (engineGrey <= 0.0031308f ? engineGrey * 12.92f : 1.055f * powf(engineGrey, 1.0f / 2.4f) - 0.055f)
                              : engineGrey;
    const int expected = static_cast<int>(stored * 255.0f + 0.5f);
    printf("  tone map: centre %u (engine curve's grey %d), hud %u %u %u\n", centre[0], expected, hud[0], hud[1], hud[2]);
    check(s[11] == 1, "the TMVS curve replaces the engine's tone map");
    check(std::abs(static_cast<int>(centre[0]) - expected) <= 2, "mid grey matches the engine's curve");
    check(hud[0] == 255 && hud[1] == 0 && hud[2] == 0, "the interface is still untouched");
    check(s[12] == 0, "with every scene effect at 0 the scene effects don't run");
    // The scene effects on the engine's G-buffer (AO, contact shadows, haze).
    set("AOStrength", "1");
    set("ShadowStrength", "0.9");
    set("FogDensity", "0.5");
    for (int i = 0; i < 3; i++) frame(true, centre, hud);
    stats(s);
    printf("  scene effects: centre %u, hud %u %u %u\n", centre[0], hud[0], hud[1], hud[2]);
    check(s[12] == 1 && s[11] == 1, "the scene effects run in the HDR stage");
    check(centre[0] > 0 && centre[0] < 255, "the scene effects give a picture");
    check(hud[0] == 255 && hud[1] == 0 && hud[2] == 0, "the interface stays untouched with the scene effects");
    for (const char* key : kSceneKeys) set(key, "0");
    set("Bloom", "0.3");
    for (int i = 0; i < 2; i++) frame(true, centre, hud);
    stats(s);
    check(s[11] == 1 && centre[0] > 0, "with bloom the HDR stage runs and draws");
    set("HdrStage", "0");
    set("ToneCurve", "0");
    set("Bloom", "0");

    // The F8 menu (the TMNF mod's, shared): each page once, with the preset gallery and its
    // embedded pictures. It must draw over the image and not crash.
    set("ShowMenu", "1");
    for (int page = 0; page < 7; page++) {
        set("MenuPage", std::to_string(page).c_str());
        for (int i = 0; i < 2; i++) frame(true, centre, hud);
    }
    set("MenuPage", "0");
    requestCapture();
    for (int i = 0; i < 3; i++) frame(true, centre, hud);
    stats(s);
    check(s[0] == 1, "with the menu open the effects still go in before the interface");
    set("ShowMenu", "0");
    hdrClear = 0.0f;

    // Photo mode (F6): the interface draws on the finished image are skipped, the effects still
    // go in before where it starts.
    set("PhotoMode", "1");
    for (int i = 0; i < 3; i++) frame(true, centre, hud);
    stats(s);
    printf("  photo mode: centre %u, hud %u %u %u\n", centre[0], hud[0], hud[1], hud[2]);
    check(s[0] == 1, "in photo mode the effects still go in before the interface");
    check(!(hud[0] == 255 && hud[1] == 0 && hud[2] == 0) && hud[0] == centre[0], "photo mode hides the interface");
    {
        // F12: photo_NNN.png in the data folder (written on a thread).
        const std::string pattern = std::string(data) + "\\photo_*.png";
        auto count = [&] {
            int n = 0;
            WIN32_FIND_DATAA fd;
            HANDLE find = FindFirstFileA(pattern.c_str(), &fd);
            if (find != INVALID_HANDLE_VALUE) {
                do n++;
                while (FindNextFileA(find, &fd));
                FindClose(find);
            }
            return n;
        };
        const int before = count();
        set("PhotoShot", "1");
        for (int i = 0; i < 2; i++) frame(true, centre, hud);
        for (int i = 0; i < 50 && count() == before; i++) Sleep(100);
        check(count() == before + 1, "F12 in photo mode saves a photo");
    }
    set("PhotoMode", "0");
    for (int i = 0; i < 2; i++) frame(true, centre, hud);
    check(hud[0] == 255 && hud[1] == 0 && hud[2] == 0, "the interface is back after photo mode");

    requestCapture();
    for (int i = 0; i < 3; i++) frame(true, centre, hud);
    WIN32_FIND_DATAA fd;
    const std::string pattern = std::string(data) + "\\captures\\capture_*";
    HANDLE find = FindFirstFileA(pattern.c_str(), &fd);
    std::string last;
    if (find != INVALID_HANDLE_VALUE) {
        do last = fd.cFileName;
        while (FindNextFileA(find, &fd));
        FindClose(find);
    }
    const std::string folder = std::string(data) + "\\captures\\" + last;
    auto exists = [&](const char* file) {
        return GetFileAttributesA((folder + "\\" + file).c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    check(!last.empty() && exists("trace.txt") && exists("input.dds") && exists("input.bmp") && exists("engine_depth.dds") && exists("engine_normal.dds") && exists("camera.txt") &&
              exists("output.dds") && exists("hdr_t0.dds"),
          "a capture writes trace, HDR input, input, depth and output");
    printf("  capture: %s\n", folder.c_str());

    // Soft shadows (--shadow). Off: the engine's shader draws its sun shadow pass. On: the mod's,
    // a penumbra around the caster's edge and the same light and shadow away from it. Off again:
    // the engine's light mask to the bit. After the draw the game's shader and slot 5 are back.
    if (shadowPs) {
        auto maskRed = [&](const std::vector<unsigned char>& mask, UINT x, UINT y) { return static_cast<int>(mask[(y * SW + x) * 4 + 2]); };
        const std::vector<unsigned char> engineMask = shadowPixels;
        const bool engineRestored = shadowRestored;
        check(maskRed(engineMask, 8, SH / 2) > 250 && maskRed(engineMask, SW - 8, SH / 2) < 5,
              "the engine's sun shadow pass draws its light mask (lit, then the caster's shadow)");
        check(set("SoftShadows", "3") == 1, "SoftShadows is a setting");
        for (int i = 0; i < 3; i++) frame(true, centre, hud);
        const std::vector<unsigned char> softMask = shadowPixels;
        const bool softRestored = shadowRestored;
        int edge = 0, away = 0;
        for (UINT y = 0; y < SH; y++) {
            for (UINT x = 0; x < SW; x++) {
                const int d = std::abs(maskRed(softMask, x, y) - maskRed(engineMask, x, y));
                if (x >= SW / 2 - 8 && x < SW / 2 + 8) edge = d > edge ? d : edge;
                else if (x < SW / 2 - 24 || x >= SW / 2 + 24) away = d > away ? d : away;
            }
        }
        printf("  soft shadows: at the shadow's edge up to %d apart from the engine's, away from it %d\n", edge, away);
        check(edge > 20 && away <= 1, "SoftShadows 3: the mod's shader draws a penumbra at the edge, the same light and shadow elsewhere");
        check(engineRestored && softRestored, "after the shadow draw the engine's shader and the game's b5/s5 are bound again");
        const std::vector<char> logText = readFile((std::string(data) + "\\tmvs-mp.log").c_str());
        check(std::string(logText.begin(), logText.end()).find("soft shadows: the mod's PCSS shader draws") != std::string::npos,
              "the log says that the mod's shader draws the sun shadow pass");
        set("SoftShadows", "0");
        for (int i = 0; i < 3; i++) frame(true, centre, hud);
        check(shadowPixels == engineMask && shadowRestored, "SoftShadows back at 0: the engine's light mask to the bit");
    }

    // A map change: the engine makes new targets, its light mask among them; the face-normal pass
    // comes first in the frame and doesn't read the light mask. The mod must follow the new one
    // (it kept the first map's: a frozen light mask, ghost shapes on the road).
    if (decomp) {
        withDecomp = true;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* newMask = nullptr;
        ID3D11RenderTargetView* newMaskRtv = nullptr;
        device->CreateTexture2D(&td, nullptr, &newMask);
        device->CreateRenderTargetView(newMask, nullptr, &newMaskRtv);
        const float maskColour[4] = {0.2f, 0.4f, 0.6f, 1.0f};
        ctx->ClearRenderTargetView(newMaskRtv, maskColour);
        gbufferSrv[0]->Release();
        device->CreateShaderResourceView(newMask, nullptr, &gbufferSrv[0]);
        for (int i = 0; i < 3; i++) frame(true, centre, hud);
        requestCapture();
        for (int i = 0; i < 3; i++) frame(true, centre, hud);
        std::string newest;
        find = FindFirstFileA(pattern.c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do newest = fd.cFileName;
            while (FindNextFileA(find, &fd));
            FindClose(find);
        }
        const std::vector<char> dds = readFile((std::string(data) + "\\captures\\" + newest + "\\engine_lightmask.dds").c_str());
        // The first texel after the header (148 bytes with the DX10 extension, else 128).
        const size_t at = dds.size() > 88 && !memcmp(&dds[84], "DX10", 4) ? 148 : 128;
        auto texelIs = [&](size_t i, int v) { return std::abs(static_cast<int>(static_cast<unsigned char>(dds[at + i])) - v) <= 1; };
        const bool same = newest != last && dds.size() >= at + 4 && texelIs(0, 51) && texelIs(1, 102) && texelIs(2, 153);
        check(same, "after a map change the light mask is the engine's new one (face-normal pass first)");
        newMaskRtv->Release();
        newMask->Release();
    }

    // EngineLightDraws: a self-illuminated draw of the lighting pass is drawn again into the light
    // buffer, twice (as it is, then subtracted with its light texture black): the buffer holds its
    // own light only. The engine's HDR image must be the same with it on and off.
    if (selfIllum) {
        withSelfIllum = true;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        const float gbufferClear[4][4] = {{0.5f, 0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f, 1}, {0, 0, 0, 0}, {0.5f, 0.5f, 1, 1}};
        for (int i = 0; i < 4; i++) {
            ID3D11RenderTargetView* rtv = nullptr;
            device->CreateRenderTargetView(gbuffer[i], nullptr, &rtv);
            if (rtv) {
                ctx->ClearRenderTargetView(rtv, gbufferClear[i]);
                rtv->Release();
            }
        }
        auto newestCapture = [&] {
            std::string newest;
            find = FindFirstFileA(pattern.c_str(), &fd);
            if (find != INVALID_HANDLE_VALUE) {
                do newest = fd.cFileName;
                while (FindNextFileA(find, &fd));
                FindClose(find);
            }
            return std::string(data) + "\\captures\\" + newest;
        };
        float hdrOff[3] = {}, hdrOn[3] = {}, light[3] = {};
        set("EngineLightDraws", "0");
        requestCapture();
        for (int i = 0; i < 3; i++) frame(true, centre, hud);
        const std::string off = newestCapture();
        const bool haveOff = firstTexel(off + "\\hdr_t0.dds", hdrOff);
        const bool noLightsOff = GetFileAttributesA((off + "\\engine_lights.dds").c_str()) == INVALID_FILE_ATTRIBUTES;
        set("EngineLightDraws", "1");
        requestCapture();
        for (int i = 0; i < 3; i++) frame(true, centre, hud);
        const std::string on = newestCapture();
        const bool haveOn = firstTexel(on + "\\hdr_t0.dds", hdrOn) && firstTexel(on + "\\engine_lights.dds", light);
        set("EngineLightDraws", "0");
        withSelfIllum = false;
        printf("  self-illuminated: HDR off %.4f %.4f %.4f, on %.4f %.4f %.4f, light buffer %.4f %.4f %.4f\n", hdrOff[0], hdrOff[1],
               hdrOff[2], hdrOn[0], hdrOn[1], hdrOn[2], light[0], light[1], light[2]);
        check(haveOff && noLightsOff, "with EngineLightDraws off there is no light buffer");
        check(haveOn && hdrOn[0] == hdrOff[0] && hdrOn[1] == hdrOff[1] && hdrOn[2] == hdrOff[2],
              "with EngineLightDraws on the engine's HDR image is unchanged");
        const float want[3] = {0.5f, 0.25f, 0.125f};
        bool own = haveOn, lit = haveOn;
        for (int i = 0; i < 3; i++) {
            own = own && std::fabs(light[i] - want[i]) < 0.004f;
            lit = lit && hdrOn[i] - light[i] > 0.1f; // its lit surface (about 0.126) stayed out
        }
        check(own && lit, "the light buffer holds the material's own light, not its lit surface");
    }

    // Resize like a resolution change.
    bbRtv->Release();
    backBuffer->Release();
    hr = swapChain->ResizeBuffers(1, 800, 450, DXGI_FORMAT_UNKNOWN, 0);
    check(SUCCEEDED(hr), "ResizeBuffers works with the mod's references released");

    printf(g_failures ? "harness: %d check(s) failed\n" : "harness: all checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
