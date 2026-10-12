#pragma once
// Sky stage: the TMNF mod's custom skies (clear sky, starry night, black hole, aurora, ring world),
// the game's sky enhanced (gradient, sun glow), volumetric clouds, lightning in the sky, the sky
// average (the haze's colour), and after the composite the light shafts and the lens flare.
// Shaders: shaders/sky.hlsl (passes), shaders/sky.hlsli (what PS_Lighting calls for sky pixels and
// the sky's light on the scene).
#include <d3d11.h>

namespace tmshaders {
struct Settings;
}

namespace tmvs_mp {

class Effects;
struct SceneFrame;

class SkyStage {
public:
    bool init(ID3D11Device* device);
    void release();
    void releaseTargets();
    // Settings this stage implements (the menu shows only supported ones).
    static bool supports(const char* key);

    // What the composite reads (null: not drawn this frame).
    struct Output {
        ID3D11ShaderResourceView* sky = nullptr;     // full res: the finished sky (rgb display-linear HDR)
        ID3D11ShaderResourceView* average = nullptr; // 1x1: sky colour near the horizon (rgb), confidence (a)
        ID3D11ShaderResourceView* clouds = nullptr;  // half res: in-scattered light (rgb), transmittance (a)
    };
    // Before the lighting composite (frame constants, samplers and engine textures are bound).
    Output run(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame, const tmshaders::Settings& s);
    // After the composite, weather and cinema: light shafts and lens flare (fx.current() -> fx.next(),
    // then fx.swap()).
    void post(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame, const tmshaders::Settings& s);

private:
    // Shaders, targets and state live in effects_sky.cpp (it needs effects.h, which includes this).
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace tmvs_mp
