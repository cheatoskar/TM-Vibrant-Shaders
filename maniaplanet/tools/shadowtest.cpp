// Offline test of the soft sun shadows (src/softshadow.cpp, shaders/softshadow.hlsli) against the
// engine's own sun shadow shader, on the inputs of its shadow pass.
//
//   mp_shadowtest <folder> <out dir> <engine shader.dxbc> [--light 0.5,1,2] [--reach 40] [--bench N]
//                 [--variants <dir of .dxbc>]
//
// <folder> holds what an F12 capture writes for the pass: shadow_cb.bin (its ShaderP), scenep.bin
// (SceneP), engine_shadowdepth.dds (t0), engine_shadowmap0/1.dds (t1, t2), engine_shadowsplits.dds
// (t3, the split atlas) and engine_shadownormal.dds (the face normal); captures from before the
// atlas was saved need those two made up (not in this repo). The engine's shader is one of its
// variants from the GpuCache (tools/gpucache.py).
//
// Draws the pass like the engine (same constants, textures, samplers) through softshadow::begin and
// end, as the game's hooks do, and writes each result as <name>.f32 (width, height, then RGBA
// floats) into <out dir>:
//   engine       the engine's shader
//   exact        the replacement in mode 1 (its port of the engine's shader): must equal engine
//   soft_<L>     the replacement at light size L degrees
//   debug_<L>    mode 2: soft, engine, penumbra radius (m / 4), filter split
// and prints how far exact is from engine, the GPU time of each, and the checks: the engine's
// shader, b5 and s5 are bound again after the draw; a variant it doesn't know, constants of the
// wrong size and a missing atlas keep the engine's shader.
// --variants: each engine variant in the folder against the port (the constants cut down to its
// split count, sprite and opacity maps made up).
#include "softshadow.h"
#include "log.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace tmvs_mp;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) g_failures++;
}

std::vector<char> readFile(const std::string& path) {
    std::vector<char> data;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return data;
    fseek(f, 0, SEEK_END);
    data.resize(static_cast<size_t>(ftell(f)));
    fseek(f, 0, SEEK_SET);
    if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
    fclose(f);
    return data;
}

struct Dds {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width = 0, height = 0, pitch = 0;
    std::vector<char> data;
};

bool readDds(const std::string& path, Dds& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint32_t header[32] = {}, dx10[5] = {};
    const bool ok = fread(header, 4, 32, f) == 32 && header[0] == 0x20534444 && header[21] == 0x30315844 &&
                    fread(dx10, 4, 5, f) == 5;
    if (ok) {
        out.format = static_cast<DXGI_FORMAT>(dx10[0]);
        out.height = header[3];
        out.width = header[4];
        out.pitch = header[5];
        out.data.resize(static_cast<size_t>(out.pitch) * out.height);
        if (fread(out.data.data(), 1, out.data.size(), f) != out.data.size()) out.data.clear();
    }
    fclose(f);
    return ok && !out.data.empty();
}

// The texture and the view the engine reads it through (typeless 16 bit -> UNORM).
struct Texture {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    UINT width = 0, height = 0;
};

DXGI_FORMAT viewFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    default: return f;
    }
}

Texture makeTexture(ID3D11Device* device, DXGI_FORMAT format, UINT w, UINT h, const void* data, UINT pitch) {
    Texture t;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {data, pitch, 0};
    if (FAILED(device->CreateTexture2D(&d, &init, &t.tex))) return t;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = viewFormat(format);
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(t.tex, &sv, &t.srv);
    t.width = w;
    t.height = h;
    return t;
}

Texture load(ID3D11Device* device, const std::string& path) {
    Dds dds;
    if (!readDds(path, dds)) return {};
    Texture t = makeTexture(device, dds.format, dds.width, dds.height, dds.data.data(), dds.pitch);
    if (!t.srv) printf("cannot use %s (format %d)\n", path.c_str(), dds.format);
    return t;
}

const char* kVertexShader = R"(
float4 VS(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
}
)";

struct Image {
    UINT w = 0, h = 0;
    std::vector<float> rgba;
};

