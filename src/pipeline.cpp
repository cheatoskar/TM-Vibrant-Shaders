#include "pipeline.h"
#include "weather.h"
#include "log.h"
#include "config.h"
#include "noise.h"
#include "tmvs_bytecode.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace tmshaders {
namespace {

const char* const kEntryPoints[] = {
    "PS_LinearDepth", "PS_Prepare",  "PS_DownsampleND", "PS_HeightMerge", "PS_HeightSplat", "PS_OcclusionShadow", "PS_BilateralBlur",
    "PS_ShadowHeight", "PS_Volumetric", "PS_GI", "PS_GITemporal",
    "PS_SkyClear",    "PS_SkyStars", "PS_SkyBlackHole", "PS_SkyAurora",   "PS_SkyRing",     "PS_AuroraHalf",
    "PS_SkyAverage",  "PS_Shelter",    "PS_Clouds",     "PS_Reflect",     "PS_SpillDown",   "PS_SpillBlur",       "PS_Lighting",
    "PS_RainDrop",    "PS_RainSplash", "PS_Spray", "PS_TrailPoint", "PS_Trail", "PS_SnowFlake",
    "PS_Focus",       "PS_DofBlur",  "PS_Cinematic",    "PS_RayMask",     "PS_RayBlur",     "PS_BloomDown",       "PS_BloomUp",
    "PS_Luminance",   "PS_Adapt",    "PS_Final",        "PS_FXAA",        "PS_TAA",         "PS_Sharpen",         "PS_Copy",
    "PS_CopyDepth",   "PS_WaterLevel", "PS_Resolve",    "PS_LightingEngine", "PS_SpillDownEngine",
    "PS_LightGridInit", "PS_LightGridSweep", "PS_PrepareGBuffer", "PS_LampSource", "PS_LampDown", "PS_LampUp",
    "PS_LightingGameLight", "PS_FsrEasu", "PS_FsrRcas", "PS_SpillTemporal",
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
    // After a device reset the shaders are still there (releaseForReset): creating the 58 passes
    // again took most of the 1.6 s the game stood still after every alt-tab. Another device
    // (or none yet) gets everything new.
    const bool kept = m_shaders[0] && m_shaderDevice == device;
    if (!kept && m_shaderDevice) release();
    if (!m_quad.init(device)) return false;
    if (kept) TMVS_LOG("pipeline: %d passes kept through the device reset", kPassCount);
    else if (!compileAll(device)) return false;
    m_shaderDevice = device;
    if (!m_stateBlock) device->CreateStateBlock(D3DSBT_ALL, &m_stateBlock);
    m_ready = true;
    m_historyValid = false;
    return true;
}

const char* Pipeline::passName(int pass) {
    if (pass == kProfileCasters) return "Casters";
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
    frame.outer = m_outerDisjoint;
    if (!frame.outer) frame.disjoint->Issue(D3DISSUE_BEGIN);
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
    if (!frame->outer) frame->disjoint->Issue(D3DISSUE_END);
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
    if (frame.outer) disjoint = FALSE; // the caller's bracket tells it, not read here: the checks below
    else if (!get(frame.disjoint, &disjoint, sizeof(disjoint))) return false;
    if (!get(frame.frequency, &frequency, sizeof(frequency))) return false;
    UINT64 times[ProfileFrame::kMaxStamps] = {};
    for (int i = 0; i < frame.count; i++) {
        if (!get(frame.stamps[i], &times[i], sizeof(UINT64))) return false;
    }
    frame.pending = false;
    // Without its own disjoint query: a frame whose stamps run backwards or span more than a
    // second (the clock changed under them) is left out rather than smoothed in.
    if (frame.outer && frequency != 0) {
        for (int i = 1; i < frame.count && !disjoint; i++) disjoint = times[i] < times[i - 1];
        if (frame.count >= 2 && times[frame.count - 1] - times[0] > frequency) disjoint = TRUE;
    }
    if (disjoint || frequency == 0 || frame.count < 2) return true;
    float ms[kProfileSlots] = {};
    for (int i = 1; i < frame.count; i++) {
        if (frame.pass[i] >= 0) ms[frame.pass[i]] += static_cast<float>(static_cast<double>(times[i] - times[i - 1]) * 1000.0 / frequency);
    }
    const float total = static_cast<float>(static_cast<double>(times[frame.count - 1] - times[0]) * 1000.0 / frequency);
    const float k = m_totalMs > 0.0f ? 0.1f : 1.0f;
    for (int p = 0; p < kProfileSlots; p++) m_passMs[p] += (ms[p] - m_passMs[p]) * k;
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

void Pipeline::releaseForReset() {
    m_ready = false;
    releaseProfile();
    gfx::release(m_stateBlock); // state blocks don't live through a reset
    destroyTargets();
}

void Pipeline::release() {
    m_ready = false;
    m_shaderDevice = nullptr;
    releaseProfile();
    for (auto& shader : m_shaders) gfx::release(shader);
    gfx::release(m_stateBlock);
    gfx::release(m_solidPS);
    gfx::release(m_paletteLookupPS);
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
    const DWORD started = GetTickCount();

    IDirect3DPixelShader9* compiled[kPassCount] = {};
    m_lastError.clear();
    bool ok = true;
    for (int i = 0; i < kPassCount; i++) {
        if (fromDisk) {
            compiled[i] = gfx::compilePixelShader(device, source, kEntryPoints[i], "tmvs.hlsl", &m_lastError);
        } else {
            const HRESULT hr = device->CreatePixelShader(reinterpret_cast<const DWORD*>(kPrecompiledShaders[i].code), &compiled[i]);
            if (FAILED(hr)) TMVS_LOG("pipeline: CreatePixelShader(%s) failed: 0x%08lX", kEntryPoints[i], static_cast<unsigned long>(hr));
        }
        if (!compiled[i]) ok = false;
    }
    if (!ok) {
        for (auto& shader : compiled) gfx::release(shader);
        if (m_lastError.empty()) m_lastError = "CreatePixelShader failed";
        // The usual cause: a shader longer than the driver allows (ps_3_0: at least 512 slots,
        // many drivers 4096, some 32768).
        D3DCAPS9 caps{};
        device->GetDeviceCaps(&caps);
        TMVS_LOG("pipeline: shader creation failed (driver allows %lu pixel shader instruction slots, PS version %lu.%lu)",
                 caps.MaxPixelShader30InstructionSlots, (caps.PixelShaderVersion >> 8) & 0xFF, caps.PixelShaderVersion & 0xFF);
        return false;
    }
    for (int i = 0; i < kPassCount; i++) {
        gfx::release(m_shaders[i]);
        m_shaders[i] = compiled[i];
    }
    D3DCAPS9 caps{};
    device->GetDeviceCaps(&caps);
    TMVS_LOG("pipeline: %d passes ready (%s, driver allows %lu pixel shader instruction slots) in %lu ms", kPassCount,
             fromDisk ? "compiled from disk" : "precompiled", caps.MaxPixelShader30InstructionSlots, GetTickCount() - started);
    return true;
}

bool Pipeline::ensureTargets(IDirect3DDevice9* device, UINT width, UINT height) {
    if (m_width == width && m_height == height && m_hdr.valid()) return true;
    // RenderScale: a resize between the scaled and the full image keeps the world's state (the
    // 1x1 targets below are kept as they are by create()).
    const bool keep = m_keepWorld && m_hdr.valid();
    destroyTargets(keep);
    const UINT hw = (width + 1) / 2, hh = (height + 1) / 2;
    bool ok = true;
    ok &= m_linearDepth.create(device, width, height, D3DFMT_R32F);
    ok &= m_nd.create(device, width, height, D3DFMT_A16B16G16R16F);
    ok &= m_ndHalf.create(device, hw, hh, D3DFMT_A16B16G16R16F);
    ok &= m_occlusion.create(device, hw, hh, D3DFMT_A8R8G8B8);
    ok &= m_occlusionTmp.create(device, hw, hh, D3DFMT_A8R8G8B8);
    for (auto& t : m_skyAverage) ok &= t.create(device, 1, 1, D3DFMT_A16B16G16R16F);
    if (!keep) m_skyValid = false;
    for (auto& t : m_shelter) ok &= t.create(device, 1, 1, D3DFMT_A16B16G16R16F);
    if (!keep) m_shelterValid = false;
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
    for (auto& t : m_spillHistory) ok &= t.create(device, (width + 3) / 4, (height + 3) / 4, D3DFMT_A16B16G16R16F);
    m_spillValid = false;
    ok &= m_focus[0].create(device, 1, 1, D3DFMT_R32F);
    ok &= m_focus[1].create(device, 1, 1, D3DFMT_R32F);
    ok &= m_dof.create(device, hw, hh, D3DFMT_A16B16G16R16F);
    if (!ok) {
        destroyTargets();
        return false;
    }
    m_width = width;
    m_height = height;
    if (!keep) m_historyValid = false; // the exposure (its 1x1 targets kept)
    m_taaValid = false;
    if (!keep) m_heightValid = false;
    TMVS_LOG("pipeline: render targets created for %ux%u%s", width, height, keep ? " (height map, trail and exposure kept)" : "");
    return true;
}

void Pipeline::destroyTargets(bool keepWorld) {
    // The screen's size: these follow the image.
    m_linearDepth.destroy();
    m_nd.destroy();
    m_ndHalf.destroy();
    m_occlusion.destroy();
    m_occlusionTmp.destroy();
    m_sky.destroy();
    for (auto& t : m_gi) t.destroy();
    for (auto& t : m_giHistory) t.destroy();
    m_hdr.destroy();
    for (auto& t : m_rays) t.destroy();
    for (auto& t : m_bloomDown) t.destroy();
    for (auto& t : m_bloomUp) t.destroy();
    m_lampSource.destroy();
    for (auto& t : m_lampDown) t.destroy();
    for (auto& t : m_lampUp) t.destroy();
    m_lampFailed = false;
    m_ldr.destroy();
    m_ldr2.destroy();
    m_materials.destroy();
    m_materialIds.destroy();
    m_depthCopy.destroy();
    m_clouds.destroy();
    m_reflect.destroy();
    for (auto& t : m_spill) t.destroy();
    for (auto& t : m_spillHistory) t.destroy();
    m_dof.destroy();
    m_dofFull.destroy();
    m_dofFull2.destroy();
    m_dof2.destroy();
    m_hdr2.destroy();
    for (auto& t : m_taa) t.destroy();
    m_scaled.destroy();
    m_fsr.destroy();
    gfx::release(m_splatPoints); // one point per 5 pixels of the depth
    m_taaValid = false;
    m_width = m_height = 0;
    if (keepWorld) return;
    // The world's (or no size at all): RenderScale's resizes keep these.
    for (auto& t : m_skyAverage) t.destroy();
    for (auto& t : m_shelter) t.destroy();
    m_luminance.destroy();
    for (auto& t : m_adapted) t.destroy();
    for (auto& t : m_waterLevel) t.destroy();
    gfx::release(m_waterRead);
    gfx::release(m_waterDone);
    m_waterPending = -1;
    for (auto& t : m_focus) t.destroy();
    m_heightFrame.destroy();
    for (auto& t : m_heightMap) t.destroy();
    for (auto& t : m_ceilingMap) t.destroy();
    m_shadowHeight.destroy();
    for (auto& t : m_lightGrid) t.destroy();
    gfx::release(m_heightDepth);
    gfx::release(m_cloudNoise);
    gfx::release(m_lut);
    gfx::release(m_rainVB);
    gfx::release(m_rainIB);
    gfx::release(m_trailVB);
    gfx::release(m_trailIB);
    m_trailPoints.destroy();
    m_trailReset = true;
    m_heightValid = false;
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

// PS_Focus's c33: the car to focus on (DofAutofocus Car), w = 0 for the middle of the screen
// as before. Always set: c33 holds whatever the pass before left there. A click in the photo
// mode and a set focus distance win. The log says once per change whether the engine gave the
// car (whether its position reaches replays is the open question).
void Pipeline::focusOnCarConstants(IDirect3DDevice9* device, const Inputs& in, const Settings& s) {
    const bool wanted = s.dofAutofocus == 1 && s.focusDistance <= 0.0f && in.focusPoint[0] < 0.0f;
    const bool onCar = wanted && in.focusCarKnown;
    const float car[4] = {in.focusCar[0], in.focusCar[1], in.focusCar[2], onCar ? 1.0f : 0.0f};
    device->SetPixelShaderConstantF(33, car, 1);
    const int state = !wanted ? -1 : (onCar ? 1 : 0);
    // At most 20 lines: in a replay whose car position comes and goes it would change every frame.
    if (wanted && state != m_focusOnCar && m_focusLogs < 20) {
        m_focusLogs++;
        TMVS_LOG("pipeline: depth of field focuses %s%s", onCar ? "on the car (engine position)" : "on the middle (no car position from the engine)",
                 m_focusLogs == 20 ? " (further changes not logged)" : "");
    }
    m_focusOnCar = state;
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

void Pipeline::blackHoleDirection(const Settings& s, float toward[3]) {
    const float az = s.blackHoleAzimuth * 3.14159265f / 180.0f, el = s.blackHoleElevation * 3.14159265f / 180.0f;
    const float rot = s.skyRotation * 3.14159265f / 180.0f;
    const float d[3] = {sinf(az) * cosf(el), sinf(el), cosf(az) * cosf(el)};
    toward[0] = cosf(rot) * d[0] - sinf(rot) * d[2];
    toward[1] = d[1];
    toward[2] = sinf(rot) * d[0] + cosf(rot) * d[2];
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

    // The black hole of the space skies (it turns with the sky). Where there is one it is the
    // light of the scene instead of the sun: shafts, shadows, flare and glow come from it.
    const bool blackHole = blackHoleLights(s);
    float hole[3];
    blackHoleDirection(s, hole);
    const float holeConstants[4] = {hole[0], hole[1], hole[2], blackHole ? s.skyEffectSize : 0.0f};
    device->SetPixelShaderConstantF(40, holeConstants, 1);

    // Sun: world -> view is the row-vector product with the view rotation.
    float sw[3] = {in.sunDirection[0], in.sunDirection[1], in.sunDirection[2]};
    if (blackHole) memcpy(sw, hole, sizeof(sw));
    float len = sqrtf(sw[0] * sw[0] + sw[1] * sw[1] + sw[2] * sw[2]);
    if (len > 1e-5f) for (float& v : sw) v /= len;
    float sv[3];
    for (int j = 0; j < 3; j++) sv[j] = sw[0] * V[0 * 4 + j] + sw[1] * V[1 * 4 + j] + sw[2] * V[2 * 4 + j];
    // Night skies replace the sun: no sun light, shadows or shafts (the black hole brings its
    // own). On night maps the game's "sun" is the moon: soft light, no shafts or flare.
    const bool ringWorld = s.skyMode == 5; // space sky, lit by the game's sun without a black hole
    const bool nightSky = s.skyMode >= 2 && !ringWorld;
    const bool mapNight = in.sunColorKnown && classifyMood(in.sunColor) == Mood::Night;
    const bool gameNight = mapNight && !blackHole; // the moon lights the scene
    const bool sunKnown = blackHole || (in.sunKnown && len > 1e-5f && !nightSky);
    // A bigger (closer) black hole shines brighter.
    const float daylight = blackHole ? fminf(0.3f + 0.12f * s.skyEffectSize, 1.0f)
                                     : smoothstepf(-0.08f, 0.18f, sw[1]) * (gameNight ? 0.35f : 1.0f);
    c[6][0] = sv[0];
    c[6][1] = sv[1];
    c[6][2] = sv[2];
    c[6][3] = sunKnown ? 1.0f : 0.0f;
    c[7][0] = sw[0];
    c[7][1] = sw[1];
    c[7][2] = sw[2];
    c[7][3] = sunKnown ? daylight : 1.0f;
    for (int j = 0; j < 3; j++) m_sunWorld[j] = sw[j];
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
    // The black hole's light: the warm white of its inner disk.
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
    c[15][1] = s.autoExposure * (nightSky || mapNight ? 0.3f : 1.0f); // night stays dark
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
    // A black hole lights the scene like a dim, warm sun: less dark than a starry night.
    c[20][1] = s.skyNight >= 0.0f ? s.skyNight
                                  : (nightSky ? (mapNight || blackHole ? 0.35f : 0.95f) : (ringWorld && !mapNight ? 0.35f : 0.0f));
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
    c[35][2] = m_heightWorld;
    c[35][3] = s.longShadowRange;
    c[36][0] = s.neonLight;
    // The game's sun, even under a night sky: 1 known, 0 unknown, -1 the map is a night map
    // (its light is the moon: no sun glare to tell from lamps, nothing baked to take out).
    // EngineMoon: the game's light is the night's, moved to the black hole or off, whatever its
    // colour says (a sunset map lit at midnight keeps a warm, "dusk" moon colour): no sun baked
    // to take out, nothing white is glare. Only this flag: the exposure and grade stay.
    c[36][1] = mapNight || in.moonReplaced ? -1.0f : (in.sunKnown && len > 1e-5f ? 1.0f : 0.0f);
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
    // w: how much farther AO, shadows, bounce light and reflections reach (photo mode).
    // No sea level from the game (TMUF Island): the one measured from its water material.
    // Stadium water blocks (2): once the water material was seen on the map, water only on
    // it (3; dirt lies at the blocks' height too).
    const bool measured = in.water[2] >= 0.0f && in.water[2] < 0.5f && m_seaKnown;
    float kind = measured ? 1.0f : in.water[2];
    if (kind > 1.5f && m_waterMaterialSeen && m_materialsOn) kind = 3.0f;
    const float water[4] = {measured ? m_seaLevel : in.water[0], measured ? m_seaLevel : in.water[1], kind, in.photo ? 3.0f : 1.0f};
    device->SetPixelShaderConstantF(38, water, 1);
    // The direction the snow drifts (as the flakes in drawRain): walls facing it catch snow.
    const float drift[2] = {1.0f + s.wind * 7.0f, 0.5f + s.wind * 3.0f};
    const float driftLength = sqrtf(drift[0] * drift[0] + drift[1] * drift[1]);
    const float snow[4] = {s.snow, s.snowCover, drift[0] / driftLength, drift[1] / driftLength};
    device->SetPixelShaderConstantF(39, snow, 1);
    const float sky3[4] = {s.auroraSpeed, s.lensDrops ? fminf(s.snow, 1.5f) : 0.0f, s.reflectionBlur, 1.0f / fmaxf(s.hazeDistance, 0.1f)};
    device->SetPixelShaderConstantF(41, sky3, 1);
    // Day-for-night: how much of the game's baked sun comes out, and where that sun is (the
    // game's own, also under a black hole sky, whose light is the hole). Every night sky over a
    // day map takes it out fully; the scene's darkness (u_Sky.y) is a separate thing (a black
    // hole lights the scene like a dim sun, but not from where the game's sun was).
    float gameSunDir[3] = {in.sunDirection[0], in.sunDirection[1], in.sunDirection[2]};
    const float gameSunLen = sqrtf(gameSunDir[0] * gameSunDir[0] + gameSunDir[1] * gameSunDir[1] + gameSunDir[2] * gameSunDir[2]);
    if (gameSunLen > 1e-5f) for (float& v : gameSunDir) v /= gameSunLen;
    const float removal = c[20][1]; // (as before: the shader uses u_Sky.y; kept for later)
    const float sunOut[4] = {removal, gameSunDir[0], gameSunDir[1], gameSunDir[2]};
    device->SetPixelShaderConstantF(43, sunOut, 1);
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
float4 c_Texel : register(c7);  // full-res texel size of the depth (x, y)
float4 c_Mode  : register(c8);  // x: 1 = undersides (roofs, bridges) instead of top surfaces, y: max depth, z: 1 = keep the car out
sampler2D s_depth : register(s0);
struct VSOut { float4 pos : POSITION; float2 data : TEXCOORD0; float size : PSIZE; };
// World position of the depth at uv. D3D9 pixel centre: the game drew texel centre uv at
// ndc 2(uv - texel / 2) - 1.
float3 worldAt(float2 uv, out float z) {
    z = tex2Dlod(s_depth, float4(uv, 0, 0)).r;
    float2 ndc = float2(uv.x * 2.0 - 1.0 - c_Texel.x, 1.0 - uv.y * 2.0 + c_Texel.y);
    float3 v = float3((ndc.x - c_Proj.z) * z / c_Proj.x, (ndc.y - c_Proj.w) * z / c_Proj.y, z);
    return float3(c_V0.w, c_V1.w, c_V2.w) + v.x * c_V0.xyz + v.y * c_V1.xyz + v.z * c_V2.xyz;
}
// Height of what is seen at uv if it is close (the road beside and behind the car), else very high.
float groundAt(float2 uv) {
    float zz;
    float3 g = worldAt(uv, zz);
    return zz > 0.3 && zz < 24.0 ? g.y : 1e5;
}
VSOut main(float2 uv : TEXCOORD0) {
    VSOut o;
    float z;
    float3 w = worldAt(uv, z);
    float2 m = (w.xz - c_Map.xy) * c_Map.z;
    // Beyond ~400 m the depth is too coarse to be useful.
    bool valid = z > 0.3 && z < c_Mode.y && all(m > 0.0) && all(m < 1.0);
    if (valid && c_Mode.x > 0.5) {
        // Undersides only: the surface faces down (normal from the neighbouring depths, on
        // one surface - not across an edge).
        float zx, zy;
        float3 wx = worldAt(uv + float2(c_Texel.x * 2.0, 0.0), zx);
        float3 wy = worldAt(uv + float2(0.0, c_Texel.y * 2.0), zy);
        float3 n = cross(wx - w, wy - w);
        float tol = 0.03 * z + 0.05;
        valid = n.y < -0.7 * length(n) && abs(zx - z) < tol && abs(zy - z) < tol;
    }
    // The player's car (close, in the middle of the lower screen) must not cast long
    // shadows: it moves on and would leave stale copies of itself behind (dark car shapes
    // in the haze, shadows of a car that is gone). Only what stands up to a car's height
    // above the road there: excluding everything close left holes in the map (an arch just
    // driven under, the road) that read as "nothing casts a shadow here", so a low light
    // drew bright screen-sized boxes onto the road.
    float road = min(min(groundAt(float2(0.3, 0.92)), groundAt(float2(0.7, 0.92))), groundAt(float2(0.5, 0.99)));
    // No road close below (in the air): everything close there, as before.
    bool carHeight = road > 1e4 || (w.y > road + 0.35 && w.y < road + 2.2);
    if (c_Mode.z > 0.5 && z < 16.0 && uv.x > 0.25 && uv.x < 0.75 && uv.y > 0.35 && carHeight) valid = false;
    // Also wherever the car is on screen: a car-sized box around the closest surface below
    // the screen centre (found like the spray and the snow do).
    float z0 = tex2Dlod(s_depth, float4(0.5, 0.6, 0, 0)).r;
    float z1 = tex2Dlod(s_depth, float4(0.5, 0.65, 0, 0)).r;
    float z2 = tex2Dlod(s_depth, float4(0.5, 0.7, 0, 0)).r;
    float cz = min(z0, min(z1, z2));
    float3 fwd = c_V2.xyz; // view forward in the world
    if (valid && c_Mode.z > 0.5 && cz > 1.5 && cz < 16.0 && abs(fwd.y) < 0.45) {
        float cy = cz == z0 ? 0.6 : (cz == z1 ? 0.65 : 0.7);
        float3 cv = float3((0.0 - c_Proj.z) * cz / c_Proj.x, (1.0 - cy * 2.0 - c_Proj.w) * cz / c_Proj.y, cz);
        float3 tail = float3(c_V0.w, c_V1.w, c_V2.w) + cv.x * c_V0.xyz + cv.y * c_V1.xyz + cv.z * c_V2.xyz;
        float3 f = normalize(float3(fwd.x, 0.0, fwd.z) + 1e-5);
        float3 d = w - (tail + f * 1.6);
        // Only at the height of the point found (the car's body): the road around it stays in
        // the map, also when that point is the car's nose and the box reaches over the road.
        // A point on the road itself is no car: no box (it cut a hole into the road around it).
        if (tail.y > road + 0.2 && abs(dot(d, f)) < 3.0 && abs(dot(d, float3(f.z, 0.0, -f.x))) < 1.6 && d.y > -0.25 && d.y < 2.0) valid = false;
    }
    o.pos = valid ? float4(m.x * 2.0 - 1.0 + c_Half.x, 1.0 - m.y * 2.0 + c_Half.y, saturate(0.5 - (w.y - c_Map.w) / 1000.0), 1.0)
                  : float4(-10.0, -10.0, 0.0, 1.0);
    o.data = float2(w.y + 10000.0, 0.0);
    // Far ground seen at a grazing angle: the samples (every 5th pixel row) land metres apart
    // and leave empty stripes in the map, which a low light draws as striped shadow edges.
    // Each point covers the gap to the next row - only on one surface (about the same
    // height), so wall tops don't spread onto the floor beside them.
    o.size = 1.0;
    if (valid && c_Mode.x < 0.5 && z > 25.0) {
        float zn;
        float3 wn = worldAt(uv + float2(0.0, c_Texel.y * 5.0), zn);
        float gap = length(wn.xz - w.xz) * c_Map.z / c_Half.y; // in map texels
        if (abs(wn.y - w.y) < 0.15 * gap / (c_Map.z / c_Half.y) + 0.3) o.size = clamp(gap, 1.0, 6.0);
    }
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
        // Bilinear heights give the long shadows smooth edges instead of 0.6 m stairs
        // (stretched along a low light). Point sampling where the format can't be filtered.
        m_heightFilter = d3d && SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format, D3DUSAGE_QUERY_FILTER,
                                                                  D3DRTYPE_TEXTURE, D3DFMT_R32F));
        if (d3d) d3d->Release();
        TMVS_LOG("pipeline: height map %s", m_heightFilter ? "filtered (smooth long-shadow edges)" : "point sampled, jittered");
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
    // Only the splat points went with a resize (keepWorldOnResize): the maps hold on.
    const bool mapsKept = m_heightFrame.valid() && m_heightMap[0].valid() && m_heightMap[1].valid() && m_heightDepth;
    bool ok = m_heightFrame.create(device, m_heightSize, m_heightSize, D3DFMT_R32F);
    ok &= m_heightMap[0].create(device, m_heightSize, m_heightSize, D3DFMT_R32F);
    ok &= m_heightMap[1].create(device, m_heightSize, m_heightSize, D3DFMT_R32F);
    ok &= m_ceilingMap[0].create(device, m_heightSize, m_heightSize, D3DFMT_R32F);
    ok &= m_ceilingMap[1].create(device, m_heightSize, m_heightSize, D3DFMT_R32F);
    ok &= m_shadowHeight.create(device, m_heightSize / 2, m_heightSize / 2, D3DFMT_R32F);
    if (ok && !m_heightDepth) {
        ok = SUCCEEDED(device->CreateDepthStencilSurface(m_heightSize, m_heightSize, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE,
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
    if (!mapsKept || !ok) m_heightValid = false;
    m_shadowHeightStale = true;
    return ok;
}

void Pipeline::updateHeightMap(IDirect3DDevice9* device, const Inputs& in, const Settings& s, bool ceilings) {
    const float* V = in.view;
    const float* P = in.projection;
    float camera[3];
    for (int j = 0; j < 3; j++) camera[j] = -(V[12] * V[j * 4 + 0] + V[13] * V[j * 4 + 1] + V[14] * V[j * 4 + 2]);

    // Keep the camera near the middle; move the map in whole texels so history lines up.
    const float texel = m_heightWorld / m_heightSize;
    const float snap = texel * 16.0f;
    float shift[2] = {0.0f, 0.0f};
    const float cx = m_heightOrigin[0] + m_heightWorld * 0.5f, cz = m_heightOrigin[1] + m_heightWorld * 0.5f;
    if (!m_heightValid || fabsf(camera[0] - cx) > 40.0f || fabsf(camera[2] - cz) > 40.0f) {
        const float ox = floorf((camera[0] - m_heightWorld * 0.5f) / snap) * snap;
        const float oz = floorf((camera[2] - m_heightWorld * 0.5f) / snap) * snap;
        shift[0] = (ox - m_heightOrigin[0]) / m_heightWorld;
        shift[1] = (oz - m_heightOrigin[1]) / m_heightWorld;
        m_heightOrigin[0] = ox;
        m_heightOrigin[1] = oz;
    }

    // 1. Splat this frame's depth, 2. merge it into the persistent map. Twice with shelter:
    // the top surfaces (shadows, splashes, snow), then the undersides of roofs and bridges,
    // which the floor seen below them never wears down (rain and snow stay out there).
    const int previous = m_heightIndex;
    m_heightIndex ^= 1;
    for (int pass = 0; pass < (ceilings ? 2 : 1); pass++) {
        IDirect3DSurface9* savedDepth = nullptr;
        device->GetDepthStencilSurface(&savedDepth);
        device->SetRenderTarget(0, m_heightFrame.surface);
        device->SetDepthStencilSurface(m_heightDepth);
        D3DVIEWPORT9 viewport = {0, 0, m_heightSize, m_heightSize, 0.0f, 1.0f};
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
        float vc[7][4] = {};
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
        vc[4][2] = 1.0f / m_heightWorld;
        vc[4][3] = camera[1];
        vc[5][0] = -1.0f / m_heightSize;
        vc[5][1] = 1.0f / m_heightSize;
        vc[6][0] = 1.0f / m_linearDepth.width;
        vc[6][1] = 1.0f / m_linearDepth.height;
        device->SetVertexShaderConstantF(1, &vc[0][0], 7);
        // Beyond ~400 m the depth is too coarse to be useful (photo mode: stills, the map is larger).
        const float mode[4] = {static_cast<float>(pass), in.photo ? 1200.0f : 400.0f, in.photo ? 0.0f : 1.0f, 0.0f};
        device->SetVertexShaderConstantF(8, mode, 1);
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
        gfx::Target* map = pass ? m_ceilingMap : m_heightMap;
        const bool fresh = pass ? !m_ceilingValid : !m_heightValid;
        bind(device, 0, map[previous].texture, false);
        bind(device, 1, m_heightFrame.texture, false);
        bind(device, 2, m_linearDepth.texture, false); // what the camera sees past (a lower column)
        passConstants(device, shift[0], shift[1], fresh ? 1.0f : 0.0f, pass ? 0.0f : 0.08f);
        const float texelSize[4] = {1.0f / m_heightSize, m_heightOrigin[0], m_heightOrigin[1], m_heightWorld};
        device->SetPixelShaderConstantF(33, texelSize, 1);
        runPass(device, kHeightMerge, map[m_heightIndex]);
    }
    m_ceilingValid = ceilings;
    m_heightValid = true;

    // The frame constants went up before this re-centre: with the old origin, the shadow and
    // volumetric passes read the shifted map ~40 m off for one frame (shadows popping up).
    const float map[4] = {m_heightOrigin[0], m_heightOrigin[1], m_heightWorld, s.longShadowRange};
    device->SetPixelShaderConstantF(35, map, 1);
}

// LongShadowQuality 1: the height map turned so that +x points to the light, then swept towards
// it (see longShadowGrid in tmvs.hlsl). Only when the map, the light or the range changed (the
// map: every other frame). Returns false where the GPU lacks the format: the trace stays.
bool Pipeline::updateLightGrid(IDirect3DDevice9* device, const Settings& s) {
    if (!m_lightGridSupported) return false;
    if (!m_lightGrid[0].valid()) {
        IDirect3D9* d3d = nullptr;
        device->GetDirect3D(&d3d);
        D3DDEVICE_CREATION_PARAMETERS cp{};
        device->GetCreationParameters(&cp);
        D3DDISPLAYMODE mode{};
        device->GetDisplayMode(0, &mode);
        const bool target = d3d && SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format, D3DUSAGE_RENDERTARGET,
                                                                     D3DRTYPE_TEXTURE, D3DFMT_G32R32F));
        m_lightGridFilter = d3d && SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format, D3DUSAGE_QUERY_FILTER,
                                                                     D3DRTYPE_TEXTURE, D3DFMT_G32R32F));
        if (d3d) d3d->Release();
        bool ok = target && m_lightGrid[0].create(device, m_heightSize, m_heightSize, D3DFMT_G32R32F) &&
                  m_lightGrid[1].create(device, m_heightSize, m_heightSize, D3DFMT_G32R32F);
        TMVS_LOG("pipeline: long-shadow light grid %s", ok ? (m_lightGridFilter ? "ready (filtered)" : "ready (point sampled)")
                                                           : "unavailable (no G32R32F target) - long shadows are traced in steps");
        if (!ok) {
            for (auto& t : m_lightGrid) t.destroy();
            m_lightGridSupported = false;
            return false;
        }
        m_lightGridStale = true;
    }
    // The light's horizontal direction is the grid's x. Straight overhead it has none: any will do,
    // nothing is far from its caster then.
    const float sx = m_sunWorld[0], sy = m_sunWorld[1], sz = m_sunWorld[2];
    const float horizontal = sqrtf(sx * sx + sz * sz);
    const float lx = horizontal > 1e-3f ? sx / horizontal : 1.0f, lz = horizontal > 1e-3f ? sz / horizontal : 0.0f;
    const float ch = fmaxf(horizontal, 1e-3f);
    // The trace's margin: it grows by this much per metre along the ray (more under a low light).
    const float margin = 0.012f + 0.02f * fminf(fmaxf(1.0f - sy * 3.0f, 0.0f), 1.0f);
    const float texel = m_heightWorld / m_heightSize;
    // The corner around the height map's middle, in whole cells along and across the light, so the
    // cells keep their place while the map stays put.
    const float mx = m_heightOrigin[0] + m_heightWorld * 0.5f, mz = m_heightOrigin[1] + m_heightWorld * 0.5f;
    const float along = floorf((mx * lx + mz * lz - m_heightWorld * 0.5f) / texel) * texel;
    const float across = floorf((-mx * lz + mz * lx - m_heightWorld * 0.5f) / texel) * texel;
    const float grid[2][4] = {{lx, lz, along, across}, {1.0f / m_heightWorld, ch, margin, s.longShadowRange}};
    const bool moved = memcmp(grid, m_lightGridConstants, sizeof(grid)) != 0;
    if (!m_lightGridStale && !moved) return true;
    memcpy(m_lightGridConstants, grid, sizeof(grid));
    m_lightGridStale = false;

    device->SetPixelShaderConstantF(52, &grid[0][0], 2);
    bind(device, 0, m_heightMap[m_heightIndex].texture, false);
    passConstants(device, 0.5f / m_heightSize, 0.0f);
    runPass(device, kLightGridInit, m_lightGrid[0]);
    // Sweeps of four: each cell takes the best of the 4 windows after it, so the window grows
    // x4 a sweep; the last one is cut to the range (its windows overlap: a maximum doesn't mind).
    // Capped: an absurd range from a hand-edited file would overflow window * 4 and never end.
    const float wanted = ceilf(s.longShadowRange * ch / texel);
    const int cells = wanted < 1048576.0f ? static_cast<int>(wanted) : 1048576;
    int window = 1, index = 0;
    while (window < cells) {
        const int next = window * 4 < cells ? window * 4 : cells;
        float offset[3];
        for (int j = 0; j < 3; j++) offset[j] = static_cast<float>(std::min((j + 1) * window, next - window)) / m_heightSize;
        // How far the ray climbs (and its margin grows) per uv unit along the grid.
        const float drop = (sy + margin) / ch * m_heightWorld;
        bind(device, 0, m_lightGrid[index].texture, false);
        passConstants(device, offset[0], offset[1], offset[2], drop);
        const float metres[4] = {m_heightWorld, 0.0f, 0.0f, 0.0f};
        device->SetPixelShaderConstantF(33, metres, 1);
        runPass(device, kLightGridSweep, m_lightGrid[index ^ 1]);
        index ^= 1;
        window = next;
    }
    m_lightGridIndex = index;
    return true;
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

void Pipeline::setLut(const std::vector<uint32_t>& texels, int size) {
    gfx::release(m_lut);
    const bool valid = size >= 2 && texels.size() == static_cast<size_t>(size) * size * size;
    m_lutTexels = valid ? texels : std::vector<uint32_t>();
    m_lutSize = valid ? size : 0;
}

bool Pipeline::ensureLut(IDirect3DDevice9* device) {
    if (m_lut) return true;
    if (m_lutSize == 0) return false;
    const int size = m_lutSize;
    IDirect3DVolumeTexture9* staging = nullptr;
    if (FAILED(device->CreateVolumeTexture(size, size, size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &staging, nullptr))) return false;
    D3DLOCKED_BOX box{};
    if (SUCCEEDED(staging->LockBox(0, &box, nullptr, 0))) {
        for (int z = 0; z < size; z++) {
            for (int y = 0; y < size; y++) {
                memcpy(static_cast<BYTE*>(box.pBits) + static_cast<size_t>(z) * box.SlicePitch + static_cast<size_t>(y) * box.RowPitch,
                       &m_lutTexels[(static_cast<size_t>(z) * size + y) * size], size * 4);
            }
        }
        staging->UnlockBox(0);
    }
    bool ok = SUCCEEDED(device->CreateVolumeTexture(size, size, size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_lut, nullptr)) &&
              SUCCEEDED(device->UpdateTexture(staging, m_lut));
    staging->Release();
    if (!ok) {
        gfx::release(m_lut);
        m_lutSize = 0; // don't try every frame
    }
    return ok;
}

// One colour (the material debug view, the class mask), and material id -> its class's colour.
const char* const kSolidPS = "float4 c : register(c32); float4 main() : COLOR0 { return c; }";
const char* const kPalettePS = "sampler2D ids : register(s0); sampler2D palette : register(s1);\n"
                               "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
                               "    float id = floor(tex2D(ids, uv).r * 255.0 + 0.5);\n"
                               "    return tex2D(palette, float2((id + 0.5) / 256.0, 0.5));\n"
                               "}\n";

bool Pipeline::buildMaterialMask(IDirect3DDevice9* device, const Inputs& in) {
    // The game's depth-stencil is our INTZ texture; its stencil holds the material class of
    // every pixel (written while the game drew, see the plugin). Bound as the depth-stencil
    // again for a stencil test per class (no texture of ours may be bound meanwhile). Not in
    // the previewer: its depth is a plain R32F texture.
    if (in.materials && in.materialMask) return true; // the previewer: from capture_NNN.mat
    // RenderScale: the depth is a copy of the scene's corner, the stencil is the game's own.
    IDirect3DTexture9* const stencil = in.stencil ? in.stencil : in.depth;
    D3DSURFACE_DESC desc{};
    if (!in.materials || FAILED(stencil->GetLevelDesc(0, &desc)) || desc.Format != static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z')))
        return false;
    if (in.materialClasses) return buildMaterialIds(device, in);
    if (!m_materials.create(device, in.width, in.height, D3DFMT_A8R8G8B8)) return false;
    if (!m_solidPS) m_solidPS = gfx::compilePixelShader(device, kSolidPS, "main", "Solid");
    if (!m_solidPS) return false;
    for (int i = 0; i < 16; i++) device->SetTexture(i, nullptr);
    IDirect3DSurface9* ds = nullptr;
    if (FAILED(stencil->GetSurfaceLevel(0, &ds)) || !ds) return false;
    device->SetRenderTarget(0, m_materials.surface);
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
    device->SetDepthStencilSurface(ds);
    device->SetPixelShader(m_solidPS);
    device->SetRenderState(D3DRS_STENCILENABLE, TRUE);
    device->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
    device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
    device->SetRenderState(D3DRS_STENCILMASK, 0x07);
    device->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
    // r light, g car 1 / glass 0.5 / grass 0.25, b water, a light added on top.
    for (int id = 1; id <= kMaterialClasses; id++) {
        const float color[4] = {id == 1 ? 1.0f : 0.0f, id == 2 ? 1.0f : (id == 3 ? 0.5f : (id == 4 ? 0.25f : 0.0f)), id == 5 ? 1.0f : 0.0f, 0.0f};
        device->SetPixelShaderConstantF(32, color, 1);
        device->SetRenderState(D3DRS_STENCILREF, static_cast<DWORD>(id));
        m_quad.draw(device, in.width, in.height);
    }
    device->SetRenderState(D3DRS_STENCILMASK, kAddedLightBit);
    device->SetRenderState(D3DRS_STENCILREF, kAddedLightBit);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_ALPHA);
    const float added[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    device->SetPixelShaderConstantF(32, added, 1);
    m_quad.draw(device, in.width, in.height);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
    device->SetRenderState(D3DRS_STENCILMASK, 0xFF);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetDepthStencilSurface(nullptr);
    ds->Release();
    return true;
}

// F12's frame: the stencil holds material ids (1..255). One pass per stencil bit adds that
// bit to m_materialIds (r = id / 255), then a palette (256 x 1, the class colour of each id)
// turns the ids into the usual class mask. No added-light bit in this frame.
bool Pipeline::buildMaterialIds(IDirect3DDevice9* device, const Inputs& in) {
    if (!m_materials.create(device, in.width, in.height, D3DFMT_A8R8G8B8)) return false;
    if (!m_materialIds.create(device, in.width, in.height, D3DFMT_A8R8G8B8)) return false;
    if (!m_solidPS) m_solidPS = gfx::compilePixelShader(device, kSolidPS, "main", "Solid");
    if (!m_paletteLookupPS)
        m_paletteLookupPS = gfx::compilePixelShader(device, kPalettePS, "main", "MaterialPalette");
    if (!m_solidPS || !m_paletteLookupPS) return false;
    // Filled in system memory and uploaded (no managed pool on D3D9Ex).
    IDirect3DTexture9* staging = nullptr;
    IDirect3DTexture9* palette = nullptr;
    if (FAILED(device->CreateTexture(256, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &staging, nullptr))) return false;
    D3DLOCKED_RECT locked{};
    if (SUCCEEDED(staging->LockRect(0, &locked, nullptr, 0))) {
        uint32_t* texels = static_cast<uint32_t*>(locked.pBits);
        for (int id = 0; id < 256; id++) {
            const int c = in.materialClasses[id];
            const uint32_t r = c == 1 ? 255u : 0u, g = c == 2 ? 255u : (c == 3 ? 128u : (c == 4 ? 64u : 0u)), b = c == 5 ? 255u : 0u;
            texels[id] = (r << 16) | (g << 8) | b;
        }
        staging->UnlockRect(0);
    }
    const bool uploaded = SUCCEEDED(device->CreateTexture(256, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &palette, nullptr)) &&
                          SUCCEEDED(device->UpdateTexture(staging, palette));
    staging->Release();
    if (!uploaded) {
        gfx::release(palette);
        return false;
    }
    for (int i = 0; i < 16; i++) device->SetTexture(i, nullptr);
    IDirect3DSurface9* ds = nullptr;
    if (FAILED((in.stencil ? in.stencil : in.depth)->GetSurfaceLevel(0, &ds)) || !ds) {
        palette->Release();
        return false;
    }
    device->SetRenderTarget(0, m_materialIds.surface);
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
    device->SetDepthStencilSurface(ds);
    device->SetPixelShader(m_solidPS);
    device->SetRenderState(D3DRS_STENCILENABLE, TRUE);
    device->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
    device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
    device->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    for (int bit = 0; bit < 8; bit++) {
        const float color[4] = {static_cast<float>(1 << bit) / 255.0f, 0.0f, 0.0f, 0.0f};
        device->SetPixelShaderConstantF(32, color, 1);
        device->SetRenderState(D3DRS_STENCILMASK, 1u << bit);
        device->SetRenderState(D3DRS_STENCILREF, 1u << bit);
        m_quad.draw(device, in.width, in.height);
    }
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILMASK, 0xFF);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetDepthStencilSurface(nullptr);
    ds->Release();
    device->SetRenderTarget(0, m_materials.surface);
    device->SetPixelShader(m_paletteLookupPS);
    bind(device, 0, m_materialIds.texture, false);
    bind(device, 1, palette, false);
    m_quad.draw(device, in.width, in.height);
    device->SetTexture(0, nullptr);
    device->SetTexture(1, nullptr);
    palette->Release();
    return true;
}

bool Pipeline::readMaterialIds(IDirect3DDevice9* device, std::vector<uint8_t>& out) {
    std::vector<uint32_t> texels;
    if (!m_materialIds.surface || !readColor(device, m_materialIds.surface, texels)) return false;
    out.resize(texels.size());
    for (size_t i = 0; i < texels.size(); i++) out[i] = static_cast<uint8_t>((texels[i] >> 16) & 0xFF);
    return true;
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
float4 c_Map    : register(c9);  // height map corner x, z, 1 / world size, 1 = shelter (map valid)
float4 c_Prev0  : register(c10); // world -> previous frame's view, column 0
float4 c_Prev1  : register(c11);
float4 c_Prev2  : register(c12);
float4 c_PrevP  : register(c13); // previous P00, P11, P20, P21
float4 c_Motion : register(c14); // shutter (frames), previous frame valid, frame time (s), max streak (ndc)
float4 c_Spray  : register(c15); // speed (m/s), spray amount, 0, 0
sampler2D s_height : register(s0);
sampler2D s_depth  : register(s1);
sampler2D s_ceiling : register(s3); // undersides of roofs and bridges (0 = none seen)
struct VSOut { float4 pos : POSITION; float4 data : TEXCOORD0; };

float3 toView(float3 w) { float4 p = float4(w, 1.0); return float3(dot(p, c_V0), dot(p, c_V1), dot(p, c_V2)); }
float4 toClip(float3 v) {
    float4 c = float4(v.x * c_Proj.x + v.z * c_Proj.z, v.y * c_Proj.y + v.z * c_Proj.w, v.z * c_Proj2.x + c_Proj2.y, v.z);
    c.xy += float2(-c_Proj2.z, c_Proj2.w) * 0.5 * c.w; // D3D9 half-pixel offset
    return c;
}

// Under roofs and bridges, behind walls: the way this drop came - back up along its
// velocity, against the wind, 40 m - is blocked. The height map holds the highest surface
// seen per column, the ceiling map the undersides of roofs and bridges. So a slanting wind
// still carries rain in under a high roof, and between walls it only comes from above.
// `jitter` (0..1, per drop) moves the samples: the dry area has a ragged edge.
bool sheltered(float3 w, float3 velocity, float jitter) {
    if (c_Map.w < 0.5) return false;
    float3 d = -normalize(velocity);
    float3 p = w;
    bool under = false;
    [loop] for (int i = 1; i <= 7; i++) {
        float t = (i + jitter - 0.5) / 7.0;
        float3 q = w + d * (40.0 * t * t);
        float2 m = (q.xz - c_Map.xy) * c_Map.z;
        if (any(m < 0.0) || any(m > 1.0)) return false;
        float top = tex2Dlod(s_height, float4(m, 0, 0)).r - 10000.0;
        float roof = tex2Dlod(s_ceiling, float4(m, 0, 0)).r - 10000.0; // -10000: none
        under = roof > -9000.0 && q.y < roof;
        if (roof > -9000.0 && p.y < roof && q.y >= roof) return true; // through a roof
        if (!under && q.y < top - 0.3) return true;                    // through a wall, a block
        p = q;
    }
    return under; // still under a roof 40 m on: deep inside
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
    if (va.z < 0.4 || sheltered(w, velocity, frac(seed.w * 5.17))) return o;
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
    float2 suv = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5) + c_Proj2.zw * 0.5; // toClip's draw shift -> texel centre
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

)"
// (Split in two: MSVC limits one string literal to 16 KB.)
R"(// Snow: a box around the camera that is fixed in the world (wrapped like the rain), each
// flake drifting with the wind and fluttering. Flakes rushing past the camera stretch into
// short streaks (where they were a frame ago); those right at the lens are out of focus.
float4 c_SnowWind : register(c18); // wind x, z (m/s), fall speed (m/s), 0
struct SnowOut { float4 pos : POSITION; float4 data : TEXCOORD0; float len : TEXCOORD1; };

SnowOut snowVS(float4 seed : TEXCOORD0, float2 corner : TEXCOORD1) {
    SnowOut o;
    o.pos = float4(0, 0, -2, 1);
    o.data = 0;
    o.len = 0;
    // The box sits mostly ahead of the camera (wrapped in the world like the rain, so moving
    // it doesn't move the flakes); farther snow is the screen-space veil.
    float3 box = float3(28.0, 16.0, 28.0);
    float3 centre = c_Cam.xyz + normalize(float3(c_V2.x, 0.0, c_V2.z) + 1e-5) * 9.0;
    float t = c_Cam.w;
    float gust = 0.75 + 0.25 * sin(t * 0.6 + seed.x * 2.0) + 0.15 * sin(t * 1.7 + seed.z * 5.0);
    float3 velocity = float3(c_SnowWind.x * gust, -c_SnowWind.z * (0.7 + 0.6 * seed.w), c_SnowWind.y * gust);
    float3 local = (frac((seed.xyz * box + velocity * t - centre) / box) - 0.5) * box;
    // Flutter: each flake sways on its own circle, some calmly, some tumbling.
    float phase = seed.w * 40.0;
    float sway = 0.12 + 0.35 * frac(seed.x * 17.3);
    local.xz += float2(sin(t * (1.3 + seed.x) + phase), cos(t * (1.1 + seed.z) + phase)) * sway;
    float3 w = centre + local;
    float3 va = toView(w);
    if (va.z < 0.25 || sheltered(w, velocity, frac(seed.w * 5.17))) return o;
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
    // Flakes of 6 - 20 mm, every tenth a clump of up to 4 cm; closer than ~1.5 m they blur
    // into larger, fainter discs.
    float kind = frac(seed.w * 13.7);
    float sizeM = kind > 0.9 ? 0.025 + 0.15 * (kind - 0.9) : 0.006 + 0.016 * kind / 0.9;
    float px = sizeM * c_Proj.y / (c_Proj2.w * z);
    float blur = 5.0 * saturate((1.5 - z) / 1.25);
    float r = max(px, 1.2) + blur;
    float opacity = saturate(px / 1.2) * 0.85 + 0.15;
    opacity *= 1.0 / (1.0 + blur * 0.35);
    // Tumbling flakes catch the light now and then.
    opacity *= 0.7 + 0.3 * sin(t * (3.0 + 4.0 * seed.z) + phase);
    float2 d = (nb - na) / c_Proj2.zw;      // streak in pixels
    float len = length(d);
    float2 dir = len > 1e-3 ? d / len : float2(0.0, 1.0);
    float lenR = len / r;
    opacity /= 1.0 + lenR * 0.5;             // a long streak is the same flake spread out
    float along = corner.y * (lenR + 2.0) - 1.0;
    float2 n = na + (dir * along + float2(-dir.y, dir.x) * corner.x) * r * c_Proj2.zw;
    o.pos = float4(n * ca.w, ca.z, ca.w);
    float edge = saturate((0.5 - max(abs(local.x), abs(local.z)) / box.x) * 8.0); // hide the box edges
    edge *= saturate((0.5 - abs(local.y) / box.y) * 8.0);
    float far = exp(-z / 24.0);
    o.data = float4(corner.x, along, z, opacity * edge * far * smoothstep(0.25, 0.6, z));
    o.len = lenR;
    return o;
}

// Spray: water thrown up by the rear tyres on a wet road. The car is found in the depth
// buffer where the chase camera keeps it (low in the middle of the screen); every particle
// leaves a tyre, rises, falls and hangs in the air while the car drives on.
// uv = texel centre; the game drew that pixel at ndc 2(uv - texel / 2) - 1 (D3D9 pixel centre).
float3 viewRay(float2 uv, float z) {
    float2 ndc = float2(uv.x * 2.0 - 1.0 - c_Proj2.z * 0.5, 1.0 - uv.y * 2.0 + c_Proj2.w * 0.5);
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
        // toClip shifted it by half a pixel for drawing; the texel centre is a whole pixel over.
        float2 buv = float2(bc.x / bc.w * 0.5 + 0.5, 0.5 - bc.y / bc.w * 0.5) + c_Proj2.zw * 0.5;
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
float4 c_Runs   : register(c20); // TrailKeepRuns: the first stamp of this run and of the 3 before it (-1e30 = none)
float4 c_Runs2  : register(c21); // x the first stamp 4 runs back; y the last run's brightness, z less per run further back
sampler2D s_trail : register(s2);
#ifndef TRAIL_W
#define TRAIL_W 128.0
#define TRAIL_POINTS 8192.0
#endif

// The ring's texture: the points in its upper half, the car's up at each in the lower half.
float4 trailPoint(float i) {
    i = fmod(i + 2.0 * TRAIL_POINTS, TRAIL_POINTS);
    float2 uv = float2((fmod(i, TRAIL_W) + 0.5) / TRAIL_W, (floor(i / TRAIL_W) + 0.5) / (2.0 * TRAIL_POINTS / TRAIL_W));
    return tex2Dlod(s_trail, float4(uv, 0, 0));
}

// The car's up at point i (unit length; the world's up where none was written).
float3 trailUp(float i) {
    i = fmod(i + 2.0 * TRAIL_POINTS, TRAIL_POINTS);
    float2 uv = float2((fmod(i, TRAIL_W) + 0.5) / TRAIL_W, 0.5 + (floor(i / TRAIL_W) + 0.5) / (2.0 * TRAIL_POINTS / TRAIL_W));
    float3 u = tex2Dlod(s_trail, float4(uv, 0, 0)).xyz;
    return dot(u, u) > 0.25 ? normalize(u) : float3(0, 1, 0);
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

// The direction of the line at point j: from its neighbours on the same line (smoothed). The
// pieces on both sides of a point then share its corners (no gaps or overlaps at the joints), and
// a car shaking in place doesn't swing the tyre lines around.
float3 trailTangent(float j, float4 pj, float3 sj) {
    float first = c_Trail.y - c_Trail.x, last = c_Trail.y - 1.0;
    float4 pm = trailPoint(j - 1.0), pp = trailPoint(j + 1.0);
    bool hasPrev = pj.w > 0.0 && j - 1.0 >= first && pm.w != 0.0 && distance(pm.xyz, pj.xyz) < 25.0;
    bool hasNext = j + 1.0 <= last && pp.w > 0.0 && distance(pp.xyz, pj.xyz) < 25.0;
    float3 m = hasPrev ? smoothPoint(j - 1.0, pm) : sj;
    float3 n = hasNext ? smoothPoint(j + 1.0, pp) : sj;
    return n - m;
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
    if (distance(a.xyz, b.xyz) > 25.0) return o; // teleported (or a wrong find: 25 m in 1/30 s is 2700 km/h)
    if (distance(a.xyz, b.xyz) < 1e-3) return o;
    // This corner's point, smoothed, and the line's direction there.
    float j = first + k + corner.y;
    float4 pj = corner.y > 0.5 ? b : a;
    float3 s = smoothPoint(j, pj);
    float3 d = trailTangent(j, pj, s);
    float len = length(d);
    if (len < 1e-3) return o;
    float stamp = corner.y > 0.5 ? b.w : abs(a.w);
    float age = c_Trail.z - stamp;
    float fade = c_Trail.w > 0.0 ? saturate(1.0 - age / c_Trail.w) : 1.0;
    // An earlier run (TrailKeepRuns) is drawn dimmer, the older the dimmer.
    float run = dot(step(stamp + 1e-3, c_Runs), float4(1, 1, 1, 1)) + step(stamp + 1e-3, c_Runs2.x);
    if (run > 0.5) fade *= c_Runs2.y - (run - 1.0) * c_Runs2.z;
    if (fade <= 0.0) return o;
    // It comes out behind the car: under and in it the line would flicker through the body
    // and the rear wheels (it runs through their centres, almost at the depth of the tread).
    // The way back to the car along the line goes to the pixels (data.w), the fade-in lies 1.6 to
    // 2.6 m behind the car at any speed. Farther back along the line it stays, also where the car
    // drives over its old line again. (It was the points' age: driving slowly the line next to the
    // car popped in and out, at speed the points lie metres apart.)
    float fromCar = 100.0;
    float newest = c_Trail.y - 1.0;
    if (newest - j < 24.0) {
        float path = 0.0;
        float3 q = trailPoint(newest).xyz;
        [loop] for (int i = 1; i <= 24; i++) {
            if (newest - i < j - 0.5) break;
            float3 r = trailPoint(newest - i).xyz;
            path += distance(q, r);
            q = r;
        }
        fromCar = path;
    }
    float3 dir = d / len;
    // Across the line in the road's plane: the car's up there (its neighbours' with it, a car
    // rocking on its springs doesn't tilt the line), not the world's. With the world's up the
    // line lay level on banked curves and walls: its lower half (on a wall all of the inner tyre's
    // line) went into the road.
    float firstJ = c_Trail.y - c_Trail.x, lastJ = c_Trail.y - 1.0;
    float3 up = trailUp(j) * 2.0;
    if (j - 1.0 >= firstJ) up += trailUp(j - 1.0);
    if (j + 1.0 <= lastJ) up += trailUp(j + 1.0);
    up = normalize(up);
    // Smoothing (the average of its neighbours) took the line under the road over a crest: the
    // point may rise along the car's up (in dips), never sink below where the car was.
    s += up * max(dot(pj.xyz - s, up), 0.0);
    float3 right = cross(up, dir);
    float rl = length(right);
    float3 across = rl > 1e-3 ? right / rl : float3(1, 0, 0);
    float3 p = s + across * c_Trail2.x;
    float3 v = toView(p);
    // Right in front of the lens (a camera on the ground after a crash) it would fill the screen.
    // Behind the camera the hardware clips the piece (dropping the corner dropped the whole piece:
    // at speed the one from the car back past the camera).
    fade *= smoothstep(0.5, 2.5, v.z);
    float3 dv = toView(p + dir) - v;
    float3 side = cross(dv, v);
    float sl = length(side);
    side = sl > 1e-6 ? side / sl : float3(1, 0, 0);
    // Flat across the line, lying on the road: turned to the camera it flapped with every swing of
    // the chase camera and stood up like a wall where the line passes beside or above the camera
    // (downhill). Only where the line runs along the car's up (flying straight up) "across" is
    // unknown: turned to the camera there.
    float flatness = saturate((rl - 0.15) / 0.35);
    float3 wide = normalize(lerp(side, normalize(toView(p + across) - v), flatness));
    float facing = dot(wide, side);
    // At least a pixel wide as seen (far away, or seen edge-on), widened towards the camera;
    // thinner lines are dimmed instead.
    float pixel = abs(v.z) * c_Proj2.z / c_Proj.x;
    float seen = c_Trail2.y * abs(facing);
    float more = max(pixel * 1.2 - seen, 0.0);
    fade *= c_Trail2.y / max(c_Trail2.y, pixel * 1.2);
    o.pos = toClip(v + (wide * c_Trail2.y + side * (facing < 0.0 ? -more : more)) * corner.x);
    o.data = float4(corner.x, fade, v.z, fromCar);
    return o;
}
)";

} // namespace

std::string Pipeline::trailSource() {
    char defines[96];
    snprintf(defines, sizeof(defines), "#define TRAIL_W %u.0\n#define TRAIL_POINTS %u.0\n", kTrailWidth, kTrailPoints);
    return std::string(defines) + kRainVS;
}

void Pipeline::runtimeShaders(std::vector<gfx::ShaderSource>& out) {
    gfx::quadShaders(out);
    // In the order the first frames need them: the height map, the trail, the material mask.
    out.push_back({kSplatVS, "main", "vs_3_0", "SplatVS"});
    out.push_back({trailSource(), "trailVS", "vs_3_0", "TrailVS"});
    out.push_back({kSolidPS, "main", "ps_3_0", "Solid"});
    out.push_back({kPalettePS, "main", "ps_3_0", "MaterialPalette"});
    for (const char* entry : {"dropVS", "splashVS", "sprayVS", "snowVS"}) out.push_back({kRainVS, entry, "vs_3_0", "RainVS"});
}

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
        m_trailVS = ok ? gfx::compileVertexShader(device, trailSource(), "trailVS", "TrailVS") : nullptr;
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
    // The points in the upper half, the car's up at each point in the lower half.
    bool ok = m_trailPoints.create(device, kTrailWidth, kTrailHeight * 2, D3DFMT_A32B32G32R32F) &&
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

void Pipeline::shareTrail(const Pipeline* from) {
    const Pipeline* was = m_trailFrom;
    m_trailFrom = from && from != this && from->m_trailPoints.texture ? from : nullptr;
    if (!m_trailFrom) {
        if (was) m_trailReset = true; // the copied counts don't belong to its own (empty) points
        return;
    }
    // Its state as of its render this frame; nothing here resets or records over it.
    m_trailReset = false;
    m_trailNewRun = false;
    m_trailHead = from->m_trailHead;
    m_trailCount = from->m_trailCount;
    m_trailTime = from->m_trailTime;
    m_trailWritten = from->m_trailWritten;
    m_trailRuns = from->m_trailRuns;
    memcpy(m_trailRunStart, from->m_trailRunStart, sizeof(m_trailRunStart));
    memcpy(m_trailRunFirst, from->m_trailRunFirst, sizeof(m_trailRunFirst));
    m_trailPause = from->m_trailPause;
    m_trailPauseAt = from->m_trailPauseAt;
    m_trailPauseLogged = true; // its log is the other pipeline's
}

void Pipeline::drawTrail(IDirect3DDevice9* device, const Inputs& in, const Settings& s, float dt) {
    // The player drives: also while a clip of the map plays (a camera in a loop, texts at the
    // start). Those clips stopped the recording, and when one ended the car was 130 - 340 m from
    // its last point: taken for a missed restart, the trail started over (every such line in the
    // owner's log came with "cinematic off"; one clip right after each start: the trail came 6 s
    // late).
    const bool recording = !m_trailFrom && (in.driving || in.clipInRace);
    // A restart the hooks didn't see (the log had "race reset" on some restarts only, the old run
    // stayed under the new one): the car jumped far between two frames it was seen in, without a
    // respawn; or, after frames without it, it is back where this run began.
    if (!m_trailReset && !m_trailNewRun && recording && in.carKnown && m_trailTime - m_trailRespawnAt > 1.0f) {
        auto apart = [](const float* a, const float* b) {
            const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            return sqrtf(dx * dx + dy * dy + dz * dz);
        };
        const float since = m_trailTime - m_trailSeenAt;
        if (since < 0.5f) {
            // The car drove for this frame's dt too (m_trailTime grows below): a frame hitch while
            // driving fast covers more than 60 m (0.25 s at 250 m/s), no restart. 350 m/s is faster
            // than any car drives.
            const float elapsed = since + dt;
            const float jump = apart(in.carPosition, m_trailSeen);
            if (jump > fmaxf(kTrailTeleport, elapsed * 350.0f)) {
                m_trailNewRun = true;
                if (m_trailLogged < 20) {
                    m_trailLogged++;
                    TMVS_LOG("pipeline: neon trail: the car jumped %.0f m in %.0f ms without a respawn: a new run", jump, elapsed * 1000.0f);
                }
            }
        } else if (m_trailKeptValid && m_trailRunAtValid && apart(in.carPosition, m_trailKept) > kTrailTeleport &&
                   apart(in.carPosition, m_trailRunAt) < 12.0f) {
            m_trailNewRun = true;
            if (m_trailLogged < 20) {
                m_trailLogged++;
                TMVS_LOG("pipeline: neon trail: the car is back where the run began (%.0f m from its last point, unseen for %.1f s): a new run",
                         apart(in.carPosition, m_trailKept), since);
            }
        }
    }
    const int keepRuns = s.trailKeepRuns < 0 ? 0 : (s.trailKeepRuns > kTrailRunsMax ? kTrailRunsMax : s.trailKeepRuns);
    if (m_trailNewRun) {
        m_trailNewRun = false;
        if (keepRuns == 0 || m_trailWritten == 0) {
            m_trailReset = true; // as before: the trail starts over
        } else if (!m_trailReset) {
            // The points stay; this run begins with a new line and its own time stamps. A run
            // without a point (the hook's restart a frame after a teleport-detected one, two quick
            // restarts) takes no slot: it would push the real previous run out.
            if (m_trailWritten != m_trailRunFirst[0]) {
                for (int k = kTrailRunsMax; k > 0; k--) {
                    m_trailRunStart[k] = m_trailRunStart[k - 1];
                    m_trailRunFirst[k] = m_trailRunFirst[k - 1];
                }
                m_trailRuns = m_trailRuns + 1 < kTrailRunsMax ? m_trailRuns + 1 : kTrailRunsMax;
            }
            // Just after the stamp of "now" (= the last frame's point, m_trailTime grows below):
            // the shader counts a point as older when its stamp + 1e-3 <= this, so that last point
            // belongs to the old run with a margin (without one it counted as this run's: a metre
            // at the end of each kept run was drawn at full brightness), and every point of this
            // run, a frame later, is newer.
            m_trailRunStart[0] = (m_trailTime + 1.0f) + 2e-3f;
            m_trailRunFirst[0] = m_trailWritten;
            m_trailBreak = true;
            m_trailKeptValid = false;
            m_trailClock = 0.0f;
            m_trailRunAtValid = false;
            m_trailFirstLogged = false;
            m_trailRunBegan = m_trailTime;
        }
    }
    if (m_trailReset) {
        m_trailReset = false;
        m_trailBreak = true;
        m_trailHead = m_trailCount = 0;
        m_trailClock = m_trailTime = 0.0f;
        m_trailKeptValid = false;
        m_trailRespawnAt = -100.0f;
        m_trailSeenAt = -100.0f;
        m_trailRunAtValid = false;
        m_trailFirstLogged = false;
        m_trailRunBegan = 0.0f;
        m_trailWritten = 0;
        m_trailRuns = 0;
        for (int k = 0; k <= kTrailRunsMax; k++) {
            m_trailRunStart[k] = 0.0f;
            m_trailRunFirst[k] = 0;
        }
        device->SetRenderTarget(0, m_trailPoints.surface);
        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 0.0f, 0);
    }
    // Fewer runs wanted (the setting lowered): the older ones go.
    if (m_trailRuns > keepRuns) m_trailRuns = keepRuns;
    {
        const UINT sinceOldest = m_trailWritten - m_trailRunFirst[m_trailRuns];
        if (m_trailCount > sinceOldest) m_trailCount = sinceOldest;
    }
    m_trailTime += dt;
    m_trailClock += dt;
    if (in.carKnown) {
        memcpy(m_trailSeen, in.carPosition, sizeof(m_trailSeen));
        m_trailSeenAt = m_trailTime;
    }
    // Record the car while you drive (not in replays and intros): every frame into the next
    // slot (the live end of the trail, so it stays on the car), kept 30 times a second. A camera
    // cut only matters to the car found in the depth buffer: the engine's car is where it is
    // whatever the camera does.
    bool live = false;
    if (in.carFromEngine && !in.carKnown) m_trailBreak = true; // a gap: the next point starts a new line
    const int pause = !recording ? 1 : (!m_temporalValid && !in.carKnown) ? 2 : (in.carFromEngine && !in.carKnown) ? 3 : 0;
    if (dt > 0.0f) {
        // The log tells when the trail records nothing for longer than half a second, and why.
        static const char* const kWhy[] = {"", "not driving (a replay, an intro or the replay viewer)", "camera cuts (the car found in the depth buffer)",
                                           "the engine lost the car"};
        if (pause != m_trailPause) {
            if (m_trailPause != 0 && m_trailPauseLogged && m_trailPauseLogs < 60) {
                m_trailPauseLogs++;
                TMVS_LOG("pipeline: neon trail: %s, over after %.1f s", kWhy[m_trailPause], m_trailTime - m_trailPauseAt);
            }
            m_trailPause = pause;
            m_trailPauseAt = m_trailTime;
            m_trailPauseLogged = false;
        }
        if (pause != 0 && !m_trailPauseLogged && m_trailTime - m_trailPauseAt > 0.5f && m_trailPauseLogs < 60) {
            m_trailPauseLogged = true;
            m_trailPauseLogs++;
            TMVS_LOG("pipeline: neon trail records nothing for 0.5 s: %s", kWhy[pause]);
        }
    }
    if (pause == 0 && dt > 0.0f) {
        live = true;
        bind(device, 0, m_nd.texture, false);
        const float stamp = m_trailTime + 1.0f; // never 0 (= no car)
        // The engine's car position when known (exact, also off screen), else the shader
        // finds the car in the depth buffer.
        passConstants(device, m_trailBreak ? -stamp : stamp, in.carKnown ? 1.0f : 0.0f);
        const float car[4] = {in.carPosition[0], in.carPosition[1], in.carPosition[2], 0.0f};
        device->SetPixelShaderConstantF(33, car, 1);
        // One pixel of the ring buffer. (SetRenderTarget resets the scissor rect: set it after.)
        device->SetRenderTarget(0, m_trailPoints.surface);
        const LONG px = static_cast<LONG>(m_trailHead % kTrailWidth), py = static_cast<LONG>(m_trailHead / kTrailWidth);
        const RECT pixel = {px, py, px + 1, py + 1};
        device->SetScissorRect(&pixel);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
        device->SetPixelShader(m_shaders[kTrailPoint]);
        m_quad.draw(device, kTrailWidth, kTrailHeight * 2);
        // And the car's up at that point, in the lower half (the world's up for the depth
        // buffer's car).
        const float up[4] = {in.carKnown ? in.carUp[0] : 0.0f, in.carKnown ? in.carUp[1] : 1.0f, in.carKnown ? in.carUp[2] : 0.0f, 0.0f};
        if (in.carKnown && up[1] < 0.866f && !m_trailTiltLogged) {
            m_trailTiltLogged = true;
            TMVS_LOG("pipeline: neon trail: the car's up tilts %.0f degrees (%.2f %.2f %.2f; a banked curve, a wall or a loop): the line lies along the road",
                     acosf(up[1] < -1.0f ? -1.0f : (up[1] > 1.0f ? 1.0f : up[1])) * 57.2958f, up[0], up[1], up[2]);
        }
        passConstants(device, 0.0f, 0.0f, 1.0f);
        device->SetPixelShaderConstantF(33, up, 1);
        const RECT upPixel = {px, py + static_cast<LONG>(kTrailHeight), px + 1, py + static_cast<LONG>(kTrailHeight) + 1};
        device->SetScissorRect(&upPixel);
        m_quad.draw(device, kTrailWidth, kTrailHeight * 2);
        profileMark(kTrailPoint);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        // With the engine's position: kept 60 times a second while the car moves, and every
        // metre (at Island speeds 30 a second were 5-7 m apart, the line went angular in curves).
        // Less than 30 cm from the last point (standing, shaking after a crash) is no line: those
        // points pointed every way and fanned the tyre lines out around the car. The car found
        // in the depth buffer: 30 a second, as before.
        float moved = 0.0f;
        const bool measured = in.carKnown && m_trailKeptValid;
        if (measured) {
            const float dx = in.carPosition[0] - m_trailKept[0], dy = in.carPosition[1] - m_trailKept[1], dz = in.carPosition[2] - m_trailKept[2];
            moved = sqrtf(dx * dx + dy * dy + dz * dz);
            if (moved < kTrailMinStep) live = false;
        }
        const bool keep = measured ? (m_trailClock >= 1.0f / 60.0f && moved >= kTrailMinStep) || moved >= 1.0f
                                   : m_trailClock >= 1.0f / 30.0f;
        if (keep) {
            m_trailClock = moved >= 1.0f ? 0.0f : fmodf(m_trailClock, measured ? 1.0f / 60.0f : 1.0f / 30.0f);
            memcpy(m_trailKept, in.carPosition, sizeof(m_trailKept));
            m_trailKeptValid = in.carKnown;
            if (in.carKnown && !m_trailRunAtValid) {
                memcpy(m_trailRunAt, in.carPosition, sizeof(m_trailRunAt));
                m_trailRunAtValid = true;
            }
            // The run's first point that leaves the start (the line shows from it on): when.
            if (!m_trailFirstLogged && measured && m_trailLogged < 40) {
                m_trailFirstLogged = true;
                m_trailLogged++;
                TMVS_LOG("pipeline: neon trail: the run's line begins %.2f s after the run began (%s)", m_trailTime - m_trailRunBegan,
                         in.carKnown ? (in.driving ? "the engine's car" : "the engine's car, a clip of the map plays") : "the depth buffer's car");
            }
            m_trailBreak = false;
            m_trailHead = (m_trailHead + 1) % kTrailPoints;
            if (m_trailCount < kTrailPoints) m_trailCount++;
            m_trailWritten++;
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
    // Earlier runs: where each began (a point stamped before it belongs to an older run; -1e30 =
    // none kept) and their brightness, 0.6 for the last run down to 0.6 / n for the n-th.
    float runs[8] = {-1e30f, -1e30f, -1e30f, -1e30f, -1e30f, 0.6f, 0.0f, 0.0f};
    for (int k = 0; k < m_trailRuns && k < kTrailRunsMax; k++) runs[k] = m_trailRunStart[k];
    if (m_trailRuns > 0) runs[6] = 0.6f / static_cast<float>(m_trailRuns);
    device->SetVertexShaderConstantF(20, runs, 2);

    device->SetRenderTarget(0, m_hdr.surface);
    D3DVIEWPORT9 viewport = {0, 0, in.width, in.height, 0.0f, 1.0f};
    device->SetViewport(&viewport);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    // Alpha too: it adds the trail's coverage to the 1 the lighting wrote (TAA's mask).
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    bind(device, 0, m_nd.texture, false);
    device->SetTexture(D3DVERTEXTEXTURESAMPLER2, m_trailFrom ? m_trailFrom->m_trailPoints.texture : m_trailPoints.texture);
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
    const UINT quads = kRainDrops + kRainSplashes > kSnowFlakes ? kRainDrops + kRainSplashes : kSnowFlakes;
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
    vc[9][2] = 1.0f / m_heightWorld;
    vc[9][3] = splashes && s.weatherShelter ? 1.0f : 0.0f; // shelter (the height map is valid)
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
    const float thrown = s.spray * fmaxf(s.wetness, s.snowCover * 2.0f); // water, or powder snow (a lot of it)
    const float spray[4] = {speed, thrown * fminf(fmaxf((speed - 4.0f) / 25.0f, 0.0f), 1.0f), 0.0f, 0.0f};
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
    bind(device, 2, m_skyAverage[m_skyIndex].texture, false);
    bind(device, 3, s.neonLight > 0.0f ? m_spillHistory[m_spillIndex].texture : nullptr, true);
    bind(device, 4, m_adapted[m_adaptIndex].texture, false);
    device->SetVertexDeclaration(m_rainDecl);
    device->SetStreamSource(0, m_rainVB, 0, 24);
    device->SetIndices(m_rainIB);
    device->SetTexture(D3DVERTEXTEXTURESAMPLER0, m_heightMap[m_heightIndex].texture);
    device->SetTexture(D3DVERTEXTEXTURESAMPLER1, m_linearDepth.texture);
    device->SetTexture(D3DVERTEXTEXTURESAMPLER3, m_ceilingValid ? m_ceilingMap[m_heightIndex].texture : nullptr);
    for (DWORD sampler = D3DVERTEXTEXTURESAMPLER0; sampler <= D3DVERTEXTEXTURESAMPLER3; sampler++) {
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
    device->SetTexture(D3DVERTEXTEXTURESAMPLER3, nullptr);
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

// Before a temporal frame moves the previous camera on: what its repeats need.
void Pipeline::keepRepeatCamera() {
    memcpy(m_repeatPrevView, m_prevView, sizeof(m_prevView));
    memcpy(m_repeatPrevProjection, m_prevProjection, sizeof(m_prevProjection));
    m_repeatHavePrevious = m_havePrevious;
    m_repeatTemporalValid = m_temporalValid;
}

// A repeat sees the camera before its frame as the previous one (twice: back again).
void Pipeline::swapRepeatCamera() {
    std::swap(m_prevView, m_repeatPrevView);
    std::swap(m_prevProjection, m_repeatPrevProjection);
    std::swap(m_havePrevious, m_repeatHavePrevious);
}

namespace {

// Full-screen passes with none of the game's leftover state (alpha test, stencil, scissor and
// clip planes would drop pixels: the depth capture came out all 0 on maps with water).
// Returns whether software vertex processing was on, for the caller to restore.
BOOL setPassStates(IDirect3DDevice9* device) {
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
    return softwareVP;
}

} // namespace

bool Pipeline::render(IDirect3DDevice9* device, const Inputs& in, const Settings& s, IDirect3DSurface9* output) {
    if (!m_ready || !in.color || !in.depth) return false;
    if (!ensureTargets(device, in.width, in.height)) return false;
    // RenderScale: every pass at the scene's size into m_scaled, then up to `output` at the end
    // (upscaleOutput). The look's sharpening is skipped then, FSR's own (RCAS) takes its place:
    // sharpened before EASU, its halos would be scaled up with the image.
    const bool upscale = in.outputWidth > 0 && in.outputHeight > 0 && (in.outputWidth != in.width || in.outputHeight != in.height);
    IDirect3DSurface9* const screen = output;
    if (upscale) {
        if (!m_scaled.create(device, in.width, in.height, D3DFMT_A8R8G8B8)) return false;
        output = m_scaled.surface;
    }
    m_frame++;

    const BOOL softwareVP = setPassStates(device);

    // The game's materials (lamps, cars, glass, grass) for the lighting and the final pass.
    const bool materials = buildMaterialMask(device, in);
    m_materialsOn = materials;
    m_materialTexture = in.materialMask ? in.materialMask : m_materials.texture;
    // z: a night sky replaces the game's, its sunset clouds and their mirror image in the sea go.
    const float gameSkyOut = skyReplacesSun(s) && !s.gameSkyClouds ? 1.0f : 0.0f;
    const float materialConstants[4] = {materials ? 1.0f : 0.0f, 0.0f, gameSkyOut, s.toneKnee};
    device->SetPixelShaderConstantF(44, materialConstants, 1);
    // x 8: the scale of the car test in the lighting pass (saves an instruction there, it is at the limit).
    // zw: lamps by their material glow from 0.4 to 0.8 of the game's white, only with GameHdr.
    const float cleanConstants[4] = {s.cleanHighlights ? 8.0f : 0.0f, in.hdr ? 1.0f : 0.0f, in.hdr ? 2.5f : 0.0f, -1.0f};
    device->SetPixelShaderConstantF(45, cleanConstants, 1);
    // c46 (EngineLights): x s14 holds the added light; y its extra strength per HighlightBoost;
    // z the reflections' boost of bright hits (1: as before; 0: the light sources are known);
    // w the neon strips' glow (they are plain block material to the game: found by their colour).
    const bool engineLights = s.engineLights && in.hdr;
    const float engineConstants[4] = {engineLights && in.added ? 1.0f : 0.0f, 0.5f, engineLights ? 0.0f : 1.0f, s.borderGlow};
    device->SetPixelShaderConstantF(46, engineConstants, 1);
    // c51 x (CleanWater): the reflection on the game's water skips what it can't see (the tops
    // of the ramps over the pools) and takes no dry-track blur. Only where the engine tells the
    // water's height (or it was measured): the colour guess also takes blue kerbs for water.
    const bool waterKnown = in.water[2] > 0.5f || (in.water[2] >= 0.0f && m_seaKnown);
    // c51 (EngineLights, the lamps: what the game's additive draws added). Floodlights: their own
    // strength, apart from the neon borders (BorderGlow) and HighlightBoost (1 = as before). y: the
    // part taken out of their core, 1 - Floodlights x (1 - LampGlare): LampGlare's part is spread
    // around them in the final pass (0 = as before; below 0 the core gets brighter). z (LampSkyHalos):
    // the part added over the sky as well, at night only (by day the game's sun glow is an added
    // light too). w: Floodlights for the light they spill onto their surroundings (NeonLight).
    const bool lampMask = engineLights && in.added;
    // Without its targets (allocation failed) no glare is added back: the core keeps its light.
    const bool lampLight = lampMask && !m_lampFailed && (s.lampGlare > 0.0f || s.lampHaze > 0.0f);
    const float floodlights = lampMask ? fmaxf(s.floodlights, 0.0f) : 1.0f;
    const float lampGlareTaken = lampLight ? fminf(fmaxf(s.lampGlare, 0.0f), 1.0f) : 0.0f;
    const float lampCore = floodlights * (1.0f - lampGlareTaken);
    const bool lampNight = skyReplacesSun(s) || (in.sunColorKnown && classifyMood(in.sunColor) == Mood::Night);
    const float lampSky = s.lampSkyHalos && lampMask && lampNight ? lampCore : 0.0f;
    const float waterConstants[4] = {s.cleanWater && waterKnown ? 1.0f : 0.0f, 1.0f - lampCore, lampSky, floodlights};
    device->SetPixelShaderConstantF(51, waterConstants, 1);

    // Temporal state: history is only valid for the same camera as last frame.
    const bool temporal = in.temporal;
    // 640 m around the camera (0.625 m a texel): at speed the shadows ahead were only known
    // 120..160 m out, the next blocks' shadows popped up in front of the car. Photo mode: 960 m,
    // far views get long shadows. (Not the video export: with photo mode's reach and map its
    // frames looked different from the replay, water reflections noisier, lit walls darker.)
    const UINT heightSize = 1024;
    const float heightWorld = in.photo ? 960.0f : 640.0f;
    if (heightSize != m_heightSize || heightWorld != m_heightWorld) {
        m_heightSize = heightSize;
        m_heightWorld = heightWorld;
        gfx::release(m_heightDepth); // recreated at the new size with the maps
        m_heightValid = false;
        m_ceilingValid = false;
    }
    const bool cut = temporal && detectCameraCut(in);
    m_temporalValid = temporal && !cut;
    if (cut) m_taaValid = false;
    // A repeat of the last frame: its motion (the camera before it), swapped back at the end.
    if (in.repeat) {
        m_temporalValid = m_repeatTemporalValid;
        swapRepeatCamera();
    }

    profileBegin(device);
    setFrameConstants(device, in, s);
    const UINT hw = m_ndHalf.width, hh = m_ndHalf.height;

    // 1. Geometry reconstruction.
    bind(device, 0, in.depth, false);
    runPass(device, kLinearDepth, m_linearDepth);
    bind(device, 0, m_linearDepth.texture, false);
    // GameGBuffer: the game's own normals where they belong to the surface seen (a pass of its
    // own: without them PS_Prepare is as it was).
    const bool gameNormals = in.gbuffer && s.gameGBuffer;
    if (gameNormals) bind(device, 1, in.gbuffer, false);
    runPass(device, gameNormals ? kPrepareGBuffer : kPrepare, m_nd);
    bind(device, 0, m_nd.texture, false);
    passConstants(device, 1.0f / in.width, 1.0f / in.height);
    runPass(device, kDownsampleND, m_ndHalf);

    if (!s.enabled && in.photo) {
        // Vanilla photo mode: only depth of field and sharpening on the game's original image (in.color).
        // The time step from the last frame of this camera: m_historyValid belongs to the exposure,
        // which never runs here. With it the focus froze where it was first measured (the shaders
        // off from the start, or after a resize): clicks and the auto focus didn't move it.
        const float dt = m_havePrevious ? fmaxf(in.time - m_lastTime, 0.0f) : 0.0f;
        if (temporal) m_lastTime = in.time;
        IDirect3DTexture9* current = in.color;
        const bool dof = s.depthOfField > 0.0f;
        if (dof && m_hdr2.create(device, in.width, in.height, D3DFMT_A16B16G16R16F)) {
            gfx::Target* dofTarget = &m_dof;
            if (m_dofFull.create(device, in.width, in.height, D3DFMT_A16B16G16R16F)) dofTarget = &m_dofFull;
            int focus = m_focusIndex;
            if (temporal && !in.freeze) {
                focus ^= 1;
                bind(device, 0, m_nd.texture, false);
                bind(device, 1, m_focus[m_focusIndex].texture, false);
                passConstants(device, 1.0f - expf(-dt * 4.0f), m_temporalValid ? 0.0f : 1.0f, in.focusPoint[0], in.focusPoint[1]);
                focusOnCarConstants(device, in, s);
                runPass(device, kFocus, m_focus[focus]);
                m_focusIndex = focus;
            }
            bind(device, 0, in.color, true);
            bind(device, 1, dofTarget == &m_dofFull ? m_nd.texture : m_ndHalf.texture, false);
            bind(device, 2, m_focus[focus].texture, false);
            passConstants(device, 64.0f, 0.0f, 1.0f);
            runPass(device, kDofBlur, *dofTarget);
            gfx::Target& second = dofTarget == &m_dofFull ? m_dofFull2 : m_dof2;
            if (second.create(device, dofTarget->width, dofTarget->height, D3DFMT_A16B16G16R16F)) {
                bind(device, 0, dofTarget->texture, true);
                passConstants(device, 32.0f, 0.0f, 0.3f);
                runPass(device, kDofBlur, second);
                dofTarget = &second;
            }
            bind(device, 0, in.color, true);
            bind(device, 1, dofTarget->texture, true);
            bind(device, 2, m_nd.texture, false);
            runPass(device, kCinematic, m_hdr2);
            current = m_hdr2.texture;
        }
        const bool sharpen = s.sharpen > 0.001f && !upscale;
        bind(device, 0, current, false);
        bind(device, 1, nullptr, false);
        passConstants(device, 1.0f / in.width, 1.0f / in.height);
        runPass(device, sharpen ? kSharpen : kCopy, output, in.width, in.height);
        if (upscale) upscaleOutput(device, in, s, screen);
        profileEnd();
        if (in.repeat) swapRepeatCamera();
        if (temporal) {
            keepRepeatCamera();
            memcpy(m_prevView, in.view, sizeof(m_prevView));
            memcpy(m_prevProjection, in.projection, sizeof(m_prevProjection));
            m_havePrevious = true;
        }
        for (int i = 0; i < 10; i++) device->SetTexture(i, nullptr);
        if (softwareVP) device->SetSoftwareVertexProcessing(TRUE);
        return true;
    }
    // 1a. The sea the game reports no height for (TMUF Island), from its water material.
    if (materials && s.waterSurfaces > 0.0f && ((in.water[2] >= 0.0f && in.water[2] < 0.5f) || in.water[2] > 1.5f)) measureSeaLevel(device);

    // 1b. Long-range shadows: update the world-space height map.
    // Extra cameras (replay blends) use the map as it is: it follows the main camera.
    // Rain splashes land on it too.
    const bool wantLong = s.longShadows > 0.0f && s.shadowStrength > 0.0f && m_sunKnown;
    const bool wantVolume = s.volumetricLight > 0.0f && m_sunKnown;
    if (wantLong || wantVolume) m_castersWanted = true;
    if (m_heightReset) {
        m_heightReset = false;
        m_heightValid = false;
        m_heightHoldUntil = in.time + 4.0f; // the respawn camera lasts ~1.5 s
    }
    if (in.driving || in.time > m_heightHoldUntil) m_heightHoldUntil = -1.0f;
    const bool splat = temporal && m_heightHoldUntil < 0.0f;
    const bool heightMap = (wantLong || wantVolume || s.rain > 0.0f || s.snow > 0.0f || s.snowCover > 0.0f) && ensureHeightMap(device) && (splat || m_heightValid);
    // Every other frame is enough for a map of the static world (saves ~0.15 ms).
    // Roofs and bridges for the rain and snow, only while it falls.
    const bool ceilings = s.weatherShelter && (s.rain > 0.0f || s.snow > 0.0f);
    if (heightMap && splat && (!m_heightValid || (m_frame & 1))) {
        updateHeightMap(device, in, s, ceilings);
        m_shadowHeightStale = true;
        m_lightGridStale = true;
    }
    if (!ceilings) m_ceilingValid = false;
    const bool longShadows = heightMap && wantLong;
    // 1c. CasterShadows: the game's own static meshes drawn from the light, built up over the
    // map: around the camera (c47-c50, s3) and over the whole track (c54-c57, s4). For the long
    // shadows and the volumetric light alike, so the shafts' gaps line up with the shadows.
    // The constants go up every frame: the game's pixel shaders use these registers too.
    casters::Maps casterMaps;
    float camera[3];
    for (int j = 0; j < 3; j++) camera[j] = -(in.view[12] * in.view[j * 4 + 0] + in.view[13] * in.view[j * 4 + 1] + in.view[14] * in.view[j * 4 + 2]);
    profileMark(-1);
    const bool casterShadows = (longShadows || (heightMap && wantVolume)) && in.casterMap && in.casterMap(device, m_sunWorld, camera, casterMaps);
    profileMark(kProfileCasters); // the maps' draws (new meshes, the near map following the camera)
    const casters::MapInfo* casterInfo[2] = {&casterMaps.nearMap, &casterMaps.farMap};
    for (int k = 0; k < 2; k++) {
        float constants[4][4] = {};
        const casters::MapInfo& map = *casterInfo[k];
        if (casterShadows && map.texture) {
            memcpy(constants, map.rows, sizeof(map.rows));
            constants[3][0] = 1.0f;
            constants[3][1] = map.texel[0];
            constants[3][2] = map.texel[1];
            constants[3][3] = map.bias;
        }
        device->SetPixelShaderConstantF(k == 0 ? 47 : 54, &constants[0][0], 4);
    }
    const bool lightGrid = longShadows && s.longShadowQuality > 0 && updateLightGrid(device, s);
    const float noGrid[2][4] = {};
    device->SetPixelShaderConstantF(52, lightGrid ? &m_lightGridConstants[0][0] : &noGrid[0][0], 2);
    const float longConstants[4] = {s.taa ? 1.0f : 0.0f, m_temporalValid ? 1.0f : 0.0f, s.taa ? static_cast<float>(m_frame % 64) : 0.0f,
                                    longShadows ? s.longShadows : 0.0f};
    device->SetPixelShaderConstantF(34, longConstants, 1);

    // 2. AO + sun shadows, bilateral blur.
    if (s.aoStrength > 0.0f || s.shadowStrength > 0.0f || s.debugView == 3 || s.debugView == 4 || s.debugView == 7) {
        bind(device, 0, m_ndHalf.texture, false);
        bind(device, 1, m_nd.texture, false);
        if (lightGrid) {
            bind(device, 2, m_lightGrid[m_lightGridIndex].texture, m_lightGridFilter);
        } else {
            bind(device, 2, longShadows ? m_heightMap[m_heightIndex].texture : nullptr, m_heightFilter);
        }
        if (casterShadows) {
            bind(device, 3, casterMaps.nearMap.texture, false);
            bind(device, 4, casterMaps.farMap.texture, false);
        }
        runPass(device, kOcclusionShadow, m_occlusion);
        if (casterShadows) {
            device->SetTexture(3, nullptr); // depth textures bound as textures
            device->SetTexture(4, nullptr);
        }
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
    // c37 before the march, which reads its range (u_Volume.y): set after it, the march took the
    // value of whatever set c37 last (the game's own shaders in game, the compare split's other
    // pipeline, the previous frame).
    const bool gi = s.globalIllumination > 0.0f;
    const float lensDrops = s.lensDrops ? fminf(s.rain, 1.0f) : 0.0f;
    const float lensSnow = s.lensDrops ? fminf(s.snow, 1.5f) : 0.0f;
    const float volumeConstants[4] = {volume ? s.volumetricLight : 0.0f, 150.0f, gi ? s.globalIllumination : 0.0f, lensDrops};
    device->SetPixelShaderConstantF(37, volumeConstants, 1);
    if (volume) {
        static const float kVolumeSteps[3] = {8.0f, 12.0f, 16.0f};
        const int q = s.quality < 0 ? 0 : (s.quality > 2 ? 2 : s.quality);
        // Only when the map or the sun changed (the map: every other frame).
        const bool sunMoved = fabsf(m_sunWorld[0] - m_shadowHeightSun[0]) + fabsf(m_sunWorld[1] - m_shadowHeightSun[1]) +
                                  fabsf(m_sunWorld[2] - m_shadowHeightSun[2]) > 1e-4f;
        if (m_shadowHeightStale || sunMoved) {
            bind(device, 0, m_heightMap[m_heightIndex].texture, false);
            runPass(device, kShadowHeight, m_shadowHeight);
            for (int j = 0; j < 3; j++) m_shadowHeightSun[j] = m_sunWorld[j];
            m_shadowHeightStale = false;
        }
        bind(device, 0, m_ndHalf.texture, false);
        bind(device, 1, m_shadowHeight.texture, false); // R32F: no filtering on older GPUs
        if (casterShadows) {
            bind(device, 3, casterMaps.nearMap.texture, false);
            bind(device, 4, casterMaps.farMap.texture, false);
        }
        passConstants(device, kVolumeSteps[q], 0.0f);
        runPass(device, kVolumetric, m_rays[1]);
        if (casterShadows) {
            device->SetTexture(3, nullptr);
            device->SetTexture(4, nullptr);
        }
    }
    // 2c. One-bounce global illumination, blurred like the AO.
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
    const float dt = m_historyValid ? fmaxf(in.time - m_lastTime, 0.0f) : 0.0f;
    if (temporal) m_lastTime = in.time;

    // The fog and haze colour, eased over time: with a few sky pixels flickering through a
    // gap (between walls, in a building) it jumped every frame and the fog came and went.
    // Extra cameras reuse it, like the exposure.
    if ((temporal && !in.freeze) || !m_skyValid) {
        const int previous = m_skyIndex;
        m_skyIndex ^= 1;
        bind(device, 0, in.color, false);
        bind(device, 1, m_nd.texture, false);
        bind(device, 2, m_sky.texture, false);
        bind(device, 3, m_skyAverage[previous].texture, false);
        passConstants(device, 1.0f - expf(-dt / 0.4f), (m_skyValid && m_temporalValid) ? 0.0f : 1.0f);
        runPass(device, kSkyAverage, m_skyAverage[m_skyIndex]);
        m_skyValid = true;
    }
    const gfx::Target& skyAverage = m_skyAverage[m_skyIndex];
    const bool clouds = s.volumetricClouds > 0.0f && ensureCloudNoise(device);
    if (clouds) {
        bind(device, 4, skyAverage.texture, false);
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
        bind(device, 13, materials ? m_materialTexture : nullptr, false);
        runPass(device, kReflect, m_reflect);
    }
    const bool spill = s.neonLight > 0.0f;
    if (spill) {
        bind(device, 0, in.color, true);
        bind(device, 1, m_nd.texture, false);
        bind(device, 13, materials ? m_materialTexture : nullptr, false);
        // EngineLights: the sources are what the game drew as light (its float image less what its
        // additive draws added, their light, the lamp material, the neon strips), not bright colours.
        if (engineLights) {
            bind(device, 0, in.hdr, true);
            if (in.added) bind(device, 14, in.added, true);
        }
        runPass(device, engineLights ? kSpillDownEngine : kSpillDown, m_spill[0]);
        device->SetTexture(14, nullptr);
        bind(device, 0, m_spill[0].texture, true);
        passConstants(device, 1.0f / m_spill[0].width, 0.0f);
        runPass(device, kSpillBlur, m_spill[1]);
        bind(device, 0, m_spill[1].texture, true);
        passConstants(device, 0.0f, 1.0f / m_spill[0].height);
        runPass(device, kSpillBlur, m_spill[0]);
        // Over frames: found anew in every frame, the light flickered on the walls next to a
        // strip as the camera moved.
        const int previous = m_spillIndex;
        m_spillIndex ^= 1;
        bind(device, 0, m_spill[0].texture, false);
        bind(device, 1, m_spillHistory[previous].texture, true);
        passConstants(device, (m_spillValid && m_temporalValid && m_spillHistoryOn) ? 1.0f : 0.0f, 0.0f);
        runPass(device, kSpillTemporal, m_spillHistory[m_spillIndex]);
        m_spillValid = true;
    } else {
        m_spillValid = false;
    }

    // 4. Lighting composite.
    bind(device, 0, in.color, false);
    bind(device, 1, m_nd.texture, false);
    bind(device, 2, m_occlusion.texture, false);
    bind(device, 3, m_ndHalf.texture, false);
    bind(device, 4, skyAverage.texture, false);
    bind(device, 5, m_sky.texture, false);
    bind(device, 6, m_clouds.texture, true);
    bind(device, 8, m_reflect.texture, true);
    bind(device, 9, m_spillHistory[m_spillIndex].texture, true);
    bind(device, 10, m_rays[1].texture, true);
    bind(device, 11, m_giHistory[m_giIndex].texture, false);
    bind(device, 13, materials ? m_materialTexture : nullptr, false);
    if (in.hdr) bind(device, 0, in.hdr, false); // GameHdr: light above 1.0 (the guesses in there clamp it)
    // EngineLights: light sources from the game, not guessed (s14 = what its additive draws added).
    if (engineLights && in.added) bind(device, 14, in.added, false);
    // GameLight: the light the game's shaders lit each surface with (s15): the night sky spares
    // what the game's lamps light. A pass of its own: without it PS_LightingEngine is as it was.
    const bool gameLight = engineLights && in.gameLight && s.gameLight;
    if (gameLight) bind(device, 15, in.gameLight, false);
    runPass(device, gameLight ? kLightingGameLight : (engineLights ? kLightingEngine : kLighting), m_hdr);
    if (gameLight) device->SetTexture(15, nullptr);
    device->SetTexture(14, nullptr);
    device->SetTexture(10, nullptr);
    device->SetTexture(11, nullptr);
    device->SetTexture(8, nullptr);
    device->SetTexture(9, nullptr);

    // 4b. Neon trail behind the car, into the HDR image so it blooms.
    if (s.neonTrail > 0.0f && ensureTrail(device)) drawTrail(device, in, s, temporal ? dt : 0.0f);
    else m_trailReset = true;

    // 5. Depth of field and motion blur (cinematic, HDR).
    gfx::Target* hdr = &m_hdr;
    IDirect3DTexture9* dofFocus = nullptr; // the focus distance the blur used (final pass)
    const bool motionBlur = s.motionBlur > 0.0f && m_temporalValid;
    const bool dof = s.depthOfField > 0.0f;
    if ((motionBlur || dof) && m_hdr2.create(device, in.width, in.height, D3DFMT_A16B16G16R16F)) {
        // Photo mode: the blur at full resolution (half resolution showed as coarse, blocky
        // edges once the image stands still and gets looked at). The video export too: it has
        // the time, and its frames get looked at one by one.
        const bool best = in.photo || in.video;
        gfx::Target* dofTarget = &m_dof;
        if (dof && best && m_dofFull.create(device, in.width, in.height, D3DFMT_A16B16G16R16F)) dofTarget = &m_dofFull;
        if (dof) {
            int focus = m_focusIndex;
            if (temporal && !in.freeze) {
                focus ^= 1;
                bind(device, 0, m_nd.texture, false);
                bind(device, 1, m_focus[m_focusIndex].texture, false);
                passConstants(device, 1.0f - expf(-dt * 4.0f), m_temporalValid ? 0.0f : 1.0f, in.focusPoint[0], in.focusPoint[1]);
                focusOnCarConstants(device, in, s);
                runPass(device, kFocus, m_focus[focus]);
                m_focusIndex = focus;
            }
            bind(device, 0, m_hdr.texture, true);
            bind(device, 1, dofTarget == &m_dofFull ? m_nd.texture : m_ndHalf.texture, false);
            bind(device, 2, m_focus[focus].texture, false);
            dofFocus = m_focus[focus].texture;
            // Photo mode: four times the samples and no per-pixel noise (nothing averages it
            // out over frames: TAA is off by default, tiles freeze it). The video export too.
            passConstants(device, best ? 64.0f : 16.0f, best ? 0.0f : 1.0f, 1.0f);
            runPass(device, kDofBlur, *dofTarget);
            // A second, smaller pass over the first smooths the grain of the 16 samples and the
            // rings the spiral leaves on bright lights.
            gfx::Target& second = dofTarget == &m_dofFull ? m_dofFull2 : m_dof2;
            if (second.create(device, dofTarget->width, dofTarget->height, D3DFMT_A16B16G16R16F)) {
                bind(device, 0, dofTarget->texture, true);
                passConstants(device, best ? 32.0f : 12.0f, best ? 0.0f : 1.0f, 0.3f);
                runPass(device, kDofBlur, second);
                dofTarget = &second;
            }
        }
        bind(device, 0, m_hdr.texture, true);
        bind(device, 1, dofTarget->texture, true);
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

    // 7b. Lamp glare (LampGlare, LampHaze): only the lamps' light (what the game's additive draws
    // added) at a quarter of the resolution, then a pyramid from an eighth down to 1/128, summed
    // back up with weights per level that shape its tail. Normalised: the haze glow carries as much
    // light as the haze took from the lamps (LampHaze 1), the glare a part of what the lighting
    // pass took from their core (LampGlare).
    float lampScale[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool lampReady = false;
    if (lampLight) {
        UINT lw = (in.width + 3) / 4, lh = (in.height + 3) / 4;
        lampReady = m_lampSource.create(device, lw, lh, D3DFMT_A16B16G16R16F);
        for (int i = 0; i < kLampLevels && lampReady; i++) {
            lw = lw > 3 ? (lw + 1) / 2 : 2;
            lh = lh > 3 ? (lh + 1) / 2 : 2;
            lampReady = m_lampDown[i].create(device, lw, lh, D3DFMT_A16B16G16R16F) &&
                        (i == kLampLevels - 1 || m_lampUp[i].create(device, lw, lh, D3DFMT_A16B16G16R16F));
        }
        // Out of video memory: no new try (and log line) every frame, the next map or device
        // reset tries again. What was created goes back.
        if (!lampReady) {
            m_lampFailed = true;
            m_lampSource.destroy();
            for (auto& t : m_lampDown) t.destroy();
            for (auto& t : m_lampUp) t.destroy();
            TMVS_LOG("pipeline: lamp glare and haze off: their render targets failed (until the next map)");
        }
    }
    if (lampReady) {
        bind(device, 0, in.hdr, true);
        bind(device, 1, m_nd.texture, false);
        bind(device, 14, in.added, true);
        runPass(device, kLampSource, m_lampSource);
        device->SetTexture(14, nullptr);
        const gfx::Target* above = &m_lampSource;
        for (int i = 0; i < kLampLevels; i++) {
            bind(device, 0, above->texture, true);
            passConstants(device, 1.0f / above->width, 1.0f / above->height);
            runPass(device, kLampDown, m_lampDown[i]);
            above = &m_lampDown[i];
        }
        // Weight of each wider level against the one above it. Above 1 the wide levels carry more:
        // a long soft tail that reaches the arch and the air around a floodlight (0.7, most of it
        // close to the lamp, only filled the gaps between the bulbs again: the same flat slab). The
        // haze's light lies wide, scattered in the air between the camera and the lamp.
        const float glareStep = 1.2f, hazeStep = 1.0f;
        float glareNorm = 0.0f, hazeNorm = 0.0f, glareLevel = 1.0f, hazeLevel = 1.0f;
        for (int i = 0; i < kLampLevels; i++) {
            glareNorm += glareLevel;
            hazeNorm += hazeLevel;
            glareLevel *= glareStep;
            hazeLevel *= hazeStep;
        }
        for (int i = kLampLevels - 2; i >= 0; i--) {
            const gfx::Target& lower = (i == kLampLevels - 2) ? m_lampDown[kLampLevels - 1] : m_lampUp[i + 1];
            bind(device, 0, lower.texture, true);
            bind(device, 1, m_lampDown[i].texture, true);
            passConstants(device, 1.0f / lower.width, 1.0f / lower.height, glareStep, hazeStep);
            runPass(device, kLampUp, m_lampUp[i]);
        }
        // Of the light taken from the core, 40% comes back as glare. All of it was too much: the
        // lamps carry as much light as the rest of the night scene (A01's start: 0.05 a pixel), and
        // spread out it washed the scene grey. The rest was clipped to white in the core anyway.
        // The source holds the lamps' light at Floodlights 1.
        lampScale[0] = floodlights * lampGlareTaken * 0.4f / glareNorm;
        lampScale[1] = floodlights * fmaxf(s.lampHaze, 0.0f) / hazeNorm;
    }

    // 8. Exposure (extra cameras reuse the adapted value).
    if (temporal && !in.freeze) {
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

    // Drops on the lens sit on the lens: after TAA (it would smear them along the camera's
    // motion), in the last pass.
    const bool post = s.debugView == 0;
    const bool drops = (lensDrops > 0.0f || lensSnow > 0.0f) && post;
    // Roof over the camera, eased like the sky average: the lens dries off under a bridge.
    const bool shelter = drops && heightMap && m_heightValid && s.weatherShelter;
    if (shelter && (temporal || !m_shelterValid)) {
        const int previous = m_shelterIndex;
        m_shelterIndex ^= 1;
        bind(device, 0, m_heightMap[m_heightIndex].texture, false);
        bind(device, 1, m_shelter[previous].texture, false);
        bind(device, 2, m_ceilingValid ? m_ceilingMap[m_heightIndex].texture : nullptr, false);
        // The way the rain comes (as the drops fall: wind, fall speed), or the snow's.
        const float rainVelocity[4] = {2.5f + s.wind * 5.0f, -9.0f, 1.0f + s.wind * 2.0f, 0.0f};
        const float snowVelocity[4] = {(1.0f + s.wind * 7.0f) * fminf(s.snow, 1.5f), -1.1f, (0.5f + s.wind * 3.0f) * fminf(s.snow, 1.5f), 0.0f};
        device->SetPixelShaderConstantF(33, s.rain > 0.0f ? rainVelocity : snowVelocity, 1);
        passConstants(device, 1.0f - expf(-dt / 0.8f), (m_shelterValid && m_temporalValid) ? 0.0f : 1.0f);
        runPass(device, kShelter, m_shelter[m_shelterIndex]);
        m_shelterValid = true;
    } else if (!shelter) {
        m_shelterValid = false;
    }

    // 9. Final grade.
    bind(device, 0, hdr->texture, true);
    bind(device, 1, m_bloomUp[0].texture, true);
    bind(device, 2, m_rays[0].texture, true);
    bind(device, 3, m_adapted[m_adaptIndex].texture, false);
    bind(device, 4, m_nd.texture, false);
    bind(device, 5, m_occlusion.texture, true);
    bind(device, 6, m_bloomDown[2].texture, true);
    // The colour look-up table: texel centres of an N^3 table (scale, offset), mix.
    const bool lut = in.lut > 0.001f && s.debugView == 0 && ensureLut(device);
    if (lut) {
        device->SetTexture(12, m_lut);
        device->SetSamplerState(12, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(12, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(12, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(12, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(12, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(12, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
        device->SetSamplerState(12, D3DSAMP_SRGBTEXTURE, FALSE);
    }
    const float n = static_cast<float>(m_lutSize > 0 ? m_lutSize : 2);
    // Night maps: light sources never end up darker than the game drew them (the grade and
    // the tone curve pulled lamps and the surfaces they light to grey). s8 = the game's image.
    // Not with EngineLights: the lamps glow as the game drew them, nothing needs putting back.
    const bool mapNight = in.sunColorKnown && classifyMood(in.sunColor) == Mood::Night && s.debugView == 0 && !engineLights;
    if (mapNight) bind(device, 8, in.color, false);
    bind(device, 13, materials ? m_materialTexture : nullptr, false);
    // Where the depth of field blurs, the lamps aren't put back sharp: s9 = its focus distance.
    if (dofFocus) bind(device, 9, dofFocus, false);
    const float finalMaterials[4] = {materials ? 1.0f : 0.0f, dofFocus ? 1.0f : 0.0f, gameSkyOut, s.toneKnee};
    device->SetPixelShaderConstantF(44, finalMaterials, 1);
    const float lutConstants[4] = {lut ? (in.lut > 1.0f ? 1.0f : in.lut) : 0.0f, (n - 1.0f) / n, 0.5f / n, mapNight ? 1.0f : 0.0f};
    device->SetPixelShaderConstantF(42, lutConstants, 1);
    passConstants(device, 1.0f / norm, 0.0f, s.nightSurfaces);
    // c33 (final pass): the lamp glare's and the lamp haze's strength (0: off, s10 not read).
    device->SetPixelShaderConstantF(33, lampScale, 1);
    if (lampReady) bind(device, 10, m_lampUp[0].texture, true);
    // Disabled post passes are skipped entirely, not just passed through.
    const bool fxaa = s.fxaa && post;
    const bool taa = s.taa && post && temporal && m_taa[0].create(device, in.width, in.height, D3DFMT_A16B16G16R16F) &&
                     m_taa[1].create(device, in.width, in.height, D3DFMT_A16B16G16R16F);
    const bool sharpen = s.sharpen > 0.001f && post && !upscale;
    // Chain: Final -> [FXAA] -> [TAA] -> [Sharpen | Copy] -> output (RenderScale: -> EASU -> RCAS).
    IDirect3DTexture9* current = nullptr;
    if (!fxaa && !taa && !sharpen && !drops) {
        runPass(device, kFinal, output, in.width, in.height);
    } else {
        runPass(device, kFinal, m_ldr);
        current = m_ldr.texture;
    }
    if (lampReady) device->SetTexture(10, nullptr); // the next frame draws into it
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
        bind(device, 3, m_hdr.texture, false); // alpha: where the neon trail lies
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
        bind(device, 1, shelter ? m_shelter[m_shelterIndex].texture : nullptr, false);
        passConstants(device, 1.0f / in.width, 1.0f / in.height);
        runPass(device, sharpen ? kSharpen : kCopy, output, in.width, in.height);
    }
    // Rain particles go on top of the finished image: TAA would erase thin, fast drops as
    // flicker, and keeping them out of its history avoids trails.
    if ((s.rain > 0.0f || s.wetness > 0.0f || s.snow > 0.0f || s.snowCover > 0.0f) && post && ensureRain(device))
        drawRain(device, in, s, dt, heightMap && m_heightValid, output);
    if (upscale) upscaleOutput(device, in, s, screen);
    profileEnd();

    if (in.repeat) swapRepeatCamera();
    if (temporal) {
        keepRepeatCamera();
        memcpy(m_prevView, in.view, sizeof(m_prevView));
        memcpy(m_prevProjection, in.projection, sizeof(m_prevProjection));
        m_havePrevious = true;
    }
    for (int i = 0; i < 10; i++) device->SetTexture(i, nullptr);
    for (int i = 12; i < 16; i++) device->SetTexture(i, nullptr); // the LUT, the materials: nothing of ours stays bound (Reset)
    if (softwareVP) device->SetSoftwareVertexProcessing(TRUE);
    return true;
}

// Every 15 frames the water material on screen is summed up (PS_WaterLevel) and read back once
// the GPU has got past it (an event query, asked without waiting). A flat sheet of it (within
// 0.6 m) is the sea. GetRenderTargetData waits for everything queued before it: read two frames
// later regardless, it stalled the CPU until the GPU had caught up (92 ms in the previewer, up
// to the 3 frames the game lets the GPU fall behind in game, 4 times a second).
void Pipeline::measureSeaLevel(IDirect3DDevice9* device) {
    // Not there after 6 frames (the previewer, which queues frames without presenting them): read
    // anyway, as before.
    const bool done = m_waterPending < 0 || !m_waterDone || m_waterDone->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_OK ||
                      m_frame - m_waterDrawn >= 6;
    if (m_waterPending >= 0 && m_frame - m_waterDrawn >= 2 && done) {
        const gfx::Target& t = m_waterLevel[m_waterPending];
        m_waterPending = -1;
        if (!m_waterRead) device->CreateOffscreenPlainSurface(1, 1, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &m_waterRead, nullptr);
        D3DLOCKED_RECT locked{};
        if (m_waterRead && t.surface && SUCCEEDED(device->GetRenderTargetData(t.surface, m_waterRead)) &&
            SUCCEEDED(m_waterRead->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
            float v[4];
            memcpy(v, locked.pBits, sizeof(v));
            m_waterRead->UnlockRect();
            if (v[1] >= 8.0f && !m_waterMaterialSeen) {
                m_waterMaterialSeen = true;
                TMVS_LOG("pipeline: the map's water is the game's water material (%.0f points)", v[1]);
            }
            if (v[1] >= 24.0f && v[3] - v[2] < 0.6f) {
                const float level = v[0] / v[1];
                if (!m_seaKnown || fabsf(level - m_seaLevel) > 0.3f)
                    TMVS_LOG("pipeline: sea level %.2f m, measured on the water material (%.0f points)", level, v[1]);
                m_seaLevel = level;
                m_seaKnown = true;
            }
        }
    }
    if (m_waterPending < 0 && m_frame % 15 == 0) {
        const int i = m_waterIndex ^= 1;
        if (m_waterLevel[i].create(device, 1, 1, D3DFMT_A32B32G32R32F)) {
            bind(device, 0, m_nd.texture, false);
            bind(device, 1, m_materialTexture, false);
            runPass(device, kWaterLevel, m_waterLevel[i]);
            if (!m_waterDone) device->CreateQuery(D3DQUERYTYPE_EVENT, &m_waterDone);
            if (m_waterDone) m_waterDone->Issue(D3DISSUE_END);
            m_waterPending = i;
            m_waterDrawn = m_frame;
        }
    }
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

bool Pipeline::copyDepthArea(IDirect3DDevice9* device, IDirect3DTexture9* depth, const RECT& area, gfx::Target& out) {
    if (!m_ready) return false;
    D3DSURFACE_DESC desc{};
    depth->GetLevelDesc(0, &desc);
    const UINT w = static_cast<UINT>(area.right - area.left), h = static_cast<UINT>(area.bottom - area.top);
    if (!out.create(device, w, h, D3DFMT_R32F)) return false;
    const BOOL softwareVP = setPassStates(device);
    bind(device, 0, depth, false);
    passConstants(device, static_cast<float>(area.left) / desc.Width, static_cast<float>(area.top) / desc.Height,
                  static_cast<float>(w) / desc.Width, static_cast<float>(h) / desc.Height);
    runPass(device, kCopyDepth, out);
    device->SetTexture(0, nullptr);
    if (softwareVP) device->SetSoftwareVertexProcessing(TRUE);
    return true;
}

bool Pipeline::resolveScene(IDirect3DDevice9* device, IDirect3DTexture9* scene, IDirect3DSurface9* target, const RECT& area) {
    return copyScene(device, scene, target, area, false);
}

// RenderScale: m_scaled (the finished image at the scene's size) up to `output` with FSR 1. RCAS
// by FsrSharpness, AMD's sharpness as a factor, 2^-stops: 1 is its strongest (0 stops), its
// default of 0.2 stops is 0.87, ours of 0.8 is 0.32 stops. Debug views get EASU only.
void Pipeline::upscaleOutput(IDirect3DDevice9* device, const Inputs& in, const Settings& s, IDirect3DSurface9* output) {
    const UINT ow = in.outputWidth, oh = in.outputHeight;
    const float amount = s.debugView == 0 ? fminf(s.fsrSharpness, 1.0f) : 0.0f;
    const bool rcas = amount > 0.001f && m_fsr.create(device, ow, oh, D3DFMT_A8R8G8B8);
    bind(device, 0, m_scaled.texture, false);
    passConstants(device, static_cast<float>(in.width), static_cast<float>(in.height), 1.0f / in.width, 1.0f / in.height);
    runPass(device, kFsrEasu, rcas ? m_fsr.surface : output, ow, oh);
    if (rcas) {
        bind(device, 0, m_fsr.texture, false);
        passConstants(device, 1.0f / ow, 1.0f / oh, amount);
        runPass(device, kFsrRcas, output, ow, oh);
    }
    device->SetTexture(0, nullptr);
}

bool Pipeline::stretchScene(IDirect3DDevice9* device, IDirect3DTexture9* scene, const RECT& from, IDirect3DSurface9* target, const RECT& to) {
    if (!m_ready || !scene || !target || to.right <= to.left || to.bottom <= to.top) return false;
    D3DSURFACE_DESC sceneDesc{}, desc{};
    scene->GetLevelDesc(0, &sceneDesc);
    target->GetDesc(&desc);
    // PS_Resolve reads u_Pass0.xy + uv * u_Pass0.zw, uv over the whole target: `to` maps onto `from`.
    const float sx = static_cast<float>(from.right - from.left) / sceneDesc.Width * desc.Width / (to.right - to.left);
    const float sy = static_cast<float>(from.bottom - from.top) / sceneDesc.Height * desc.Height / (to.bottom - to.top);
    const float ox = static_cast<float>(from.left) / sceneDesc.Width - static_cast<float>(to.left) / desc.Width * sx;
    const float oy = static_cast<float>(from.top) / sceneDesc.Height - static_cast<float>(to.top) / desc.Height * sy;
    const BOOL softwareVP = setPassStates(device);
    device->SetRenderTarget(0, target); // resets the scissor rect: set it after
    device->SetScissorRect(&to);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    bind(device, 0, scene, true);
    passConstants(device, ox, oy, sx, sy);
    const float mode[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // c33.x 0: cut to 0..1
    device->SetPixelShaderConstantF(33, mode, 1);
    runPass(device, kResolve, target, desc.Width, desc.Height);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetTexture(0, nullptr);
    if (softwareVP) device->SetSoftwareVertexProcessing(TRUE);
    return true;
}

bool Pipeline::downscale(IDirect3DDevice9* device, IDirect3DTexture9* source, gfx::Target& out, UINT w, UINT h, D3DFORMAT format, bool depth) {
    if (!m_ready || !source || !out.create(device, w, h, depth ? D3DFMT_R32F : format)) return false;
    const BOOL softwareVP = setPassStates(device);
    bind(device, 0, source, false);
    passConstants(device, 0.0f, 0.0f, 1.0f, 1.0f);
    const float mode[4] = {1.0f, 1.0f, 0.0f, 0.0f}; // c33 xy 1: as it is, alpha too (PS_Resolve)
    device->SetPixelShaderConstantF(33, mode, 1);
    runPass(device, depth ? kCopyDepth : kResolve, out);
    device->SetTexture(0, nullptr);
    if (softwareVP) device->SetSoftwareVertexProcessing(TRUE);
    return true;
}

bool Pipeline::addScene(IDirect3DDevice9* device, IDirect3DTexture9* added, IDirect3DSurface9* target, const RECT& area) {
    return copyScene(device, added, target, area, true);
}

bool Pipeline::capScene(IDirect3DDevice9* device, IDirect3DSurface9* target, const RECT& area) {
    if (!m_ready || !target) return false;
    if (!m_solidPS) m_solidPS = gfx::compilePixelShader(device, kSolidPS, "main", "Solid");
    if (!m_solidPS) return false;
    D3DSURFACE_DESC desc{};
    target->GetDesc(&desc);
    const BOOL softwareVP = setPassStates(device);
    device->SetRenderTarget(0, target); // resets the scissor rect: set it after
    device->SetScissorRect(&area);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    // The smaller of what is there and the ceiling (the blend factors don't count for MIN); the
    // colour only, the game's alpha stays.
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_MIN);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    const float ceiling[4] = {kSceneCeiling, kSceneCeiling, kSceneCeiling, kSceneCeiling};
    device->SetPixelShaderConstantF(32, ceiling, 1);
    device->SetPixelShader(m_solidPS);
    m_quad.draw(device, desc.Width, desc.Height);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    if (softwareVP) device->SetSoftwareVertexProcessing(TRUE);
    return true;
}

bool Pipeline::fillBars(IDirect3DDevice9* device, IDirect3DTexture9* band, IDirect3DSurface9* target, const RECT& area, bool top, bool bottom) {
    if (!m_ready || !band || !target) return false;
    D3DSURFACE_DESC desc{};
    target->GetDesc(&desc);
    const LONG width = static_cast<LONG>(desc.Width), height = static_cast<LONG>(desc.Height);
    const LONG bandHeight = area.bottom - area.top;
    if (area.left != 0 || area.right != width || bandHeight < 1 || area.top < 0 || area.bottom > height) return false;
    const float h = static_cast<float>(bandHeight), scale = static_cast<float>(height) / h;
    const BOOL softwareVP = setPassStates(device);
    device->SetRenderTarget(0, target);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    device->SetPixelShader(m_shaders[kResolve]);
    bind(device, 0, band, false);
    const float mode[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // c33.x 0: cut to 0..1 (an 8-bit image anyway)
    device->SetPixelShaderConstantF(33, mode, 1);
    // PS_Resolve reads the band at u_Pass0.xy + uv * u_Pass0.zw, uv over the whole target: row y
    // above the band gets band row (top - 1 - y), row y below it band row (2 h - 1 - (y - top)).
    // The scissor rect after SetRenderTarget (which resets it); the quad sets only the viewport.
    bool drew = false;
    if (top && area.top > 0) {
        const RECT rows = {0, area.top > bandHeight ? area.top - bandHeight : 0, width, area.top};
        device->SetScissorRect(&rows);
        passConstants(device, 0.0f, static_cast<float>(area.top) / h, 1.0f, -scale);
        m_quad.draw(device, desc.Width, desc.Height);
        drew = true;
    }
    if (bottom && area.bottom < height) {
        const RECT rows = {0, area.bottom, width, height - area.bottom > bandHeight ? area.bottom + bandHeight : height};
        device->SetScissorRect(&rows);
        passConstants(device, 0.0f, (2.0f * h + static_cast<float>(area.top)) / h, 1.0f, -scale);
        m_quad.draw(device, desc.Width, desc.Height);
        drew = true;
    }
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetTexture(0, nullptr);
    if (softwareVP) device->SetSoftwareVertexProcessing(TRUE);
    return drew;
}

bool Pipeline::copyScene(IDirect3DDevice9* device, IDirect3DTexture9* source, IDirect3DSurface9* target, const RECT& area, bool add) {
    if (!m_ready) return false;
    D3DSURFACE_DESC desc{};
    target->GetDesc(&desc);
    const BOOL softwareVP = setPassStates(device);
    device->SetRenderTarget(0, target); // resets the scissor rect: set it after
    device->SetScissorRect(&area);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    if (add) {
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    }
    bind(device, 0, source, false);
    passConstants(device, 0.0f, 0.0f, 1.0f, 1.0f);
    const float mode[4] = {add ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f}; // c33.x: 1 = as it is (added), 0 = cut to 0..1
    device->SetPixelShaderConstantF(33, mode, 1);
    runPass(device, kResolve, target, desc.Width, desc.Height);
    if (add) device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetTexture(0, nullptr);
    if (softwareVP) device->SetSoftwareVertexProcessing(TRUE);
    return true;
}

bool Pipeline::readHalfColor(IDirect3DDevice9* device, IDirect3DSurface9* surface, std::vector<uint16_t>& out) {
    D3DSURFACE_DESC desc{};
    surface->GetDesc(&desc);
    if (desc.Format != D3DFMT_A16B16G16R16F) return false;
    IDirect3DSurface9* system = nullptr;
    if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &system, nullptr)))
        return false;
    bool ok = SUCCEEDED(device->GetRenderTargetData(surface, system));
    D3DLOCKED_RECT locked{};
    if (ok && SUCCEEDED(system->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        out.resize(static_cast<size_t>(desc.Width) * desc.Height * 4);
        for (UINT y = 0; y < desc.Height; y++) {
            memcpy(&out[static_cast<size_t>(y) * desc.Width * 4], static_cast<const BYTE*>(locked.pBits) + static_cast<size_t>(locked.Pitch) * y,
                   desc.Width * 8);
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
    const BOOL softwareVP = setPassStates(device);
    bind(device, 0, depth, false);
    passConstants(device, 0.0f, 0.0f, 1.0f, 1.0f);
    runPass(device, kCopyDepth, m_depthCopy);
    device->SetTexture(0, nullptr);
    if (softwareVP) device->SetSoftwareVertexProcessing(TRUE);

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
    // Captures in a resized window once came out with depth 0 everywhere; say what was read.
    if (ok && std::all_of(out.begin(), out.end(), [](float d) { return d == 0.0f; }))
        TMVS_LOG("capture: depth is all 0 (depth texture %ux%u format %u, pool %u)", desc.Width, desc.Height,
                 static_cast<unsigned>(desc.Format), static_cast<unsigned>(desc.Pool));
    return ok;
}

} // namespace tmshaders
