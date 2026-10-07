#include "pipeline.h"
#include "weather.h"
#include "log.h"
#include "config.h"
#include "noise.h"
#include "tmvs_bytecode.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace tmshaders {
namespace {

const char* const kEntryPoints[] = {
    "PS_LinearDepth", "PS_Prepare",  "PS_DownsampleND", "PS_HeightMerge", "PS_HeightSplat", "PS_OcclusionShadow", "PS_BilateralBlur",
    "PS_ShadowHeight", "PS_Volumetric", "PS_GI", "PS_GITemporal",
    "PS_SkyClear",    "PS_SkyStars", "PS_SkyBlackHole", "PS_SkyAurora",   "PS_SkyRing",     "PS_AuroraHalf",
    "PS_SkyAverage",  "PS_Clouds",     "PS_Reflect",     "PS_SpillDown",   "PS_SpillBlur",       "PS_Lighting",
    "PS_RainDrop",    "PS_RainSplash", "PS_Spray", "PS_TrailPoint", "PS_Trail", "PS_SnowFlake",
    "PS_Focus",       "PS_DofBlur",  "PS_Cinematic",    "PS_RayMask",     "PS_RayBlur",     "PS_BloomDown",       "PS_BloomUp",
    "PS_Luminance",   "PS_Adapt",    "PS_Final",        "PS_FXAA",        "PS_TAA",         "PS_Sharpen",         "PS_Copy",
    "PS_CopyDepth",
};

bool readFile(const std::wstring& path, std::string& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(size));
    size_t read = fread(&out[0], 1, out.size(), f);
    fclose(f);
    return read == out.size();
}

float smoothstepf(float a, float b, float x) {
    float t = (x - a) / (b - a);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

bool Pipeline::init(IDirect3DDevice9* device, const std::wstring& shaderDir) {
    m_shaderDir = shaderDir;
    if (!m_quad.init(device)) return false;
    if (!compileAll(device)) return false;
    if (!m_stateBlock) device->CreateStateBlock(D3DSBT_ALL, &m_stateBlock);
    m_ready = true;
    m_historyValid = false;
    return true;
}

const char* Pipeline::passName(int pass) {
    return pass >= 0 && pass < kPassCount ? kEntryPoints[pass] + 3 : "?";
}

void Pipeline::profileBegin(IDirect3DDevice9* device) {
    m_currentProfile = nullptr;
    if (!m_profiling) return;
    ProfileFrame& frame = m_profile[m_profileIndex];
    m_profileIndex = (m_profileIndex + 1) % kProfileFrames;
    if (frame.pending && !profileResolve(frame, false)) return; // GPU still busy: skip this frame
    if (!frame.disjoint) {
        if (FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &frame.disjoint))) return;
        device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &frame.frequency);
        for (auto& stamp : frame.stamps) device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &stamp);
    }
    if (!frame.frequency || !frame.stamps[ProfileFrame::kMaxStamps - 1]) return;
    frame.count = 0;
    frame.disjoint->Issue(D3DISSUE_BEGIN);
    m_currentProfile = &frame;
    profileMark(-1);
}

void Pipeline::profileMark(int pass) {
    ProfileFrame* frame = m_currentProfile;
    if (!frame || frame->count >= ProfileFrame::kMaxStamps) return;
    frame->pass[frame->count] = pass;
    frame->stamps[frame->count++]->Issue(D3DISSUE_END);
}

void Pipeline::profileEnd() {
    ProfileFrame* frame = m_currentProfile;
    if (!frame) return;
    frame->disjoint->Issue(D3DISSUE_END);
    frame->frequency->Issue(D3DISSUE_END);
    frame->pending = true;
    m_currentProfile = nullptr;
}

bool Pipeline::profileResolve(ProfileFrame& frame, bool wait) {
    const DWORD flags = wait ? D3DGETDATA_FLUSH : 0;
    BOOL disjoint = TRUE;
    UINT64 frequency = 0;
    auto get = [&](IDirect3DQuery9* query, void* data, DWORD size) {
        HRESULT hr;
        while ((hr = query->GetData(data, size, flags)) == S_FALSE && wait) {
        }
        return hr == S_OK;
    };
    if (!get(frame.disjoint, &disjoint, sizeof(disjoint)) || !get(frame.frequency, &frequency, sizeof(frequency))) return false;
    UINT64 times[ProfileFrame::kMaxStamps] = {};
    for (int i = 0; i < frame.count; i++) {
        if (!get(frame.stamps[i], &times[i], sizeof(UINT64))) return false;
    }
    frame.pending = false;
    if (disjoint || frequency == 0 || frame.count < 2) return true;
    float ms[kPassCount] = {};
    for (int i = 1; i < frame.count; i++) {
        if (frame.pass[i] >= 0) ms[frame.pass[i]] += static_cast<float>(static_cast<double>(times[i] - times[i - 1]) * 1000.0 / frequency);
    }
    const float total = static_cast<float>(static_cast<double>(times[frame.count - 1] - times[0]) * 1000.0 / frequency);
    const float k = m_totalMs > 0.0f ? 0.1f : 1.0f;
    for (int p = 0; p < kPassCount; p++) m_passMs[p] += (ms[p] - m_passMs[p]) * k;
    m_totalMs += (total - m_totalMs) * k;
    return true;
}

void Pipeline::collectProfile(bool wait) {
    for (auto& frame : m_profile) {
        if (frame.pending) profileResolve(frame, wait);
    }
}

void Pipeline::releaseProfile() {
    for (auto& frame : m_profile) {
        gfx::release(frame.disjoint);
        gfx::release(frame.frequency);
        for (auto& stamp : frame.stamps) gfx::release(stamp);
        frame.pending = false;
    }
    m_currentProfile = nullptr;
}

void Pipeline::release() {
    m_ready = false;
    releaseProfile();
    for (auto& shader : m_shaders) gfx::release(shader);
    gfx::release(m_stateBlock);
    gfx::release(m_splatVS);
    gfx::release(m_splatDecl);
    gfx::release(m_dropVS);
    gfx::release(m_splashVS);
    gfx::release(m_sprayVS);
    gfx::release(m_snowVS);
    gfx::release(m_rainDecl);
    gfx::release(m_trailVS);
    gfx::release(m_trailDecl);
    m_quad.destroy();
    destroyTargets();
}

bool Pipeline::reloadShaders(IDirect3DDevice9* device) {
    if (!m_ready) return false;
    bool ok = compileAll(device);
    TMVS_LOG("pipeline: shader reload %s", ok ? "succeeded" : "FAILED (keeping previous shaders)");
    return ok;
}

bool Pipeline::compileAll(IDirect3DDevice9* device) {
    static_assert(sizeof(kPrecompiledShaders) / sizeof(kPrecompiledShaders[0]) == kPassCount,
                  "CMakeLists TMVS_ENTRIES must match kEntryPoints");
    // Development: compile tmvs.hlsl from disk (hot reload). Otherwise use the bytecode
    // fxc produced at build time.
    std::string source;
    const bool fromDisk = !m_shaderDir.empty() && readFile(m_shaderDir + L"\\tmvs.hlsl", source);

    IDirect3DPixelShader9* compiled[kPassCount] = {};
    m_lastError.clear();
    bool ok = true;
    for (int i = 0; i < kPassCount; i++) {
        if (fromDisk) {
            compiled[i] = gfx::compilePixelShader(device, source, kEntryPoints[i], "tmvs.hlsl", &m_lastError);
        } else {
            device->CreatePixelShader(reinterpret_cast<const DWORD*>(kPrecompiledShaders[i].code), &compiled[i]);
        }
        if (!compiled[i]) ok = false;
    }
    if (!ok) {
        for (auto& shader : compiled) gfx::release(shader);
        if (m_lastError.empty()) m_lastError = "CreatePixelShader failed";
        TMVS_LOG("pipeline: shader creation failed");
        return false;
    }
    for (int i = 0; i < kPassCount; i++) {
        gfx::release(m_shaders[i]);
        m_shaders[i] = compiled[i];
    }
    TMVS_LOG("pipeline: %d passes ready (%s)", kPassCount, fromDisk ? "compiled from disk" : "precompiled");
    return true;
}

bool Pipeline::ensureTargets(IDirect3DDevice9* device, UINT width, UINT height) {
    if (m_width == width && m_height == height && m_hdr.valid()) return true;
    destroyTargets();
    const UINT hw = (width + 1) / 2, hh = (height + 1) / 2;
    bool ok = true;
    ok &= m_linearDepth.create(device, width, height, D3DFMT_R32F);
    ok &= m_nd.create(device, width, height, D3DFMT_A16B16G16R16F);
    ok &= m_ndHalf.create(device, hw, hh, D3DFMT_A16B16G16R16F);
    ok &= m_occlusion.create(device, hw, hh, D3DFMT_A8R8G8B8);
    ok &= m_occlusionTmp.create(device, hw, hh, D3DFMT_A8R8G8B8);
    ok &= m_skyAverage.create(device, 1, 1, D3DFMT_A16B16G16R16F);
    ok &= m_sky.create(device, width, height, D3DFMT_A16B16G16R16F);
    ok &= m_gi[0].create(device, (width + 3) / 4, (height + 3) / 4, D3DFMT_A16B16G16R16F);
    ok &= m_gi[1].create(device, (width + 3) / 4, (height + 3) / 4, D3DFMT_A16B16G16R16F);
    for (auto& t : m_giHistory) ok &= t.create(device, (width + 3) / 4, (height + 3) / 4, D3DFMT_A16B16G16R16F);
    m_giValid = false;
    ok &= m_hdr.create(device, width, height, D3DFMT_A16B16G16R16F);
    ok &= m_rays[0].create(device, hw, hh, D3DFMT_A16B16G16R16F);
    ok &= m_rays[1].create(device, hw, hh, D3DFMT_A16B16G16R16F);
    UINT bw = width, bh = height;
    for (int i = 0; i < kBloomLevels; i++) {
        bw = bw > 3 ? (bw + 1) / 2 : 2;
        bh = bh > 3 ? (bh + 1) / 2 : 2;
        ok &= m_bloomDown[i].create(device, bw, bh, D3DFMT_A16B16G16R16F);
        if (i < kBloomLevels - 1) ok &= m_bloomUp[i].create(device, bw, bh, D3DFMT_A16B16G16R16F);
    }
    ok &= m_luminance.create(device, 1, 1, D3DFMT_R32F);
    ok &= m_adapted[0].create(device, 1, 1, D3DFMT_R32F);
    ok &= m_adapted[1].create(device, 1, 1, D3DFMT_R32F);
    ok &= m_ldr.create(device, width, height, D3DFMT_A8R8G8B8);
    ok &= m_ldr2.create(device, width, height, D3DFMT_A8R8G8B8);
    // Optional effects: small or cheap enough to keep around.
    ok &= m_clouds.create(device, hw, hh, D3DFMT_A16B16G16R16F);
    ok &= m_reflect.create(device, hw, hh, D3DFMT_A16B16G16R16F);
    ok &= m_spill[0].create(device, (width + 3) / 4, (height + 3) / 4, D3DFMT_A16B16G16R16F);
    ok &= m_spill[1].create(device, (width + 3) / 4, (height + 3) / 4, D3DFMT_A16B16G16R16F);
    ok &= m_focus[0].create(device, 1, 1, D3DFMT_R32F);
    ok &= m_focus[1].create(device, 1, 1, D3DFMT_R32F);
    ok &= m_dof.create(device, hw, hh, D3DFMT_A16B16G16R16F);
    if (!ok) {
        destroyTargets();
        return false;
    }
    m_width = width;
    m_height = height;
    m_historyValid = false;
    m_taaValid = false;
    m_heightValid = false;
    TMVS_LOG("pipeline: render targets created for %ux%u", width, height);
    return true;
}

