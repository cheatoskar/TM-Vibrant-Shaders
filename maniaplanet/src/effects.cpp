#include "effects.h"
#include "config.h"
#include "hooks.h"
#include "log.h"
#include "options.h"
#include "settings.h"
#include "weather.h"
#include "shaders/PS_BilateralBlur.h"
#include "shaders/PS_DownsampleND.h"
#include "shaders/PS_GI.h"
#include "shaders/PS_GITemporal.h"
#include "shaders/PS_Lighting.h"
#include "shaders/PS_LightingMoon.h"
#include "shaders/PS_OcclusionShadow.h"
#include "shaders/PS_SpillBlur.h"
#include "shaders/PS_SpillDown.h"
#include "shaders/PS_SpillTemporal.h"
#include "shaders/PS_Volumetric.h"
#include "shaders/VS_Full.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace tmvs_mp {
namespace {

template <typename T> void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

float smoothstepf(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

const char* const kPassNames[kPassCount] = {
    "DownsampleND", "OcclusionShadow", "BilateralBlur", "GI",       "GITemporal", "SkyClear",   "SkyStars", "SkyBlackHole",
    "SkyAurora",    "SkyRing",         "AuroraHalf",    "SkyAverage", "Clouds",   "Reflect",    "SpillDown", "SpillBlur",
    "SpillTemporal", "Lighting",     "RainDrop",        "RainSplash",    "Spray",    "SnowFlake",  "Trail",      "Focus",    "DofBlur",
    "Cinematic",    "RayMask",         "RayBlur",       "BloomDown", "BloomUp",   "Final",      "TAA",      "Sharpen",
    "Volumetric"};

// The settings ManiaPlanet can do so far (the menu shows only these). The look (hdr.hlsl), then
// the scene effects (scene.hlsl), then the stages as they come. Not DisableGameFX: the engine's
// bloom stays (hooks.cpp skipEngineBloom), so it changed nothing here.
const char* const kSupported[] = {
    // look and tone map
    "Exposure", "Contrast", "ToneKnee", "ToneCurve", "Saturation", "Vibrance", "Temperature", "Tint", "Lift", "Gamma", "Gain",
    "SplitToning", "Bloom", "BloomRadius", "Vignette", "FilmGrain", "ChromaticAberration", "Sharpen",
    // scene
    "AOStrength", "AORadius", "ShadowStrength", "ShadowLength", "NeonLight", "GlobalIllumination", "SunLight", "AmbientTint",
    "SunColor", "GameSunColor", "SkyColor", "SkyEnhance", "SunGlow", "FogDensity", "FogHeightFalloff", "FogSunScatter",
    "HazeDistance", "HighlightBoost", "Quality", "AutoQuality", "TargetFPS", "SunElevation", "SunAzimuth",
    "VolumetricLight", "GameNight", "EngineMoon"};

} // namespace

const char* passName(int pass) {
    return pass >= 0 && pass < kPassCount ? kPassNames[pass] : "?";
}

// ---- Target ----

bool Target::create(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT fmt) {
    if (tex && w == width && h == height && format == fmt) return true;
    release();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = std::max(1u, width);
    d.Height = std::max(1u, height);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &tex)) || FAILED(device->CreateRenderTargetView(tex, nullptr, &rtv)) ||
        FAILED(device->CreateShaderResourceView(tex, nullptr, &srv))) {
        TMVS_LOG("effects: a %ux%u target (format %d) failed", d.Width, d.Height, fmt);
        release();
        return false;
    }
    w = d.Width;
    h = d.Height;
    format = fmt;
    return true;
}

void Target::release() {
    safeRelease(srv);
    safeRelease(rtv);
    safeRelease(tex);
    w = h = 0;
    format = DXGI_FORMAT_UNKNOWN;
}

// ---- Profiler ----

bool Profiler::init(ID3D11Device* device) {
    D3D11_QUERY_DESC q = {};
    for (Frame& f : m_frames) {
        q.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(device->CreateQuery(&q, &f.disjoint))) return false;
        q.Query = D3D11_QUERY_TIMESTAMP;
        for (auto& s : f.stamps)
            if (FAILED(device->CreateQuery(&q, &s))) return false;
    }
    m_ready = true;
    return true;
}

void Profiler::release() {
    for (Frame& f : m_frames) {
        safeRelease(f.disjoint);
        for (auto& s : f.stamps) safeRelease(s);
        f = Frame{};
    }
    m_current = nullptr;
    m_ready = false;
}

void Profiler::begin(ID3D11DeviceContext* ctx) {
    m_current = nullptr;
    if (!m_ready) return;
    Frame& f = m_frames[m_index];
    if (f.pending) return; // its results never came: skip a frame rather than wait
    ctx->Begin(f.disjoint);
    ctx->End(f.stamps[0]);
    f.count = 1;
    f.pass[0] = -1;
    m_current = &f;
}

void Profiler::mark(ID3D11DeviceContext* ctx, int pass) {
    if (!m_current || m_current->count >= kStamps) return;
    ctx->End(m_current->stamps[m_current->count]);
    m_current->pass[m_current->count++] = pass;
}

void Profiler::end(ID3D11DeviceContext* ctx) {
    if (!m_current) return;
    ctx->End(m_current->disjoint);
    m_current->pending = true;
    m_current = nullptr;
    m_index = (m_index + 1) % kFrames;
}

