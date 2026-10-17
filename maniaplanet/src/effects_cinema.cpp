#include "effects_cinema.h"
#include "effects.h"
#include "log.h"
#include "settings.h"
#include "shaders/PS_Cinematic.h"
#include "shaders/PS_DofBlur.h"
#include "shaders/PS_Focus.h"
#include "shaders/PS_TAA.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace tmvs_mp {
namespace {

template <typename T> void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

const char* const kSupported[] = {"MotionBlur", "DepthOfField", "FocusDistance", "BokehSize", "CinematicOnlyInReplays", "TAA", "DofAutofocus"};

} // namespace

struct CinemaStage::State {
    ID3D11Device* device = nullptr;
    ID3D11PixelShader* focus = nullptr;
    ID3D11PixelShader* dof = nullptr;
    ID3D11PixelShader* cinematic = nullptr;
    ID3D11PixelShader* taa = nullptr;

    Target focusTarget[2]; // 1x1: the focus distance (m), eased over time
    int focusIndex = 0;
    bool focusValid = false;
    Target dofHalf[2];     // half res: the blur and its smoothing pass
    Target dofFull[2];     // full res (photo mode)
    Target history[2];     // full res: TAA history (rgb, view z)
    int historyIndex = 0;
    bool historyValid = false;
    double lastTime = -1.0;
    float lastCar[3] = {}; // DofAutofocus: where the car was last seen (photo mode keeps it)
    bool lastCarKnown = false;
    int focusOnCar = -2;   // what the log said last (-1 not wanted, 0 the middle, 1 the car)

    void releaseTargets() {
        for (auto& t : focusTarget) t.release();
        for (auto& t : dofHalf) t.release();
        for (auto& t : dofFull) t.release();
        for (auto& t : history) t.release();
        focusValid = false;
        historyValid = false;
    }
};

bool CinemaStage::init(ID3D11Device* device) {
    release();
    m_state = new State;
    State& st = *m_state;
    st.device = device;
    bool ok = SUCCEEDED(device->CreatePixelShader(g_PS_Focus, sizeof(g_PS_Focus), nullptr, &st.focus));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_DofBlur, sizeof(g_PS_DofBlur), nullptr, &st.dof));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_Cinematic, sizeof(g_PS_Cinematic), nullptr, &st.cinematic));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_TAA, sizeof(g_PS_TAA), nullptr, &st.taa));
    if (!ok) {
        TMVS_LOG("cinema: creating the shaders failed");
        release();
    }
    return ok;
}

void CinemaStage::release() {
    if (!m_state) return;
    m_state->releaseTargets();
    safeRelease(m_state->focus);
    safeRelease(m_state->dof);
    safeRelease(m_state->cinematic);
    safeRelease(m_state->taa);
    delete m_state;
    m_state = nullptr;
}

void CinemaStage::releaseTargets() {
    if (m_state) m_state->releaseTargets();
}

bool CinemaStage::supports(const char* key) {
    for (const char* k : kSupported)
        if (!strcmp(k, key)) return true;
    return false;
}

