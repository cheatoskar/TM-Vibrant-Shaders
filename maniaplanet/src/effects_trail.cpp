#include "effects_trail.h"
#include "effects.h"
#include "log.h"
#include "maplook.h"
#include "settings.h"
#include "shaders/PS_Trail.h"
#include "shaders/VS_Trail.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace tmvs_mp {
namespace {

template <typename T> void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// trail.hlsl TrailParams (b4)
struct TrailConstants {
    float trail[4];  // points, next index (unwrapped), now (s), fade after (s)
    float trail2[4]; // sideways offset (m), half width (m), ring size, 0
    float colour[4]; // rgb * strength
};

} // namespace

bool TrailStage::init(ID3D11Device* device) {
    bool ok = SUCCEEDED(device->CreateVertexShader(g_VS_Trail, sizeof(g_VS_Trail), nullptr, &m_vs));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_Trail, sizeof(g_PS_Trail), nullptr, &m_ps));
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = kPoints;
    td.Height = 1;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ok &= SUCCEEDED(device->CreateTexture2D(&td, nullptr, &m_pointsTex)) &&
          SUCCEEDED(device->CreateShaderResourceView(m_pointsTex, nullptr, &m_pointsSrv));
    D3D11_BUFFER_DESC cb = {};
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.ByteWidth = sizeof(TrailConstants);
    ok &= SUCCEEDED(device->CreateBuffer(&cb, nullptr, &m_cb));
    // Additive into the HDR image; its alpha stays.
    D3D11_BLEND_DESC bd = {};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = rt.DestBlend = D3D11_BLEND_ONE;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
    rt.DestBlendAlpha = D3D11_BLEND_ONE;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    ok &= SUCCEEDED(device->CreateBlendState(&bd, &m_add));
    m_points.assign(kPoints * 4, 0.0f);
    m_ready = ok;
    if (!ok) TMVS_LOG("trail: creating shaders or buffers failed");
    return ok;
}

void TrailStage::release() {
    safeRelease(m_vs);
    safeRelease(m_ps);
    safeRelease(m_pointsSrv);
    safeRelease(m_pointsTex);
    safeRelease(m_cb);
    safeRelease(m_add);
    m_ready = false;
}

bool TrailStage::supports(const char* key) {
    static const char* const kKeys[] = {"NeonTrail", "TrailColor", "TrailWidth", "TrailTyres", "TrailDuration"};
    for (const char* k : kKeys)
        if (!strcmp(k, key)) return true;
    return false;
}

void TrailStage::clear() {
    m_head = m_count = 0;
    m_time = m_clock = 0.0f;
    m_keptValid = false;
    m_startValid = false;
    m_break = true;
    std::fill(m_points.begin(), m_points.end(), 0.0f);
}

// Records the car: every frame into the next slot (the live end, so it stays on the car), kept
// 60 times a second while it moves, or every metre. A jump (a respawn) starts a new line; a frame
// without the car doesn't (it left holes). True: the live end is drawn (slot m_head).
bool TrailStage::record(float dt, const float* car) {
    if (!car) {
        // A frame without the car (its constants not found this time): the live end stays where
        // it was last seen for a few frames. (Without it the line's end fell back to the last kept
        // point and jumped forward again: the fade-in behind the car stuttered.)
        m_liveFrames = m_liveFrames > 0 ? m_liveFrames - 1 : 0;
        return m_liveFrames > 0;
    }
    m_liveFrames = 0;
    if (dt <= 0.0f) return false;
    if (!m_startValid) {
        memcpy(m_start, car, sizeof(m_start));
        m_startValid = true;
    }
    float moved = 0.0f;
    if (m_keptValid) {
        const float dx = car[0] - m_kept[0], dy = car[1] - m_kept[1], dz = car[2] - m_kept[2];
        moved = sqrtf(dx * dx + dy * dy + dz * dz);
        if (moved > 25.0f) m_break = true;
    }
    const float stamp = m_time + 1.0f; // never 0 (= no car)
    float* p = &m_points[m_head * 4];
    memcpy(p, car, sizeof(float) * 3);
    p[3] = m_break ? -stamp : stamp;
    // Kept: 60 a second while it moves, and every metre. Less than 30 cm from the last point
    // (standing, shaking after a crash) is no line: those points pointed every way and fanned the
    // tyre lines out around the car.
    if ((m_clock >= 1.0f / 60.0f && moved >= kMinStep) || moved >= 1.0f || !m_keptValid) {
        m_clock = moved >= 1.0f ? 0.0f : fmodf(m_clock, 1.0f / 60.0f);
        memcpy(m_kept, car, sizeof(m_kept));
        m_keptValid = true;
        m_break = false;
        m_head = (m_head + 1) % kPoints;
        if (m_count < kPoints) m_count++;
        return false; // just kept: it's the newest point now
    }
    // The live end follows the car from 1 cm on. (It waited for 30 cm, like a kept point: above
    // 60 frames a second the line's end fell 20-30 cm behind the car every second or third frame
    // and caught up again, and the fade-in behind the car, measured from that end, stuttered. The
    // live piece itself lies inside the fade-in, so it can't fan out.)
    if (moved < 0.01f) return false;
    m_liveFrames = 3;
    return true;
}