void Pipeline::destroyTargets() {
    m_linearDepth.destroy();
    m_nd.destroy();
    m_ndHalf.destroy();
    m_occlusion.destroy();
    m_occlusionTmp.destroy();
    m_skyAverage.destroy();
    m_sky.destroy();
    for (auto& t : m_gi) t.destroy();
    for (auto& t : m_giHistory) t.destroy();
    m_hdr.destroy();
    for (auto& t : m_rays) t.destroy();
    for (auto& t : m_bloomDown) t.destroy();
    for (auto& t : m_bloomUp) t.destroy();
    m_luminance.destroy();
    for (auto& t : m_adapted) t.destroy();
    m_ldr.destroy();
    m_ldr2.destroy();
    m_depthCopy.destroy();
    m_clouds.destroy();
    m_reflect.destroy();
    for (auto& t : m_spill) t.destroy();
    for (auto& t : m_focus) t.destroy();
    m_dof.destroy();
    m_hdr2.destroy();
    for (auto& t : m_taa) t.destroy();
    m_heightFrame.destroy();
    for (auto& t : m_heightMap) t.destroy();
    m_shadowHeight.destroy();
    gfx::release(m_heightDepth);
    gfx::release(m_splatPoints);
    gfx::release(m_cloudNoise);
    gfx::release(m_rainVB);
    gfx::release(m_rainIB);
    gfx::release(m_trailVB);
    gfx::release(m_trailIB);
    m_trailPoints.destroy();
    m_trailReset = true;
    m_taaValid = false;
    m_heightValid = false;
    m_width = m_height = 0;
}

void Pipeline::beginStateSave(IDirect3DDevice9*) {
    if (m_stateBlock) m_stateBlock->Capture();
}

void Pipeline::endStateSave(IDirect3DDevice9*) {
    if (m_stateBlock) m_stateBlock->Apply();
}

void Pipeline::bind(IDirect3DDevice9* device, int stage, IDirect3DBaseTexture9* texture, bool linear) {
    device->SetTexture(stage, texture);
    const DWORD filter = linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    device->SetSamplerState(stage, D3DSAMP_MINFILTER, filter);
    device->SetSamplerState(stage, D3DSAMP_MAGFILTER, filter);
    device->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
    device->SetSamplerState(stage, D3DSAMP_MAXMIPLEVEL, 0);
}

void Pipeline::passConstants(IDirect3DDevice9* device, float a, float b, float c, float d) {
    const float v[4] = {a, b, c, d};
    device->SetPixelShaderConstantF(32, v, 1);
}

void Pipeline::runPass(IDirect3DDevice9* device, Pass pass, gfx::Target& target) {
    runPass(device, pass, target.surface, target.width, target.height);
}

void Pipeline::runPass(IDirect3DDevice9* device, Pass pass, IDirect3DSurface9* target, UINT width, UINT height) {
    device->SetRenderTarget(0, target);
    device->SetPixelShader(m_shaders[pass]);
    m_quad.draw(device, width, height);
    profileMark(pass);
}

void Pipeline::setFrameConstants(IDirect3DDevice9* device, const Inputs& in, const Settings& s) {
    const float* V = in.view;
    const float* P = in.projection;
    float c[37][4] = {};

    c[0][0] = static_cast<float>(in.width);
    c[0][1] = static_cast<float>(in.height);
    c[0][2] = 1.0f / static_cast<float>(in.width);
    c[0][3] = 1.0f / static_cast<float>(in.height);

    c[1][0] = P[0];
    c[1][1] = P[5];
    c[1][2] = P[8];
    c[1][3] = P[9];
    c[2][0] = P[10];
    c[2][1] = P[14];
    c[2][2] = in.time;
    c[2][3] = static_cast<float>(m_frame % 1024);

    // View->world rotation is the transpose of the view matrix's 3x3; camera = -t * R^T.
    float camera[3];
    for (int j = 0; j < 3; j++) camera[j] = -(V[12] * V[j * 4 + 0] + V[13] * V[j * 4 + 1] + V[14] * V[j * 4 + 2]);
    for (int i = 0; i < 3; i++) {
        c[3 + i][0] = V[0 * 4 + i];
        c[3 + i][1] = V[1 * 4 + i];
        c[3 + i][2] = V[2 * 4 + i];
        c[3 + i][3] = camera[i];
    }

    // Sun: world -> view is the row-vector product with the view rotation.
    float sw[3] = {in.sunDirection[0], in.sunDirection[1], in.sunDirection[2]};
    float len = sqrtf(sw[0] * sw[0] + sw[1] * sw[1] + sw[2] * sw[2]);
    if (len > 1e-5f) for (float& v : sw) v /= len;
    float sv[3];
    for (int j = 0; j < 3; j++) sv[j] = sw[0] * V[0 * 4 + j] + sw[1] * V[1 * 4 + j] + sw[2] * V[2 * 4 + j];
    // Night skies replace the sun: no sun light, shadows or shafts. On night maps the game's
    // "sun" is the moon: soft light, no shafts or flare.
    const bool ringWorld = s.skyMode == 5; // space sky, but lit by the game's sun
    const bool nightSky = s.skyMode >= 2 && !ringWorld;
    const bool gameNight = in.sunColorKnown && classifyMood(in.sunColor) == Mood::Night;
    const bool sunKnown = in.sunKnown && len > 1e-5f && !nightSky;
    const float daylight = smoothstepf(-0.08f, 0.18f, sw[1]) * (gameNight ? 0.35f : 1.0f);
    c[6][0] = sv[0];
    c[6][1] = sv[1];
    c[6][2] = sv[2];
    c[6][3] = sunKnown ? 1.0f : 0.0f;
    c[7][0] = sw[0];
    c[7][1] = sw[1];
    c[7][2] = sw[2];
    c[7][3] = sunKnown ? daylight : 1.0f;
    m_sunKnown = sunKnown;

    // Sun position on screen.
    float sunFade = 0.0f, inFront = 0.0f, su = 0.5f, sv2 = -1.0f;
    if (sunKnown && sv[2] > 0.05f) {
        float ndcX = sv[0] * P[0] / sv[2] + P[8];
        float ndcY = sv[1] * P[5] / sv[2] + P[9];
        su = ndcX * 0.5f + 0.5f;
        sv2 = 0.5f - ndcY * 0.5f;
        float edge = fminf(fminf(su + 0.3f, 1.3f - su), fminf(sv2 + 0.3f, 1.3f - sv2));
        sunFade = smoothstepf(0.0f, 0.3f, edge) * smoothstepf(0.05f, 0.3f, sv[2]) * (gameNight ? 0.0f : daylight);
        inFront = 1.0f;
    }
    c[8][0] = su;
    c[8][1] = sv2;
    c[8][2] = sunFade;
    m_sunFade = sunFade;
    c[8][3] = inFront;

    // Sun colour: the preset's colour blended with the game's light (carries the time of day).
    float gameSun[3] = {in.sunColor[0], in.sunColor[1], in.sunColor[2]};
    const float gameLuma = 0.2126f * gameSun[0] + 0.7152f * gameSun[1] + 0.0722f * gameSun[2];
    const float presetLuma = 0.2126f * s.sunColor[0] + 0.7152f * s.sunColor[1] + 0.0722f * s.sunColor[2];
    const float w = (in.sunColorKnown && gameLuma > 1e-3f) ? s.gameSunColor : 0.0f;
    for (int i = 0; i < 3; i++) {
        const float game = gameLuma > 1e-3f ? gameSun[i] * presetLuma / gameLuma : s.sunColor[i];
        c[9][i] = s.sunColor[i] + (game - s.sunColor[i]) * w;
    }
    c[9][3] = s.sunLight;
    for (int i = 0; i < 3; i++) c[10][i] = s.skyColor[i];
    c[10][3] = s.ambientTint;

    c[11][0] = s.aoStrength;
    c[11][1] = s.aoRadius;
    c[11][2] = s.shadowStrength;
    c[11][3] = s.shadowLength;
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
    c[15][1] = s.autoExposure * (nightSky || gameNight ? 0.3f : 1.0f); // night stays dark
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
    c[19][0] = V[4];
    c[19][1] = V[5];
    c[19][2] = V[6];
    c[19][3] = camera[1];

    c[20][0] = static_cast<float>(s.skyMode);
    // Day-for-night only when the map itself isn't already a night map.
    c[20][1] = s.skyNight >= 0.0f ? s.skyNight : (nightSky ? (gameNight ? 0.35f : 0.95f) : (ringWorld && !gameNight ? 0.35f : 0.0f));
    c[20][2] = s.skyRotation * 3.14159265f / 180.0f;
    c[20][3] = s.skyBrightness;
    c[21][0] = s.cloudAmount;
    c[21][1] = s.starAmount;
    c[21][2] = s.skyEffectSize;
    c[21][3] = s.planetSize;

    static const float kAoSamples[] = {6.0f, 10.0f, 16.0f};
    static const float kShadowSteps[] = {10.0f, 16.0f, 24.0f};
    static const float kCloudSteps[] = {12.0f, 16.0f, 22.0f};
    static const float kLongShadowSteps[] = {12.0f, 18.0f, 24.0f};
    const int q = s.quality < 0 ? 0 : (s.quality > 2 ? 2 : s.quality);
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
    c[27][2] = s.planetAzimuth * 3.14159265f / 180.0f;
    c[27][3] = s.planetElevation * 3.14159265f / 180.0f;

    // Previous camera: world -> previous view columns, previous projection.
    const float* W = m_havePrevious ? m_prevView : V;
    const float* Q = m_havePrevious ? m_prevProjection : P;
    for (int j = 0; j < 3; j++) {
        c[28 + j][0] = W[0 * 4 + j];
        c[28 + j][1] = W[1 * 4 + j];
        c[28 + j][2] = W[2 * 4 + j];
        c[28 + j][3] = W[12 + j];
    }
    c[31][0] = Q[0];
    c[31][1] = Q[5];
    c[31][2] = Q[8];
    c[31][3] = Q[9];

    c[35][0] = m_heightOrigin[0];
    c[35][1] = m_heightOrigin[1];
    c[35][2] = kHeightMapWorld;
    c[35][3] = s.longShadowRange;
    c[36][0] = s.neonLight;
    c[36][1] = in.sunKnown && len > 1e-5f ? 1.0f : 0.0f; // the game's sun, even under a night sky
    c[36][2] = weather::lightningFlash(in.time, s.lightning);
    // The bolt of the current strike: its direction is picked once, near where the camera
    // looks at that moment (a bolt behind you is a wasted strike), and then stays put.
    weather::Strike strike;
    c[36][3] = -1.0f;
    if (weather::currentStrike(in.time, s.lightning, strike)) {
        if (strike.slot != m_boltSlot) {
            m_boltSlot = strike.slot;
            m_boltAzimuth = atan2f(V[2], V[10]) + strike.side * 1.6f; // view forward (world x, z)
            m_boltAzimuth = fmodf(m_boltAzimuth + 6.2831853f * 2.0f, 6.2831853f);
        }
        // Packed: azimuth (0..2pi) + 7 * distance step (0..50).
        c[36][3] = m_boltAzimuth + 7.0f * floorf(strike.distance * 50.0f + 0.5f);
    }

    device->SetPixelShaderConstantF(0, &c[0][0], 32);
    device->SetPixelShaderConstantF(35, &c[35][0], 2);
    const float water[4] = {in.water[0], in.water[1], in.water[2], 0.0f};
    device->SetPixelShaderConstantF(38, water, 1);
    const float snow[4] = {s.snow, s.snowCover, 0.0f, 0.0f};
    device->SetPixelShaderConstantF(39, snow, 1);
}

// --- Long-range shadows -----------------------------------------------------