void CinemaStage::run(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame, const tmshaders::Settings& s) {
    if (!m_state) return;
    State& st = *m_state;
    const UINT w = fx.width(), h = fx.height();
    const double dtRaw = st.lastTime >= 0.0 ? frame.time - st.lastTime : 0.0;
    const float dt = static_cast<float>(std::clamp(dtRaw, 0.0, 0.1));
    st.lastTime = frame.time;
    const bool history = fx.historyValid();

    // ManiaPlanet can't tell replays from driving yet: with "only in replays and video export"
    // on, depth of field and motion blur run in photo mode and in the video export only.
    const bool cinematic = !s.cinematicOnlyInReplays || frame.photo || !frame.driving;
    const bool dof = cinematic && s.depthOfField > 0.0f;
    // Motion blur needs this frame's motion (the engine's vectors, or the previous camera); after
    // a camera cut both would smear the whole picture for a frame.
    const bool motionBlur = cinematic && s.motionBlur > 0.0f && history;
    const int debug = s.debugView;
    const bool debugView = debug == 40 || debug == 41;

    if (dof || motionBlur || debugView) {
        ID3D11ShaderResourceView* dofResult = nullptr;
        ID3D11ShaderResourceView* focus = nullptr;
        if (dof || debug == 40) {
            const UINT hw = std::max(1u, w / 2), hh = std::max(1u, h / 2);
            bool ok = st.focusTarget[0].create(st.device, 1, 1, DXGI_FORMAT_R32_FLOAT) &&
                      st.focusTarget[1].create(st.device, 1, 1, DXGI_FORMAT_R32_FLOAT);
            // Photo mode: the blur at full resolution (half resolution shows as coarse, blocky edges
            // once the picture stands still and gets looked at).
            Target* blur = frame.photo ? st.dofFull : st.dofHalf;
            const UINT bw = frame.photo ? w : hw, bh = frame.photo ? h : hh;
            ok = ok && blur[0].create(st.device, bw, bh, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
                 blur[1].create(st.device, bw, bh, DXGI_FORMAT_R16G16B16A16_FLOAT);
            if (ok) {
                const int next = 1 - st.focusIndex;
                const float f0[4] = {1.0f - expf(-dt * 4.0f), (st.focusValid && history) ? 0.0f : 1.0f, frame.focus[0], frame.focus[1]};
                // DofAutofocus Car (TMNF's): the player's car from the engine (the neon trail's), about
                // its middle; PS_Focus projects it and averages its own pixels' depths. A click in
                // the photo mode and a set focus distance win. Photo mode keeps the last position
                // (a paused game may draw no car). Off screen or unknown: the middle, as before.
                if (frame.car && frame.car->valid) {
                    for (int i = 0; i < 3; i++) st.lastCar[i] = frame.car->position[i] + frame.car->up[i] * 0.4f;
                    st.lastCarKnown = true;
                }
                const bool wanted = s.dofAutofocus == 1 && s.focusDistance <= 0.0f && frame.focus[0] < 0.0f;
                const bool onCar = wanted && st.lastCarKnown && ((frame.car && frame.car->valid) || frame.photo);
                const float f1[4] = {st.lastCar[0], st.lastCar[1], st.lastCar[2], onCar ? 1.0f : 0.0f};
                const int state = !wanted ? -1 : (onCar ? 1 : 0);
                if (wanted && state != st.focusOnCar)
                    TMVS_LOG("cinema: depth of field focuses %s", onCar ? "on the car (engine position)" : "on the middle (no car from the engine)");
                st.focusOnCar = state;
                fx.run(ctx, kFocus, st.focus, st.focusTarget[next], &st.focusTarget[st.focusIndex].srv, 1, f0, f1);
                st.focusIndex = next;
                st.focusValid = true;
                focus = st.focusTarget[next].srv;

                // Photo mode: four times the samples and no per-pixel noise (nothing averages it out).
                const float full = frame.photo ? 1.0f : 0.0f;
                ID3D11ShaderResourceView* in[3] = {fx.current(), fx.halfNormalDepth(), focus};
                const float p0[4] = {frame.photo ? 64.0f : 16.0f, frame.photo ? 0.0f : 1.0f, 1.0f, full};
                fx.run(ctx, kDofBlur, st.dof, blur[0], in, 3, p0);
                // A second, smaller pass smooths the grain of the samples and the rings the spiral
                // leaves on bright lights.
                ID3D11ShaderResourceView* in2[3] = {blur[0].srv, fx.halfNormalDepth(), focus};
                const float p1[4] = {frame.photo ? 32.0f : 12.0f, frame.photo ? 0.0f : 1.0f, 0.3f, full};
                fx.run(ctx, kDofBlur, st.dof, blur[1], in2, 3, p1);
                dofResult = blur[1].srv;
            }
        }
        ID3D11ShaderResourceView* in[3] = {fx.current(), dofResult, focus};
        const float p0[4] = {dofResult ? 1.0f : 0.0f, motionBlur ? s.motionBlur : 0.0f, 0.0f, 0.0f};
        fx.run(ctx, kCinematic, st.cinematic, fx.next(), in, 3, p0);
        fx.swap();
    }

    // TAA: the image and the next history in one pass (two targets).
    if (s.taa) {
        if (!st.history[0].create(st.device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
            !st.history[1].create(st.device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
            st.historyValid = false;
            return;
        }
        const int previous = st.historyIndex, next = 1 - st.historyIndex;
        ID3D11ShaderResourceView* none[8] = {};
        ctx->PSSetShaderResources(0, 8, none);
        ID3D11RenderTargetView* rtvs[2] = {fx.next().rtv, st.history[next].rtv};
        ctx->OMSetRenderTargets(2, rtvs, nullptr);
        D3D11_VIEWPORT vp = {0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
        ctx->RSSetViewports(1, &vp);
        const float p0[4] = {st.historyValid && history ? 1.0f : 0.0f, 0, 0, 0};
        fx.setPassConstants(ctx, p0, nullptr);
        ID3D11ShaderResourceView* in[2] = {fx.current(), st.history[previous].srv};
        ctx->PSSetShaderResources(0, 2, in);
        ctx->PSSetShader(st.taa, nullptr, 0);
        ctx->Draw(3, 0);
        fx.profiler().mark(ctx, kTAA);
        ID3D11RenderTargetView* noRtv[2] = {};
        ctx->OMSetRenderTargets(2, noRtv, nullptr);
        fx.swap();
        st.historyIndex = next;
        st.historyValid = true;
    } else {
        st.historyValid = false;
    }
}

} // namespace tmvs_mp
