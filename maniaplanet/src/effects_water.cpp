#include "effects_water.h"
#include "effects.h"
#include "hooks.h"
#include "log.h"
#include "settings.h"
#include "water11.h"
#include "waterfx.h"
#include "shaders/PS_WaterLevel.h"
#include "shaders/PS_WaterSplash.h"
#include "shaders/VS_WaterSplash.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace tmvs_mp {
namespace {

template <typename T> void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// The Stadium car's axles and track (as effects_weather.cpp), for a car whose bones show no wheels.
constexpr float kFrontAxle = 1.7f;
constexpr float kRearAxle = -1.2f;
constexpr float kHalfTrack = 0.86f;

// weather.hlsl WaterParams (b5)
struct WaterConstants {
    float level[4];     // height, setting, wake, drips
    float entries[8][4];
    float kind[4];      // kind, entry, seconds since out, ring
    float car[5][4];    // position (w 1), forward (w front axle), right (w rear axle), up (w half track), velocity (w contact)
};

// How big the splash is for a car this fast, coming in this steeply: a car that skims in flat
// throws up less than one that drops in (TMNF).
float entryStrength(float speed, float sinAngle) {
    const float fast = std::clamp((speed - 2.0f) / 30.0f, 0.0f, 1.0f);
    return powf(fast, 0.8f) * (0.45f + 0.55f * std::min(sinAngle * 2.2f, 1.0f));
}

} // namespace

float weatherCarForwardSign(); // effects_weather.cpp: which of the car's model axes is ahead

bool WaterStage::init(ID3D11Device* device) {
    bool ok = SUCCEEDED(device->CreatePixelShader(g_PS_WaterLevel, sizeof(g_PS_WaterLevel), nullptr, &m_levelPs));
    ok &= SUCCEEDED(device->CreateVertexShader(g_VS_WaterSplash, sizeof(g_VS_WaterSplash), nullptr, &m_splashVs));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_WaterSplash, sizeof(g_PS_WaterSplash), nullptr, &m_splashPs));
    D3D11_BUFFER_DESC cb = {};
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.ByteWidth = sizeof(WaterConstants);
    ok &= SUCCEEDED(device->CreateBuffer(&cb, nullptr, &m_cb));
    D3D11_BLEND_DESC bd = {};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
    rt.DestBlendAlpha = D3D11_BLEND_ONE;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    ok &= SUCCEEDED(device->CreateBlendState(&bd, &m_over));
    // The water pictures (r droplet, g foam, b a ripple), the TMNF mod's (TMVS\textures\water\ or built in).
    const std::vector<uint32_t> texels = tmshaders::waterfx::buildAtlas(tmshaders::log::dataDir() + L"\\textures\\water");
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = tmshaders::waterfx::kAtlasSize;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {texels.data(), tmshaders::waterfx::kAtlasSize * 4u, 0};
    ok &= SUCCEEDED(device->CreateTexture2D(&td, &init, &m_pictures)) &&
          SUCCEEDED(device->CreateShaderResourceView(m_pictures, nullptr, &m_picturesSrv));
    // One texel per plane, read back.
    td.Width = water::kMaxPlanes;
    td.Height = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ok &= SUCCEEDED(device->CreateTexture2D(&td, nullptr, &m_levelTex)) &&
          SUCCEEDED(device->CreateRenderTargetView(m_levelTex, nullptr, &m_levelRtv));
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& t : m_staging) ok &= SUCCEEDED(device->CreateTexture2D(&td, nullptr, &t));
    m_ready = ok;
    if (!ok) TMVS_LOG("water: the splash's shaders or targets failed");
    return ok;
}

void WaterStage::release() {
    safeRelease(m_levelPs);
    safeRelease(m_splashVs);
    safeRelease(m_splashPs);
    safeRelease(m_cb);
    safeRelease(m_over);
    safeRelease(m_picturesSrv);
    safeRelease(m_pictures);
    safeRelease(m_levelRtv);
    safeRelease(m_levelTex);
    for (auto& t : m_staging) safeRelease(t);
    for (auto& p : m_stagingPending) p = false;
    m_ready = false;
}

bool WaterStage::supports(const char* key) {
    return !strcmp(key, "WaterClarity") || !strcmp(key, "WaterSplash");
}