namespace {

// Splats every 5th depth pixel into a top-down height map: one point per sample, placed
// at its world x/z, carrying its world height. The depth test keeps the highest one.
const char* kSplatVS = R"(
float4 c_Proj : register(c1);   // P00, P11, P20, P21
float4 c_V0   : register(c2);   // view -> world rows, camera position in w
float4 c_V1   : register(c3);
float4 c_V2   : register(c4);
float4 c_Map  : register(c5);   // map corner x, z, 1 / world size, camera height
float4 c_Half : register(c6);   // half-texel offset of the map
sampler2D s_depth : register(s0);
struct VSOut { float4 pos : POSITION; float2 data : TEXCOORD0; };
VSOut main(float2 uv : TEXCOORD0) {
    VSOut o;
    float z = tex2Dlod(s_depth, float4(uv, 0, 0)).r;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float3 v = float3((ndc.x - c_Proj.z) * z / c_Proj.x, (ndc.y - c_Proj.w) * z / c_Proj.y, z);
    float3 w = float3(c_V0.w, c_V1.w, c_V2.w) + v.x * c_V0.xyz + v.y * c_V1.xyz + v.z * c_V2.xyz;
    float2 m = (w.xz - c_Map.xy) * c_Map.z;
    // Beyond ~400 m the depth is too coarse to be useful.
    bool valid = z > 0.3 && z < 400.0 && all(m > 0.0) && all(m < 1.0);
    // The player's car (close, in the middle of the lower screen) must not cast long
    // shadows: it moves on and would leave stale blotches behind and below it.
    if (z < 16.0 && uv.x > 0.25 && uv.x < 0.75 && uv.y > 0.35) valid = false;
    o.pos = valid ? float4(m.x * 2.0 - 1.0 + c_Half.x, 1.0 - m.y * 2.0 + c_Half.y, saturate(0.5 - (w.y - c_Map.w) / 1000.0), 1.0)
                  : float4(-10.0, -10.0, 0.0, 1.0);
    o.data = float2(w.y + 10000.0, 0.0);
    return o;
}
)";

} // namespace

bool Pipeline::ensureHeightMap(IDirect3DDevice9* device) {
    if (!m_heightSupported) return false;
    if (m_heightFrame.valid() && m_heightDepth && m_splatPoints) return true;
    if (!m_splatVS) {
        // Vertex texture fetch of R32F: standard on shader model 3 hardware, but check.
        IDirect3D9* d3d = nullptr;
        device->GetDirect3D(&d3d);
        D3DDEVICE_CREATION_PARAMETERS cp{};
        device->GetCreationParameters(&cp);
        D3DDISPLAYMODE mode{};
        device->GetDisplayMode(0, &mode);
        const bool vtf = d3d && SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format, D3DUSAGE_QUERY_VERTEXTEXTURE,
                                                                  D3DRTYPE_TEXTURE, D3DFMT_R32F));
        if (d3d) d3d->Release();
        if (!vtf) {
            TMVS_LOG("pipeline: no vertex texture fetch for R32F - long-range shadows disabled");
            m_heightSupported = false;
            return false;
        }
        m_splatVS = gfx::compileVertexShader(device, kSplatVS, "main", "SplatVS");
        const D3DVERTEXELEMENT9 elements[] = {
            {0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            D3DDECL_END(),
        };
        device->CreateVertexDeclaration(elements, &m_splatDecl);
        if (!m_splatVS || !m_splatDecl) {
            m_heightSupported = false;
            return false;
        }
    }
    bool ok = m_heightFrame.create(device, kHeightMapSize, kHeightMapSize, D3DFMT_R32F);
    ok &= m_heightMap[0].create(device, kHeightMapSize, kHeightMapSize, D3DFMT_R32F);
    ok &= m_heightMap[1].create(device, kHeightMapSize, kHeightMapSize, D3DFMT_R32F);
    ok &= m_shadowHeight.create(device, kHeightMapSize / 2, kHeightMapSize / 2, D3DFMT_R32F);
    if (ok && !m_heightDepth) {
        ok = SUCCEEDED(device->CreateDepthStencilSurface(kHeightMapSize, kHeightMapSize, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE,
                                                         &m_heightDepth, nullptr));
    }
    if (ok && !m_splatPoints) {
        const UINT cols = m_width / 5, rows = m_linearDepth.height / 5;
        m_splatCount = cols * rows;
        ok = m_splatCount > 0 && SUCCEEDED(device->CreateVertexBuffer(m_splatCount * 8, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &m_splatPoints, nullptr));
        float* data = nullptr;
        if (ok && SUCCEEDED(m_splatPoints->Lock(0, 0, reinterpret_cast<void**>(&data), 0))) {
            for (UINT y = 0; y < rows; y++) {
                for (UINT x = 0; x < cols; x++) {
                    *data++ = (x * 5.0f + 2.5f) / m_width;
                    *data++ = (y * 5.0f + 2.5f) / m_linearDepth.height;
                }
            }
            m_splatPoints->Unlock();
        } else {
            ok = false;
        }
    }
    if (!ok) {
        TMVS_LOG("pipeline: height map resources failed - long-range shadows disabled");
        m_heightSupported = false;
    }
    m_heightValid = false;
    return ok;
}

void Pipeline::updateHeightMap(IDirect3DDevice9* device, const Inputs& in, const Settings& s) {
    const float* V = in.view;
    const float* P = in.projection;
    float camera[3];
    for (int j = 0; j < 3; j++) camera[j] = -(V[12] * V[j * 4 + 0] + V[13] * V[j * 4 + 1] + V[14] * V[j * 4 + 2]);

    // Keep the camera near the middle; move the map in whole texels so history lines up.
    const float texel = kHeightMapWorld / kHeightMapSize;
    const float snap = texel * 16.0f;
    float shift[2] = {0.0f, 0.0f};
    const float cx = m_heightOrigin[0] + kHeightMapWorld * 0.5f, cz = m_heightOrigin[1] + kHeightMapWorld * 0.5f;
    if (!m_heightValid || fabsf(camera[0] - cx) > 40.0f || fabsf(camera[2] - cz) > 40.0f) {
        const float ox = floorf((camera[0] - kHeightMapWorld * 0.5f) / snap) * snap;
        const float oz = floorf((camera[2] - kHeightMapWorld * 0.5f) / snap) * snap;
        shift[0] = (ox - m_heightOrigin[0]) / kHeightMapWorld;
        shift[1] = (oz - m_heightOrigin[1]) / kHeightMapWorld;
        m_heightOrigin[0] = ox;
        m_heightOrigin[1] = oz;
    }

    // 1. Splat this frame's depth.
    IDirect3DSurface9* savedDepth = nullptr;
    device->GetDepthStencilSurface(&savedDepth);
    device->SetRenderTarget(0, m_heightFrame.surface);
    device->SetDepthStencilSurface(m_heightDepth);
    D3DVIEWPORT9 viewport = {0, 0, kHeightMapSize, kHeightMapSize, 0.0f, 1.0f};
    device->SetViewport(&viewport);
    device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
    device->SetRenderState(D3DRS_POINTSPRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_POINTSCALEENABLE, FALSE);
    device->SetTexture(D3DVERTEXTEXTURESAMPLER0, m_linearDepth.texture);
    device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    float vc[6][4] = {};
    vc[0][0] = P[0];
    vc[0][1] = P[5];
    vc[0][2] = P[8];
    vc[0][3] = P[9];
    for (int i = 0; i < 3; i++) {
        vc[1 + i][0] = V[0 * 4 + i];
        vc[1 + i][1] = V[1 * 4 + i];
        vc[1 + i][2] = V[2 * 4 + i];
        vc[1 + i][3] = camera[i];
    }
    vc[4][0] = m_heightOrigin[0];
    vc[4][1] = m_heightOrigin[1];
    vc[4][2] = 1.0f / kHeightMapWorld;
    vc[4][3] = camera[1];
    vc[5][0] = -1.0f / kHeightMapSize;
    vc[5][1] = 1.0f / kHeightMapSize;
    device->SetVertexShaderConstantF(1, &vc[0][0], 6);
    device->SetVertexShader(m_splatVS);
    device->SetVertexDeclaration(m_splatDecl);
    device->SetPixelShader(m_shaders[kHeightSplat]);
    device->SetStreamSource(0, m_splatPoints, 0, 8);
    device->DrawPrimitive(D3DPT_POINTLIST, 0, m_splatCount);
    device->SetStreamSource(0, nullptr, 0, 0);
    device->SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetDepthStencilSurface(savedDepth);
    if (savedDepth) savedDepth->Release();
    profileMark(kHeightSplat);

    // 2. Merge into the persistent map.
    const int previous = m_heightIndex;
    m_heightIndex ^= 1;
    bind(device, 0, m_heightMap[previous].texture, false);
    bind(device, 1, m_heightFrame.texture, false);
    passConstants(device, shift[0], shift[1], m_heightValid ? 0.0f : 1.0f, 0.08f);
    runPass(device, kHeightMerge, m_heightMap[m_heightIndex]);
    m_heightValid = true;
    (void)s;
}

bool Pipeline::ensureCloudNoise(IDirect3DDevice9* device) {
    if (m_cloudNoise) return true;
    const int size = 64;
    const std::vector<uint32_t>& texels = noise::cloudVolume(size);
    IDirect3DVolumeTexture9* staging = nullptr;
    if (FAILED(device->CreateVolumeTexture(size, size, size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &staging, nullptr))) return false;
    D3DLOCKED_BOX box{};
    if (SUCCEEDED(staging->LockBox(0, &box, nullptr, 0))) {
        for (int z = 0; z < size; z++) {
            for (int y = 0; y < size; y++) {
                memcpy(static_cast<BYTE*>(box.pBits) + static_cast<size_t>(z) * box.SlicePitch + static_cast<size_t>(y) * box.RowPitch,
                       &texels[(static_cast<size_t>(z) * size + y) * size], size * 4);
            }
        }
        staging->UnlockBox(0);
    }
    bool ok = SUCCEEDED(device->CreateVolumeTexture(size, size, size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_cloudNoise, nullptr)) &&
              SUCCEEDED(device->UpdateTexture(staging, m_cloudNoise));
    staging->Release();
    if (!ok) gfx::release(m_cloudNoise);
    return ok;
}

// --- Rain particles ---------------------------------------------------------

