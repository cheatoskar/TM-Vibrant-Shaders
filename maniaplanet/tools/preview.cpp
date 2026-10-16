// Offline previewer for the ManiaPlanet build: renders a capture (F12 in the game) through the
// same renderer as the game, without the game.
//
//   mp_preview <capture folder> <out.bmp> [--preset Name] [--view N] [--set Key=Value ...]
//              [--hdr] [--tonecb file]
//
// --frames N runs the HDR stage N times 1/60 s apart (TAA, motion blur and auto focus need a
// previous frame; the camera stays, the engine's motion vectors are the captured ones), --photo
// runs it in photo mode, --focus u,v sets the photo mode's focus point.
//
// --hdr runs the HDR stage first (the mod's tone map and bloom on engine_hdr.dds with
// engine_exposure.dds), with the engine's tone-map constants from tonemap_cb.bin in the capture
// (captures from before 0.2 lack it: pass one with --tonecb).
//
// The capture holds the image the mod graded (input.dds and the view format it was drawn with),
// the engine's textures (engine_*.dds) and its camera (scenev.bin). --view takes the menu's views:
// 0 normal, 1 split, 2 depth, 3 normals, 4 motion, 5 light buffer, 6 world grid.
#include "effects_trail.h"
#include "engine_frame.h"
#include "log.h"
#include "renderer.h"
#include "config.h"
#include "options.h"
#include "photo11.h"
#include "settings.h"
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <cmath>
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