void Profiler::collect(ID3D11DeviceContext* ctx) {
    for (Frame& f : m_frames) {
        if (!f.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        if (ctx->GetData(f.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 stamps[kStamps] = {};
        bool ok = !dj.Disjoint && dj.Frequency > 0;
        for (int i = 0; ok && i < f.count; i++)
            ok = ctx->GetData(f.stamps[i], &stamps[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (!ok) {
            if (dj.Disjoint) f.pending = false;
            continue;
        }
        f.pending = false;
        float ms[kPassCount] = {};
        for (int i = 1; i < f.count; i++) {
            const int p = f.pass[i];
            if (p >= 0 && p < kPassCount) ms[p] += static_cast<float>(static_cast<double>(stamps[i] - stamps[i - 1]) * 1000.0 / dj.Frequency);
        }
        float total = 0.0f;
        for (int p = 0; p < kPassCount; p++) {
            m_ms[p] = m_ms[p] * 0.9f + ms[p] * 0.1f;
            total += m_ms[p];
        }
        m_total = total;
    }
}

// ---- Effects ----

bool Effects::init(ID3D11Device* device) {
    m_device = device;
    bool ok = SUCCEEDED(device->CreateVertexShader(g_VS_Full, sizeof(g_VS_Full), nullptr, &m_vs));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_DownsampleND, sizeof(g_PS_DownsampleND), nullptr, &m_ndHalf));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_OcclusionShadow, sizeof(g_PS_OcclusionShadow), nullptr, &m_occlusion));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_BilateralBlur, sizeof(g_PS_BilateralBlur), nullptr, &m_blur));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_GI, sizeof(g_PS_GI), nullptr, &m_gi));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_GITemporal, sizeof(g_PS_GITemporal), nullptr, &m_giTemporal));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_SpillDown, sizeof(g_PS_SpillDown), nullptr, &m_spillDown));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_SpillBlur, sizeof(g_PS_SpillBlur), nullptr, &m_spillBlur));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_SpillTemporal, sizeof(g_PS_SpillTemporal), nullptr, &m_spillTemporal));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_Lighting, sizeof(g_PS_Lighting), nullptr, &m_lighting));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_LightingMoon, sizeof(g_PS_LightingMoon), nullptr, &m_lightingMoon));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_Volumetric, sizeof(g_PS_Volumetric), nullptr, &m_volumetric));

    D3D11_BUFFER_DESC cb = {};
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.ByteWidth = sizeof(m_c);
    ok &= SUCCEEDED(device->CreateBuffer(&cb, nullptr, &m_frameCb));
    cb.ByteWidth = 32;
    ok &= SUCCEEDED(device->CreateBuffer(&cb, nullptr, &m_passCb));

    D3D11_SAMPLER_DESC sd = {};
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    const D3D11_FILTER filters[4] = {D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_FILTER_MIN_MAG_MIP_LINEAR,
                                     D3D11_FILTER_MIN_MAG_MIP_POINT};
    for (int i = 0; i < 4; i++) {
        sd.Filter = filters[i];
        sd.AddressU = sd.AddressV = sd.AddressW = i < 2 ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
        ok &= SUCCEEDED(device->CreateSamplerState(&sd, &m_samplers[i]));
    }
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ok &= SUCCEEDED(device->CreateRasterizerState(&rd, &m_rs));
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ok &= SUCCEEDED(device->CreateDepthStencilState(&dd, &m_dss));
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok &= SUCCEEDED(device->CreateBlendState(&bd, &m_blend));
    m_profiler.init(device);
    if (!m_sky.init(device)) TMVS_LOG("effects: the sky stage failed to start");
    if (!m_weather.init(device)) TMVS_LOG("effects: the weather stage failed to start");
    if (!m_cinema.init(device)) TMVS_LOG("effects: the cinema stage failed to start");
    if (!m_trail.init(device)) TMVS_LOG("effects: the neon trail failed to start");
    if (!ok) TMVS_LOG("effects: creating shaders or states failed (feature level 0x%x)", device->GetFeatureLevel());
    return ok;
}

void Effects::releaseTargets() {
    m_ndHalf_.release();
    m_colourHalf.release();
    for (auto& t : m_occ) t.release();
    for (auto& t : m_gi_) t.release();
    for (auto& t : m_giHistory) t.release();
    for (auto& t : m_spill) t.release();
    for (auto& t : m_spillHistory) t.release();
    for (auto& t : m_volume) t.release();
    for (auto& t : m_hdr) t.release();
    m_result = nullptr;
    m_sky.releaseTargets();
    m_weather.releaseTargets();
    m_cinema.releaseTargets();
    m_w = m_h = 0;
    m_havePrevious = false;
}

void Effects::release() {
    releaseTargets();
    m_sky.release();
    m_weather.release();
    m_cinema.release();
    m_trail.release();
    m_profiler.release();
    safeRelease(m_vs);
    safeRelease(m_ndHalf);
    safeRelease(m_occlusion);
    safeRelease(m_blur);
    safeRelease(m_gi);
    safeRelease(m_giTemporal);
    safeRelease(m_spillDown);
    safeRelease(m_spillBlur);
    safeRelease(m_spillTemporal);
    safeRelease(m_lighting);
    safeRelease(m_lightingMoon);
    safeRelease(m_volumetric);
    safeRelease(m_frameCb);
    safeRelease(m_passCb);
    for (auto& s : m_samplers) safeRelease(s);
    safeRelease(m_rs);
    safeRelease(m_dss);
    safeRelease(m_blend);
    m_device = nullptr;
}

bool Effects::wanted(const tmshaders::Settings& s) {
    return s.aoStrength > 0.0f || s.shadowStrength > 0.0f || s.globalIllumination > 0.0f || s.neonLight > 0.0f || s.sunLight > 0.0f ||
           s.ambientTint > 0.0f || s.fogDensity > 0.0f || s.skyMode > 0 || s.skyEnhance > 0.0f || s.debugView != 0 ||
           s.volumetricClouds > 0.0f || s.godRays > 0.0f || s.lensFlare > 0.0f || s.wetness > 0.0f || s.rain > 0.0f || s.snow > 0.0f ||
           s.puddles > 0.0f || s.reflections > 0.0f || s.lightning > 0.0f || s.depthOfField > 0.0f || s.motionBlur > 0.0f || s.taa ||
           s.volumetricLight > 0.0f || s.neonTrail > 0.0f;
}

bool Effects::supports(const char* key) {
    if (!strcmp(key, "VideoExportBest")) return true; // the replay editor's Shoot video (hooks.cpp, VideoWatch)
    for (const char* k : kSupported)
        if (!strcmp(k, key)) return true;
    return SkyStage::supports(key) || WeatherStage::supports(key) || CinemaStage::supports(key) || TrailStage::supports(key);
}

