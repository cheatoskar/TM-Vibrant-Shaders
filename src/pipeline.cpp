#include "pipeline.h"
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
    "PS_Sky",         "PS_SkyAverage", "PS_Clouds",     "PS_Reflect",     "PS_SpillDown",   "PS_SpillBlur",       "PS_Lighting",
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
    gfx::release(m_heightDepth);
    gfx::release(m_splatPoints);
    gfx::release(m_cloudNoise);
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
    c[20][1] = s.skyNight >= 0.0f ? s.skyNight : (nightSky ? (gameNight ? 0.35f : 0.95f) : (ringWorld && !gameNight ? 0.2f : 0.0f));
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

    device->SetPixelShaderConstantF(0, &c[0][0], 32);
    device->SetPixelShaderConstantF(35, &c[35][0], 2);
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
    const bool longShadows = s.longShadows > 0.0f && s.shadowStrength > 0.0f && m_sunKnown && ensureHeightMap(device) &&
                             (temporal || m_heightValid);
    if (longShadows && temporal) updateHeightMap(device, in, s);
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

    // 3. Custom sky, the average sky colour (fog), volumetric clouds.
    if (s.skyMode > 0) {
        bind(device, 0, m_nd.texture, false);
        runPass(device, kSky, m_sky);
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
        bind(device, 0, in.color, false);
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
    runPass(device, kLighting, m_hdr);
    device->SetTexture(8, nullptr);
    device->SetTexture(9, nullptr);

    const float dt = m_historyValid ? fmaxf(in.time - m_lastTime, 0.0f) : 0.0f;
    if (temporal) m_lastTime = in.time;

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
    // Chain: Final -> [FXAA] -> [TAA] -> [Sharpen | Copy] -> output.
    IDirect3DTexture9* current = nullptr;
    if (!fxaa && !taa && !sharpen) {
        runPass(device, kFinal, output, in.width, in.height);
    } else {
        runPass(device, kFinal, m_ldr);
        current = m_ldr.texture;
    }
    if (fxaa) {
        bind(device, 0, current, true);
        passConstants(device, 1.0f / in.width, 1.0f / in.height);
        if (!taa && !sharpen) {
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
        bind(device, 0, current, false);
        bind(device, 1, m_taa[previous].texture, true);
        bind(device, 2, m_nd.texture, false);
        const float taaValid[4] = {1.0f, (m_taaValid && m_temporalValid) ? 1.0f : 0.0f, static_cast<float>(m_frame % 64),
                                   longConstants[3]};
        device->SetPixelShaderConstantF(34, taaValid, 1);
        runPass(device, kTAA, m_taa[m_taaIndex]);
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