// --trailtest: the trail's recording on a car driving straight along x at 144 frames a second
// (frame times +-20 %, 3 % of the frames without the car): how far its drawn end lags behind the
// car, and how evenly the fade-in behind the car (1.6 m from that end) moves from frame to frame.
// "old" is the recording before 2026-10-16 (the live end only from 30 cm on, none without the car).
int trailTest() {
    struct Old { // the old recording, for comparison
        float kept = 0, clock = 0, liveX = 0;
        bool keptValid = false;
        float newest = 0;
        void step(float dt, bool seen, float x) {
            clock += dt;
            if (!seen) { newest = kept; return; }
            const float moved = keptValid ? std::fabs(x - kept) : 0.0f;
            bool live = moved >= 0.3f || !keptValid;
            if ((clock >= 1.0f / 60.0f && moved >= 0.3f) || moved >= 1.0f || !keptValid) {
                clock = moved >= 1.0f ? 0.0f : std::fmod(clock, 1.0f / 60.0f);
                kept = x;
                keptValid = true;
                live = false;
            }
            newest = live ? x : kept;
        }
    };
    unsigned seed = 12345;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
    for (float speed : {8.0f, 25.0f, 60.0f}) {
        TrailStage trail;
        trail.startRecording();
        Old old;
        float x = 0.0f, lastFadeNew = 0.0f, lastFadeOld = 0.0f;
        double lagNew = 0, lagOld = 0, jerkNew = 0, jerkOld = 0;
        int backNew = 0, backOld = 0, frames = 0;
        for (int f = 0; f < 2000; f++) {
            const float dt = (1.0f / 144.0f) * (0.8f + 0.4f * rnd());
            x += speed * dt;
            const bool seen = rnd() > 0.03f;
            const float car[3] = {x, 0.0f, 0.0f};
            trail.tick(dt);
            const bool live = trail.record(dt, seen ? car : nullptr);
            const float newest = trail.point(live ? trail.head() : trail.head() + 16384 - 1)[0];
            old.step(dt, seen, x);
            const float fadeNew = newest - 1.6f, fadeOld = old.newest - 1.6f;
            if (f > 10) {
                frames++;
                lagNew += x - newest;
                lagOld += x - old.newest;
                // How far the fade-in's move this frame differs from the car's.
                jerkNew += std::fabs((fadeNew - lastFadeNew) - speed * dt);
                jerkOld += std::fabs((fadeOld - lastFadeOld) - speed * dt);
                backNew += fadeNew < lastFadeNew - 1e-4f;
                backOld += fadeOld < lastFadeOld - 1e-4f;
            }
            lastFadeNew = fadeNew;
            lastFadeOld = fadeOld;
        }
        printf("trail test %4.0f m/s, 144 fps: end behind the car  new %.3f m, old %.3f m (mean); fade-in step off the car's  "
               "new %.4f m, old %.4f m (mean); frames it went back  new %d, old %d of %d\n",
               speed, lagNew / frames, lagOld / frames, jerkNew / frames, jerkOld / frames, backNew, backOld, frames);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--trailtest")) return trailTest();
    if (argc < 3) {
        printf("usage: mp_preview <capture folder> <out.bmp> [--preset Name] [--view N] [--set Key=Value ...]\n");
        return 2;
    }
    tmshaders::log::setFileName(L"tmvs-mp-preview.log"); // never truncate the game's log
    const std::string folder = argv[1];
    bool hdrStage = false;
    double time = 10.0;
    int frames = 1;       // --frames: the HDR stage this many times, 1/60 s apart (TAA, motion blur, auto focus)
    bool photo = false;   // --photo: photo mode (full quality, full-res depth of field)
    float trailCar[3] = {}; // --car x,y,z: a car driving to there over the frames (the neon trail)
    bool haveTrailCar = false;
    float trailFrom[3] = {}; // --carfrom x,y,z: where it starts (default: from the camera's side, at --car's height)
    bool haveTrailFrom = false;
    float focus[2] = {-1.0f, -1.0f}; // --focus u,v: the photo mode's focus point
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
        if (opt == "--photo") {
            photo = true;
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
        else if (opt == "--frames") frames = std::max(1, atoi(value.c_str()));
        else if (opt == "--car") haveTrailCar = sscanf(value.c_str(), "%f,%f,%f", &trailCar[0], &trailCar[1], &trailCar[2]) == 3;
        else if (opt == "--carfrom") haveTrailFrom = sscanf(value.c_str(), "%f,%f,%f", &trailFrom[0], &trailFrom[1], &trailFrom[2]) == 3;
        else if (opt == "--focus") {
            if (sscanf(value.c_str(), "%f,%f", &focus[0], &focus[1]) != 2) {
                printf("--focus wants u,v\n");
                return 2;
            }
        }
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
    if (FILE* f = fopen((folder + "/scenep.bin").c_str(), "rb")) {
        camera.hasSceneP = fread(camera.rawP, 1, sizeof(camera.rawP), f) == sizeof(camera.rawP);
        fclose(f);
        if (haveCamera && decodeSceneP(camera)) printf("  sun from the lighting constants\n");
    }
    printf("  camera %s\n", haveCamera ? "found" : "missing");

    // The sun shadow pass (volumetric light): constants, raster scale, comparison sampler.
    EngineShadow shadow;
    {
        std::vector<char> cb;
        if (FILE* f = fopen((folder + "/shadow_cb.bin").c_str(), "rb")) {
            fseek(f, 0, SEEK_END);
            cb.resize(static_cast<size_t>(ftell(f)));
            fseek(f, 0, SEEK_SET);
            if (cb.empty() || fread(cb.data(), 1, cb.size(), f) != cb.size()) cb.clear();
            fclose(f);
        }
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        bool haveRaster = false;
        if (FILE* f = fopen((folder + "/shadow.txt").c_str(), "r")) {
            int filter = 0, au = 0, av = 0, aw = 0, cmp = 0;
            haveRaster = fscanf(f, "raster %f %f %f %f\nfilter %d\naddress %d %d %d\ncompare %d", &shadow.raster[0], &shadow.raster[1],
                                &shadow.raster[2], &shadow.raster[3], &filter, &au, &av, &aw, &cmp) == 9;
            if (haveRaster) {
                sd.Filter = static_cast<D3D11_FILTER>(filter);
                sd.AddressU = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(au);
                sd.AddressV = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(av);
                sd.AddressW = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(aw);
                sd.ComparisonFunc = static_cast<D3D11_COMPARISON_FUNC>(cmp);
            }
            fclose(f);
        }
        if (!cb.empty() && haveRaster) {
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = static_cast<UINT>(cb.size());
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA init = {cb.data(), 0, 0};
            device->CreateBuffer(&bd, &init, &shadow.constants);
            device->CreateSamplerState(&sd, &shadow.sampler);
            shadow.constantsSize = bd.ByteWidth;
            shadow.valid = shadow.constants && shadow.sampler && tex[kTexShadowMap0] && tex[kTexShadowMap1];
        }
        printf("  sun shadow pass %s\n", shadow.valid ? "found (volumetric light)" : "missing");
    }

    Look look;
    look.settings = &config.settings;
    look.lut = config.lutFile;
    look.lutAmount = config.lutAmount;
    // --photo --set Enabled=0: photo mode with the shaders off, as hooks.cpp prepareLook.
    tmshaders::Settings vanilla = config.settings;
    if (photo && !config.settings.enabled) {
        photo::vanillaLook(vanilla);
        look.settings = &vanilla;
        look.lutAmount = 0.0f;
        printf("  photo mode with the shaders off: the camera's effects only\n");
    }
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
        engine.shadow = shadow.valid ? &shadow : nullptr;
        engine.photo = photo;
        engine.focus[0] = focus[0];
        engine.focus[1] = focus[1];
        for (int f = 0; f < frames; f++) {
            engine.time = time + (f - frames + 1) / 60.0;
            // The car: from the camera's side (or --carfrom) towards --car, arriving in the last frame.
            EngineCar car;
            if (haveTrailCar && haveCamera) {
                const float k = frames > 1 ? static_cast<float>(f) / (frames - 1) : 1.0f;
                for (int i = 0; i < 3; i++) car.position[i] = camera.eye[i] + (trailCar[i] - camera.eye[i]) * (0.3f + 0.7f * k);
                car.position[1] = trailCar[1];
                if (haveTrailFrom)
                    for (int i = 0; i < 3; i++) car.position[i] = trailFrom[i] + (trailCar[i] - trailFrom[i]) * k;
                car.valid = true;
                engine.car = &car;
            }
            if (!renderer.toneMap(ctx, rtv, in[0], in[1], engineConstants, look, engine)) {
                printf("the HDR stage failed\n");
                return 1;
            }
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
