#include "effects_sky.h"
#include "effects.h"
#include "log.h"
#include "noise.h"
#include "settings.h"
#include "shaders/PS_AuroraHalf.h"
#include "shaders/PS_Clouds.h"
#include "shaders/PS_FlareDown.h"
#include "shaders/PS_RayBlur.h"
#include "shaders/PS_RayMask.h"
#include "shaders/PS_SkyAurora.h"
#include "shaders/PS_SkyAverage.h"
#include "shaders/PS_SkyBlackHole.h"
#include "shaders/PS_SkyClear.h"
#include "shaders/PS_SkyGame.h"
#include "shaders/PS_SkyRing.h"
#include "shaders/PS_SkyStars.h"
#include "shaders/PS_SunPost.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace tmvs_mp {
namespace {

template <typename T> void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

const char* const kSupported[] = {"SkyMode",       "SkyNight",      "SkyRotation",     "SkyBrightness",   "CloudAmount",
                                  "StarAmount",    "AuroraSpeed",   "SkyEffectSize",   "BlackHoleAzimuth", "BlackHoleElevation",
                                  "PlanetSize",    "PlanetType",    "PlanetView",      "PlanetAzimuth",   "PlanetElevation",
                                  "VolumetricClouds", "CloudCoverage", "CloudHeight",  "GodRays",         "GodRayDecay",
                                  "LensFlare",     "SunElevation",  "SunAzimuth",      "Lightning"};

// The tone curves of hdr.hlsl on a grey value, to know what our display-linear value shows as.
struct ToneConstants {
    float a, b, bc, outScale, de, df, ef, grey;
};

float engineCurve(const ToneConstants& t, float x) {
    const float f = (x * (t.a * x + t.bc) + t.de) / (x * (t.a * x + t.b) + t.df) - t.ef;
    return f * t.outScale + 3.0f * f * t.grey;
}

float shoulder(float x, float knee) {
    if (x < knee) return x;
    return knee + (1.0f - knee) * (1.0f - std::exp(-(x - knee) / ((1.0f - knee) * 1.7f)));
}

float aces(float x) {
    return std::clamp(x * (2.51f * x + 0.03f) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
}

// The scale y (a geometric bisection like hdr.hlsl's matchScale) at which curve(0.18 y) reaches
// `target`; the curves rise monotonically.
template <typename F> float solve(F curve, float target) {
    float lo = 0.01f, hi = 100.0f;
    for (int k = 0; k < 40; k++) {
        const float mid = std::sqrt(lo * hi);
        if (curve(0.18f * mid) < target) lo = mid;
        else hi = mid;
    }
    return std::sqrt(lo * hi);
}

// TMNF's linear colour v -> the display-linear value here that the tone curve shows as TMNF
// showed v (mid grey exactly; with the TMVS curve every value, it is TMNF's curve).
float tmnfScale(const ToneConstants& t, int curve, float knee) {
    knee = std::clamp(knee, 0.2f, 0.95f);
    const float grey = engineCurve(t, 0.18f);
    const auto engine = [&](float x) { return engineCurve(t, x); };
    const auto tmvs = [&](float x) { return shoulder(x, knee); };
    const float tmnfGrey = shoulder(0.18f, knee);
    if (curve == 1) return solve(tmvs, tmnfGrey) / solve(tmvs, grey); // the match hdr.hlsl applies
    if (curve == 2) return solve(aces, tmnfGrey) / solve(aces, grey);
    return solve(engine, tmnfGrey);
}

} // namespace

struct SkyStage::Impl {
    ID3D11Device* device = nullptr;
    ID3D11PixelShader* skyPs[6] = {}; // game sky, then modes 1-5
    ID3D11PixelShader* auroraHalf = nullptr;
    ID3D11PixelShader* average = nullptr;
    ID3D11PixelShader* cloudsPs = nullptr;
    ID3D11PixelShader* rayMask = nullptr;
    ID3D11PixelShader* rayBlur = nullptr;
    ID3D11PixelShader* flareDown = nullptr;
    ID3D11PixelShader* sunPost = nullptr;

    ID3D11Texture3D* noise = nullptr;
    ID3D11ShaderResourceView* noiseSrv = nullptr;
    bool noiseFailed = false;

    UINT w = 0, h = 0;
    Target sky;      // full: the finished sky
    Target aurora;   // half: the aurora's curtains (TMNF units)
    Target clouds;   // half: volumetric clouds
    Target avg[2];   // 1x1 ping-pong: the sky average, eased over time
    Target rays[2];  // half: shaft mask and blur
    Target flare[2]; // quarter, eighth: the flare's source
    int avgIndex = 0;
    bool avgValid = false;
    double lastTime = 0.0;