void TrailStage::draw(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame, const tmshaders::Settings& s) {
    if (!m_ready) return;
    const float dt = m_lastTime >= 0.0 ? static_cast<float>(std::clamp(frame.time - m_lastTime, 0.0, 0.25)) : 0.0f;
    m_lastTime = frame.time;
    const EngineCar* car = frame.car;
    const bool carSeen = car && car->valid;
    // The car's origin lies on the road (capture 030: 2 mm under it, 2 cm under it to 3 cm over it
    // as the car rides), and the road hides the line from 4.2 cm under it (the shader's depth test,
    // seen from the chase camera): a car pressed into its springs took the line with it. 10 cm
    // over the origin along the car's up it stays clear of the road.
    float position[3] = {};
    if (carSeen)
        for (int i = 0; i < 3; i++) position[i] = car->position[i] + car->up[i] * kLift;
    // Another map (or the editor): start over. Without the car (a free camera far away, a menu on
    // top) the trail stays as it is.
    if (maplook::mapSerial() != m_mapSerial) {
        m_mapSerial = maplook::mapSerial();
        clear();
    }
    // The run starts again: the car jumps back to where the trail began (a respawn at a checkpoint
    // only breaks the line).
    if (carSeen && m_keptValid && m_startValid) {
        const float dx = position[0] - m_kept[0], dy = position[1] - m_kept[1], dz = position[2] - m_kept[2];
        const float sx = position[0] - m_start[0], sy = position[1] - m_start[1], sz = position[2] - m_start[2];
        if (dx * dx + dy * dy + dz * dz > 25.0f * 25.0f && sx * sx + sy * sy + sz * sz < 12.0f * 12.0f) clear();
    }
    // Another car taken up (the camera went to another, or the one followed was a ghost): a new
    // line, none from the old car's place to the new one's.
    if (carSeen && car->track != m_track) {
        m_track = car->track;
        m_break = true;
        m_liveFrames = 0;
    }
    if (s.neonTrail <= 0.0f) {
        m_break = true;
        return;
    }
    m_time += dt;
    m_clock += dt;

    const bool live = record(dt, carSeen ? position : nullptr);
    const UINT points = live ? std::min(m_count + 1, kPoints) : m_count;
    const UINT next = live ? (m_head + 1) % kPoints : m_head;
    if (points < 2) return;

    ctx->UpdateSubresource(m_pointsTex, 0, nullptr, m_points.data(), kPoints * 16, 0);
    TrailConstants c = {};
    c.trail[0] = static_cast<float>(points);
    c.trail[1] = static_cast<float>(next + kPoints);
    c.trail[2] = m_time + 1.0f;
    c.trail[3] = s.trailDuration;
    c.trail2[1] = s.trailWidth * 0.5f;
    c.trail2[2] = static_cast<float>(kPoints);
    const float strength = s.neonTrail * 2.0f;
    for (int i = 0; i < 3; i++) c.colour[i] = s.trailColor[i] * strength;

    ID3D11RenderTargetView* rtv = fx.currentRtv();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = {0, 0, static_cast<float>(fx.width()), static_cast<float>(fx.height()), 0, 1};
    ctx->RSSetViewports(1, &vp);
    const float factor[4] = {};
    ctx->OMSetBlendState(m_add, factor, 0xffffffff);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_vs, nullptr, 0);
    ctx->PSSetShader(m_ps, nullptr, 0);
    ctx->VSSetShaderResources(0, 1, &m_pointsSrv);
    // Two lines from the rear tyres, or one from the middle of the car.
    const float offsets[2] = {s.trailTyres ? -0.75f : 0.0f, 0.75f};
    for (int line = 0; line < (s.trailTyres ? 2 : 1); line++) {
        c.trail2[0] = offsets[line];
        ctx->UpdateSubresource(m_cb, 0, nullptr, &c, 0, 0);
        ctx->VSSetConstantBuffers(4, 1, &m_cb);
        ctx->PSSetConstantBuffers(4, 1, &m_cb);
        ctx->Draw((points - 1) * 6, 0);
    }
    fx.profiler().mark(ctx, kTrail);
    ID3D11ShaderResourceView* none = nullptr;
    ctx->VSSetShaderResources(0, 1, &none);
    ID3D11Buffer* noCb = nullptr;
    ctx->VSSetConstantBuffers(4, 1, &noCb);
    ctx->PSSetConstantBuffers(4, 1, &noCb);
}

} // namespace tmvs_mp
