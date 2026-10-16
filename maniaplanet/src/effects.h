#pragma once
// The TMNF mod's scene effects for ManiaPlanet (D3D11): ambient occlusion and contact shadows on
// the engine's real normals, bounce light, neon light, custom skies, haze, weather, depth of field,
// motion blur and light shafts. They run at the engine's tone-map draw on its HDR image; the
// tone map (renderer.cpp, hdr.hlsl) then reads their result.
//
// The constants keep the TMNF shader's layout (shaders/common.hlsli), so its effects carry over.
// Stages (sky, weather, cinema) live in their own files and use the helpers below.
#include "effects_cinema.h"
#include "effects_sky.h"
#include "effects_trail.h"
#include "effects_weather.h"
#include "engine_frame.h"
#include <d3d11.h>
#include <cstdint>

namespace tmshaders {
struct Settings;
}

namespace tmvs_mp {

// A texture the effects draw into and read back.
struct Target {
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    UINT w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    bool create(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT fmt);
    void release();
    bool ready() const { return srv != nullptr; }
};

// Passes, named as in TMNF (the menu's Performance page groups them by these names).
enum Pass : int {
    kDownsampleND,
    kOcclusionShadow,
    kBilateralBlur,
    kGI,
    kGITemporal,
    kSkyClear, // one per sky mode (1..5), in mode order
    kSkyStars,
    kSkyBlackHole,
    kSkyAurora,
    kSkyRing,
    kAuroraHalf,
    kSkyAverage,
    kClouds,
    kReflect,
    kSpillDown,
    kSpillBlur,
    kLighting,
    kRainDrop,
    kRainSplash,
    kSpray,
    kSnowFlake,
    kTrail,
    kFocus,
    kDofBlur,
    kCinematic,
    kRayMask,
    kRayBlur,
    kBloomDown,
    kBloomUp,
    kFinal,
    kTAA,
    kSharpen,
    kVolumetric,
    kPassCount
};
const char* passName(int pass);

// GPU time per pass with timestamp queries (results arrive a few frames late).
class Profiler {
public:
    bool init(ID3D11Device* device);
    void release();
    void begin(ID3D11DeviceContext* ctx); // once per frame, before the first pass
    void mark(ID3D11DeviceContext* ctx, int pass); // the time since the last mark goes to `pass`
    void end(ID3D11DeviceContext* ctx);
    void collect(ID3D11DeviceContext* ctx); // reads finished frames (never waits)
    float time(int pass) const { return m_ms[pass]; } // smoothed ms
    float total() const { return m_total; }

private:
    static constexpr int kFrames = 4, kStamps = 48;
    struct Frame {
        ID3D11Query* disjoint = nullptr;
        ID3D11Query* stamps[kStamps] = {};
        int pass[kStamps] = {};
        int count = 0;
        bool pending = false;
    };
    Frame m_frames[kFrames];
    Frame* m_current = nullptr;
    int m_index = 0;
    float m_ms[kPassCount] = {};
    float m_total = 0.0f;
    bool m_ready = false;
};

// What the engine handed over at its tone-map draw.
struct SceneFrame {
    ID3D11ShaderResourceView* hdr = nullptr;      // the engine's HDR image (scene + its bloom)
    ID3D11ShaderResourceView* exposure = nullptr; // its 2x2 exposure texture
    ID3D11Buffer* engineTone = nullptr;           // its tone-map constants (exposure uv)
    ID3D11ShaderResourceView* const* engine = nullptr; // kTexCount views of its textures (null = missing)
    const Camera* camera = nullptr;
    const EngineShadow* shadow = nullptr;         // the engine's sun shadow pass (volumetric light)
    const EngineCar* car = nullptr;               // the player's car (neon trail)
    double time = 0.0;                            // seconds
    bool photo = false;                           // photo mode: effects reach farther
    bool driving = true;                          // not a replay (cinematic effects only in replays)
    float focus[2] = {-1.0f, -1.0f};              // photo mode: the focus point clicked (uv), -1 = auto focus
};

class Effects {
public:
    bool init(ID3D11Device* device);
    void release();
    // Size-dependent resources: dropped on ResizeBuffers.
    void releaseTargets();

    // The scene effects on the engine's HDR image. True: result() is the image to tone map, with
    // the engine's exposure already applied. False: nothing to do (or no depth / camera).
    bool scene(ID3D11DeviceContext* ctx, const SceneFrame& frame, const tmshaders::Settings& s);
    ID3D11ShaderResourceView* result() const { return m_result; }

    // True when any scene effect is switched on in these settings (else the tone map reads the
    // engine's image as before).
    static bool wanted(const tmshaders::Settings& s);
    // The settings ManiaPlanet can do (the menu shows only these).
    static bool supports(const char* key);

    Profiler& profiler() { return m_profiler; }
    const Profiler& profiler() const { return m_profiler; }