namespace {

// Drops: a box around the camera that is fixed in the world (wrapped as the camera moves),
// each drop a screen-aligned streak along its motion relative to the camera.
// Splashes: random spots in a box around the camera, placed on the height map and kept
// only where that spot is the visible surface.
const char* kRainVS = R"(
float4 c_V0     : register(c0);  // world -> view, column 0 (rotation, translation)
float4 c_V1     : register(c1);
float4 c_V2     : register(c2);
float4 c_Proj   : register(c3);  // P00, P11, P20, P21
float4 c_Proj2  : register(c4);  // P22, P32, 2 / width, 2 / height
float4 c_Cam    : register(c5);  // camera world xyz, time
float4 c_Streak : register(c6);  // streak (world m), 0
float4 c_Box    : register(c7);  // box size, box height, fall speed, min width (px)
float4 c_Wind   : register(c8);  // wind x, z (m/s), splash rate (1/s), splash box size
float4 c_Map    : register(c9);  // height map corner x, z, 1 / world size, 0
float4 c_Prev0  : register(c10); // world -> previous frame's view, column 0
float4 c_Prev1  : register(c11);
float4 c_Prev2  : register(c12);
float4 c_PrevP  : register(c13); // previous P00, P11, P20, P21
float4 c_Motion : register(c14); // shutter (frames), previous frame valid, frame time (s), max streak (ndc)
float4 c_Spray  : register(c15); // speed (m/s), spray amount, 0, 0
sampler2D s_height : register(s0);
sampler2D s_depth  : register(s1);
struct VSOut { float4 pos : POSITION; float4 data : TEXCOORD0; };

float3 toView(float3 w) { float4 p = float4(w, 1.0); return float3(dot(p, c_V0), dot(p, c_V1), dot(p, c_V2)); }
float4 toClip(float3 v) {
    float4 c = float4(v.x * c_Proj.x + v.z * c_Proj.z, v.y * c_Proj.y + v.z * c_Proj.w, v.z * c_Proj2.x + c_Proj2.y, v.z);
    c.xy += float2(-c_Proj2.z, c_Proj2.w) * 0.5 * c.w; // D3D9 half-pixel offset
    return c;
}

VSOut dropVS(float4 seed : TEXCOORD0, float2 corner : TEXCOORD1) {
    VSOut o;
    o.pos = float4(0, 0, -2, 1);
    o.data = 0;
    float3 box = float3(c_Box.x, c_Box.y, c_Box.x);
    float3 velocity = float3(c_Wind.x, -c_Box.z * (0.85 + 0.3 * seed.w), c_Wind.y);
    float3 local = (frac((seed.xyz * box + velocity * c_Cam.w - c_Cam.xyz) / box) - 0.5) * box;
    float3 w = c_Cam.xyz + local;
    float3 va = toView(w);
    if (va.z < 0.4) return o;
    float4 ca = toClip(va);
    float2 na = ca.xy / ca.w, nb;
    if (c_Motion.y > 0.5) {
        // The streak is the drop's real motion on screen: where it was a frame ago (camera
        // turning and moving included), stretched to the shutter time.
        float4 p = float4(w - velocity * c_Motion.z, 1.0);
        float3 vp = float3(dot(p, c_Prev0), dot(p, c_Prev1), dot(p, c_Prev2));
        if (vp.z < 0.4) return o;
        float2 np = float2(vp.x * c_PrevP.x / vp.z + c_PrevP.z, vp.y * c_PrevP.y / vp.z + c_PrevP.w);
        float2 move = (np - na) * c_Motion.x;
        float l = length(move);
        nb = na + (l > c_Motion.w ? move * (c_Motion.w / l) : move);
    } else {
        float3 vb = toView(w + c_Streak.xyz);
        if (vb.z < 0.4) return o;
        float4 cb = toClip(vb);
        nb = cb.xy / cb.w;
    }
    float2 d = (nb - na) / c_Proj2.zw;
    float len = length(d);
    float2 dir = len > 1e-3 ? d / len : float2(0.0, 1.0);
    if (len < 4.0) nb = na + dir * 4.0 * c_Proj2.zw; // a drop is never a dot
    float z = va.z;
    // Real width, about 2.5 mm; thinner than the minimum means fainter, not wider.
    float widthPx = 0.0025 * c_Proj.y / (c_Proj2.w * z);
    float shown = max(widthPx, c_Box.w);
    float2 n = lerp(na, nb, corner.y) + float2(-dir.y, dir.x) * corner.x * shown * 0.5 * c_Proj2.zw;
    o.pos = float4(n * ca.w, ca.z, ca.w);
    float edge = saturate(2.0 - 4.0 * max(abs(local.x), abs(local.z)) / box.x); // hide the box edges
    // Drops right in front of the lens are out of focus (a sharp one there looks painted on
    // the screen), far ones fade into the rain haze. Some drops catch more light than others.
    float depth = smoothstep(0.7, 2.5, z) * exp(-z / 28.0) * (0.55 + 0.9 * frac(seed.w * 7.31));
    o.data = float4(corner.x, corner.y, z, edge * depth * sqrt(saturate(widthPx / shown)));
    return o;
}

VSOut splashVS(float4 seed : TEXCOORD0, float2 corner : TEXCOORD1) {
    VSOut o;
    o.pos = float4(0, 0, -2, 1);
    o.data = 0;
    float cycle = c_Cam.w * c_Wind.z * (0.8 + 0.4 * seed.w) + seed.z;
    float k = floor(cycle), age = frac(cycle) / 0.3;
    if (age > 1.0) return o;
    float2 r = frac(sin(float2(k * 12.9898 + seed.x * 78.233, k * 39.3468 + seed.y * 11.135)) * 43758.5453);
    // The box sits ahead of the camera, where the ground is in view.
    float size = c_Wind.w;
    float2 ahead = c_Cam.xz + normalize(c_V2.xz + 1e-4) * size * 0.4;
    float2 xz = ahead + (frac((r * size - ahead) / size) - 0.5) * size;
    float2 m = (xz - c_Map.xy) * c_Map.z;
    if (any(m < 0.0) || any(m > 1.0)) return o;
    float h = tex2Dlod(s_height, float4(m, 0, 0)).r;
    if (h <= 0.0) return o;
    float3 v = toView(float3(xz.x, h - 10000.0, xz.y));
    if (v.z < 0.6) return o;
    // Only where this spot is the surface you see (not under a bridge, not on a wall top
    // seen from the side).
    float4 c = toClip(v);
    float2 suv = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
    if (any(suv < 0.0) || any(suv > 1.0)) return o;
    float sceneZ = tex2Dlod(s_depth, float4(suv, 0, 0)).r;
    if (abs(sceneZ - v.z) > 0.25 + v.z * 0.02) return o;
    float s = 0.08 + 0.06 * seed.w;
    float3 up = float3(c_V0.y, c_V1.y, c_V2.y);
    float3 right = normalize(float3(1.0, 0.0, 0.0) - up * up.x);
    o.pos = toClip(v + right * corner.x * s * 1.7 + up * corner.y * s * 1.6);
    o.data = float4(corner.x, corner.y, v.z, age);
    return o;
}

// Snow: a box around the camera that is fixed in the world (wrapped like the rain), each
// flake drifting with the wind and fluttering. Flakes rushing past the camera stretch into
// short streaks (where they were a frame ago); those right at the lens are out of focus.
float4 c_SnowWind : register(c18); // wind x, z (m/s), fall speed (m/s), 0
struct SnowOut { float4 pos : POSITION; float4 data : TEXCOORD0; float len : TEXCOORD1; };

SnowOut snowVS(float4 seed : TEXCOORD0, float2 corner : TEXCOORD1) {
    SnowOut o;
    o.pos = float4(0, 0, -2, 1);
    o.data = 0;
    o.len = 0;
    float3 box = float3(20.0, 14.0, 20.0); // farther snow is the screen-space veil
    float t = c_Cam.w;
    float gust = 0.75 + 0.25 * sin(t * 0.6 + seed.x * 2.0) + 0.15 * sin(t * 1.7 + seed.z * 5.0);
    float3 velocity = float3(c_SnowWind.x * gust, -c_SnowWind.z * (0.7 + 0.6 * seed.w), c_SnowWind.y * gust);
    float3 local = (frac((seed.xyz * box + velocity * t - c_Cam.xyz) / box) - 0.5) * box;
    // Flutter: each flake sways on its own small circle.
    float phase = seed.w * 40.0;
    local.xz += float2(sin(t * (1.3 + seed.x) + phase), cos(t * (1.1 + seed.z) + phase)) * 0.25;
    float3 w = c_Cam.xyz + local;
    float3 va = toView(w);
    if (va.z < 0.25) return o;
    float4 ca = toClip(va);
    float2 na = ca.xy / ca.w;
    float2 nb = na;
    if (c_Motion.y > 0.5) {
        float4 p = float4(w - velocity * c_Motion.z, 1.0);
        float3 vp = float3(dot(p, c_Prev0), dot(p, c_Prev1), dot(p, c_Prev2));
        if (vp.z > 0.25) {
            float2 np = float2(vp.x * c_PrevP.x / vp.z + c_PrevP.z, vp.y * c_PrevP.y / vp.z + c_PrevP.w);
            float2 move = (np - na) * c_Motion.x * 0.5;
            float l = length(move);
            nb = na + (l > c_Motion.w * 0.5 ? move * (c_Motion.w * 0.5 / l) : move);
        }
    }
    float z = va.z;
    // Flakes of 8 - 22 mm (clumps in a blizzard); closer than ~1.5 m they blur into larger,
    // fainter discs.
    float sizeM = 0.008 + 0.014 * frac(seed.w * 13.7);
    float px = sizeM * c_Proj.y / (c_Proj2.w * z);
    float blur = 5.0 * saturate((1.5 - z) / 1.25);
    float r = max(px, 1.2) + blur;
    float opacity = saturate(px / 1.2) * 0.85 + 0.15;
    opacity *= 1.0 / (1.0 + blur * 0.35);
    float2 d = (nb - na) / c_Proj2.zw;      // streak in pixels
    float len = length(d);
    float2 dir = len > 1e-3 ? d / len : float2(0.0, 1.0);
    float lenR = len / r;
    opacity /= 1.0 + lenR * 0.5;             // a long streak is the same flake spread out
    float along = corner.y * (lenR + 2.0) - 1.0;
    float2 n = na + (dir * along + float2(-dir.y, dir.x) * corner.x) * r * c_Proj2.zw;
    o.pos = float4(n * ca.w, ca.z, ca.w);
    float edge = saturate(2.0 - 4.0 * max(abs(local.x), abs(local.z)) / box.x); // hide the box edges
    float far = exp(-z / 14.0);
    o.data = float4(corner.x, along, z, opacity * edge * far * smoothstep(0.25, 0.6, z));
    o.len = lenR;
    return o;
}

// Spray: water thrown up by the rear tyres on a wet road. The car is found in the depth
// buffer where the chase camera keeps it (low in the middle of the screen); every particle
// leaves a tyre, rises, falls and hangs in the air while the car drives on.
float3 viewRay(float2 uv, float z) {
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return float3((ndc.x - c_Proj.z) / c_Proj.x * z, (ndc.y - c_Proj.w) / c_Proj.y * z, z);
}

VSOut sprayVS(float4 seed : TEXCOORD0, float2 corner : TEXCOORD1) {
    VSOut o;
    o.pos = float4(0, 0, -2, 1);
    o.data = 0;
    float speed = c_Spray.x;
    if (speed < 4.0 || c_Spray.y <= 0.0) return o;
    // The car: the closest surface on three points below the screen centre.
    // (The road right below the car would be closer still: stay above it.)
    float z0 = tex2Dlod(s_depth, float4(0.5, 0.6, 0, 0)).r;
    float z1 = tex2Dlod(s_depth, float4(0.5, 0.65, 0, 0)).r;
    float z2 = tex2Dlod(s_depth, float4(0.5, 0.7, 0, 0)).r;
    float bestZ = min(z0, min(z1, z2));
    float2 bestUV = float2(0.5, bestZ == z0 ? 0.6 : (bestZ == z1 ? 0.65 : 0.7));
    if (bestZ < 1.5 || bestZ > 16.0) return o; // no car in front of the camera
    if (abs(c_V2.y) > 0.45) return o;           // camera pitched up/down: quarter pipe, loop, wall ride
    float3 v = viewRay(bestUV, bestZ);
    float3 car = c_Cam.xyz + c_V0.xyz * v.x + c_V1.xyz * v.y + c_V2.xyz * v.z;
    float3 fwd = normalize(float3(c_V2.x, 0.0, c_V2.z) + 1e-5); // camera forward, level
    float3 right = float3(fwd.z, 0.0, -fwd.x);
    // Only with the tyres on the road (slopes included, jumps and falls not): the road next to
    // the rear tyres (the camera can't see it behind the car) must be where the tyres are.
    // Either side will do: a barrier may stand on the other.
    float under = car.y - 0.85; // the point found is ~0.85 m above the road
    float grounded = 0.0;
    for (int k = 0; k < 2; k++) {
        float3 beside = car + right * (k == 0 ? -1.7 : 1.7);
        beside.y = under;
        float3 bv = toView(beside);
        if (bv.z < 0.5) continue;
        float4 bc = toClip(bv);
        float2 buv = float2(bc.x / bc.w * 0.5 + 0.5, 0.5 - bc.y / bc.w * 0.5);
        float3 rv = viewRay(buv, tex2Dlod(s_depth, float4(buv, 0, 0)).r);
        float roadY = c_Cam.y + c_V0.y * rv.x + c_V1.y * rv.y + c_V2.y * rv.z;
        grounded = max(grounded, saturate(1.0 - (abs(roadY - under) - 0.45) / 0.45));
    }
    if (grounded <= 0.0) return o;
    float life = 0.6;
    float age = frac(c_Cam.w / life * (0.85 + 0.3 * seed.w) + seed.z);
    float ts = age * life;
    float side = seed.x < 0.5 ? -1.0 : 1.0;
    // Gusts: each tyre throws more or less water as it rolls through wetter and drier
    // patches (two slow waves per side); fewer particles where it is drier.
    float t = c_Cam.w;
    float gust = 0.6 + 0.2 * sin(t * (1.7 + 0.6 * side) + side) + 0.15 * sin(t * (4.3 - 1.1 * side) + 2.0 * side);
    if (frac(seed.w * 5.31 + seed.z * 2.7) > gust * grounded) return o;
    // The point found is the car's tail (rear wing / engine cover): the tyres sit below it.
    float3 tyre = car + right * side * 0.8 - fwd * 0.2;
    tyre.y = car.y - 0.8;
    float3 vel = -fwd * (1.5 + 3.0 * seed.y) + float3(0, 1.2 + 2.8 * frac(seed.y * 7.1), 0) + right * side * (0.3 + 1.5 * frac(seed.x * 13.3));
    // Thrown backwards and swept along in the car's wake, so it trails close behind.
    float3 p = tyre - fwd * speed * ts * 0.25 + vel * ts;
    p.y = max(p.y - 4.9 * ts * ts, tyre.y - 0.05);
    float3 view = toView(p);
    if (view.z < 0.5) return o;
    float size = lerp(0.15, 1.1, sqrt(age)) * (0.7 + 0.6 * frac(seed.w * 3.7)) * (0.6 + 0.4 * gust);
    o.pos = toClip(view + float3(corner.x, corner.y * 2.0 - 1.0, 0.0) * size);
    o.data = float4(corner.x, corner.y * 2.0 - 1.0, view.z, age);
    return o;
}

// Neon trail: quad k joins point k and k + 1 of the ring buffer, widened towards the camera.
float4 c_Trail  : register(c16); // points written, next index, now (s), fade after (s, 0 = never)
float4 c_Trail2 : register(c17); // sideways offset (m), half width (m), 0, 0
sampler2D s_trail : register(s2);
#ifndef TRAIL_W
#define TRAIL_W 128.0
#define TRAIL_POINTS 8192.0
#endif

float4 trailPoint(float i) {
    i = fmod(i + 2.0 * TRAIL_POINTS, TRAIL_POINTS);
    float2 uv = float2((fmod(i, TRAIL_W) + 0.5) / TRAIL_W, (floor(i / TRAIL_W) + 0.5) / (TRAIL_POINTS / TRAIL_W));
    return tex2Dlod(s_trail, float4(uv, 0, 0));
}

// The car is found anew in every frame, so the raw points wobble (most in jumps, when the
// camera swings): each point is averaged with its neighbours on the same line (weights
// 1 2 3 4 3 2 1). The window stays symmetric (as many points before as after), so the
// newest end isn't pulled back: it stays on the car.
float3 smoothPoint(float i, float4 centre) {
    float first = c_Trail.y - c_Trail.x, last = c_Trail.y - 1.0; // oldest / newest point
    float4 back[3], ahead[3];
    float nb = 0.0, nf = 0.0;
    bool open = centre.w > 0.0;  // backwards: stop at the start of this line
    [unroll] for (int k = 1; k <= 3; k++) {
        back[k - 1] = trailPoint(i - k);
        open = open && i - k >= first && back[k - 1].w != 0.0 && distance(back[k - 1].xyz, centre.xyz) < 12.0;
        nb += open ? 1.0 : 0.0;
        open = open && back[k - 1].w > 0.0;
    }
    open = true;                 // forwards: stop before the next line starts
    [unroll] for (int k = 1; k <= 3; k++) {
        ahead[k - 1] = trailPoint(i + k);
        open = open && i + k <= last && ahead[k - 1].w > 0.0 && distance(ahead[k - 1].xyz, centre.xyz) < 12.0;
        nf += open ? 1.0 : 0.0;
    }
    float n = min(nb, nf);
    float3 sum = centre.xyz * 4.0;
    float weight = 4.0;
    [unroll] for (int k = 1; k <= 3; k++) {
        if (k <= n) {
            sum += (back[k - 1].xyz + ahead[k - 1].xyz) * (4.0 - k);
            weight += 2.0 * (4.0 - k);
        }
    }
    return sum / weight;
}

VSOut trailVS(float4 seed : TEXCOORD0, float2 corner : TEXCOORD1) {
    VSOut o;
    o.pos = float4(0, 0, -2, 1);
    o.data = 0;
    float k = seed.x;
    if (k >= c_Trail.x - 1.0) return o;
    float first = c_Trail.y - c_Trail.x;
    float4 a = trailPoint(first + k), b = trailPoint(first + k + 1.0);
    // No car found at one end, or b starts a new line (respawn).
    if (a.w == 0.0 || b.w <= 0.0) return o;
    if (distance(a.xyz, b.xyz) > 40.0) return o; // teleported
    a.xyz = smoothPoint(first + k, a);
    b.xyz = smoothPoint(first + k + 1.0, b);
    float3 d = b.xyz - a.xyz;
    float len = length(d);
    if (len < 1e-3) return o;
    float age = c_Trail.z - lerp(abs(a.w), b.w, corner.y);
    float fade = c_Trail.w > 0.0 ? saturate(1.0 - age / c_Trail.w) : 1.0;
    if (fade <= 0.0) return o;
    float3 dir = d / len;
    float3 right = cross(float3(0, 1, 0), dir);
    float rl = length(right);
    right = rl > 0.1 ? right / rl : float3(1, 0, 0);
    float3 p = lerp(a.xyz, b.xyz, corner.y) + right * c_Trail2.x;
    float3 v = toView(p);
    if (v.z < 0.3) return o;
    float3 dv = toView(p + dir) - v;
    float3 side = cross(dv, v);
    float sl = length(side);
    if (sl < 1e-5) return o;
    // At least a pixel wide (no flicker far away); thinner lines are dimmed instead.
    float pixel = v.z * c_Proj2.z / c_Proj.x;
    float halfWidth = max(c_Trail2.y, pixel * 1.2);
    fade *= c_Trail2.y / halfWidth;
    o.pos = toClip(v + side / sl * halfWidth * corner.x);
    o.data = float4(corner.x, fade, v.z, age);
    return o;
}
)";

} // namespace