bool WaterStage::wanted(const tmshaders::Settings& s) {
    return s.waterSplash > 0.0f && water::fogView() != nullptr;
}

// The surface near the car, one texel per plane (PS_WaterLevel), copied for a read-back.
void WaterStage::measure(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame) {
    ID3D11ShaderResourceView* fog = water::fogView();
    const int planes = water::planeCount();
    if (!fog || planes <= 0 || !frame.car || !frame.car->valid) return;
    if (m_stagingPending[m_stagingNext]) return; // all three still on their way
    ID3D11Buffer* const* buffers = water::planeBuffers();
    ID3D11Buffer* cbs[water::kMaxPlanes] = {};
    for (int i = 0; i < planes; i++) cbs[i] = buffers[i];
    ctx->PSSetConstantBuffers(6, water::kMaxPlanes, cbs);
    const float p0[4] = {frame.car->position[0], frame.car->position[1], frame.car->position[2], static_cast<float>(planes)};
    const float p1[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    fx.run(ctx, kWaterSplash, m_levelPs, m_levelRtv, water::kMaxPlanes, 1, &fog, 1, p0, p1);
    ID3D11Buffer* none[water::kMaxPlanes] = {};
    ctx->PSSetConstantBuffers(6, water::kMaxPlanes, none);
    ctx->CopyResource(m_staging[m_stagingNext], m_levelTex);
    m_stagingPending[m_stagingNext] = true;
    m_stagingNext = (m_stagingNext + 1) % kReadbacks;
}

// The read-backs the GPU has finished (never waits): the plane nearest the tyres' bottom that the
// camera sees around the car is the water's surface there.
void WaterStage::collect(ID3D11DeviceContext* ctx, double time, float bottom) {
    hooks::Inside own; // the mod's read-back, not the game's (the video export watch)
    for (int k = 0; k < kReadbacks; k++) {
        const int i = (m_stagingNext + k) % kReadbacks; // oldest first
        if (!m_stagingPending[i]) continue;
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (FAILED(ctx->Map(m_staging[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) break;
        float v[water::kMaxPlanes][4];
        memcpy(v, m.pData, sizeof(v));
        ctx->Unmap(m_staging[i], 0);
        m_stagingPending[i] = false;
        int best = -1;
        for (int p = 0; p < water::kMaxPlanes; p++) {
            // Three points on it, the car not more than 4 m under it (a lake up on a ledge beside
            // the car is not the water it drives in).
            if (!(v[p][0] >= 3.0f) || !(v[p][3] < 4.0f) || !std::isfinite(v[p][2])) continue;
            if (best < 0 || std::fabs(v[p][2] - bottom) < std::fabs(v[best][2] - bottom)) best = p;
        }
        if (best >= 0) {
            m_level = v[best][2];
            m_levelKnown = true;
            m_levelSeen = time;
            if (m_levelLogs < 12 && std::fabs(m_level - m_loggedLevel) > 0.3f) {
                m_levelLogs++;
                m_loggedLevel = m_level;
                TMVS_LOG("water: surface near the car at %.2f m (%.0f of %.0f points on it, the car %.2f m under it)", m_level, v[best][0],
                         v[best][1], v[best][3]);
            }
        }
    }
    // Not seen for a second and a half (the car far from any water, the camera elsewhere): unknown.
    if (m_levelKnown && time - m_levelSeen > 1.5) m_levelKnown = false;
}

// The car's tyres against the surface, once per frame on the effects' clock (TMNF updateWater): going
// under starts a splash (where, how fast, how steeply), staying under keeps the wake, coming out
// starts the drips. A jump of the car (respawn, replay cut) is none of these.
void WaterStage::follow(const SceneFrame& frame, const tmshaders::Settings& s, float dt) {
    const EngineCar* car = frame.car && frame.car->valid ? frame.car : nullptr;
    if (!car || dt <= 1e-4f || s.waterSplash <= 0.0f || !m_levelKnown) {
        m_valid = m_in = false;
        m_wake = m_inFor = 0.0f;
        return;
    }
    const float* p = car->position;
    const float bottom = p[1]; // the Stadium car's origin lies on the road (effects_weather.cpp)
    const float level = m_level;
    const float depth = level - bottom;
    const float moved = sqrtf((p[0] - m_prev[0]) * (p[0] - m_prev[0]) + (p[1] - m_prev[1]) * (p[1] - m_prev[1]) + (p[2] - m_prev[2]) * (p[2] - m_prev[2]));
    if (!m_valid || moved > 150.0f * dt + 1.0f) {
        m_in = depth > 0.05f;
        m_inFor = 0.0f;
        if (!m_in) m_wake = 0.0f;
    } else if (!m_in && depth > 0.05f) {
        // Where it crossed the surface, between the last frame and this one.
        const float before = level - m_prevBottom;
        const float f = std::clamp((0.0f - before) / (depth - before), 0.0f, 1.0f);
        float v[3];
        for (int j = 0; j < 3; j++) v[j] = (p[j] - m_prev[j]) / dt;
        const float speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        const float horizontal = sqrtf(v[0] * v[0] + v[2] * v[2]);
        if (speed > 2.0f) {
            Entry& e = m_entries[m_entryNext++ & 3];
            for (int j = 0; j < 3; j++) {
                e.position[j] = m_prev[j] + (p[j] - m_prev[j]) * f;
                e.velocity[j] = v[j];
            }
            e.position[1] = level;
            const float sinAngle = std::max(-v[1], 0.0f) / speed;
            e.strength = entryStrength(speed, sinAngle);
            e.time = frame.time;
            if (frame.time - m_logTime >= 1.0 || frame.time < m_logTime) {
                m_logTime = frame.time;
                TMVS_LOG("water: splash at %.0f m/s, %.0f deg entry (strength %.2f)", speed, atan2f(std::max(-v[1], 0.0f), horizontal) * 57.29578f,
                         e.strength);
            }
        }
        m_in = true;
        m_inFor = 0.0f;
    } else if (m_in && depth < -0.05f) {
        m_in = false;
        m_exitTime = frame.time;
        m_drip = std::min(m_inFor / 0.5f, 1.0f);
    }
    if (m_in) m_inFor += dt;
    const float target = m_in ? std::min(depth / 0.35f, 1.0f) : 0.0f;
    m_wake += (target - m_wake) * std::min(dt * 6.0f, 1.0f);
    memcpy(m_prev, p, sizeof(m_prev));
    m_prevBottom = bottom;
    m_valid = true;
}

void WaterStage::draw(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame, const tmshaders::Settings& s) {
    if (!m_ready) return;
    const float dt = m_lastTime >= 0.0 ? static_cast<float>(std::clamp(frame.time - m_lastTime, 0.0, 0.25)) : 0.0f;
    m_lastTime = frame.time;
    if (s.waterSplash <= 0.0f) {
        m_valid = m_in = false;
        m_wake = 0.0f;
        return;
    }
    const EngineCar* car = frame.car && frame.car->valid ? frame.car : nullptr;
    // The car's velocity from its position on the effects' clock (more than 150 m/s: a respawn or a cut).
    if (car && car->track != m_track) {
        m_track = car->track;
        m_carPrevTime = -1.0;
        memset(m_velocity, 0, sizeof(m_velocity));
    }
    if (car && frame.time != m_carPrevTime) {
        const double since = frame.time - m_carPrevTime;
        bool measured = false;
        if (m_carPrevTime >= 0.0 && since > 1e-4 && since < 0.25) {
            float v[3];
            for (int j = 0; j < 3; j++) v[j] = static_cast<float>((car->position[j] - m_carPrev[j]) / since);
            if (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] < 150.0f * 150.0f) {
                for (int j = 0; j < 3; j++) m_velocity[j] += (v[j] - m_velocity[j]) * 0.5f;
                measured = true;
            }
        }
        if (!measured) memset(m_velocity, 0, sizeof(m_velocity));
        memcpy(m_carPrev, car->position, sizeof(m_carPrev));
        m_carPrevTime = frame.time;
    }

    if (m_test) {
        m_level = m_testLevel;
        m_levelKnown = car != nullptr;
        m_levelSeen = frame.time;
    } else {
        collect(ctx, frame.time, car ? car->position[1] : 0.0f);
        measure(ctx, fx, frame);
        fx.bindFrame(ctx);
    }
    follow(frame, s, dt);

    // What to draw: the young entries, the wake while the car is in, the drips after it came out.
    WaterConstants wc = {};
    bool any = false;
    for (int i = 0; i < 4; i++) {
        const Entry& e = m_entries[i];
        const double age = frame.time - e.time;
        const bool young = age >= 0.0 && age < 6.0 && e.strength > 0.0f;
        any = any || young;
        for (int j = 0; j < 3; j++) {
            wc.entries[i * 2][j] = e.position[j];
            wc.entries[i * 2 + 1][j] = e.velocity[j];
        }
        wc.entries[i * 2][3] = young ? static_cast<float>(age) : -1.0f;
        wc.entries[i * 2 + 1][3] = e.strength;
    }
    float dripSince = static_cast<float>(frame.time - m_exitTime);
    float drip = m_drip;
    if (dripSince < 0.0f || dripSince > 3.2f) {
        dripSince = -1.0f;
        drip = 0.0f;
    }
    const bool withCar = car && m_levelKnown;
    const bool driving = withCar && (m_wake > 0.01f || dripSince >= 0.0f);
    if (!any && !driving) return;

    wc.level[0] = m_level;
    wc.level[1] = s.waterSplash;
    wc.level[2] = m_wake;
    wc.level[3] = drip;
    if (car) {
        const float sign = weatherCarForwardSign();
        float front = kFrontAxle, rear = kRearAxle, half = kHalfTrack;
        if (car->wheels.known) {
            const float a = car->wheels.zA * sign, b = car->wheels.zB * sign;
            front = std::max(a, b);
            rear = std::min(a, b);
            half = car->wheels.halfTrack;
        }
        for (int j = 0; j < 3; j++) {
            wc.car[0][j] = car->position[j];
            wc.car[1][j] = car->forward[j] * sign;
            wc.car[2][j] = car->right[j];
            wc.car[3][j] = car->up[j];
            wc.car[4][j] = m_velocity[j];
        }
        wc.car[0][3] = 1.0f;
        wc.car[1][3] = front;
        wc.car[2][3] = rear;
        wc.car[3][3] = half;
        wc.car[4][3] = 0.0f;
    }

    ctx->VSSetConstantBuffers(5, 1, &m_cb);
    ctx->PSSetConstantBuffers(5, 1, &m_cb);
    ID3D11ShaderResourceView* none[8] = {};
    ctx->PSSetShaderResources(0, 8, none);
    ID3D11RenderTargetView* rtv = fx.currentRtv();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = {0, 0, static_cast<float>(fx.width()), static_cast<float>(fx.height()), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ID3D11ShaderResourceView* in[2] = {fx.halfColour(), m_picturesSrv};
    ctx->PSSetShaderResources(0, 2, in);
    const float factor[4] = {};
    ctx->OMSetBlendState(m_over, factor, 0xffffffff);
    ctx->VSSetShader(m_splashVs, nullptr, 0);
    ctx->PSSetShader(m_splashPs, nullptr, 0);
    auto drawKind = [&](float kind, float entry, float ring, UINT quads) {
        wc.kind[0] = kind;
        wc.kind[1] = entry;
        wc.kind[2] = dripSince;
        wc.kind[3] = ring;
        ctx->UpdateSubresource(m_cb, 0, nullptr, &wc, 0, 0);
        ctx->Draw(quads * 6, 0);
    };
    for (int i = 0; i < 4; i++) {
        if (wc.entries[i * 2][3] < 0.0f) continue;
        drawKind(0.0f, static_cast<float>(i), 0.0f, 420); // the fountain
        drawKind(1.0f, static_cast<float>(i), 0.0f, 90);  // its mist
        for (int ring = 0; ring < 3; ring++) drawKind(2.0f, static_cast<float>(i), static_cast<float>(ring), 1);
        drawKind(3.0f, static_cast<float>(i), 0.0f, 14);  // its foam
    }
    if (withCar && m_wake > 0.01f) {
        drawKind(4.0f, 0.0f, 0.0f, 160); // the wake
        drawKind(5.0f, 0.0f, 0.0f, 120); // the bow wave
    }
    if (withCar && dripSince >= 0.0f) drawKind(6.0f, 0.0f, 0.0f, 120);
    ID3D11Buffer* noCb = nullptr;
    ctx->VSSetConstantBuffers(5, 1, &noCb);
    ctx->PSSetConstantBuffers(5, 1, &noCb);
    fx.profiler().mark(ctx, kWaterSplash);
}

} // namespace tmvs_mp
