// Offline previewer for the ManiaPlanet build: renders a capture (F12 in the game) through the
// same renderer as the game, without the game.
//
//   mp_preview <capture folder> <out.bmp> [--preset Name] [--view N] [--set Key=Value ...]
//              [--hdr] [--tonecb file]
//
// --hdr runs the HDR stage first (the mod's tone map and bloom on engine_hdr.dds with
// engine_exposure.dds), with the engine's tone-map constants from tonemap_cb.bin in the capture
// (captures from before 0.2 lack it: pass one with --tonecb).
//
// The capture holds the image the mod graded (input.dds and the view format it was drawn with),
// the engine's textures (engine_*.dds) and its camera (scenev.bin). --view takes the menu's views:
// 0 normal, 1 split, 2 depth, 3 normals, 4 motion, 5 light buffer, 6 world grid.
#include "engine_frame.h"
#include "log.h"
#include "renderer.h"
#include "config.h"
#include "options.h"
#include "settings.h"
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace tmvs_mp;

namespace {

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

// The resource format for a captured (typed) format, so the views the game used can be made again.
DXGI_FORMAT resourceFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    default: return f;
    }
}

ID3D11Texture2D* upload(ID3D11Device* device, const Dds& dds, UINT bind) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = dds.width;
    d.Height = dds.height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = resourceFormat(dds.format);
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init = {dds.data.data(), dds.pitch, 0};
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(device->CreateTexture2D(&d, &init, &tex))) {
        printf("cannot create a %ux%u texture of format %d\n", dds.width, dds.height, d.Format);
        return nullptr;
    }
    return tex;
}

