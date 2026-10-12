#include "effects_weather.h"
#include "audio.h"
#include "effects.h"
#include "log.h"
#include "settings.h"
#include "shaders/PS_LensDrops.h"
#include "shaders/PS_RainDrop.h"
#include "shaders/PS_RainSplash.h"
#include "shaders/PS_Reflect.h"
#include "shaders/PS_SnowFlake.h"
#include "shaders/VS_RainDrop.h"
#include "shaders/VS_RainSplash.h"
#include "shaders/VS_SnowFlake.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

namespace tmvs_mp {
namespace {

template <typename T> void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// Particles at the strongest setting (2 = downpour, blizzard), as in TMNF.
constexpr UINT kRainDrops = 24000;
constexpr UINT kRainSplashes = 5200;
constexpr UINT kSnowFlakes = 64000;

// weather.hlsl WeatherParams (b4)
struct WeatherConstants {
    float streak[4]; // streak (world m), time (s)
    float box[4];    // box size, box height, fall speed, min width (px)
    float wind[4];   // wind x, z, splashes per second per particle, splash box size
    float motion[4]; // shutter (frames), previous frame valid, frame time (s), longest streak (ndc)
    float snow[4];   // snow wind x, z, fall speed, 0
    float light[4];  // 0, 0, 0, intensity
};

// ---- Sound ----
// The weather sound (src/audio.cpp, shared with TMNF) wants an update every frame, also while
// no scene is drawn (menus, loading), so it can fade out. The stage only runs with a scene: it
// hands the settings to a thread of its own, which updates the sound at 60 Hz and counts the
// scene as gone when no frame came for a quarter of a second.
struct SoundFeed {
    std::mutex lock;
    tmshaders::Settings settings;
    double time = 0.0;          // the effects' time of the last frame (s)
    LARGE_INTEGER at = {};      // when it came
    std::atomic<bool> running{false};
};

SoundFeed& soundFeed() {
    static SoundFeed* feed = new SoundFeed; // never destroyed: the thread may outlive the statics at exit
    return *feed;
}

void soundLoop() {
    SoundFeed& feed = soundFeed();
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    for (;;) {
        Sleep(16);
        tmshaders::Settings s;
        double time;
        LARGE_INTEGER at;
        {
            std::lock_guard<std::mutex> guard(feed.lock);
            s = feed.settings;
            time = feed.time;
            at = feed.at;
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double since = static_cast<double>(now.QuadPart - at.QuadPart) / static_cast<double>(frequency.QuadPart);
        // The effects' clock runs on between frames (thunder follows its flash on time).
        tmshaders::audio::update(s, static_cast<float>(time + since), since < 0.25, nullptr);
    }
}

// Only the game plays sound (not the previewer or the test harness, which link the same code).
bool inTheGame() {
    static const bool game = [] {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        return _wcsnicmp(name, L"ManiaPlanet", 11) == 0;
    }();
    return game;
}

} // namespace

bool WeatherStage::init(ID3D11Device* device) {
    bool ok = SUCCEEDED(device->CreatePixelShader(g_PS_Reflect, sizeof(g_PS_Reflect), nullptr, &m_reflectPs));
    ok &= SUCCEEDED(device->CreateVertexShader(g_VS_RainDrop, sizeof(g_VS_RainDrop), nullptr, &m_dropVs));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_RainDrop, sizeof(g_PS_RainDrop), nullptr, &m_dropPs));
    ok &= SUCCEEDED(device->CreateVertexShader(g_VS_RainSplash, sizeof(g_VS_RainSplash), nullptr, &m_splashVs));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_RainSplash, sizeof(g_PS_RainSplash), nullptr, &m_splashPs));
    ok &= SUCCEEDED(device->CreateVertexShader(g_VS_SnowFlake, sizeof(g_VS_SnowFlake), nullptr, &m_snowVs));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_SnowFlake, sizeof(g_PS_SnowFlake), nullptr, &m_snowPs));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_LensDrops, sizeof(g_PS_LensDrops), nullptr, &m_lensPs));
    D3D11_BUFFER_DESC cb = {};
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.ByteWidth = sizeof(WeatherConstants);
    ok &= SUCCEEDED(device->CreateBuffer(&cb, nullptr, &m_cb));
    // The HDR image's alpha stays as it is (the passes after read only its colour).
    D3D11_BLEND_DESC bd = {};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE;
    rt.DestBlend = D3D11_BLEND_ONE;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
    rt.DestBlendAlpha = D3D11_BLEND_ONE;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    ok &= SUCCEEDED(device->CreateBlendState(&bd, &m_add));
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    ok &= SUCCEEDED(device->CreateBlendState(&bd, &m_over));
    m_ready = ok;
    if (!ok) TMVS_LOG("weather: creating shaders or states failed");
    return ok;
}