bool Pipeline::ensureTrail(IDirect3DDevice9* device) {
    if (!m_trailSupported) return false;
    if (m_trailVB && m_trailIB && m_trailPoints.valid()) return true;
    if (!m_trailVS) {
        // The points are read in the vertex shader (A32B32G32R32F vertex texture) and the
        // ribbon is blended into the FP16 HDR target.
        IDirect3D9* d3d = nullptr;
        device->GetDirect3D(&d3d);
        D3DDEVICE_CREATION_PARAMETERS cp{};
        device->GetCreationParameters(&cp);
        D3DDISPLAYMODE mode{};
        device->GetDisplayMode(0, &mode);
        const bool ok = d3d &&
                        SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                                         D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE,
                                                         D3DFMT_A32B32G32R32F)) &&
                        SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                                         D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING,
                                                         D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F));
        if (d3d) d3d->Release();
        char defines[96];
        snprintf(defines, sizeof(defines), "#define TRAIL_W %u.0\n#define TRAIL_POINTS %u.0\n", kTrailWidth, kTrailPoints);
        const std::string source = std::string(defines) + kRainVS;
        m_trailVS = ok ? gfx::compileVertexShader(device, source.c_str(), "trailVS", "TrailVS") : nullptr;
        const D3DVERTEXELEMENT9 elements[] = {
            {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
            D3DDECL_END(),
        };
        if (m_trailVS) device->CreateVertexDeclaration(elements, &m_trailDecl);
        if (!m_trailVS || !m_trailDecl) {
            TMVS_LOG("pipeline: neon trail unavailable (no float vertex textures or FP16 blending)");
            m_trailSupported = false;
            return false;
        }
    }
    bool ok = m_trailPoints.create(device, kTrailWidth, kTrailHeight, D3DFMT_A32B32G32R32F) &&
              SUCCEEDED(device->CreateVertexBuffer(kTrailPoints * 4 * 24, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &m_trailVB, nullptr)) &&
              SUCCEEDED(device->CreateIndexBuffer(kTrailPoints * 6 * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &m_trailIB, nullptr));
    float* v = nullptr;
    if (ok && SUCCEEDED(m_trailVB->Lock(0, 0, reinterpret_cast<void**>(&v), 0))) {
        static const float kCorners[4][2] = {{-1.0f, 0.0f}, {1.0f, 0.0f}, {-1.0f, 1.0f}, {1.0f, 1.0f}};
        for (UINT q = 0; q < kTrailPoints; q++) {
            for (const auto& corner : kCorners) {
                *v++ = static_cast<float>(q);
                *v++ = 0.0f;
                *v++ = 0.0f;
                *v++ = 0.0f;
                *v++ = corner[0];
                *v++ = corner[1];
            }
        }
        m_trailVB->Unlock();
    } else {
        ok = false;
    }
    WORD* index = nullptr;
    if (ok && SUCCEEDED(m_trailIB->Lock(0, 0, reinterpret_cast<void**>(&index), 0))) {
        for (UINT q = 0; q < kTrailPoints; q++) {
            const WORD b = static_cast<WORD>(q * 4);
            const WORD tri[6] = {b, static_cast<WORD>(b + 1), static_cast<WORD>(b + 2), static_cast<WORD>(b + 2), static_cast<WORD>(b + 1),
                                 static_cast<WORD>(b + 3)};
            memcpy(index, tri, sizeof(tri));
            index += 6;
        }
        m_trailIB->Unlock();
    } else {
        ok = false;
    }
    if (!ok) {
        gfx::release(m_trailVB);
        gfx::release(m_trailIB);
        m_trailPoints.destroy();
        TMVS_LOG("pipeline: neon trail buffers failed");
        m_trailSupported = false;
    }
    m_trailReset = true;
    return ok;
}

