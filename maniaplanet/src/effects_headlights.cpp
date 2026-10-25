#include "effects_headlights.h"
#include "effects.h"
#include "log.h"
#include "settings.h"
#include "shaders/PS_Headlights.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace tmvs_mp {
namespace {

template <typename T> void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// headlights.hlsl HeadlightParams (b4)
struct HeadlightConstants {
    float head0[4]; // strength, cos of the cone's edge, cos of its bright core, range (m)
    float head1[4]; // left lamp (world), glare
    float head2[4]; // right lamp (world), beam in the air
    float head3[4]; // the beams' axis (world)
    float spec[4];  // the engine's SpecMaxExp, SpecScale x its light's w (0: no glint)
};

// The lamps on the Stadium car (TMNF's estimate, src/pipeline.cpp): 1.9 m ahead of its origin,
// 0.62 m to each side. Its origin lies on the road here (capture 030: 2 mm under it), TMNF's at
// about the wheel centres: 0.5 m up. With wheels from its bones: 0.2 m ahead of the front axle,
// 0.15 m over the wheel centres, 0.72 of the half track out. Aimed 4 degrees down at the road.
constexpr float kLampAhead = 1.9f;
constexpr float kLampUp = 0.5f;
constexpr float kLampSide = 0.62f;
constexpr float kPitch = 0.07f;

// The chase camera looks along the car: until the car's motion shows which of its model's axes is
// ahead (the spray's check, effects_weather.cpp), the camera's view decides, so the lamps point
// the right way already while it stands at the start. 30 frames where the camera holds the car
// in its view and the two point along each other (within 37 degrees, on the ground plane); 8 in
// 10 of them must agree, else it counts again. Shared by the stages (the compare split's second one).
struct CameraVote {
    int votes[2] = {}; // along +z, along -z
    int total = 0;
    bool decided = false;
    float sign = 1.0f;
    double lastTime = -1.0;
};
CameraVote g_cameraVote;

} // namespace

bool HeadlightStage::init(ID3D11Device* device) {
    bool ok = SUCCEEDED(device->CreatePixelShader(g_PS_Headlights, sizeof(g_PS_Headlights), nullptr, &m_ps));
    D3D11_BUFFER_DESC cb = {};
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.ByteWidth = sizeof(HeadlightConstants);
    ok &= SUCCEEDED(device->CreateBuffer(&cb, nullptr, &m_cb));
    // Added to the HDR image; its alpha stays.
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
    m_ready = ok;
    if (!ok) TMVS_LOG("headlights: creating the shader or buffers failed");
    return ok;
}

void HeadlightStage::release() {
    safeRelease(m_ps);
    safeRelease(m_cb);
    safeRelease(m_add);
    m_ready = false;
}

bool HeadlightStage::supports(const char* key) {
    static const char* const kKeys[] = {"CarHeadlights", "CarHeadlightRange", "CarHeadlightBeam"};
    for (const char* k : kKeys)
        if (!strcmp(k, key)) return true;
    return false;
}

float HeadlightStage::forwardSign(const float carForward[3], const float carPosition[3], const float cameraEye[3],
                                  const float cameraForward[3], double time, int* from) {
    bool motion = false;
    const float sign = carForwardSign(motion);
    if (motion) {
        if (from) *from = 2;
        return sign;
    }
    CameraVote& v = g_cameraVote;
    if (!v.decided && time != v.lastTime) {
        v.lastTime = time;
        // The car where a chase camera holds it: 1.5 - 15 m ahead, within about 27 degrees of the
        // middle of the view (capture 034: 5.5 m ahead, 2.3 m below it), or the camera inside it
        // (the cockpit view).
        float rel[3];
        for (int i = 0; i < 3; i++) rel[i] = carPosition[i] - cameraEye[i];
        const float along = rel[0] * cameraForward[0] + rel[1] * cameraForward[1] + rel[2] * cameraForward[2];
        const float dist2 = rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2];
        const float side2 = dist2 - along * along;
        const bool held = (along > 1.5f && along < 15.0f && side2 < 0.25f * along * along) || dist2 < 4.0f;
        const float ax = carForward[0], az = carForward[2], cx = cameraForward[0], cz = cameraForward[2];
        const float la = sqrtf(ax * ax + az * az), lc = sqrtf(cx * cx + cz * cz);
        if (held && la > 0.3f && lc > 0.3f) {
            const float d = (ax * cx + az * cz) / (la * lc);
            if (std::fabs(d) > 0.8f) {
                v.votes[d < 0.0f ? 1 : 0]++;
                if (++v.total >= 30) {
                    if (v.votes[1] * 10 >= v.total * 8 || v.votes[0] * 10 >= v.total * 8) {
                        v.decided = true;
                        v.sign = v.votes[1] > v.votes[0] ? -1.0f : 1.0f;
                        TMVS_LOG("headlights: the car's front from the camera's view: its model's %sz in %d of %d frames",
                                 v.sign < 0.0f ? "-" : "+", std::max(v.votes[0], v.votes[1]), v.total);
                    } else {
                        v.votes[0] = v.votes[1] = v.total = 0; // no clear answer: count again
                    }
                }
            }
        }
    }
    if (from) *from = v.decided ? 1 : 0;
    return v.sign;
}