void WeatherStage::releaseTargets() {
    safeRelease(m_reflectSrv);
    safeRelease(m_reflectRtv);
    safeRelease(m_reflectTex);
    m_reflectW = m_reflectH = 0;
    m_lastTime = -1.0;
}

void WeatherStage::release() {
    releaseTargets();
    safeRelease(m_reflectPs);
    safeRelease(m_dropVs);
    safeRelease(m_dropPs);
    safeRelease(m_splashVs);
    safeRelease(m_splashPs);
    safeRelease(m_snowVs);
    safeRelease(m_snowPs);
    safeRelease(m_lensPs);
    safeRelease(m_cb);
    safeRelease(m_add);
    safeRelease(m_over);
    m_ready = false;
}

bool WeatherStage::supports(const char* key) {
    static const char* const kKeys[] = {"Wetness", "Rain",      "Snow",     "SnowCover",    "Puddles", "Reflections",
                                        "ReflectionBlur", "Lightning", "LensDrops", "WeatherSound", "Wind"};
    for (const char* k : kKeys)
        if (!strcmp(k, key)) return true;
    return false;
}

ID3D11ShaderResourceView* WeatherStage::reflections(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame,
                                                    const tmshaders::Settings& s) {
    (void)frame;
    // Puddles need wet roads (as in TMNF), the dry track its Reflections.
    if (!m_ready || (s.wetness <= 0.0f && s.reflections <= 0.0f)) return nullptr;
    const UINT w = std::max(1u, fx.width() / 2), h = std::max(1u, fx.height() / 2);
    if (!m_reflectTex || m_reflectW != w || m_reflectH != h) {
        releaseTargets();
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Device* device = fx.device();
        if (FAILED(device->CreateTexture2D(&d, nullptr, &m_reflectTex)) ||
            FAILED(device->CreateRenderTargetView(m_reflectTex, nullptr, &m_reflectRtv)) ||
            FAILED(device->CreateShaderResourceView(m_reflectTex, nullptr, &m_reflectSrv))) {
            TMVS_LOG("weather: the reflection target %ux%u failed", w, h);
            releaseTargets();
            return nullptr;
        }
        m_reflectW = w;
        m_reflectH = h;
    }
    ID3D11ShaderResourceView* nd = fx.halfNormalDepth();
    fx.run(ctx, kReflect, m_reflectPs, m_reflectRtv, w, h, &nd, 1);
    return m_reflectSrv;
}