    // Read-back of the engine's tone-map constants (a few frames late, never waiting).
    static constexpr int kReadbacks = 3;
    ID3D11Buffer* toneCopy[kReadbacks] = {};
    bool toneCopyPending[kReadbacks] = {};
    UINT toneCopySize = 0;
    int toneCopyIndex = 0;
    float loggedScale = 0.0f;

    void releaseTargets() {
        sky.release();
        aurora.release();
        clouds.release();
        for (auto& t : avg) t.release();
        for (auto& t : rays) t.release();
        for (auto& t : flare) t.release();
        w = h = 0;
        avgValid = false;
    }

    bool ensureTargets(UINT width, UINT height) {
        if (sky.ready() && w == width && h == height) return true;
        releaseTargets();
        const UINT hw = std::max(1u, width / 2), hh = std::max(1u, height / 2);
        bool ok = sky.create(device, width, height, DXGI_FORMAT_R11G11B10_FLOAT);
        ok &= aurora.create(device, hw, hh, DXGI_FORMAT_R11G11B10_FLOAT);
        ok &= clouds.create(device, hw, hh, DXGI_FORMAT_R16G16B16A16_FLOAT);
        for (auto& t : avg) ok &= t.create(device, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
        for (auto& t : rays) ok &= t.create(device, hw, hh, DXGI_FORMAT_R11G11B10_FLOAT);
        ok &= flare[0].create(device, std::max(1u, width / 4), std::max(1u, height / 4), DXGI_FORMAT_R11G11B10_FLOAT);
        ok &= flare[1].create(device, std::max(1u, width / 8), std::max(1u, height / 8), DXGI_FORMAT_R11G11B10_FLOAT);
        if (!ok) {
            releaseTargets();
            return false;
        }
        w = width;
        h = height;
        return true;
    }

    // The tiling 3D noise of the volumetric clouds (TMNF's, generated once on the CPU).
    bool ensureNoise() {
        if (noiseSrv) return true;
        if (noiseFailed) return false;
        const UINT size = 64;
        const std::vector<uint32_t>& texels = tmshaders::noise::cloudVolume(size);
        D3D11_TEXTURE3D_DESC d = {};
        d.Width = d.Height = d.Depth = size;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // A8R8G8B8 texels: r = Perlin-Worley, g/b/a = Worley octaves
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data = {texels.data(), size * 4, size * size * 4};
        if (FAILED(device->CreateTexture3D(&d, &data, &noise)) || FAILED(device->CreateShaderResourceView(noise, nullptr, &noiseSrv))) {
            TMVS_LOG("sky: the cloud noise texture failed");
            safeRelease(noise);
            noiseFailed = true;
            return false;
        }
        return true;
    }

    void updateScale(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame, const tmshaders::Settings& s) {
        if (!frame.engineTone) return;
        D3D11_BUFFER_DESC bd;
        frame.engineTone->GetDesc(&bd);
        if (bd.ByteWidth < sizeof(ToneConstants)) return;
        if (toneCopySize != bd.ByteWidth) {
            for (auto& b : toneCopy) safeRelease(b);
            for (auto& p : toneCopyPending) p = false;
            D3D11_BUFFER_DESC sd = {};
            sd.ByteWidth = bd.ByteWidth;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            for (auto& b : toneCopy)
                if (FAILED(device->CreateBuffer(&sd, nullptr, &b))) return;
            toneCopySize = bd.ByteWidth;
        }
        // The oldest copy first: its data is a few frames old and should be ready.
        for (int k = 1; k <= kReadbacks; k++) {
            const int i = (toneCopyIndex + k) % kReadbacks;
            if (!toneCopyPending[i]) continue;
            D3D11_MAPPED_SUBRESOURCE m;
            if (ctx->Map(toneCopy[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) continue;
            ToneConstants t;
            memcpy(&t, m.pData, sizeof(t));
            ctx->Unmap(toneCopy[i], 0);
            toneCopyPending[i] = false;
            // A real Hable curve: positive terms, a finite output scale.
            if (!(t.a > 0.0f && t.df > 0.0f && std::isfinite(t.outScale) && t.outScale > 0.0f && std::isfinite(t.grey))) continue;
            const float scale = tmnfScale(t, s.toneCurve, s.toneKnee);
            if (std::isfinite(scale) && scale > 0.05f && scale < 20.0f) {
                fx.setTmnfScale(scale);
                if (std::fabs(scale - loggedScale) > 0.05f * loggedScale) {
                    loggedScale = scale;
                    TMVS_LOG("sky: TMNF colours x %.3f here (tone curve %d)", scale, s.toneCurve);
                }
            }
        }
        if (!toneCopyPending[toneCopyIndex]) {
            ctx->CopyResource(toneCopy[toneCopyIndex], frame.engineTone);
            toneCopyPending[toneCopyIndex] = true;
        }
        toneCopyIndex = (toneCopyIndex + 1) % kReadbacks;
    }
};

bool SkyStage::init(ID3D11Device* device) {
    if (!m_impl) m_impl = new Impl;
    Impl& d = *m_impl;
    d.device = device;
    bool ok = SUCCEEDED(device->CreatePixelShader(g_PS_SkyGame, sizeof(g_PS_SkyGame), nullptr, &d.skyPs[0]));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_SkyClear, sizeof(g_PS_SkyClear), nullptr, &d.skyPs[1]));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_SkyStars, sizeof(g_PS_SkyStars), nullptr, &d.skyPs[2]));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_SkyBlackHole, sizeof(g_PS_SkyBlackHole), nullptr, &d.skyPs[3]));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_SkyAurora, sizeof(g_PS_SkyAurora), nullptr, &d.skyPs[4]));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_SkyRing, sizeof(g_PS_SkyRing), nullptr, &d.skyPs[5]));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_AuroraHalf, sizeof(g_PS_AuroraHalf), nullptr, &d.auroraHalf));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_SkyAverage, sizeof(g_PS_SkyAverage), nullptr, &d.average));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_Clouds, sizeof(g_PS_Clouds), nullptr, &d.cloudsPs));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_RayMask, sizeof(g_PS_RayMask), nullptr, &d.rayMask));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_RayBlur, sizeof(g_PS_RayBlur), nullptr, &d.rayBlur));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_FlareDown, sizeof(g_PS_FlareDown), nullptr, &d.flareDown));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_SunPost, sizeof(g_PS_SunPost), nullptr, &d.sunPost));
    if (!ok) TMVS_LOG("sky: creating the shaders failed");
    return ok;
}