bool Effects::ensureTargets(UINT w, UINT h) {
    if (m_hdr[0].ready() && m_w == w && m_h == h) return true;
    releaseTargets();
    const UINT hw = std::max(1u, w / 2), hh = std::max(1u, h / 2);
    const UINT qw = std::max(1u, w / 4), qh = std::max(1u, h / 4);
    bool ok = m_ndHalf_.create(m_device, hw, hh, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok &= m_colourHalf.create(m_device, hw, hh, DXGI_FORMAT_R11G11B10_FLOAT);
    for (auto& t : m_occ) ok &= t.create(m_device, hw, hh, DXGI_FORMAT_R8G8B8A8_UNORM);
    for (auto& t : m_gi_) ok &= t.create(m_device, qw, qh, DXGI_FORMAT_R11G11B10_FLOAT);
    for (auto& t : m_giHistory) ok &= t.create(m_device, qw, qh, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (auto& t : m_spill) ok &= t.create(m_device, qw, qh, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (auto& t : m_spillHistory) ok &= t.create(m_device, qw, qh, DXGI_FORMAT_R16G16B16A16_FLOAT);
    m_spillValid = false;
    for (auto& t : m_volume) ok &= t.create(m_device, hw, hh, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (auto& t : m_hdr) ok &= t.create(m_device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (!ok) {
        releaseTargets();
        return false;
    }
    m_w = w;
    m_h = h;
    TMVS_LOG("effects: targets for %ux%u", w, h);
    return true;
}

void Effects::setCommonState(ID3D11DeviceContext* ctx) {
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ID3D11Buffer* noBuffer = nullptr;
    UINT zero = 0;
    ctx->IASetVertexBuffers(0, 1, &noBuffer, &zero, &zero);
    ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    ctx->VSSetShader(m_vs, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->RSSetState(m_rs);
    ctx->RSSetScissorRects(0, nullptr);
    const float blendFactor[4] = {};
    ctx->OMSetBlendState(m_blend, blendFactor, 0xffffffff);
    ctx->OMSetDepthStencilState(m_dss, 0);
}

void Effects::bindFrame(ID3D11DeviceContext* ctx) {
    setCommonState(ctx);
    ctx->PSSetSamplers(0, 4, m_samplers);
    ID3D11Buffer* cbs[2] = {m_frameCb, m_passCb};
    ctx->PSSetConstantBuffers(0, 2, cbs);
    ctx->VSSetConstantBuffers(0, 2, cbs);
}

void Effects::setPassConstants(ID3D11DeviceContext* ctx, const float* pass0, const float* pass1) {
    float p[8] = {};
    if (pass0) memcpy(p, pass0, 16);
    if (pass1) memcpy(p + 4, pass1, 16);
    ctx->UpdateSubresource(m_passCb, 0, nullptr, p, 0, 0);
}

void Effects::run(ID3D11DeviceContext* ctx, int pass, ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv, UINT w, UINT h,
                  ID3D11ShaderResourceView* const* inputs, UINT inputCount, const float* pass0, const float* pass1) {
    ID3D11ShaderResourceView* none[8] = {};
    ctx->PSSetShaderResources(0, 8, none);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = {0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    ctx->RSSetViewports(1, &vp);
    setPassConstants(ctx, pass0, pass1);
    if (inputCount) ctx->PSSetShaderResources(0, std::min(inputCount, 8u), inputs);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->Draw(3, 0);
    m_profiler.mark(ctx, pass);
}

void Effects::run(ID3D11DeviceContext* ctx, int pass, ID3D11PixelShader* ps, const Target& out, ID3D11ShaderResourceView* const* inputs,
                  UINT inputCount, const float* pass0, const float* pass1) {
    run(ctx, pass, ps, out.rtv, out.w, out.h, inputs, inputCount, pass0, pass1);
}

// The TMNF constants (pipeline.cpp, setFrameConstants) from the settings and the engine's camera.
void Effects::buildConstants(const SceneFrame& frame, const tmshaders::Settings& s) {
    const Camera& cam = *frame.camera;
    float(&c)[49][4] = m_c;
    memset(c, 0, sizeof(c));
    constexpr float kPi = 3.14159265f;

    c[0][0] = static_cast<float>(m_w);
    c[0][1] = static_cast<float>(m_h);
    c[0][2] = 1.0f / m_w;
    c[0][3] = 1.0f / m_h;

    // The projection the engine drew with: viewProj = P * W2C, so P = viewProj * W2C^-1. Its
    // camera projection (SceneV) differs from it in the depth row (0.05 against 0.0496: the depth
    // buffer is written with viewProj's).
    const float* W = cam.worldToCamera; // 3 rows of 4
    // W2C^-1 = [R^T | -R^T t].
    float inv[16] = {};
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) inv[r * 4 + k] = W[k * 4 + r];
    for (int r = 0; r < 3; r++) inv[r * 4 + 3] = -(inv[r * 4 + 0] * W[3] + inv[r * 4 + 1] * W[7] + inv[r * 4 + 2] * W[11]);
    inv[15] = 1.0f;
    float P[16] = {};
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) {
            float sum = 0.0f;
            for (int j = 0; j < 4; j++) sum += cam.viewProj[r * 4 + j] * inv[j * 4 + k];
            P[r * 4 + k] = sum;
        }
    // clip.x = P00 x + P20 z, clip.y = P11 y + P21 z, clip.z = P22 z + P32, clip.w = z.
    c[1][0] = P[0];
    c[1][1] = P[5];
    c[1][2] = P[2];
    c[1][3] = P[6];
    c[2][0] = P[10];
    c[2][1] = P[11];
    c[2][2] = static_cast<float>(std::fmod(frame.time, 4096.0));
    c[2][3] = static_cast<float>(m_frame % 1024);
    for (int i = 0; i < 3; i++) {
        c[3 + i][0] = W[i * 4 + 0];
        c[3 + i][1] = W[i * 4 + 1];
        c[3 + i][2] = W[i * 4 + 2];
        c[3 + i][3] = cam.eye[i];
    }
    auto toView = [&](const float w[3], float v[3]) {
        for (int i = 0; i < 3; i++) v[i] = W[i * 4 + 0] * w[0] + W[i * 4 + 1] * w[1] + W[i * 4 + 2] * w[2];
    };

    // The black hole of the space skies (it turns with the sky).
    const bool blackHole = (s.skyMode == 3 || s.skyMode == 5) && s.skyEffectSize > 0.0f;
    float hole[3];
    {
        const float az = s.blackHoleAzimuth * kPi / 180.0f, el = s.blackHoleElevation * kPi / 180.0f;
        const float rot = s.skyRotation * kPi / 180.0f;
        const float d[3] = {sinf(az) * cosf(el), sinf(el), cosf(az) * cosf(el)};
        hole[0] = cosf(rot) * d[0] - sinf(rot) * d[2];
        hole[1] = d[1];
        hole[2] = sinf(rot) * d[0] + cosf(rot) * d[2];
    }
    c[40][0] = hole[0];
    c[40][1] = hole[1];
    c[40][2] = hole[2];
    c[40][3] = blackHole ? s.skyEffectSize : 0.0f;

    // The engine's sun (or the preset's override), then the effects' light (the black hole's).
    float gameSun[3] = {cam.sunDirection[0], cam.sunDirection[1], cam.sunDirection[2]};
    if (s.sunElevationOverride >= 0.0f) {
        const float az = s.sunAzimuthOverride * kPi / 180.0f, el = s.sunElevationOverride * kPi / 180.0f;
        gameSun[0] = sinf(az) * cosf(el);
        gameSun[1] = sinf(el);
        gameSun[2] = cosf(az) * cosf(el);
    }
    float sw[3] = {gameSun[0], gameSun[1], gameSun[2]};
    if (blackHole) memcpy(sw, hole, sizeof(sw));
    float sv[3];
    toView(sw, sv);
    const bool ringWorld = s.skyMode == 5;
    const bool nightSky = s.skyMode >= 2 && !ringWorld;
    // The map's time of day from the engine when it tells it (its night moon passed for a day sun
    // by its colour: taken out under the black hole, shadows came out brighter than the ground).
    const int dayMood = dayTimeMood(cam);
    const bool mapNight = dayMood >= 0 ? dayMood == 0 : tmshaders::classifyMood(cam.sunColour) == tmshaders::Mood::Night;
    const bool gameNight = mapNight && !blackHole;
    const bool sunKnown = blackHole || (cam.sunDirectionKnown && !nightSky);
    const float daylight = blackHole ? fminf(0.3f + 0.12f * s.skyEffectSize, 1.0f) : smoothstepf(-0.08f, 0.18f, sw[1]) * (gameNight ? 0.35f : 1.0f);
    for (int i = 0; i < 3; i++) {
        c[6][i] = sv[i];
        c[7][i] = sw[i];
    }
    c[6][3] = sunKnown ? 1.0f : 0.0f;
    c[7][3] = sunKnown ? daylight : 1.0f;

    // Sun on screen.
    float sunFade = 0.0f, inFront = 0.0f, su = 0.5f, sv2 = -1.0f;
    if (sunKnown && sv[2] > 0.05f) {
        const float ndcX = (P[0] * sv[0] + P[2] * sv[2]) / sv[2];
        const float ndcY = (P[5] * sv[1] + P[6] * sv[2]) / sv[2];
        su = ndcX * 0.5f + 0.5f;
        sv2 = 0.5f - ndcY * 0.5f;
        const float edge = fminf(fminf(su + 0.3f, 1.3f - su), fminf(sv2 + 0.3f, 1.3f - sv2));
        sunFade = smoothstepf(0.0f, 0.3f, edge) * smoothstepf(0.05f, 0.3f, sv[2]) * (gameNight ? 0.0f : daylight);
        inFront = 1.0f;
    }
    c[8][0] = su;
    c[8][1] = sv2;
    c[8][2] = sunFade;
    c[8][3] = inFront;

    // Sun colour: the preset's blended with the engine's light (carries the time of day).
    const float* gs = cam.sunColour;
    const float gameLuma = 0.2126f * gs[0] + 0.7152f * gs[1] + 0.0722f * gs[2];
    const float presetLuma = 0.2126f * s.sunColor[0] + 0.7152f * s.sunColor[1] + 0.0722f * s.sunColor[2];
    const float w = gameLuma > 1e-3f ? s.gameSunColor : 0.0f;
    for (int i = 0; i < 3; i++) {
        const float game = gameLuma > 1e-3f ? gs[i] * presetLuma / gameLuma : s.sunColor[i];
        c[9][i] = s.sunColor[i] + (game - s.sunColor[i]) * w;
    }
    if (blackHole) {
        const float disk[3] = {1.0f, 0.8f, 0.6f};
        for (int i = 0; i < 3; i++) c[9][i] = disk[i] * presetLuma / 0.83f;
    }
    c[9][3] = s.sunLight;
    for (int i = 0; i < 3; i++) c[10][i] = s.skyColor[i];
    c[10][3] = s.ambientTint;

    c[11][0] = s.aoStrength;
    c[11][1] = s.aoRadius;
    c[11][2] = s.shadowStrength;
    // Contact shadows: the engine's shadow map covers the long ones.
    c[11][3] = std::min(s.shadowLength, 4.0f);
    c[12][0] = s.fogDensity;
    c[12][1] = s.fogHeightFalloff;
    c[12][2] = s.fogSunScatter;
    c[12][3] = s.skyEnhance;
    c[13][0] = s.godRays;
    c[13][1] = s.godRayDecay;
    c[13][2] = s.sunGlow;
    c[13][3] = s.highlightBoost;
    c[14][0] = s.bloom;
    c[14][1] = s.bloomRadius;
    c[14][2] = s.lensFlare;
    c[14][3] = s.chromaticAberration;
    c[15][0] = exp2f(s.exposure);
    c[15][1] = s.autoExposure * (nightSky || mapNight ? 0.3f : 1.0f);
    c[15][2] = s.contrast;
    c[15][3] = s.saturation;
    c[16][0] = s.vibrance;
    c[16][1] = s.temperature;
    c[16][2] = s.tint;
    c[16][3] = s.shadowTint;
    c[17][0] = s.lift;
    c[17][1] = s.gamma;
    c[17][2] = s.gain;
    c[17][3] = s.vignette;
    c[18][0] = s.filmGrain;
    c[18][1] = s.sharpen;
    c[18][2] = s.fxaa ? 1.0f : 0.0f;
    c[18][3] = static_cast<float>(s.debugView);
    // World up in view space (the y column of the rotation), camera height.
    c[19][0] = W[1];
    c[19][1] = W[5];
    c[19][2] = W[9];
    c[19][3] = cam.eye[1];

    c[20][0] = static_cast<float>(s.skyMode);
    c[20][1] = s.skyNight >= 0.0f ? s.skyNight
                                  : (nightSky ? (mapNight || blackHole ? 0.35f : 0.95f) : (ringWorld && !mapNight ? 0.35f : 0.0f));
    c[20][2] = s.skyRotation * kPi / 180.0f;
    c[20][3] = s.skyBrightness;
    c[21][0] = s.cloudAmount;
    c[21][1] = s.starAmount;
    c[21][2] = s.skyEffectSize;
    c[21][3] = s.planetSize;

    static const float kAoSamples[] = {6.0f, 10.0f, 16.0f};
    static const float kShadowSteps[] = {8.0f, 12.0f, 16.0f};
    static const float kCloudSteps[] = {12.0f, 16.0f, 22.0f};
    static const float kLongShadowSteps[] = {12.0f, 18.0f, 24.0f};
    const int q = std::clamp(s.quality, 0, 2);
    c[22][0] = kAoSamples[q];
    c[22][1] = kShadowSteps[q];
    c[22][2] = kCloudSteps[q];
    c[22][3] = kLongShadowSteps[q];

    c[23][0] = s.wetness;
    c[23][1] = s.rain;
    c[23][2] = s.puddles;
    c[23][3] = s.waterSurfaces;
    c[24][0] = s.motionBlur;
    c[24][1] = s.depthOfField;
    c[24][2] = s.focusDistance;
    c[24][3] = s.bokehSize;
    c[25][0] = s.grassDetail;
    c[25][1] = s.mowingStripes;
    c[25][2] = s.wind;
    c[25][3] = s.reflections;
    c[26][0] = s.volumetricClouds;
    c[26][1] = s.cloudCoverage;
    c[26][2] = s.cloudHeight;
    c[26][3] = fminf(fmaxf(s.cloudHeight * 0.6f, 400.0f), 1500.0f);
    c[27][0] = static_cast<float>(s.planetType);
    c[27][1] = static_cast<float>(s.planetView);
    c[27][2] = s.planetAzimuth * kPi / 180.0f;
    c[27][3] = s.planetElevation * kPi / 180.0f;

    // Previous camera (reprojection): world -> previous view rows, previous projection.
    const float* pw = m_havePrevious ? m_prevW2C : W;
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 4; k++) c[28 + r][k] = pw[r * 4 + k];
    if (m_havePrevious) memcpy(c[31], m_prevProj, sizeof(m_prevProj));
    else {
        c[31][0] = P[0];
        c[31][1] = P[5];
        c[31][2] = P[2];
        c[31][3] = P[6];
    }

    const auto has = [&](int t) { return frame.engine && frame.engine[t] ? 1.0f : 0.0f; };
    c[32][0] = has(kTexNormal);
    c[32][1] = has(kTexDiffuse);
    c[32][2] = has(kTexLightMask);
    c[32][3] = has(kTexMotion);
    c[33][0] = cam.sunDirectionKnown ? (mapNight ? -1.0f : 1.0f) : 0.0f;
    c[33][1] = has(kTexSpecular);
    c[33][3] = s.cleanHighlights ? 1.0f : 0.0f; // the sun's glint goes with the sun under night skies
    c[34][0] = s.taa ? 1.0f : 0.0f;
    c[34][1] = m_temporalValid ? 1.0f : 0.0f;
    c[34][2] = static_cast<float>(m_frame % 64);
    for (int i = 0; i < 3; i++) c[35][i] = cam.ambient[i];
    c[36][0] = s.neonLight;
    c[36][1] = mapNight ? -1.0f : (cam.sunDirectionKnown ? 1.0f : 0.0f);
    c[36][2] = tmshaders::weather::lightningFlash(static_cast<float>(frame.time), s.lightning);
    // The bolt of the current strike: its direction is picked once, near where the camera looks
    // at that moment (a bolt behind you is a wasted strike), and then stays put.
    c[36][3] = -1.0f;
    tmshaders::weather::Strike strike;
    if (tmshaders::weather::currentStrike(static_cast<float>(frame.time), s.lightning, strike)) {
        if (strike.slot != m_boltSlot) {
            m_boltSlot = strike.slot;
            m_boltAzimuth = atan2f(W[8], W[10]) + strike.side * 1.6f; // view forward (world x, z)
            m_boltAzimuth = fmodf(m_boltAzimuth + 6.2831853f * 2.0f, 6.2831853f);
        }
        // Packed: azimuth (0..2pi) + 7 * distance step (0..50).
        c[36][3] = m_boltAzimuth + 7.0f * floorf(strike.distance * 50.0f + 0.5f);
    }
    c[37][0] = s.volumetricLight;
    c[37][1] = 160.0f;
    c[37][2] = s.globalIllumination;
    c[37][3] = s.lensDrops ? fminf(s.rain, 1.0f) : 0.0f; // drops on the lens, only while it rains
    c[38][3] = frame.photo ? 3.0f : 1.0f;
    const float drift[2] = {1.0f + s.wind * 7.0f, 0.5f + s.wind * 3.0f};
    const float driftLength = sqrtf(drift[0] * drift[0] + drift[1] * drift[1]);
    c[39][0] = s.snow;
    c[39][1] = s.snowCover;
    c[39][2] = drift[0] / driftLength;
    c[39][3] = drift[1] / driftLength;
    c[41][0] = s.auroraSpeed;
    c[41][1] = s.lensDrops ? fminf(s.snow, 1.5f) : 0.0f;
    c[41][2] = s.reflectionBlur;
    c[41][3] = 1.0f / fmaxf(s.hazeDistance, 0.1f);
    // How much of the engine's sun comes out: every night sky over a day map (not on night maps,
    // whose "sun" is the moon already).
    // On night maps the moon stays (EngineMoon 0 and 1: ManiaPlanet's moon can't be moved), EngineMoon 2
    // takes it out too.
    c[43][0] = nightSky && (!mapNight || s.engineMoon == 2) ? 1.0f : 0.0f;
    const float glen = sqrtf(gameSun[0] * gameSun[0] + gameSun[1] * gameSun[1] + gameSun[2] * gameSun[2]);
    for (int i = 0; i < 3; i++) c[43][1 + i] = glen > 1e-5f ? cam.sunDirection[i] / glen : 0.0f;
    // EngineLightDraws: t17 holds the light the engine drew as light (engine_lights.cpp).
    c[44][0] = options::current().engineLightDraws && has(kTexLights) > 0.5f ? 1.0f : 0.0f;
    c[44][1] = s.gameNight ? 1.0f : 0.0f;
    c[44][2] = nightSky ? 1.0f : 0.0f;
    c[44][3] = s.toneKnee;
    // The engine's cloud factor at its darkest (it lights with 2 x a factor between its clouds' min
    // and max): how much of the estimated sun can go under a night sky (scene.hlsl).
    if (cam.hasSceneP) {
        float cmin[4];
        memcpy(cmin, cam.rawP + kScenePCloudsMin, sizeof(cmin));
        const bool ok = std::isfinite(cmin[0]) && std::isfinite(cmin[1]) && std::isfinite(cmin[2]) && cmin[0] > 0.0f && cmin[1] > 0.0f && cmin[2] > 0.0f;
        for (int i = 0; i < 3; i++) c[42][i] = ok ? fminf(cmin[i], 1.0f) : 0.0f;
        c[42][3] = ok ? 1.0f : 0.0f;
    }
    for (int i = 0; i < 3; i++) c[45][i] = cam.sunColour[i];
    c[45][3] = cam.sunDirectionKnown ? 1.0f : 0.0f;
    for (int i = 0; i < 3; i++) c[46][i] = cam.fog[i];
    c[47][0] = 0.0f; // the passes read the engine's HDR image and expose it themselves
    c[47][1] = frame.photo ? 1.0f : 0.0f;
    c[47][2] = m_tmnfScale;

    // BlackHoleMoon (maniaplanet.ini): on a night map under the black hole, the engine's moon (its
    // diffuse light and its glint, blue on chrome and the ring sculptures) is taken out of the
    // image and the black hole lights the scene instead (scene.hlsl), with the engine's own
    // specular model: its constants (SceneP: SpecMaxExp, SpecScale, the light's w) and the
    // material's specular from the G-buffer. Only what the proxy sees; nothing in the game changes.
    const float moonStrength = options::current().blackHoleMoon;
    if (moonStrength > 0.0f && blackHole && mapNight && cam.hasSceneP && cam.sunDirectionKnown && has(kTexSpecular) > 0.5f &&
        has(kTexLightMask) > 0.5f && has(kTexNormal) > 0.5f && c[42][3] > 0.5f) {
        float gen[4], light[4], cmax[4];
        memcpy(gen, cam.rawP + kScenePLightGenSpec, sizeof(gen));
        memcpy(light, cam.rawP + kScenePLightDirRgbLinear0, sizeof(light));
        memcpy(cmax, cam.rawP + kScenePCloudsMax, sizeof(cmax));
        const float specScale = gen[3] * light[3];
        if (std::isfinite(gen[2]) && gen[2] >= 0.0f && gen[2] < 4096.0f && std::isfinite(specScale) && specScale >= 0.0f &&
            std::isfinite(cmax[0]) && std::isfinite(cmax[1]) && std::isfinite(cmax[2])) {
            // The cloud factor varies with the engine's cloud texture (not seen here): halfway
            // between its darkest and brightest, as a luma.
            float mid[3];
            for (int i = 0; i < 3; i++) mid[i] = 0.5f * (c[42][i] + fminf(fmaxf(cmax[i], c[42][i]), 1.0f));
            c[48][0] = moonStrength;
            c[48][1] = gen[2];
            c[48][2] = specScale;
            c[48][3] = 0.2126f * mid[0] + 0.7152f * mid[1] + 0.0722f * mid[2];
            c[43][0] = 0.0f; // the moon is replaced here, not taken out by EngineMoon 2's path as well
            c[33][3] = 0.0f; // nor its glint guessed by CleanHighlights: it is rebuilt exactly here
        }
    }
}

bool Effects::scene(ID3D11DeviceContext* ctx, const SceneFrame& frame, const tmshaders::Settings& s) {
    m_result = nullptr;
    if (!m_lighting || !frame.hdr || !frame.exposure || !frame.engineTone || !frame.camera || !frame.camera->valid || !frame.engine ||
        !frame.engine[kTexDepth] || !wanted(s)) {
        m_havePrevious = false;
        return false;
    }
    ID3D11Resource* res = nullptr;
    frame.hdr->GetResource(&res);
    D3D11_TEXTURE2D_DESC hd = {};
    ID3D11Texture2D* tex = nullptr;
    if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
        tex->GetDesc(&hd);
        tex->Release();
    }
    if (res) res->Release();
    if (!hd.Width || !ensureTargets(hd.Width, hd.Height)) return false;

    // A camera cut (respawn, replay camera change) starts the history over.
    const Camera& cam = *frame.camera;
    m_temporalValid = m_havePrevious;
    if (m_havePrevious) {
        const float dx = cam.eye[0] - (-(m_prevW2C[0] * m_prevW2C[3] + m_prevW2C[4] * m_prevW2C[7] + m_prevW2C[8] * m_prevW2C[11]));
        const float dy = cam.eye[1] - (-(m_prevW2C[1] * m_prevW2C[3] + m_prevW2C[5] * m_prevW2C[7] + m_prevW2C[9] * m_prevW2C[11]));
        const float dz = cam.eye[2] - (-(m_prevW2C[2] * m_prevW2C[3] + m_prevW2C[6] * m_prevW2C[7] + m_prevW2C[10] * m_prevW2C[11]));
        const float forward = cam.worldToCamera[8] * m_prevW2C[8] + cam.worldToCamera[9] * m_prevW2C[9] + cam.worldToCamera[10] * m_prevW2C[10];
        if (dx * dx + dy * dy + dz * dz > 40.0f * 40.0f || forward < 0.8f) m_temporalValid = false;
    }
    buildConstants(frame, s);
    ctx->UpdateSubresource(m_frameCb, 0, nullptr, m_c, 0, 0);
    bindFrame(ctx);
    ID3D11Buffer* engineTone = frame.engineTone;
    ctx->PSSetConstantBuffers(2, 1, &engineTone);
    ID3D11ShaderResourceView* engine[8] = {frame.engine[kTexDepth], frame.engine[kTexNormal], frame.engine[kTexDiffuse],
                                           frame.engine[kTexSpecular], frame.engine[kTexLightMask], frame.engine[kTexMotion],
                                           frame.hdr, frame.exposure};
    memcpy(m_engineViews, engine, sizeof(engine));
    ctx->PSSetShaderResources(8, 8, engine);
    ctx->VSSetShaderResources(8, 8, engine);
    ID3D11ShaderResourceView* engineLights = m_c[44][0] > 0.5f ? frame.engine[kTexLights] : nullptr;
    ctx->PSSetShaderResources(17, 1, &engineLights);

    const float hx = 1.0f / m_ndHalf_.w, hy = 1.0f / m_ndHalf_.h;
    const float qx = 1.0f / m_gi_[0].w, qy = 1.0f / m_gi_[0].h;

    // Half-res normal/depth and colour (two targets at once).
    {
        ID3D11ShaderResourceView* none[8] = {};
        ctx->PSSetShaderResources(0, 8, none);
        ID3D11RenderTargetView* rtvs[2] = {m_ndHalf_.rtv, m_colourHalf.rtv};
        ctx->OMSetRenderTargets(2, rtvs, nullptr);
        D3D11_VIEWPORT vp = {0, 0, static_cast<float>(m_ndHalf_.w), static_cast<float>(m_ndHalf_.h), 0, 1};
        ctx->RSSetViewports(1, &vp);
        setPassConstants(ctx, nullptr, nullptr);
        ctx->PSSetShader(m_ndHalf, nullptr, 0);
        ctx->Draw(3, 0);
        m_profiler.mark(ctx, kDownsampleND);
    }

    // AO and contact shadows, blurred.
    const bool occlusion = s.aoStrength > 0.0f || s.shadowStrength > 0.0f || (s.debugView >= 3 && s.debugView <= 4);
    ID3D11ShaderResourceView* occ = nullptr;
    if (occlusion) {
        run(ctx, kOcclusionShadow, m_occlusion, m_occ[0], &m_ndHalf_.srv, 1);
        ID3D11ShaderResourceView* h[2] = {m_occ[0].srv, m_ndHalf_.srv};
        const float dirH[4] = {hx, 0, 0, 0}, dirV[4] = {0, hy, 0, 0};
        run(ctx, kBilateralBlur, m_blur, m_occ[1], h, 2, dirH);
        ID3D11ShaderResourceView* v[2] = {m_occ[1].srv, m_ndHalf_.srv};
        run(ctx, kBilateralBlur, m_blur, m_occ[0], v, 2, dirV);
        occ = m_occ[0].srv;
    }

    // Bounce light, accumulated over frames.
    ID3D11ShaderResourceView* gi = nullptr;
    if (s.globalIllumination > 0.0f) {
        static const float kGiSamples[] = {6.0f, 8.0f, 12.0f};
        const float p0[4] = {kGiSamples[std::clamp(s.quality, 0, 2)], 4.0f * (frame.photo ? 3.0f : 1.0f), static_cast<float>(m_frame % 64), 0};
        ID3D11ShaderResourceView* in[2] = {m_ndHalf_.srv, m_colourHalf.srv};
        run(ctx, kGI, m_gi, m_gi_[0], in, 2, p0);
        const int prev = m_giIndex, next = 1 - m_giIndex;
        const float t0[4] = {m_temporalValid ? 1.0f : 0.0f, 0, 0, 0};
        ID3D11ShaderResourceView* tin[3] = {m_gi_[0].srv, m_giHistory[prev].srv, m_ndHalf_.srv};
        run(ctx, kGITemporal, m_giTemporal, m_giHistory[next], tin, 3, t0);
        m_giIndex = next;
        gi = m_giHistory[next].srv;
    }

    // Neon light.
    ID3D11ShaderResourceView* spill = nullptr;
    if (s.neonLight > 0.0f) {
        run(ctx, kSpillDown, m_spillDown, m_spill[0], nullptr, 0);
        const float dirH[4] = {qx, 0, 0, 0}, dirV[4] = {0, qy, 0, 0};
        run(ctx, kSpillBlur, m_spillBlur, m_spill[1], &m_spill[0].srv, 1, dirH);
        run(ctx, kSpillBlur, m_spillBlur, m_spill[0], &m_spill[1].srv, 1, dirV);
        // Over frames: found anew in every frame, the light flickered on the walls next to a
        // strip as the car or the camera moved.
        const int prev = m_spillIndex, next = 1 - m_spillIndex;
        const float t0[4] = {m_spillValid && m_temporalValid && m_spillHistoryOn ? 1.0f : 0.0f, 0, 0, 0};
        ID3D11ShaderResourceView* tin[2] = {m_spill[0].srv, m_spillHistory[prev].srv};
        run(ctx, kSpillTemporal, m_spillTemporal, m_spillHistory[next], tin, 2, t0);
        m_spillIndex = next;
        m_spillValid = true;
        spill = m_spillHistory[next].srv;
    } else {
        m_spillValid = false;
    }

    // Volumetric light: the view rays through the engine's own shadow maps. Only with the engine's
    // sun as the light (a night sky or the black hole moves it; its shadow maps don't follow).
    ID3D11ShaderResourceView* volume = nullptr;
    const bool sunIsTheLight = !((s.skyMode >= 2 && s.skyMode != 5) || ((s.skyMode == 3 || s.skyMode == 5) && s.skyEffectSize > 0.0f));
    if (s.volumetricLight > 0.0f && sunIsTheLight && frame.shadow && frame.shadow->valid && frame.shadow->constants &&
        frame.shadow->sampler && frame.engine[kTexShadowDepth] && frame.engine[kTexShadowMap0] && frame.engine[kTexShadowMap1] &&
        m_c[6][3] > 0.5f) {
        static const float kSteps[] = {12.0f, 16.0f, 24.0f};
        ID3D11Buffer* shadowCb = frame.shadow->constants;
        ctx->PSSetConstantBuffers(4, 1, &shadowCb);
        ID3D11SamplerState* cmp = frame.shadow->sampler;
        ctx->PSSetSamplers(4, 1, &cmp);
        const float p0[4] = {kSteps[std::clamp(s.quality, 0, 2)], static_cast<float>(m_frame % 64), 0, 0};
        const float p1[4] = {frame.shadow->raster[0], frame.shadow->raster[1], 0, 0};
        ID3D11ShaderResourceView* vin[3] = {frame.engine[kTexShadowDepth], frame.engine[kTexShadowMap0], frame.engine[kTexShadowMap1]};
        run(ctx, kVolumetric, m_volumetric, m_volume[0], vin, 3, p0, p1);
        ID3D11Buffer* noCb = nullptr;
        ctx->PSSetConstantBuffers(4, 1, &noCb);
        ID3D11ShaderResourceView* h[2] = {m_volume[0].srv, m_ndHalf_.srv};
        const float dirH[4] = {hx, 0, 0, 0}, dirV[4] = {0, hy, 0, 0};
        run(ctx, kVolumetric, m_blur, m_volume[1], h, 2, dirH);
        ID3D11ShaderResourceView* v[2] = {m_volume[1].srv, m_ndHalf_.srv};
        run(ctx, kVolumetric, m_blur, m_volume[0], v, 2, dirV);
        volume = m_volume[0].srv;
    }

    // The sky (custom skies, its average, clouds) and the reflections the composite reads.
    const SkyStage::Output sky = m_sky.run(ctx, *this, frame, s);
    bindFrame(ctx);
    ctx->PSSetConstantBuffers(2, 1, &engineTone);
    ctx->PSSetShaderResources(8, 8, engine);
    ID3D11ShaderResourceView* reflections = m_weather.reflections(ctx, *this, frame, s);
    bindFrame(ctx);
    ctx->PSSetConstantBuffers(2, 1, &engineTone);
    ctx->PSSetShaderResources(8, 8, engine);

    // The composite. Missing inputs read 0: no AO pass means both strengths are 0, so the shader's
    // lerp(1, occ, strength) stays 1; the sky inputs are flagged in u_Pass0.
    ID3D11ShaderResourceView* in[8] = {occ, m_ndHalf_.srv, gi, sky.sky, sky.average, sky.clouds, spill, reflections};
    const float p0[4] = {sky.sky ? 1.0f : 0.0f, sky.average ? 1.0f : 0.0f, sky.clouds ? 1.0f : 0.0f, reflections ? 1.0f : 0.0f};
    m_currentHdr = 0;
    ctx->PSSetShaderResources(16, 1, &volume);
    ctx->PSSetShaderResources(17, 1, &engineLights);
    const float p1[4] = {volume ? 1.0f : 0.0f, 0, 0, 0};
    // BlackHoleMoon acting (c48.x): the variant with the moon's relight (the plain one stays as it was).
    ID3D11PixelShader* lighting = m_c[48][0] > 0.0f && m_lightingMoon ? m_lightingMoon : m_lighting;
    run(ctx, kLighting, lighting, m_hdr[0], in, 8, p0, p1);
    ID3D11ShaderResourceView* noVolume[2] = {};
    ctx->PSSetShaderResources(16, 2, noVolume);

    // On the HDR image: cinema (depth of field, motion blur, TAA), then the weather's particles and
    // lens drops (after TAA as in TMNF: it would smear the streaks), then the sun's light shafts and
    // lens flare.
    m_cinema.run(ctx, *this, frame, s);
    bindFrame(ctx);
    ctx->PSSetConstantBuffers(2, 1, &engineTone);
    ctx->PSSetShaderResources(8, 8, engine);
    if (m_sound) m_weather.tick(s, frame.time);
    m_weather.draw(ctx, *this, frame, s);
    bindFrame(ctx);
    m_trail.draw(ctx, *this, frame, s);
    bindFrame(ctx);
    ctx->PSSetConstantBuffers(2, 1, &engineTone);
    ctx->PSSetShaderResources(8, 8, engine);
    m_sky.post(ctx, *this, frame, s);

    ID3D11ShaderResourceView* none[16] = {};
    ctx->PSSetShaderResources(0, 16, none);
    ctx->VSSetShaderResources(0, 16, none);
    m_result = current();

    memcpy(m_prevW2C, cam.worldToCamera, sizeof(m_prevW2C));
    m_prevProj[0] = m_c[1][0];
    m_prevProj[1] = m_c[1][1];
    m_prevProj[2] = m_c[1][2];
    m_prevProj[3] = m_c[1][3];
    m_havePrevious = true;
    m_frame++;
    m_lastTime = frame.time;
    return true;
}

} // namespace tmvs_mp