void Pipeline::drawTrail(IDirect3DDevice9* device, const Inputs& in, const Settings& s, float dt) {
    if (m_trailReset) {
        m_trailReset = false;
        m_trailBreak = true;
        m_trailHead = m_trailCount = 0;
        m_trailClock = m_trailTime = 0.0f;
        device->SetRenderTarget(0, m_trailPoints.surface);
        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 0.0f, 0);
    }
    m_trailTime += dt;
    m_trailClock += dt;
    // Record the car while you drive (not in replays and intros): every frame into the next
    // slot (the live end of the trail, so it stays on the car), kept 30 times a second.
    bool live = false;
    if (in.driving && dt > 0.0f && m_temporalValid) {
        live = true;
        bind(device, 0, m_nd.texture, false);
        const float stamp = m_trailTime + 1.0f; // never 0 (= no car)
        passConstants(device, m_trailBreak ? -stamp : stamp, 0.0f);
        // One pixel of the ring buffer. (SetRenderTarget resets the scissor rect: set it after.)
        device->SetRenderTarget(0, m_trailPoints.surface);
        const RECT pixel = {static_cast<LONG>(m_trailHead % kTrailWidth), static_cast<LONG>(m_trailHead / kTrailWidth),
                            static_cast<LONG>(m_trailHead % kTrailWidth) + 1, static_cast<LONG>(m_trailHead / kTrailWidth) + 1};
        device->SetScissorRect(&pixel);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
        device->SetPixelShader(m_shaders[kTrailPoint]);
        m_quad.draw(device, kTrailWidth, kTrailHeight);
        profileMark(kTrailPoint);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        if (m_trailClock >= 1.0f / 30.0f) {
            m_trailClock = fmodf(m_trailClock, 1.0f / 30.0f);
            m_trailBreak = false;
            m_trailHead = (m_trailHead + 1) % kTrailPoints;
            if (m_trailCount < kTrailPoints) m_trailCount++;
            live = false; // just kept: it's the newest point now
        }
    }
    // Points to draw: the kept ones plus the live end.
    const UINT points = live ? (m_trailCount + 1 < kTrailPoints ? m_trailCount + 1 : kTrailPoints) : m_trailCount;
    const UINT next = live ? (m_trailHead + 1) % kTrailPoints : m_trailHead;
    if (points < 2) return;

    const float* V = in.view;
    const float* P = in.projection;
    float vc[6][4] = {};
    for (int j = 0; j < 3; j++) {
        vc[j][0] = V[0 * 4 + j];
        vc[j][1] = V[1 * 4 + j];
        vc[j][2] = V[2 * 4 + j];
        vc[j][3] = V[12 + j];
    }
    vc[3][0] = P[0];
    vc[3][1] = P[5];
    vc[3][2] = P[8];
    vc[3][3] = P[9];
    vc[4][0] = P[10];
    vc[4][1] = P[14];
    vc[4][2] = 2.0f / in.width;
    vc[4][3] = 2.0f / in.height;
    device->SetVertexShaderConstantF(0, &vc[0][0], 6);
    // The vertex shader works with unwrapped indices: next + kTrailPoints keeps them positive.
    const float trail[4] = {static_cast<float>(points), static_cast<float>(next + kTrailPoints), m_trailTime + 1.0f, s.trailDuration};
    device->SetVertexShaderConstantF(16, trail, 1);

    device->SetRenderTarget(0, m_hdr.surface);
    D3DVIEWPORT9 viewport = {0, 0, in.width, in.height, 0.0f, 1.0f};
    device->SetViewport(&viewport);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
    bind(device, 0, m_nd.texture, false);
    device->SetTexture(D3DVERTEXTEXTURESAMPLER2, m_trailPoints.texture);
    device->SetSamplerState(D3DVERTEXTEXTURESAMPLER2, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(D3DVERTEXTEXTURESAMPLER2, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(D3DVERTEXTEXTURESAMPLER2, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetVertexDeclaration(m_trailDecl);
    device->SetStreamSource(0, m_trailVB, 0, 24);
    device->SetIndices(m_trailIB);
    device->SetVertexShader(m_trailVS);
    device->SetPixelShader(m_shaders[kTrail]);
    const float strength = s.neonTrail * 2.0f;
    passConstants(device, s.trailColor[0] * strength, s.trailColor[1] * strength, s.trailColor[2] * strength, 0.0f);
    const float halfWidth = s.trailWidth * 0.5f;
    const UINT quads = points - 1;
    // Two lines from the rear tyres, or one from the middle of the car.
    const float offsets[2] = {s.trailTyres ? -0.75f : 0.0f, 0.75f};
    for (int line = 0; line < (s.trailTyres ? 2 : 1); line++) {
        const float trail2[4] = {offsets[line], halfWidth, 0.0f, 0.0f};
        device->SetVertexShaderConstantF(17, trail2, 1);
        device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, quads * 4, 0, quads * 2);
    }
    profileMark(kTrail);

    device->SetStreamSource(0, nullptr, 0, 0);
    device->SetIndices(nullptr);
    device->SetTexture(D3DVERTEXTEXTURESAMPLER2, nullptr);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
}

bool Pipeline::ensureRain(IDirect3DDevice9* device) {
    if (!m_rainSupported) return false;
    if (m_rainVB && m_rainIB) return true;
    if (!m_dropVS) {
        // Additive blending into the FP16 HDR target.
        IDirect3D9* d3d = nullptr;
        device->GetDirect3D(&d3d);
        D3DDEVICE_CREATION_PARAMETERS cp{};
        device->GetCreationParameters(&cp);
        D3DDISPLAYMODE mode{};
        device->GetDisplayMode(0, &mode);
        const bool blend = d3d && SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                                                    D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING,
                                                                    D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F));
        if (d3d) d3d->Release();
        m_dropVS = blend ? gfx::compileVertexShader(device, kRainVS, "dropVS", "RainVS") : nullptr;
        m_splashVS = blend ? gfx::compileVertexShader(device, kRainVS, "splashVS", "RainVS") : nullptr;
        m_sprayVS = blend ? gfx::compileVertexShader(device, kRainVS, "sprayVS", "RainVS") : nullptr;
        m_snowVS = blend ? gfx::compileVertexShader(device, kRainVS, "snowVS", "RainVS") : nullptr;
        const D3DVERTEXELEMENT9 elements[] = {
            {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
            D3DDECL_END(),
        };
        if (m_dropVS) device->CreateVertexDeclaration(elements, &m_rainDecl);
        if (!m_dropVS || !m_splashVS || !m_sprayVS || !m_snowVS || !m_rainDecl) {
            TMVS_LOG("pipeline: rain particles unavailable (no FP16 blending or vertex shader failed)");
            m_rainSupported = false;
            return false;
        }
    }
    const UINT quads = kRainDrops + kRainSplashes;
    bool ok = SUCCEEDED(device->CreateVertexBuffer(quads * 4 * 24, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &m_rainVB, nullptr)) &&
              SUCCEEDED(device->CreateIndexBuffer(kRainChunk * 6 * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &m_rainIB, nullptr));
    float* v = nullptr;
    if (ok && SUCCEEDED(m_rainVB->Lock(0, 0, reinterpret_cast<void**>(&v), 0))) {
        unsigned state = 0x2545F491u;
        auto random = [&state]() {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>(state >> 8) / 16777216.0f;
        };
        static const float kCorners[4][2] = {{-1.0f, 0.0f}, {1.0f, 0.0f}, {-1.0f, 1.0f}, {1.0f, 1.0f}};
        for (UINT q = 0; q < quads; q++) {
            const float seed[4] = {random(), random(), random(), random()};
            for (const auto& corner : kCorners) {
                for (float x : seed) *v++ = x;
                *v++ = corner[0];
                *v++ = corner[1];
            }
        }
        m_rainVB->Unlock();
    } else {
        ok = false;
    }
    WORD* index = nullptr;
    if (ok && SUCCEEDED(m_rainIB->Lock(0, 0, reinterpret_cast<void**>(&index), 0))) {
        for (UINT q = 0; q < kRainChunk; q++) {
            const WORD b = static_cast<WORD>(q * 4);
            const WORD tri[6] = {b, static_cast<WORD>(b + 1), static_cast<WORD>(b + 2), static_cast<WORD>(b + 2), static_cast<WORD>(b + 1),
                                 static_cast<WORD>(b + 3)};
            memcpy(index, tri, sizeof(tri));
            index += 6;
        }
        m_rainIB->Unlock();
    } else {
        ok = false;
    }
    if (!ok) {
        gfx::release(m_rainVB);
        gfx::release(m_rainIB);
        TMVS_LOG("pipeline: rain particle buffers failed");
        m_rainSupported = false;
    }
    return ok;
}

void Pipeline::drawRain(IDirect3DDevice9* device, const Inputs& in, const Settings& s, float dt, bool splashes,
                        IDirect3DSurface9* target) {
    const float* V = in.view;
    const float* P = in.projection;
    float camera[3];
    for (int j = 0; j < 3; j++) camera[j] = -(V[12] * V[j * 4 + 0] + V[13] * V[j * 4 + 1] + V[14] * V[j * 4 + 2]);
    // Camera velocity (smoothed): drops streak along their motion relative to the camera.
    if (in.temporal) {
        float velocity[3] = {};
        if (m_temporalValid && dt > 1e-4f) {
            const float* W = m_prevView;
            for (int j = 0; j < 3; j++) {
                const float before = -(W[12] * W[j * 4 + 0] + W[13] * W[j * 4 + 1] + W[14] * W[j * 4 + 2]);
                velocity[j] = (camera[j] - before) / dt;
            }
        }
        for (int j = 0; j < 3; j++) m_cameraVelocity[j] += (velocity[j] - m_cameraVelocity[j]) * 0.3f;
    }
    const float wind[2] = {2.5f + s.wind * 5.0f, 1.0f + s.wind * 2.0f};
    const float fall = 9.0f;
    const float exposure = 0.03f;
    float streak[3] = {-(wind[0] - m_cameraVelocity[0]) * exposure, fall * exposure + m_cameraVelocity[1] * exposure,
                       -(wind[1] - m_cameraVelocity[2]) * exposure};
    const float length = sqrtf(streak[0] * streak[0] + streak[1] * streak[1] + streak[2] * streak[2]);
    if (length > 1.2f) for (float& x : streak) x *= 1.2f / length;

    float vc[10][4] = {};
    for (int j = 0; j < 3; j++) {
        vc[j][0] = V[0 * 4 + j];
        vc[j][1] = V[1 * 4 + j];
        vc[j][2] = V[2 * 4 + j];
        vc[j][3] = V[12 + j];
    }
    vc[3][0] = P[0];
    vc[3][1] = P[5];
    vc[3][2] = P[8];
    vc[3][3] = P[9];
    vc[4][0] = P[10];
    vc[4][1] = P[14];
    vc[4][2] = 2.0f / in.width;
    vc[4][3] = 2.0f / in.height;
    vc[5][0] = camera[0];
    vc[5][1] = camera[1];
    vc[5][2] = camera[2];
    vc[5][3] = fmodf(in.time, 1000.0f);
    vc[6][0] = streak[0];
    vc[6][1] = streak[1];
    vc[6][2] = streak[2];
    vc[7][0] = 30.0f;  // box: 15 m around the camera, farther rain is the screen-space layer
    vc[7][1] = 20.0f;
    vc[7][2] = fall;
    vc[7][3] = 1.0f;   // min width (px)
    vc[8][0] = wind[0];
    vc[8][1] = wind[1];
    vc[8][2] = 2.5f;   // splashes per second per particle
    vc[8][3] = 30.0f;
    vc[9][0] = m_heightOrigin[0];
    vc[9][1] = m_heightOrigin[1];
    vc[9][2] = 1.0f / kHeightMapWorld;
    // Previous camera for the streaks (camera turns smear the rain like a real shutter).
    const bool previous = in.temporal && m_temporalValid && m_havePrevious && dt > 1e-4f;
    float pc[5][4] = {};
    for (int j = 0; j < 3; j++) {
        pc[j][0] = m_prevView[0 * 4 + j];
        pc[j][1] = m_prevView[1 * 4 + j];
        pc[j][2] = m_prevView[2 * 4 + j];
        pc[j][3] = m_prevView[12 + j];
    }
    pc[3][0] = m_prevProjection[0];
    pc[3][1] = m_prevProjection[5];
    pc[3][2] = m_prevProjection[8];
    pc[3][3] = m_prevProjection[9];
    pc[4][0] = previous ? fminf(fmaxf(exposure / dt, 0.6f), 4.0f) : 0.0f;
    pc[4][1] = previous ? 1.0f : 0.0f;
    pc[4][2] = dt;
    pc[4][3] = 0.25f; // longest streak: 1/8 of the screen
    device->SetVertexShaderConstantF(0, &vc[0][0], 10);
    device->SetVertexShaderConstantF(10, &pc[0][0], 5);
    const float speed = sqrtf(m_cameraVelocity[0] * m_cameraVelocity[0] + m_cameraVelocity[2] * m_cameraVelocity[2]);
    // Whether the tyres touch the road is checked per particle in the depth buffer.
    const float spray[4] = {speed, s.spray * s.wetness * fminf(fmaxf((speed - 4.0f) / 25.0f, 0.0f), 1.0f), 0.0f, 0.0f};
    device->SetVertexShaderConstantF(15, spray, 1);

    device->SetRenderTarget(0, target);
    D3DVIEWPORT9 viewport = {0, 0, in.width, in.height, 0.0f, 1.0f};
    device->SetViewport(&viewport);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
    bind(device, 0, m_nd.texture, false);
    bind(device, 1, in.color, true);
    bind(device, 2, m_skyAverage.texture, false);
    bind(device, 3, s.neonLight > 0.0f ? m_spill[0].texture : nullptr, true);
    bind(device, 4, m_adapted[m_adaptIndex].texture, false);
    device->SetVertexDeclaration(m_rainDecl);
    device->SetStreamSource(0, m_rainVB, 0, 24);
    device->SetIndices(m_rainIB);
    device->SetTexture(D3DVERTEXTEXTURESAMPLER0, m_heightMap[m_heightIndex].texture);
    device->SetTexture(D3DVERTEXTEXTURESAMPLER1, m_linearDepth.texture);
    for (DWORD sampler = D3DVERTEXTEXTURESAMPLER0; sampler <= D3DVERTEXTEXTURESAMPLER1; sampler++) {
        device->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }

    // Spray behind the car (wet roads, also after the rain has stopped).
    if (spray[1] > 0.0f) {
        device->SetVertexShader(m_sprayVS);
        device->SetPixelShader(m_shaders[kSpray]);
        passConstants(device, spray[1], 0.0f);
        device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, kSprayParticles * 4, 0, kSprayParticles * 2);
        profileMark(kSpray);
    }

    // Rain 1 = steady rain, 2 = downpour (the buffers hold the downpour).
    const float amount = fminf(s.rain, 2.0f) * 0.5f;
    const UINT drops = static_cast<UINT>(kRainDrops * amount);
    if (drops > 0) {
        device->SetVertexShader(m_dropVS);
        device->SetPixelShader(m_shaders[kRainDrop]);
        passConstants(device, 1.3f, 0.0f);
        // 16-bit indices: draw in chunks, each with its own base vertex.
        for (UINT first = 0; first < drops; first += kRainChunk) {
            const UINT n = drops - first < kRainChunk ? drops - first : kRainChunk;
            device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, static_cast<INT>(first * 4), 0, n * 4, 0, n * 2);
        }
        profileMark(kRainDrop);
    }
    const UINT splashCount = static_cast<UINT>(kRainSplashes * amount);
    if (splashes && splashCount > 0) {
        device->SetVertexShader(m_splashVS);
        device->SetPixelShader(m_shaders[kRainSplash]);
        passConstants(device, 0.8f, 0.0f);
        device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, kRainDrops * 4, 0, splashCount * 4, 0, splashCount * 2);
        profileMark(kRainSplash);
    }

    // Snow 1 = steady snowfall, 2 = blizzard. Flakes are blended over the image, not added:
    // white on a white field must not glow.
    const UINT flakes = static_cast<UINT>(kSnowFlakes * fminf(s.snow, 2.0f) * 0.5f);
    if (flakes > 0) {
        float snowWind[4] = {(1.0f + s.wind * 7.0f) * fminf(s.snow, 1.5f), (0.5f + s.wind * 3.0f) * fminf(s.snow, 1.5f), 1.1f, 0.0f};
        device->SetVertexShaderConstantF(18, snowWind, 1);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetVertexShader(m_snowVS);
        device->SetPixelShader(m_shaders[kSnowFlake]);
        passConstants(device, 1.0f, 0.0f);
        for (UINT first = 0; first < flakes; first += kRainChunk) {
            const UINT n = flakes - first < kRainChunk ? flakes - first : kRainChunk;
            device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, static_cast<INT>(first * 4), 0, n * 4, 0, n * 2);
        }
        profileMark(kSnowFlake);
    }

    device->SetStreamSource(0, nullptr, 0, 0);
    device->SetIndices(nullptr);
    device->SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr);
    device->SetTexture(D3DVERTEXTEXTURESAMPLER1, nullptr);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
}