bool writeBmp(const std::string& path, const D3D11_MAPPED_SUBRESOURCE& m, UINT w, UINT h, bool bgra) {
    FILE* f = fopen(path.c_str(), "wb");
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
        const auto* src = static_cast<const unsigned char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
        for (UINT x = 0; x < w; x++) {
            const unsigned char* p = src + x * 4;
            line[x * 3] = bgra ? p[0] : p[2];
            line[x * 3 + 1] = p[1];
            line[x * 3 + 2] = bgra ? p[2] : p[0];
        }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: mp_preview <capture folder> <out.bmp> [--preset Name] [--view N] [--set Key=Value ...]\n");
        return 2;
    }
    tmshaders::log::setFileName(L"tmvs-mp-preview.log"); // never truncate the game's log
    const std::string folder = argv[1];
    bool hdrStage = false;
    double time = 10.0;
    std::string toneCbPath = folder + "/tonemap_cb.bin";

    // The look: a built-in or your preset (default Vibrant), then --set values on top.
    tmshaders::Config& config = tmshaders::Config::get();
    config.selectPreset("Vibrant");
    Options& opts = options::current();
    for (int i = 3; i < argc; i++) {
        const std::string opt = argv[i];
        if (opt == "--hdr") {
            hdrStage = true;
            continue;
        }
        if (i + 1 >= argc) {
            printf("%s needs a value\n", opt.c_str());
            return 2;
        }
        const std::string value = argv[++i];
        if (opt == "--preset") {
            if (!config.selectPreset(value)) {
                printf("unknown preset %s\n", value.c_str());
                return 2;
            }
        }
        else if (opt == "--tonecb") toneCbPath = value;
        else if (opt == "--time") time = atof(value.c_str());
        else if (opt == "--debug") config.settings.debugView = atoi(value.c_str());
        else if (opt == "--view") opts.view = static_cast<View>(atoi(value.c_str()));
        else if (opt == "--set") {
            const size_t eq = value.find('=');
            if (eq == std::string::npos || !setValue(value.substr(0, eq), value.substr(eq + 1))) {
                printf("unknown setting %s\n", value.c_str());
                return 2;
            }
        } else {
            printf("unknown option %s\n", opt.c_str());
            return 2;
        }
    }

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                                 &ctx))) {
        printf("no D3D11 device\n");
        return 2;
    }

    Dds input;
    if (!readDds(folder + "/input.dds", input)) {
        printf("%s/input.dds missing (a capture taken while the mod graded)\n", folder.c_str());
        return 2;
    }
    DXGI_FORMAT viewFormat = input.format;
    if (FILE* f = fopen((folder + "/view.txt").c_str(), "r")) {
        int v = 0;
        if (fscanf(f, "%d", &v) == 1 && v > 0) viewFormat = static_cast<DXGI_FORMAT>(v);
        fclose(f);
    }
    ID3D11Texture2D* image = upload(device, input, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    if (!image) return 2;
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = viewFormat;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(device->CreateRenderTargetView(image, &rd, &rtv))) {
        printf("cannot make a view of format %d\n", viewFormat);
        return 2;
    }

    ID3D11Resource* tex[kTexCount] = {};
    for (int i = 0; i < kTexCount; i++) {
        Dds dds;
        if (!readDds(folder + "/engine_" + engineTextureName(i) + ".dds", dds)) continue;
        tex[i] = upload(device, dds, D3D11_BIND_SHADER_RESOURCE);
        if (tex[i]) printf("  engine %s %ux%u format %d\n", engineTextureName(i), dds.width, dds.height, dds.format);
    }
    Camera camera;
    bool haveCamera = false;
    if (FILE* f = fopen((folder + "/scenev.bin").c_str(), "rb")) {
        haveCamera = fread(camera.raw, 1, sizeof(camera.raw), f) == sizeof(camera.raw) && decodeCamera(camera);
        fclose(f);
    }
    printf("  camera %s\n", haveCamera ? "found" : "missing");

    Look look;
    look.settings = &config.settings;
    look.lut = config.lutFile;
    look.lutAmount = config.lutAmount;
    if (haveCamera) {
        const float peak = std::max(camera.sunColour[0], std::max(camera.sunColour[1], camera.sunColour[2]));
        if (peak > 1e-3f)
            for (int i = 0; i < 3; i++) look.sun[i] = camera.sunColour[i] / peak;
    }
    look.split = opts.view == View::Split ? opts.split : 0.0f;

    Renderer renderer;
    if (!renderer.init(device)) return 2;
    if (hdrStage) {
        std::vector<char> cb;
        if (FILE* f = fopen(toneCbPath.c_str(), "rb")) {
            cb.resize(48);
            if (fread(cb.data(), 1, cb.size(), f) != cb.size()) cb.clear();
            fclose(f);
        }
        if (cb.empty() || !tex[kTexHdr] || !tex[kTexExposure]) {
            printf("--hdr needs engine_hdr.dds, engine_exposure.dds and the tone-map constants (%s)\n", toneCbPath.c_str());
            return 2;
        }
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = static_cast<UINT>(cb.size());
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init = {cb.data(), 0, 0};
        ID3D11Buffer* engineConstants = nullptr;
        device->CreateBuffer(&bd, &init, &engineConstants);
        ID3D11ShaderResourceView* in[2] = {};
        for (int i = 0; i < 2; i++) {
            ID3D11Texture2D* t = static_cast<ID3D11Texture2D*>(tex[i == 0 ? kTexHdr : kTexExposure]);
            D3D11_TEXTURE2D_DESC td;
            t->GetDesc(&td);
            D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
            sv.Format = readableFormat(td.Format);
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(t, &sv, &in[i]);
        }
        EngineView engine{tex, haveCamera ? &camera : nullptr};
        engine.time = time;
        if (!renderer.toneMap(ctx, rtv, in[0], in[1], engineConstants, look, engine)) {
            printf("the HDR stage failed\n");
            return 1;
        }
        printf("  HDR stage: preset %s, curve %d, bloom %.2f, scene effects %s\n", config.preset.c_str(),
               config.settings.toneCurve, config.settings.bloom, renderer.effectsRan() ? "ran" : "off");
    }
    const EngineView view{tex, haveCamera ? &camera : nullptr};
    if (!renderer.apply(ctx, rtv, view, look, opts, hdrStage)) {
        printf("the renderer refused the image\n");
        return 1;
    }

    D3D11_TEXTURE2D_DESC sd;
    image->GetDesc(&sd);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    device->CreateTexture2D(&sd, nullptr, &staging);
    ctx->CopyResource(staging, image);
    D3D11_MAPPED_SUBRESOURCE m;
    if (!staging || FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) return 1;
    const bool bgra = sd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    const bool ok = writeBmp(argv[2], m, sd.Width, sd.Height, bgra);
    ctx->Unmap(staging, 0);
    printf("%s %s\n", ok ? "wrote" : "cannot write", argv[2]);
    return ok ? 0 : 1;
}
