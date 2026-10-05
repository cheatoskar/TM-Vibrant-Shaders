#include "pipeline.h"
#include "log.h"
#include "config.h"
#include "tmvs_bytecode.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace tmshaders {
namespace {

const char* const kEntryPoints[] = {
    "PS_LinearDepth", "PS_Prepare",   "PS_DownsampleND", "PS_OcclusionShadow", "PS_BilateralBlur", "PS_Sky", "PS_SkyAverage", "PS_Lighting",
    "PS_RayMask",   "PS_RayBlur",      "PS_BloomDown",       "PS_BloomUp",       "PS_Luminance",  "PS_Adapt",
    "PS_Final",     "PS_FXAA",         "PS_Sharpen",         "PS_CopyDepth",
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
    if (!ok) {
        destroyTargets();
        return false;
    }
    m_width = width;
    m_height = height;
    m_historyValid = false;
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
    float c[23][4] = {};

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
    const bool nightSky = s.skyMode >= 2;
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
    c[20][1] = s.skyNight >= 0.0f ? s.skyNight : (nightSky ? (gameNight ? 0.35f : 0.95f) : 0.0f);
    c[20][2] = s.skyRotation * 3.14159265f / 180.0f;
    c[20][3] = s.skyBrightness;
    c[21][0] = s.cloudAmount;
    c[21][1] = s.starAmount;
    c[21][2] = s.skyEffectSize;
    c[21][3] = s.planetSize;

    static const float kAoSamples[] = {6.0f, 10.0f, 16.0f};
    static const float kShadowSteps[] = {10.0f, 16.0f, 24.0f};
    const int q = s.quality < 0 ? 0 : (s.quality > 2 ? 2 : s.quality);
    c[22][0] = kAoSamples[q];
    c[22][1] = kShadowSteps[q];

    device->SetPixelShaderConstantF(0, &c[0][0], 23);
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

    // 2. AO + sun shadows, bilateral blur.
    if (s.aoStrength > 0.0f || s.shadowStrength > 0.0f || s.debugView == 3 || s.debugView == 4) {
        bind(device, 0, m_ndHalf.texture, false);
        bind(device, 1, m_nd.texture, false);
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

    // 3. Custom sky, then the average sky colour used for fog.
    if (s.skyMode > 0) {
        bind(device, 0, m_nd.texture, false);
        runPass(device, kSky, m_sky);
    }
    bind(device, 0, in.color, false);
    bind(device, 1, m_nd.texture, false);
    bind(device, 2, m_sky.texture, false);
    runPass(device, kSkyAverage, m_skyAverage);

    // 4. Lighting composite.
    bind(device, 0, in.color, false);
    bind(device, 1, m_nd.texture, false);
    bind(device, 2, m_occlusion.texture, false);
    bind(device, 3, m_ndHalf.texture, false);
    bind(device, 4, m_skyAverage.texture, false);
    bind(device, 5, m_sky.texture, false);
    runPass(device, kLighting, m_hdr);

    // 5. Light shafts.
    // Skipped when the sun is off screen: the shafts are weighted by its on-screen fade.
    bool rays = (s.godRays > 0.0f && m_sunFade > 0.001f) || s.debugView == 5;
    if (rays) {
        bind(device, 0, m_hdr.texture, true);
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

    // 6. Bloom pyramid.
    IDirect3DTexture9* source = m_hdr.texture;
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

    // 7. Exposure.
    const float dt = m_historyValid ? fmaxf(in.time - m_lastTime, 0.0f) : 0.0f;
    m_lastTime = in.time;
    bind(device, 0, m_bloomDown[3].texture, true);
    runPass(device, kLuminance, m_luminance);
    const int previous = m_adaptIndex;
    m_adaptIndex ^= 1;
    bind(device, 0, m_luminance.texture, false);
    bind(device, 1, m_adapted[previous].texture, false);
    passConstants(device, 1.0f - expf(-dt * 1.6f), m_historyValid ? 0.0f : 1.0f);
    runPass(device, kAdapt, m_adapted[m_adaptIndex]);
    m_historyValid = true;

    // 8. Final grade.
    bind(device, 0, m_hdr.texture, true);
    bind(device, 1, m_bloomUp[0].texture, true);
    bind(device, 2, m_rays[0].texture, true);
    bind(device, 3, m_adapted[m_adaptIndex].texture, false);
    bind(device, 4, m_nd.texture, false);
    bind(device, 5, m_occlusion.texture, true);
    bind(device, 6, m_bloomDown[2].texture, true);
    passConstants(device, 1.0f / norm, 0.0f);
    // Disabled post passes are skipped entirely, not just passed through.
    const bool sharpen = s.sharpen > 0.001f && s.debugView == 0;
    const bool fxaa = s.fxaa && s.debugView == 0;
    if (fxaa) {
        runPass(device, kFinal, m_ldr);
    } else if (sharpen) {
        runPass(device, kFinal, m_ldr2);
    } else {
        runPass(device, kFinal, output, in.width, in.height);
    }

    // 9. Anti-aliasing and sharpening.
    if (fxaa) {
        bind(device, 0, m_ldr.texture, true);
        passConstants(device, 1.0f / in.width, 1.0f / in.height);
        if (sharpen) {
            runPass(device, kFXAA, m_ldr2);
        } else {
            runPass(device, kFXAA, output, in.width, in.height);
        }
    }
    if (sharpen) {
        bind(device, 0, m_ldr2.texture, false);
        passConstants(device, 1.0f / in.width, 1.0f / in.height);
        runPass(device, kSharpen, output, in.width, in.height);
    }
    profileEnd();

    for (int i = 0; i < 7; i++) device->SetTexture(i, nullptr);
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