void WeatherStage::draw(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame, const tmshaders::Settings& s) {
    if (!m_ready) return;
    const float(*c)[4] = reinterpret_cast<const float(*)[4]>(fx.frameConstants());
    const float dt = m_lastTime >= 0.0 ? static_cast<float>(std::clamp(frame.time - m_lastTime, 0.0, 0.25)) : 0.0f;
    m_lastTime = frame.time;

    // The camera's velocity: from this frame's eye and the previous frame's world -> view rows
    // (eye = -R^T t). The frame constants hold the current camera when there is no previous one.
    const float eye[3] = {c[3][3], c[4][3], c[5][3]};
    float before[3];
    for (int j = 0; j < 3; j++) before[j] = -(c[28][j] * c[28][3] + c[29][j] * c[29][3] + c[30][j] * c[30][3]);
    const bool previous = fx.historyValid() && dt > 1e-4f;
    for (int j = 0; j < 3; j++) {
        const float v = previous ? (eye[j] - before[j]) / dt : 0.0f;
        m_cameraVelocity[j] += (v - m_cameraVelocity[j]) * 0.3f;
    }

    const UINT drops = static_cast<UINT>(kRainDrops * std::min(s.rain, 2.0f) * 0.5f);
    const UINT splashes = static_cast<UINT>(kRainSplashes * std::min(s.rain, 2.0f) * 0.5f);
    const UINT flakes = static_cast<UINT>(kSnowFlakes * std::min(s.snow, 2.0f) * 0.5f);
    if (drops || flakes) {
        const float wind[2] = {2.5f + s.wind * 5.0f, 1.0f + s.wind * 2.0f};
        const float fall = 9.0f;
        const float exposure = 0.03f; // the shutter a streak stands for without a previous frame (s)
        float streak[3] = {-(wind[0] - m_cameraVelocity[0]) * exposure, fall * exposure + m_cameraVelocity[1] * exposure,
                           -(wind[1] - m_cameraVelocity[2]) * exposure};
        const float length = sqrtf(streak[0] * streak[0] + streak[1] * streak[1] + streak[2] * streak[2]);
        if (length > 1.2f)
            for (float& x : streak) x *= 1.2f / length;
        WeatherConstants wc = {};
        memcpy(wc.streak, streak, sizeof(streak));
        wc.streak[3] = static_cast<float>(std::fmod(frame.time, 1000.0));
        wc.box[0] = 30.0f; // 15 m around the camera: farther rain is the streaks in the air
        wc.box[1] = 20.0f;
        wc.box[2] = fall;
        wc.box[3] = 1.0f;
        wc.wind[0] = wind[0];
        wc.wind[1] = wind[1];
        wc.wind[2] = 2.5f;
        wc.wind[3] = 30.0f;
        wc.motion[0] = previous ? std::clamp(exposure / dt, 0.6f, 4.0f) : 0.0f;
        wc.motion[1] = previous ? 1.0f : 0.0f;
        wc.motion[2] = dt;
        wc.motion[3] = 0.25f; // longest streak: 1/8 of the screen
        const float snowAmount = std::min(s.snow, 1.5f);
        wc.snow[0] = (1.0f + s.wind * 7.0f) * snowAmount;
        wc.snow[1] = (0.5f + s.wind * 3.0f) * snowAmount;
        wc.snow[2] = 1.1f;

        ctx->VSSetConstantBuffers(4, 1, &m_cb);
        ctx->PSSetConstantBuffers(4, 1, &m_cb);
        ID3D11ShaderResourceView* none[8] = {};
        ctx->PSSetShaderResources(0, 8, none);
        ID3D11RenderTargetView* rtv = fx.currentRtv();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp = {0, 0, static_cast<float>(fx.width()), static_cast<float>(fx.height()), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        // What is behind a drop: the engine's image (half res, exposed).
        ID3D11ShaderResourceView* behind = fx.halfColour();
        ctx->PSSetShaderResources(0, 1, &behind);
        const float factor[4] = {};

        // Rain 1 = steady rain, 2 = downpour.
        if (drops) {
            wc.light[3] = 1.3f;
            ctx->UpdateSubresource(m_cb, 0, nullptr, &wc, 0, 0);
            ctx->OMSetBlendState(m_add, factor, 0xffffffff);
            ctx->VSSetShader(m_dropVs, nullptr, 0);
            ctx->PSSetShader(m_dropPs, nullptr, 0);
            ctx->Draw(drops * 6, 0);
            fx.profiler().mark(ctx, kRainDrop);
            if (splashes) {
                wc.light[3] = 0.8f;
                ctx->UpdateSubresource(m_cb, 0, nullptr, &wc, 0, 0);
                ctx->VSSetShader(m_splashVs, nullptr, 0);
                ctx->PSSetShader(m_splashPs, nullptr, 0);
                ctx->Draw(splashes * 6, 0);
                fx.profiler().mark(ctx, kRainSplash);
            }
        }
        // Snow 1 = steady snowfall, 2 = blizzard. Blended over the image, not added: white on a
        // white field must not glow.
        if (flakes) {
            wc.light[3] = 1.0f;
            ctx->UpdateSubresource(m_cb, 0, nullptr, &wc, 0, 0);
            ctx->OMSetBlendState(m_over, factor, 0xffffffff);
            ctx->VSSetShader(m_snowVs, nullptr, 0);
            ctx->PSSetShader(m_snowPs, nullptr, 0);
            ctx->Draw(flakes * 6, 0);
            fx.profiler().mark(ctx, kSnowFlake);
        }
        fx.bindFrame(ctx); // the full-screen vertex shader and opaque blending again
    }

    // Drops and flakes on the lens: only while it rains or snows.
    const float lensRain = s.lensDrops ? std::min(s.rain, 1.0f) : 0.0f;
    const float lensSnow = s.lensDrops ? std::min(s.snow, 1.5f) : 0.0f;
    if (lensRain > 0.0f || lensSnow > 0.0f) {
        ctx->PSSetConstantBuffers(4, 1, &m_cb);
        ID3D11ShaderResourceView* in = fx.current();
        fx.run(ctx, kRainDrop, m_lensPs, fx.next(), &in, 1);
        fx.swap();
    }
}

void WeatherStage::tick(const tmshaders::Settings& s, double time) {
    if (!inTheGame()) return;
    SoundFeed& feed = soundFeed();
    {
        std::lock_guard<std::mutex> guard(feed.lock);
        feed.settings = s;
        feed.time = time;
        QueryPerformanceCounter(&feed.at);
    }
    const bool wanted = s.enabled && s.weatherSound > 0.0f && (s.rain > 0.0f || s.lightning > 0.0f || s.snow > 0.0f);
    if (wanted && !feed.running.exchange(true)) {
        // Detached, like the sound's own mixer thread: at exit Windows ends both.
        std::thread(soundLoop).detach();
        TMVS_LOG("weather: sound started");
    }
}

} // namespace tmvs_mp