bool writeF32(const std::string& path, const Image& im) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        printf("cannot write %s\n", path.c_str());
        return false;
    }
    const uint32_t size[2] = {im.w, im.h};
    fwrite(size, 4, 2, f);
    fwrite(im.rgba.data(), 4, im.rgba.size(), f);
    fclose(f);
    return true;
}

struct Diff {
    double maxAbs = 0.0, meanAbs = 0.0;
    uint64_t over1 = 0, total = 0; // pixels more than one 8-bit step apart
};

// The red channel (the light mask), where the pixel isn't sky.
Diff compare(const Image& a, const Image& b, const std::vector<unsigned char>& valid) {
    Diff d;
    for (size_t i = 0; i < valid.size(); i++) {
        if (!valid[i]) continue;
        const double e = std::fabs(a.rgba[i * 4] - b.rgba[i * 4]);
        d.maxAbs = std::max(d.maxAbs, e);
        d.meanAbs += e;
        d.over1 += e > 1.5 / 255.0;
        d.total++;
    }
    if (d.total) d.meanAbs /= static_cast<double>(d.total);
    return d;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("usage: mp_shadowtest <folder> <out dir> <engine shader.dxbc> [--light 0.5,1,2] [--reach 40] [--bench N] "
               "[--variants dir]\n");
        return 2;
    }
    const std::string folder = argv[1], out = argv[2];
    std::vector<float> lights = {1.0f};
    float reach = 40.0f;
    int bench = 0;
    std::string variantsDir;
    for (int i = 4; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--light" && i + 1 < argc) {
            lights.clear();
            std::string list = argv[++i];
            size_t at = 0;
            while (at < list.size()) {
                const size_t comma = list.find(',', at);
                lights.push_back(static_cast<float>(atof(list.substr(at, comma - at).c_str())));
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
        } else if (a == "--reach" && i + 1 < argc) {
            reach = static_cast<float>(atof(argv[++i]));
        } else if (a == "--bench" && i + 1 < argc) {
            bench = atoi(argv[++i]);
        } else if (a == "--variants" && i + 1 < argc) {
            variantsDir = argv[++i];
        }
    }
    std::error_code made;
    std::filesystem::create_directories(out, made);
    // The log goes to the output folder, not the game's data folder.
    SetEnvironmentVariableA("TMVS_MP_DATA", out.c_str());
    tmshaders::log::setFileName(L"tmvs-mp-shadowtest.log");

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &ctx))) {
        printf("no D3D11 device\n");
        return 2;
    }

    // The pass's inputs.
    std::vector<char> shaderP = readFile(folder + "/shadow_cb.bin");
    std::vector<char> sceneP = readFile(folder + "/scenep.bin");
    Texture depth = load(device, folder + "/engine_shadowdepth.dds");
    Texture map0 = load(device, folder + "/engine_shadowmap0.dds");
    Texture map1 = load(device, folder + "/engine_shadowmap1.dds");
    Texture splits = load(device, folder + "/engine_shadowsplits.dds");
    Texture normal = load(device, folder + "/engine_shadownormal.dds");
    if (shaderP.size() < 176 || sceneP.size() < 112 || !depth.srv || !map0.srv || !map1.srv || !splits.srv || !normal.srv) {
        printf("%s lacks some of shadow_cb.bin, scenep.bin, engine_shadowdepth/map0/map1/splits/normal.dds\n", folder.c_str());
        return 2;
    }
    const UINT W = depth.width, H = depth.height;
    printf("shadowtest: %s, %ux%u, ShaderP %zu bytes, atlas %ux%u\n", folder.c_str(), W, H, shaderP.size(), splits.width, splits.height);
    // Sky (and nothing drawn): the engine's stencil leaves those pixels out.
    std::vector<unsigned char> valid(static_cast<size_t>(W) * H, 0);
    {
        Dds d;
        readDds(folder + "/engine_shadowdepth.dds", d);
        for (UINT y = 0; y < H; y++)
            for (UINT x = 0; x < W; x++) {
                float z;
                memcpy(&z, &d.data[static_cast<size_t>(y) * d.pitch + x * 4], 4);
                valid[static_cast<size_t>(y) * W + x] = z < 0.9999f;
            }
    }

    // Made-up extra maps for the sprite and opacity variants (the static maps stand in for the
    // sprite maps; the opacity a pattern).
    std::vector<unsigned char> pattern(256 * 256 * 4);
    for (int i = 0; i < 256 * 256; i++) {
        const int x = i % 256, y = i / 256;
        const unsigned char v = static_cast<unsigned char>(((x / 16 + y / 16) & 1) ? 255 : 128);
        pattern[i * 4] = pattern[i * 4 + 1] = pattern[i * 4 + 2] = v;
        pattern[i * 4 + 3] = 255;
    }
    Texture opacity = makeTexture(device, DXGI_FORMAT_R8G8B8A8_UNORM, 256, 256, pattern.data(), 256 * 4);

    auto buffer = [&](const void* data, UINT size) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = (size + 15) & ~15u;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        std::vector<char> padded(bd.ByteWidth, 0);
        memcpy(padded.data(), data, size);
        D3D11_SUBRESOURCE_DATA init = {padded.data(), 0, 0};
        ID3D11Buffer* b = nullptr;
        device->CreateBuffer(&bd, &init, &b);
        return b;
    };
    ID3D11Buffer* cb0 = buffer(shaderP.data(), static_cast<UINT>(shaderP.size()));
    ID3D11Buffer* cb13 = buffer(sceneP.data(), static_cast<UINT>(sceneP.size()));

    // The engine's samplers (shadow.txt: comparison, linear, border, greater-equal), the border
    // lit; the opacity map's linear one.
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.ComparisonFunc = D3D11_COMPARISON_GREATER_EQUAL;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* cmp = nullptr;
    device->CreateSamplerState(&sd, &cmp);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ID3D11SamplerState* linear = nullptr;
    device->CreateSamplerState(&sd, &linear);
    // Something at b5 and s5 before the draw, to see it come back.
    const float marker[4] = {1, 2, 3, 4};
    ID3D11Buffer* markerCb = buffer(marker, sizeof(marker));
    ID3D11SamplerState* markerSampler = nullptr;
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.MaxAnisotropy = 4;
    device->CreateSamplerState(&sd, &markerSampler);

    ID3DBlob* vsCode = nullptr;
    ID3DBlob* errors = nullptr;
    D3DCompile(kVertexShader, strlen(kVertexShader), "shadowtest", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsCode, &errors);
    if (!vsCode) {
        printf("vertex shader: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return 2;
    }
    ID3D11VertexShader* vs = nullptr;
    device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs);

    auto createEngine = [&](const std::string& path, softshadow::Variant& v) -> ID3D11PixelShader* {
        const std::vector<char> code = readFile(path);
        ID3D11PixelShader* ps = nullptr;
        if (code.empty() || FAILED(device->CreatePixelShader(code.data(), code.size(), nullptr, &ps))) return nullptr;
        softshadow::noteEngineShader(ps, code.data(), code.size());
        v = softshadow::identify(code.data(), code.size());
        return ps;
    };
    softshadow::Variant variant;
    ID3D11PixelShader* engine = createEngine(argv[3], variant);
    if (!engine || !softshadow::init(device)) {
        printf("cannot create the engine's shader %s or the replacement\n", argv[3]);
        return 2;
    }
    printf("engine shader: %s, %d splits, extras %d\n", variant.known ? "known variant" : "UNKNOWN variant", variant.splits, variant.extras);

    // Targets: the light mask's 8 bits, and a float one for the debug channels.
    auto target = [&](DXGI_FORMAT f, ID3D11Texture2D*& tex, ID3D11RenderTargetView*& rtv, ID3D11Texture2D*& staging) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = W;
        d.Height = H;
        d.MipLevels = d.ArraySize = 1;
        d.Format = f;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        device->CreateTexture2D(&d, nullptr, &tex);
        device->CreateRenderTargetView(tex, nullptr, &rtv);
        d.BindFlags = 0;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        device->CreateTexture2D(&d, nullptr, &staging);
    };
    ID3D11Texture2D *mask8 = nullptr, *mask8Staging = nullptr, *maskF = nullptr, *maskFStaging = nullptr;
    ID3D11RenderTargetView *mask8Rtv = nullptr, *maskFRtv = nullptr;
    target(DXGI_FORMAT_R8G8B8A8_UNORM, mask8, mask8Rtv, mask8Staging);
    target(DXGI_FORMAT_R32G32B32A32_FLOAT, maskF, maskFRtv, maskFStaging);

    // One draw of the pass as the engine sets it up; replace: through softshadow::begin/end.
    struct Run {
        bool replaced = false, restored = true;
    };
    auto draw = [&](ID3D11PixelShader* ps, const softshadow::Variant& v, ID3D11Buffer* constants, bool replace,
                    const softshadow::Params& params, ID3D11RenderTargetView* rtv, bool withAtlas) {
        Run r;
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ID3D11Buffer* cbs[14] = {};
        cbs[0] = constants;
        cbs[5] = markerCb;
        cbs[13] = cb13;
        ctx->PSSetConstantBuffers(0, 14, cbs);
        ID3D11ShaderResourceView* srv[8] = {depth.srv, map0.srv, map1.srv, withAtlas ? splits.srv : nullptr};
        int slot = 4;
        if (v.extras & 2) {
            srv[slot++] = map0.srv; // sprite maps
            srv[slot++] = map1.srv;
        }
        if (v.extras & 1) srv[slot++] = opacity.srv;
        srv[slot] = normal.srv;
        ctx->PSSetShaderResources(0, 8, srv);
        ID3D11SamplerState* samplers[6] = {cmp, cmp, nullptr, nullptr, nullptr, markerSampler};
        int s = 2;
        if (v.extras & 2) samplers[s++] = cmp;
        if (v.extras & 1) samplers[s++] = linear;
        ctx->PSSetSamplers(0, 6, samplers);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp = {0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
        ctx->RSSetViewports(1, &vp);
        if (replace) r.replaced = softshadow::begin(ctx, params);
        ctx->Draw(3, 0);
        if (r.replaced) {
            softshadow::end(ctx);
            ID3D11PixelShader* bound = nullptr;
            ctx->PSGetShader(&bound, nullptr, nullptr);
            ID3D11Buffer* b5 = nullptr;
            ctx->PSGetConstantBuffers(5, 1, &b5);
            ID3D11SamplerState* s5 = nullptr;
            ctx->PSGetSamplers(5, 1, &s5);
            r.restored = bound == ps && b5 == markerCb && s5 == markerSampler;
            if (bound) bound->Release();
            if (b5) b5->Release();
            if (s5) s5->Release();
        }
        ID3D11ShaderResourceView* none[8] = {};
        ctx->PSSetShaderResources(0, 8, none);
        return r;
    };
    auto read = [&](ID3D11Texture2D* tex, ID3D11Texture2D* staging, bool floats) {
        Image im;
        im.w = W;
        im.h = H;
        im.rgba.resize(static_cast<size_t>(W) * H * 4);
        ctx->CopyResource(staging, tex);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            for (UINT y = 0; y < H; y++) {
                const char* row = static_cast<const char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
                for (UINT x = 0; x < W * 4; x++) {
                    im.rgba[static_cast<size_t>(y) * W * 4 + x] =
                        floats ? reinterpret_cast<const float*>(row)[x] : static_cast<unsigned char>(row[x]) / 255.0f;
                }
            }
            ctx->Unmap(staging, 0);
        }
        return im;
    };
    // GPU time of one draw (ms), averaged.
    auto timeIt = [&](auto&& oneDraw) -> double {
        if (bench <= 0) return 0.0;
        D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        ID3D11Query *disjoint = nullptr, *t0 = nullptr, *t1 = nullptr;
        device->CreateQuery(&qd, &disjoint);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        device->CreateQuery(&qd, &t0);
        device->CreateQuery(&qd, &t1);
        oneDraw(); // warm up
        ctx->Begin(disjoint);
        ctx->End(t0);
        for (int i = 0; i < bench; i++) oneDraw();
        ctx->End(t1);
        ctx->End(disjoint);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
        while (ctx->GetData(disjoint, &dj, sizeof(dj), 0) != S_OK) Sleep(1);
        UINT64 a = 0, b = 0;
        while (ctx->GetData(t0, &a, sizeof(a), 0) != S_OK) Sleep(1);
        while (ctx->GetData(t1, &b, sizeof(b), 0) != S_OK) Sleep(1);
        disjoint->Release();
        t0->Release();
        t1->Release();
        if (dj.Disjoint || !dj.Frequency) return -1.0;
        return static_cast<double>(b - a) / static_cast<double>(dj.Frequency) * 1000.0 / bench;
    };

    softshadow::Params engineMode;
    engineMode.lightSize = 1.0f;
    engineMode.reach = reach;
    engineMode.mode = 1;

    // 1. The engine's shader, and the replacement's port of it.
    draw(engine, variant, cb0, false, {}, mask8Rtv, true);
    const Image engineImage = read(mask8, mask8Staging, false);
    writeF32(out + "/engine.f32", engineImage);
    const Run exactRun = draw(engine, variant, cb0, true, engineMode, mask8Rtv, true);
    const Image exactImage = read(mask8, mask8Staging, false);
    writeF32(out + "/exact.f32", exactImage);
    const Diff exact = compare(engineImage, exactImage, valid);
    printf("  port vs engine: %llu of %llu pixels more than one step apart (%.4f%%), max %.4f, mean %.6f\n",
           static_cast<unsigned long long>(exact.over1), static_cast<unsigned long long>(exact.total),
           exact.total ? 100.0 * exact.over1 / exact.total : 0.0, exact.maxAbs, exact.meanAbs);
    check(exactRun.replaced, "the replacement takes the pass (known variant, matching bindings)");
    check(exactRun.restored, "after the draw the engine's shader, b5 and s5 are bound again");
    check(exact.total && exact.over1 * 10000 <= exact.total, "its port of the engine's shader gives the engine's light mask (99.99 %)");
    {
        Dds lm;
        if (readDds(folder + "/engine_lightmask.dds", lm) && lm.width == W && lm.height == H) {
            // The captured light mask (red; BGRA in the file): the shadow pass times the car's
            // shadow volumes, so only where those didn't draw it is the pass alone.
            Image captured;
            captured.w = W;
            captured.h = H;
            captured.rgba.assign(static_cast<size_t>(W) * H * 4, 0.0f);
            for (UINT y = 0; y < H; y++)
                for (UINT x = 0; x < W; x++)
                    captured.rgba[(static_cast<size_t>(y) * W + x) * 4] =
                        static_cast<unsigned char>(lm.data[static_cast<size_t>(y) * lm.pitch + x * 4 + 2]) / 255.0f;
            const Diff c = compare(engineImage, captured, valid);
            printf("  engine shader here vs the captured light mask: %.2f%% of pixels apart, mean %.4f\n",
                   c.total ? 100.0 * c.over1 / c.total : 0.0, c.meanAbs);
        }
    }

    // 2. Soft at each light size, its debug channels, GPU times.
    const double engineMs = timeIt([&] { draw(engine, variant, cb0, false, {}, mask8Rtv, true); });
    if (bench) printf("  GPU: engine's shader %.3f ms\n", engineMs);
    for (float light : lights) {
        softshadow::Params p;
        p.lightSize = light;
        p.reach = reach;
        const Run run = draw(engine, variant, cb0, true, p, mask8Rtv, true);
        const Image soft = read(mask8, mask8Staging, false);
        char name[64];
        snprintf(name, sizeof(name), "/soft_%.2f.f32", light);
        writeF32(out + name, soft);
        const Diff d = compare(engineImage, soft, valid);
        p.mode = 2;
        draw(engine, variant, cb0, true, p, maskFRtv, true);
        snprintf(name, sizeof(name), "/debug_%.2f.f32", light);
        writeF32(out + name, read(maskF, maskFStaging, true));
        p.mode = 0;
        const double ms = timeIt([&] { draw(engine, variant, cb0, true, p, mask8Rtv, true); });
        printf("  light %.2f deg: %s, %.2f%% of pixels changed (mean %.4f)%s", light, run.replaced ? "replaced" : "NOT replaced",
               d.total ? 100.0 * d.over1 / d.total : 0.0, d.meanAbs, bench ? "" : "\n");
        if (bench) printf(", GPU %.3f ms\n", ms);
    }

    // 3. Where it must keep the engine's shader.
    {
        softshadow::Params p;
        p.lightSize = 1.0f;
        ID3D11Buffer* tooSmall = buffer(shaderP.data(), 160);
        check(!draw(engine, variant, tooSmall, true, p, mask8Rtv, true).replaced, "constants too small for the variant: the engine's shader");
        tooSmall->Release();
        check(!draw(engine, variant, cb0, true, p, mask8Rtv, false).replaced, "no split atlas bound: the engine's shader");
        p.lightSize = 0.0f;
        check(!draw(engine, variant, cb0, true, p, mask8Rtv, true).replaced, "light size 0: the engine's shader");
        ID3D11PixelShader* other = nullptr;
        const std::vector<char> code = readFile(argv[3]);
        std::vector<char> changed = code;
        if (changed.size() > 20) changed[4] ^= 0x5a; // another checksum: an unknown variant (not validated by the runtime)
        if (SUCCEEDED(device->CreatePixelShader(code.data(), code.size(), nullptr, &other))) {
            softshadow::noteEngineShader(other, changed.data(), changed.size());
            p.lightSize = 1.0f;
            check(!draw(other, variant, cb0, true, p, mask8Rtv, true).replaced, "an unknown variant: the engine's shader");
            other->Release();
        }
    }

    // 4. Every variant in --variants against the port, with its split count.
    if (!variantsDir.empty()) {
        WIN32_FIND_DATAA fd;
        HANDLE find = FindFirstFileA((variantsDir + "\\*.dxbc").c_str(), &fd);
        int tested = 0, equal = 0, skipped = 0;
        if (find != INVALID_HANDLE_VALUE) {
            do {
                softshadow::Variant v;
                ID3D11PixelShader* ps = createEngine(variantsDir + "\\" + fd.cFileName, v);
                if (!ps || !v.known) {
                    skipped++;
                    if (ps) ps->Release();
                    continue;
                }
                // ShaderP for v.splits: the static rows, the first splits' rows, their rectangles.
                const int n = v.splits;
                std::vector<float> src(shaderP.size() / 4), cbN(static_cast<size_t>(11 + 5 * n) * 4, 0.0f);
                memcpy(src.data(), shaderP.data(), shaderP.size());
                const int full = 5;
                for (int r = 0; r < 11 + 3 * n; r++) memcpy(&cbN[r * 4], &src[r * 4], 16);
                for (int i = 0; i < n; i++) {
                    memcpy(&cbN[(11 + 3 * n + i) * 4], &src[(11 + 3 * full + i) * 4], 16);
                    memcpy(&cbN[(11 + 4 * n + i) * 4], &src[(11 + 4 * full + i) * 4], 16);
                }
                ID3D11Buffer* b = buffer(cbN.data(), static_cast<UINT>(cbN.size() * 4));
                draw(ps, v, b, false, {}, mask8Rtv, true);
                const Image e = read(mask8, mask8Staging, false);
                const Run run = draw(ps, v, b, true, engineMode, mask8Rtv, true);
                const Image x = read(mask8, mask8Staging, false);
                const Diff d = compare(e, x, valid);
                const bool same = run.replaced && run.restored && d.over1 * 10000 <= d.total;
                printf("  %s: %d splits, extras %d: %s, %.4f%% apart, max %.3f\n", fd.cFileName, n, v.extras,
                       same ? "same" : "DIFFERENT", d.total ? 100.0 * d.over1 / d.total : 0.0, d.maxAbs);
                tested++;
                equal += same;
                b->Release();
                ps->Release();
            } while (FindNextFileA(find, &fd));
            FindClose(find);
        }
        printf("  variants: %d tested, %d the same as the engine's, %d not replaced (unknown)\n", tested, equal, skipped);
        check(tested > 0 && equal == tested, "every known variant's port gives the engine's light mask");
    }

    printf(g_failures ? "shadowtest: %d check(s) failed\n" : "shadowtest: all checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