void SkyStage::releaseTargets() {
    if (m_impl) m_impl->releaseTargets();
}

void SkyStage::release() {
    if (!m_impl) return;
    Impl& d = *m_impl;
    d.releaseTargets();
    for (auto& p : d.skyPs) safeRelease(p);
    safeRelease(d.auroraHalf);
    safeRelease(d.average);
    safeRelease(d.cloudsPs);
    safeRelease(d.rayMask);
    safeRelease(d.rayBlur);
    safeRelease(d.flareDown);
    safeRelease(d.sunPost);
    safeRelease(d.noiseSrv);
    safeRelease(d.noise);
    for (auto& b : d.toneCopy) safeRelease(b);
    delete m_impl;
    m_impl = nullptr;
}

bool SkyStage::supports(const char* key) {
    for (const char* k : kSupported)
        if (!strcmp(k, key)) return true;
    return false;
}

SkyStage::Output SkyStage::run(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame, const tmshaders::Settings& s) {
    Output out;
    if (!m_impl || !m_impl->skyPs[0] || !m_impl->sunPost) return out;
    Impl& d = *m_impl;
    d.updateScale(ctx, fx, frame, s);
    if (!d.ensureTargets(fx.width(), fx.height())) return out;
    const float* c = fx.frameConstants(); // the TMNF constants, as the shaders see them (c[n][k])
    const auto cn = [c](int n, int k) { return c[n * 4 + k]; };
    const float night = cn(20, 1);
    const bool lightning = cn(36, 2) > 0.0f;
    const int mode = std::clamp(s.skyMode, 0, 5);

    // Volumetric clouds (half res), lit with last frame's sky average (it is eased over time anyway).
    const bool clouds = s.volumetricClouds > 0.0f && d.ensureNoise();
    if (clouds) {
        ID3D11ShaderResourceView* noise = d.noiseSrv;
        ctx->PSSetShaderResources(16, 1, &noise);
        ID3D11ShaderResourceView* avg = d.avgValid ? d.avg[d.avgIndex].srv : nullptr;
        fx.run(ctx, kClouds, d.cloudsPs, d.clouds, &avg, 1);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(16, 1, &none);
        out.clouds = d.clouds.srv;
    }

    // The sky: a custom one, or the game's when anything changes it (gradient, sun glow, night,
    // clouds, lightning).
    const bool gameSky = s.skyEnhance > 0.0f || (s.sunGlow > 0.0f && cn(6, 3) > 0.5f) || night > 0.0f || clouds || lightning;
    if (mode > 0 || gameSky) {
        ID3D11ShaderResourceView* in[3] = {nullptr, nullptr, out.clouds};
        float p0[4] = {}, p1[4] = {clouds ? 1.0f : 0.0f, 0, 0, 0};
        if (mode == 4) {
            // The aurora's curtains are the costliest sky: marched at half resolution and upscaled
            // with EASU in the sky pass. Stars stay at full resolution.
            static const float kAuroraSlices[3] = {16.0f, 26.0f, 40.0f};
            const float a0[4] = {kAuroraSlices[std::clamp(s.quality, 0, 2)], 0.0f, 1.0f / fx.width(), 1.0f / fx.height()};
            fx.run(ctx, kAuroraHalf, d.auroraHalf, d.aurora, nullptr, 0, a0);
            in[1] = d.aurora.srv;
            p0[0] = static_cast<float>(d.aurora.w);
            p0[1] = static_cast<float>(d.aurora.h);
            p0[2] = 1.0f / d.aurora.w;
            p0[3] = 1.0f / d.aurora.h;
        }
        fx.run(ctx, kSkyClear + std::max(mode, 1) - 1, d.skyPs[mode], d.sky, in, 3, p0, p1);
        out.sky = d.sky.srv;
    }

    // The haze's colour, eased over time: with a few sky pixels flickering through a gap (between
    // walls, in a building) it jumped every frame and the fog came and went.
    const double dt = d.avgValid ? std::max(frame.time - d.lastTime, 0.0) : 0.0;
    d.lastTime = frame.time;
    const int previous = d.avgIndex;
    d.avgIndex ^= 1;
    const bool history = d.avgValid && fx.historyValid();
    ID3D11ShaderResourceView* avgIn[2] = {mode > 0 ? out.sky : nullptr, d.avg[previous].srv};
    const float a0[4] = {static_cast<float>(1.0 - std::exp(-dt / 0.4)), history ? 0.0f : 1.0f, 0, 0};
    const float a1[4] = {mode > 0 && out.sky ? 1.0f : 0.0f, 0, 0, 0};
    fx.run(ctx, kSkyAverage, d.average, d.avg[d.avgIndex], avgIn, 2, a0, a1);
    d.avgValid = true;
    out.average = d.avg[d.avgIndex].srv;
    return out;
}