// A replay camera cut, a respawn or a jump to another camera: temporal history from the
// previous frame would smear the old view over the new one.
bool Pipeline::detectCameraCut(const Inputs& in) const {
    if (!m_havePrevious) return true;
    const float* V = in.view;
    const float* W = m_prevView;
    float a[3], b[3];
    for (int j = 0; j < 3; j++) {
        a[j] = -(V[12] * V[j * 4 + 0] + V[13] * V[j * 4 + 1] + V[14] * V[j * 4 + 2]);
        b[j] = -(W[12] * W[j * 4 + 0] + W[13] * W[j * 4 + 1] + W[14] * W[j * 4 + 2]);
    }
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    if (dx * dx + dy * dy + dz * dz > 30.0f * 30.0f) return true;
    // View direction = third column of the view rotation.
    const float facing = V[2] * W[2] + V[6] * W[6] + V[10] * W[10];
    if (facing < 0.82f) return true; // > ~35 degrees in one frame
    return fabsf(in.projection[5] - m_prevProjection[5]) > 0.1f * fabsf(m_prevProjection[5]);
}

void Pipeline::render(IDirect3DDevice9* device, const Inputs& in, const Settings& s, IDirect3DSurface9* output) {
    if (!m_ready || !in.color || !in.depth) return;
    if (!ensureTargets(device, in.width, in.height)) return;
    m_frame++;

    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    for (int i = 1; i < 4; i++) device->SetRenderTarget(i, nullptr);
    // TrackMania's device does mixed vertex processing: our vertex shaders need hardware.
    const BOOL softwareVP = device->GetSoftwareVertexProcessing();
    if (softwareVP) device->SetSoftwareVertexProcessing(FALSE);

    // Temporal state: history is only valid for the same camera as last frame.
    const bool temporal = in.temporal;
    const bool cut = temporal && detectCameraCut(in);
    m_temporalValid = temporal && !cut;
    if (cut) m_taaValid = false;

    profileBegin(device);
    setFrameConstants(device, in, s);
    const UINT hw = m_ndHalf.width, hh = m_ndHalf.height;

    // 1. Geometry reconstruction.
    bind(device, 0, in.depth, false);
    runPass(device, kLinearDepth, m_linearDepth);
    bind(device, 0, m_linearDepth.texture, false);
    runPass(device, kPrepare, m_nd);
    bind(device, 0, m_nd.texture, false);
    passConstants(device, 1.0f / in.width, 1.0f / in.height);
    runPass(device, kDownsampleND, m_ndHalf);

    // 1b. Long-range shadows: update the world-space height map.
    // Extra cameras (replay blends) use the map as it is: it follows the main camera.
    // Rain splashes land on it too.
    const bool wantLong = s.longShadows > 0.0f && s.shadowStrength > 0.0f && m_sunKnown;
    const bool wantVolume = s.volumetricLight > 0.0f && m_sunKnown;
    if (m_heightReset) {
        m_heightReset = false;
        m_heightValid = false;
        m_heightHoldUntil = in.time + 4.0f; // the respawn camera lasts ~1.5 s
    }
    if (in.driving || in.time > m_heightHoldUntil) m_heightHoldUntil = -1.0f;
    const bool splat = temporal && m_heightHoldUntil < 0.0f;
    const bool heightMap = (wantLong || wantVolume || s.rain > 0.0f) && ensureHeightMap(device) && (splat || m_heightValid);
    // Every other frame is enough for a map of the static world (saves ~0.15 ms).
    if (heightMap && splat && (!m_heightValid || (m_frame & 1))) updateHeightMap(device, in, s);
    const bool longShadows = heightMap && wantLong;
    const float longConstants[4] = {s.taa ? 1.0f : 0.0f, m_temporalValid ? 1.0f : 0.0f, s.taa ? static_cast<float>(m_frame % 64) : 0.0f,
                                    longShadows ? s.longShadows : 0.0f};
    device->SetPixelShaderConstantF(34, longConstants, 1);

    // 2. AO + sun shadows, bilateral blur.
    if (s.aoStrength > 0.0f || s.shadowStrength > 0.0f || s.debugView == 3 || s.debugView == 4 || s.debugView == 7) {
        bind(device, 0, m_ndHalf.texture, false);
        bind(device, 1, m_nd.texture, false);
        bind(device, 2, longShadows ? m_heightMap[m_heightIndex].texture : nullptr, false);
        runPass(device, kOcclusionShadow, m_occlusion);
        bind(device, 0, m_occlusion.texture, false);
        bind(device, 1, m_ndHalf.texture, false);
        passConstants(device, 1.0f / hw, 0.0f);
        runPass(device, kBilateralBlur, m_occlusionTmp);
        bind(device, 0, m_occlusionTmp.texture, false);
        passConstants(device, 0.0f, 1.0f / hh);
        runPass(device, kBilateralBlur, m_occlusion);
    } else {
        device->SetRenderTarget(0, m_occlusion.surface);
        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFFFFFFFF, 1.0f, 0);
    }

    // 2b. Volumetric light: shadow heights from the height map, then the view rays through
    // the haze at half resolution (the light shaft target is free until later).
    const bool volume = heightMap && wantVolume;
    if (volume) {
        static const float kVolumeSteps[3] = {8.0f, 12.0f, 16.0f};
        const int q = s.quality < 0 ? 0 : (s.quality > 2 ? 2 : s.quality);
        bind(device, 0, m_heightMap[m_heightIndex].texture, false);
        runPass(device, kShadowHeight, m_shadowHeight);
        bind(device, 0, m_ndHalf.texture, false);
        bind(device, 1, m_shadowHeight.texture, false); // R32F: no filtering on older GPUs
        passConstants(device, kVolumeSteps[q], 0.0f);
        runPass(device, kVolumetric, m_rays[1]);
    }
    // 2c. One-bounce global illumination, blurred like the AO.
    const bool gi = s.globalIllumination > 0.0f;
    if (gi) {
        static const float kGISamples[3] = {4.0f, 6.0f, 8.0f};
        const int q = s.quality < 0 ? 0 : (s.quality > 2 ? 2 : s.quality);
        bind(device, 0, m_ndHalf.texture, false);
        bind(device, 1, in.color, true);
        // Own noise sequence (also without TAA): the history below averages it out.
        passConstants(device, kGISamples[q], 10.0f, static_cast<float>(m_frame % 64));
        runPass(device, kGI, m_gi[0]);
        bind(device, 0, m_gi[0].texture, false);
        bind(device, 1, m_ndHalf.texture, false);
        passConstants(device, 1.0f / m_gi[0].width, 0.0f);
        runPass(device, kBilateralBlur, m_gi[1]);
        bind(device, 0, m_gi[1].texture, false);
        passConstants(device, 0.0f, 1.0f / m_gi[0].height);
        runPass(device, kBilateralBlur, m_gi[0]);
        // Accumulate over frames: few rays per pixel would otherwise boil into blotches.
        const int previous = m_giIndex;
        m_giIndex ^= 1;
        bind(device, 0, m_gi[0].texture, false);
        bind(device, 1, m_giHistory[previous].texture, true);
        bind(device, 2, m_ndHalf.texture, false);
        passConstants(device, (m_giValid && m_temporalValid) ? 1.0f : 0.0f, 0.0f);
        runPass(device, kGITemporal, m_giHistory[m_giIndex]);
        m_giValid = true;
    } else {
        m_giValid = false;
    }
    const float lensDrops = s.lensDrops ? fminf(s.rain, 1.0f) : 0.0f;
    const float volumeConstants[4] = {volume ? s.volumetricLight : 0.0f, 150.0f, gi ? s.globalIllumination : 0.0f, lensDrops};
    device->SetPixelShaderConstantF(37, volumeConstants, 1);

    // 3. Custom sky, the average sky colour (fog), volumetric clouds.
    if (s.skyMode > 0) {
        bind(device, 0, m_nd.texture, false);
        if (s.skyMode == 4) {
            // The aurora's curtains are the costliest sky: march them at half resolution
            // (borrowing the light shaft target, which is only used later) and upscale with
            // EASU in the sky pass. Stars stay at full resolution.
            static const float kAuroraSlices[3] = {16.0f, 26.0f, 40.0f};
            const int q = s.quality < 0 ? 0 : (s.quality > 2 ? 2 : s.quality);
            passConstants(device, kAuroraSlices[q], 0.0f, 1.0f / in.width, 1.0f / in.height);
            runPass(device, kAuroraHalf, m_rays[0]);
            bind(device, 1, m_rays[0].texture, false);
            passConstants(device, static_cast<float>(m_rays[0].width), static_cast<float>(m_rays[0].height),
                          1.0f / m_rays[0].width, 1.0f / m_rays[0].height);
        }
        const int mode = s.skyMode < 1 ? 1 : (s.skyMode > 5 ? 5 : s.skyMode);
        runPass(device, static_cast<Pass>(kSkyClear + mode - 1), m_sky);
    }
    bind(device, 0, in.color, false);
    bind(device, 1, m_nd.texture, false);
    bind(device, 2, m_sky.texture, false);
    runPass(device, kSkyAverage, m_skyAverage);
    const bool clouds = s.volumetricClouds > 0.0f && ensureCloudNoise(device);
    if (clouds) {
        bind(device, 4, m_skyAverage.texture, false);
        device->SetTexture(7, m_cloudNoise);
        device->SetSamplerState(7, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(7, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(7, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(7, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        device->SetSamplerState(7, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        device->SetSamplerState(7, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);
        device->SetSamplerState(7, D3DSAMP_SRGBTEXTURE, FALSE);
        runPass(device, kClouds, m_clouds);
        device->SetTexture(7, nullptr);
    }

    // 3b. Reflections and neon light spill.
    const bool reflect = s.wetness > 0.0f || s.reflections > 0.0f || s.waterSurfaces > 0.0f;
    if (reflect) {
        bind(device, 0, in.color, true);
        bind(device, 1, m_nd.texture, false);
        bind(device, 2, m_ndHalf.texture, false);
        runPass(device, kReflect, m_reflect);
    }
    const bool spill = s.neonLight > 0.0f;
    if (spill) {
        bind(device, 0, in.color, true);
        bind(device, 1, m_nd.texture, false);
        runPass(device, kSpillDown, m_spill[0]);
        bind(device, 0, m_spill[0].texture, true);
        passConstants(device, 1.0f / m_spill[0].width, 0.0f);
        runPass(device, kSpillBlur, m_spill[1]);
        bind(device, 0, m_spill[1].texture, true);
        passConstants(device, 0.0f, 1.0f / m_spill[0].height);
        runPass(device, kSpillBlur, m_spill[0]);
    }

    // 4. Lighting composite.
    bind(device, 0, in.color, false);
    bind(device, 1, m_nd.texture, false);
    bind(device, 2, m_occlusion.texture, false);
    bind(device, 3, m_ndHalf.texture, false);
    bind(device, 4, m_skyAverage.texture, false);
    bind(device, 5, m_sky.texture, false);
    bind(device, 6, m_clouds.texture, true);
    bind(device, 8, m_reflect.texture, true);
    bind(device, 9, m_spill[0].texture, true);
    bind(device, 10, m_rays[1].texture, true);
    bind(device, 11, m_giHistory[m_giIndex].texture, false);
    runPass(device, kLighting, m_hdr);
    device->SetTexture(10, nullptr);
    device->SetTexture(11, nullptr);
    device->SetTexture(8, nullptr);
    device->SetTexture(9, nullptr);

    const float dt = m_historyValid ? fmaxf(in.time - m_lastTime, 0.0f) : 0.0f;
    if (temporal) m_lastTime = in.time;

    // 4b. Neon trail behind the car, into the HDR image so it blooms.
    if (s.neonTrail > 0.0f && ensureTrail(device)) drawTrail(device, in, s, temporal ? dt : 0.0f);
    else m_trailReset = true;

    // 5. Depth of field and motion blur (cinematic, HDR).
    gfx::Target* hdr = &m_hdr;
    const bool motionBlur = s.motionBlur > 0.0f && m_temporalValid;
    const bool dof = s.depthOfField > 0.0f;
    if ((motionBlur || dof) && m_hdr2.create(device, in.width, in.height, D3DFMT_A16B16G16R16F)) {
        if (dof) {
            int focus = m_focusIndex;
            if (temporal) {
                focus ^= 1;
                bind(device, 0, m_nd.texture, false);
                bind(device, 1, m_focus[m_focusIndex].texture, false);
                passConstants(device, 1.0f - expf(-dt * 4.0f), m_temporalValid ? 0.0f : 1.0f);
                runPass(device, kFocus, m_focus[focus]);
                m_focusIndex = focus;
            }
            bind(device, 0, m_hdr.texture, true);
            bind(device, 1, m_ndHalf.texture, false);
            bind(device, 2, m_focus[focus].texture, false);
            runPass(device, kDofBlur, m_dof);
        }
        bind(device, 0, m_hdr.texture, true);
        bind(device, 1, m_dof.texture, true);
        bind(device, 2, m_nd.texture, false);
        runPass(device, kCinematic, m_hdr2);
        hdr = &m_hdr2;
    }

    // 6. Light shafts.
    // Skipped when the sun is off screen: the shafts are weighted by its on-screen fade.
    bool rays = (s.godRays > 0.0f && m_sunFade > 0.001f) || s.debugView == 5;
    if (rays) {
        bind(device, 0, hdr->texture, true);
        bind(device, 1, m_nd.texture, false);
        runPass(device, kRayMask, m_rays[0]);
        bind(device, 0, m_rays[0].texture, true);
        passConstants(device, 1.0f, s.godRayDecay);
        runPass(device, kRayBlur, m_rays[1]);
        bind(device, 0, m_rays[1].texture, true);
        passConstants(device, 0.3f, s.godRayDecay);
        runPass(device, kRayBlur, m_rays[0]);
    } else {
        device->SetRenderTarget(0, m_rays[0].surface);
        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
    }

    // 7. Bloom pyramid.
    IDirect3DTexture9* source = hdr->texture;
    UINT sw = in.width, sh = in.height;
    for (int i = 0; i < kBloomLevels; i++) {
        bind(device, 0, source, true);
        passConstants(device, 1.0f / sw, 1.0f / sh, i == 0 ? 1.0f : 0.0f);
        runPass(device, kBloomDown, m_bloomDown[i]);
        source = m_bloomDown[i].texture;
        sw = m_bloomDown[i].width;
        sh = m_bloomDown[i].height;
    }
    float norm = 0.0f, weight = 1.0f;
    for (int i = 0; i < kBloomLevels; i++) {
        norm += weight;
        weight *= s.bloomRadius;
    }
    for (int i = kBloomLevels - 2; i >= 0; i--) {
        gfx::Target& lower = (i == kBloomLevels - 2) ? m_bloomDown[kBloomLevels - 1] : m_bloomUp[i + 1];
        bind(device, 0, lower.texture, true);
        bind(device, 1, m_bloomDown[i].texture, true);
        passConstants(device, 1.0f / lower.width, 1.0f / lower.height, s.bloomRadius);
        runPass(device, kBloomUp, m_bloomUp[i]);
    }

    // 8. Exposure (extra cameras reuse the adapted value).
    if (temporal) {
        bind(device, 0, m_bloomDown[3].texture, true);
        runPass(device, kLuminance, m_luminance);
        const int previous = m_adaptIndex;
        m_adaptIndex ^= 1;
        bind(device, 0, m_luminance.texture, false);
        bind(device, 1, m_adapted[previous].texture, false);
        passConstants(device, 1.0f - expf(-dt * 1.6f), m_historyValid ? 0.0f : 1.0f);
        runPass(device, kAdapt, m_adapted[m_adaptIndex]);
        m_historyValid = true;
    }

    // 9. Final grade.
    bind(device, 0, hdr->texture, true);
    bind(device, 1, m_bloomUp[0].texture, true);
    bind(device, 2, m_rays[0].texture, true);
    bind(device, 3, m_adapted[m_adaptIndex].texture, false);
    bind(device, 4, m_nd.texture, false);
    bind(device, 5, m_occlusion.texture, true);
    bind(device, 6, m_bloomDown[2].texture, true);
    passConstants(device, 1.0f / norm, 0.0f);
    // Disabled post passes are skipped entirely, not just passed through.
    const bool post = s.debugView == 0;
    const bool fxaa = s.fxaa && post;
    const bool taa = s.taa && post && temporal && m_taa[0].create(device, in.width, in.height, D3DFMT_A16B16G16R16F) &&
                     m_taa[1].create(device, in.width, in.height, D3DFMT_A16B16G16R16F);
    const bool sharpen = s.sharpen > 0.001f && post;
    // Drops on the lens sit on the lens: after TAA (it would smear them along the camera's
    // motion), in the last pass.
    const bool drops = lensDrops > 0.0f && post;
    // Chain: Final -> [FXAA] -> [TAA] -> [Sharpen | Copy] -> output.
    IDirect3DTexture9* current = nullptr;
    if (!fxaa && !taa && !sharpen && !drops) {
        runPass(device, kFinal, output, in.width, in.height);
    } else {
        runPass(device, kFinal, m_ldr);
        current = m_ldr.texture;
    }
    if (fxaa) {
        bind(device, 0, current, true);
        passConstants(device, 1.0f / in.width, 1.0f / in.height);
        if (!taa && !sharpen && !drops) {
            runPass(device, kFXAA, output, in.width, in.height);
            current = nullptr;
        } else {
            runPass(device, kFXAA, m_ldr2);
            current = m_ldr2.texture;
        }
    }
    if (taa) {
        const int previous = m_taaIndex;
        m_taaIndex ^= 1;
        bind(device, 0, current, in.jitter[0] != 0.0f || in.jitter[1] != 0.0f);
        bind(device, 1, m_taa[previous].texture, true);
        bind(device, 2, m_nd.texture, false);
        const float taaValid[4] = {1.0f, (m_taaValid && m_temporalValid) ? 1.0f : 0.0f, static_cast<float>(m_frame % 64),
                                   longConstants[3]};
        device->SetPixelShaderConstantF(34, taaValid, 1);
        passConstants(device, in.jitter[0], in.jitter[1], m_prevJitter[0], m_prevJitter[1]);
        runPass(device, kTAA, m_taa[m_taaIndex]);
        m_prevJitter[0] = in.jitter[0];
        m_prevJitter[1] = in.jitter[1];
        m_taaValid = true;
        current = m_taa[m_taaIndex].texture;
    } else if (temporal) {
        m_taaValid = false;
    }
    if (current) {
        bind(device, 0, current, false);
        passConstants(device, 1.0f / in.width, 1.0f / in.height);
        runPass(device, sharpen ? kSharpen : kCopy, output, in.width, in.height);
    }
    // Rain particles go on top of the finished image: TAA would erase thin, fast drops as
    // flicker, and keeping them out of its history avoids trails.
    if ((s.rain > 0.0f || s.wetness > 0.0f || s.snow > 0.0f) && post && ensureRain(device))
        drawRain(device, in, s, dt, heightMap && m_heightValid, output);
    profileEnd();

    if (temporal) {
        memcpy(m_prevView, in.view, sizeof(m_prevView));
        memcpy(m_prevProjection, in.projection, sizeof(m_prevProjection));
        m_havePrevious = true;
    }
    for (int i = 0; i < 10; i++) device->SetTexture(i, nullptr);
    if (softwareVP) device->SetSoftwareVertexProcessing(TRUE);
}

bool Pipeline::readColor(IDirect3DDevice9* device, IDirect3DSurface9* surface, std::vector<uint32_t>& out) {
    D3DSURFACE_DESC desc{};
    surface->GetDesc(&desc);
    if (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8) return false;
    IDirect3DSurface9* system = nullptr;
    if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &system, nullptr)))
        return false;
    bool ok = SUCCEEDED(device->GetRenderTargetData(surface, system));
    D3DLOCKED_RECT locked{};
    if (ok && SUCCEEDED(system->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        out.resize(static_cast<size_t>(desc.Width) * desc.Height);
        for (UINT y = 0; y < desc.Height; y++) {
            memcpy(&out[static_cast<size_t>(y) * desc.Width], static_cast<const BYTE*>(locked.pBits) + static_cast<size_t>(locked.Pitch) * y,
                   desc.Width * 4);
        }
        system->UnlockRect();
    } else {
        ok = false;
    }
    system->Release();
    return ok;
}

bool Pipeline::readDepth(IDirect3DDevice9* device, IDirect3DTexture9* depth, std::vector<float>& out) {
    D3DSURFACE_DESC desc{};
    depth->GetLevelDesc(0, &desc);
    if (!m_depthCopy.create(device, desc.Width, desc.Height, D3DFMT_R32F)) return false;
    bind(device, 0, depth, false);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    runPass(device, kCopyDepth, m_depthCopy);
    device->SetTexture(0, nullptr);

    IDirect3DSurface9* system = nullptr;
    if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &system, nullptr)))
        return false;
    bool ok = SUCCEEDED(device->GetRenderTargetData(m_depthCopy.surface, system));
    D3DLOCKED_RECT locked{};
    if (ok && SUCCEEDED(system->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        out.resize(static_cast<size_t>(desc.Width) * desc.Height);
        for (UINT y = 0; y < desc.Height; y++) {
            memcpy(&out[static_cast<size_t>(y) * desc.Width], static_cast<const BYTE*>(locked.pBits) + static_cast<size_t>(locked.Pitch) * y,
                   desc.Width * 4);
        }
        system->UnlockRect();
    } else {
        ok = false;
    }
    system->Release();
    return ok;
}

} // namespace tmshaders