    // ---- for the stages ----
    ID3D11Device* device() const { return m_device; }
    UINT width() const { return m_w; }
    UINT height() const { return m_h; }
    // A full-screen pass into `out` (whole target), inputs at t0..; the frame and engine
    // bindings (b0, b2, t8-t15) stay. pass0/pass1: u_Pass0/u_Pass1 (null = zero).
    void run(ID3D11DeviceContext* ctx, int pass, ID3D11PixelShader* ps, const Target& out, ID3D11ShaderResourceView* const* inputs,
             UINT inputCount, const float* pass0 = nullptr, const float* pass1 = nullptr);
    void run(ID3D11DeviceContext* ctx, int pass, ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv, UINT w, UINT h,
             ID3D11ShaderResourceView* const* inputs, UINT inputCount, const float* pass0 = nullptr, const float* pass1 = nullptr);
    void setPassConstants(ID3D11DeviceContext* ctx, const float* pass0, const float* pass1);
    // Binds the frame constants, samplers and the engine's textures again (after a stage drew
    // with its own state).
    void bindFrame(ID3D11DeviceContext* ctx);
    // Copies of what the stages need from the frame.
    const float* frameConstants() const { return &m_c[0][0]; }
    ID3D11VertexShader* fullScreenVS() const { return m_vs; }
    bool historyValid() const { return m_temporalValid; }
    unsigned frameIndex() const { return m_frame; }
    // The half-res normal/depth (view normal xyz, view z w) and colour (exposed) of this frame.
    ID3D11ShaderResourceView* halfNormalDepth() const { return m_ndHalf_.srv; }
    ID3D11ShaderResourceView* halfColour() const { return m_colourHalf.srv; }
    // The HDR image after the composite, ping-ponged between two full-res targets: a pass reads
    // current(), writes next(), then swap() makes that the current one. Particles draw straight
    // into currentRtv() (additive).
    ID3D11ShaderResourceView* current() const { return m_hdr[m_currentHdr].srv; }
    ID3D11RenderTargetView* currentRtv() const { return m_hdr[m_currentHdr].rtv; }
    const Target& next() const { return m_hdr[1 - m_currentHdr]; }
    void swap() { m_currentHdr = 1 - m_currentHdr; }
    // The engine's depth as the passes read it (t8), for stages that bind their own state.
    ID3D11ShaderResourceView* const* engineViews() const { return m_engineViews; }
    // TMNF's colours (its skies, sun, haze) -> display-linear units here (u_Misc.z), worked out by
    // the sky stage from the engine's tone-map constants: in effect from the next frame's constants.
    void setTmnfScale(float scale) { m_tmnfScale = scale; }
    float tmnfScale() const { return m_tmnfScale; }
    // Off for the compare split's second look: one weather sound, the one of your look.
    void setSound(bool on) { m_sound = on; }

private:
    bool ensureTargets(UINT w, UINT h);
    void buildConstants(const SceneFrame& frame, const tmshaders::Settings& s);
    void setCommonState(ID3D11DeviceContext* ctx);

    ID3D11Device* m_device = nullptr;
    ID3D11VertexShader* m_vs = nullptr;
    ID3D11PixelShader* m_ndHalf = nullptr;
    ID3D11PixelShader* m_occlusion = nullptr;
    ID3D11PixelShader* m_blur = nullptr;
    ID3D11PixelShader* m_gi = nullptr;
    ID3D11PixelShader* m_giTemporal = nullptr;
    ID3D11PixelShader* m_spillDown = nullptr;
    ID3D11PixelShader* m_spillBlur = nullptr;
    ID3D11PixelShader* m_lighting = nullptr;
    ID3D11PixelShader* m_volumetric = nullptr;
    ID3D11Buffer* m_frameCb = nullptr;
    ID3D11Buffer* m_passCb = nullptr;
    ID3D11SamplerState* m_samplers[4] = {};
    ID3D11RasterizerState* m_rs = nullptr;
    ID3D11DepthStencilState* m_dss = nullptr;
    ID3D11BlendState* m_blend = nullptr;
    Profiler m_profiler;

    UINT m_w = 0, m_h = 0;
    Target m_ndHalf_;      // half: view normal (xyz), view z (w)
    Target m_colourHalf;   // half: the engine's image, exposed (GI and neon light read it)
    Target m_occ[2];       // half: AO, contact shadow (ping-pong for the blur)
    Target m_gi_[2];       // quarter: bounce light, ping-pong for the blur
    Target m_giHistory[2]; // quarter: accumulated (rgb, view z)
    int m_giIndex = 0;
    Target m_spill[2];     // quarter: neon light (rgb, view z)
    Target m_volume[2];    // half: volumetric light (lit fraction, lit path / range), ping-pong for the blur
    Target m_hdr[2];       // full: the result (exposed HDR), ping-pong for the passes after the composite
    int m_currentHdr = 0;
    ID3D11ShaderResourceView* m_result = nullptr;
    ID3D11ShaderResourceView* m_engineViews[8] = {}; // t8-t15 of this frame (not owned)

    SkyStage m_sky;
    WeatherStage m_weather;
    CinemaStage m_cinema;
    TrailStage m_trail;

    float m_c[48][4] = {};
    float m_prevW2C[12] = {};
    float m_prevProj[4] = {};
    bool m_havePrevious = false;
    bool m_temporalValid = false;
    unsigned m_frame = 0;
    double m_lastTime = 0.0;
    // About what the engine's usual tone map gives (a sunset capture: 1.82) until it is read back.
    float m_tmnfScale = 1.8f;
    bool m_sound = true;
    int m_boltSlot = -1000000; // lightning strike the bolt direction belongs to
    float m_boltAzimuth = 0.0f;
};

} // namespace tmvs_mp