void HeadlightStage::draw(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame, const tmshaders::Settings& s) {
    if (!m_ready || s.carHeadlights <= 0.0f || !frame.camera) return;
    const EngineCar* car = frame.car && frame.car->valid ? frame.car : nullptr;
    // The car's velocity from its position frame to frame (more than 150 m/s: a respawn or a cut).
    if (car && frame.time != m_heldAt) {
        const double since = frame.time - m_heldAt;
        bool measured = false;
        if (m_heldAt >= 0.0 && since > 1e-4 && since < 0.25 && car->track == m_held.track) {
            float v[3];
            for (int j = 0; j < 3; j++) v[j] = static_cast<float>((car->position[j] - m_held.position[j]) / since);
            if (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] < 150.0f * 150.0f) {
                for (int j = 0; j < 3; j++) m_velocity[j] += (v[j] - m_velocity[j]) * 0.5f;
                measured = true;
            }
        }
        if (!measured) memset(m_velocity, 0, sizeof(m_velocity));
        m_held = *car;
        m_heldAt = frame.time;
    }
    // A frame without its draw: the car last seen, moved on at its velocity, for 0.15 s (else the
    // light blinks off for that frame).
    EngineCar held;
    if (!car && m_heldAt >= 0.0 && frame.time >= m_heldAt && frame.time - m_heldAt < 0.15) {
        held = m_held;
        for (int j = 0; j < 3; j++) held.position[j] += m_velocity[j] * static_cast<float>(frame.time - m_heldAt);
        car = &held;
    }
    if (!car) {
        if (m_logged != 0 && m_logs < 20) {
            m_logged = 0;
            m_logs++;
            TMVS_LOG("headlights: no car from the engine this frame: no headlights");
        }
        return;
    }

    const float* W = frame.camera->worldToCamera;
    const float cameraForward[3] = {W[8], W[9], W[10]};
    int from = 0;
    const float sign = forwardSign(car->forward, car->position, frame.camera->eye, cameraForward, frame.time, &from);
    float ahead = kLampAhead, up = kLampUp, side = kLampSide;
    const CarWheels& wh = car->wheels;
    if (wh.known) {
        ahead = std::clamp(std::max(wh.zA * sign, wh.zB * sign) + 0.2f, 1.0f, 3.5f);
        up = std::clamp(std::max(wh.heightA, wh.heightB) + 0.15f, 0.2f, 1.2f);
        side = std::clamp(wh.halfTrack * 0.72f, 0.4f, 0.9f);
    }
    float fwd[3], aim[3], left[3], right[3];
    for (int j = 0; j < 3; j++) {
        fwd[j] = car->forward[j] * sign;
        aim[j] = fwd[j] * cosf(kPitch) - car->up[j] * sinf(kPitch);
        const float base = car->position[j] + fwd[j] * ahead + car->up[j] * up;
        left[j] = base - car->right[j] * side;
        right[j] = base + car->right[j] * side;
    }

    HeadlightConstants c = {};
    c.head0[0] = std::min(s.carHeadlights, 1.0f);
    c.head0[1] = 0.89f;  // the cone: 27 degrees to its edge (54 across)
    c.head0[2] = 0.985f; // 10 to its bright core
    c.head0[3] = std::clamp(s.carHeadlightRange, 20.0f, 200.0f);
    memcpy(c.head1, left, sizeof(left));
    c.head1[3] = 1.0f;
    memcpy(c.head2, right, sizeof(right));
    c.head2[3] = 2.0f * std::clamp(s.carHeadlightBeam, 0.0f, 1.0f);
    memcpy(c.head3, aim, sizeof(aim));
    // The engine's specular constants (SceneP), as its lighting pass uses them for its sun.
    if (frame.camera->hasSceneP) {
        float gen[4], light[4];
        memcpy(gen, frame.camera->rawP + kScenePLightGenSpec, sizeof(gen));
        memcpy(light, frame.camera->rawP + kScenePLightDirRgbLinear0, sizeof(light));
        const float specScale = gen[3] * light[3];
        if (std::isfinite(gen[2]) && gen[2] >= 0.0f && gen[2] < 4096.0f && std::isfinite(specScale) && specScale > 0.0f) {
            c.spec[0] = gen[2];
            c.spec[1] = std::min(specScale, 4.0f);
        }
    }

    const int what = 1 + from + (wh.known ? 3 : 0);
    if (what != m_logged && m_logs < 20) {
        m_logged = what;
        m_logs++;
        TMVS_LOG("headlights: on the car's front (its model's %sz, %s), lamps %.2f m ahead, %.2f m up, %.2f m out (%s)",
                 sign < 0.0f ? "-" : "+", from == 2 ? "from its motion" : (from == 1 ? "from the camera's view" : "until the camera or its motion shows"),
                 ahead, up, side, wh.known ? "wheels from its bones" : "the Stadium car's");
    }

    ctx->UpdateSubresource(m_cb, 0, nullptr, &c, 0, 0);
    ctx->PSSetConstantBuffers(4, 1, &m_cb);
    ID3D11ShaderResourceView* none[8] = {};
    ctx->PSSetShaderResources(0, 8, none);
    ID3D11RenderTargetView* rtv = fx.currentRtv();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = {0, 0, static_cast<float>(fx.width()), static_cast<float>(fx.height()), 0, 1};
    ctx->RSSetViewports(1, &vp);
    const float factor[4] = {};
    ctx->OMSetBlendState(m_add, factor, 0xffffffff);
    ctx->PSSetShader(m_ps, nullptr, 0);
    ctx->Draw(3, 0);
    fx.profiler().mark(ctx, kHeadlights);
    ID3D11Buffer* noCb = nullptr;
    ctx->PSSetConstantBuffers(4, 1, &noCb);
}

} // namespace tmvs_mp