void SkyStage::post(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame, const tmshaders::Settings& s) {
    (void)frame;
    if (!m_impl || !m_impl->sky.ready()) return;
    Impl& d = *m_impl;
    const float* c = fx.frameConstants();
    const auto cn = [c](int n, int k) { return c[n * 4 + k]; };
    const float sunFade = cn(8, 2);
    const int debug = s.debugView;
    const bool debugView = debug >= 20 && debug <= 24;
    // Skipped when the sun is off screen: the shafts and the flare are weighted by its on-screen fade.
    const bool rays = (s.godRays > 0.0f && sunFade > 0.001f) || debug == 20;
    const bool flare = (s.lensFlare > 0.0f && sunFade > 0.001f) || debug == 24;
    if (!rays && !flare && !debugView) return;

    ID3D11ShaderResourceView* hdr = fx.current();
    if (rays) {
        fx.run(ctx, kRayMask, d.rayMask, d.rays[0], &hdr, 1);
        const float b0[4] = {1.0f, s.godRayDecay, 0, 0}, b1[4] = {0.3f, s.godRayDecay, 0, 0};
        fx.run(ctx, kRayBlur, d.rayBlur, d.rays[1], &d.rays[0].srv, 1, b0);
        fx.run(ctx, kRayBlur, d.rayBlur, d.rays[0], &d.rays[1].srv, 1, b1);
    }
    if (flare) {
        const float f0[4] = {1.0f, 0, 1.0f / fx.width(), 1.0f / fx.height()};
        fx.run(ctx, kRayBlur, d.flareDown, d.flare[0], &hdr, 1, f0);
        const float f1[4] = {0.0f, 0, 1.0f / d.flare[0].w, 1.0f / d.flare[0].h};
        fx.run(ctx, kRayBlur, d.flareDown, d.flare[1], &d.flare[0].srv, 1, f1);
    }
    ID3D11ShaderResourceView* in[6] = {hdr, rays ? d.rays[0].srv : nullptr, flare ? d.flare[1].srv : nullptr, d.clouds.srv, d.sky.srv,
                                       d.avg[d.avgIndex].srv};
    const float p0[4] = {rays ? 1.0f : 0.0f, flare ? 1.0f : 0.0f, 0, 0};
    fx.run(ctx, kRayMask, d.sunPost, fx.next(), in, 6, p0);
    fx.swap();
}

} // namespace tmvs_mp
