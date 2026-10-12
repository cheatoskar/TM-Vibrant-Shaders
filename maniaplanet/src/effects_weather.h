#pragma once
// Weather stage: the TMNF mod's wet roads, puddles, reflections (screen space), rain, snow, splashes,
// lightning and drops on the lens. Shaders: shaders/weather.hlsl (passes, particles),
// shaders/weather.hlsli (what PS_Lighting calls per surface and per pixel).
#include <d3d11.h>

namespace tmshaders {
struct Settings;
}

namespace tmvs_mp {

class Effects;
struct SceneFrame;

class WeatherStage {
public:
    bool init(ID3D11Device* device);
    void release();
    void releaseTargets();
    static bool supports(const char* key);

    // Before the lighting composite: screen-space reflections (half res: rgb, a = hit confidence)
    // for wet roads, puddles, water and the polished track. Null: none this frame.
    ID3D11ShaderResourceView* reflections(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame,
                                          const tmshaders::Settings& s);
    // After the composite: rain, snow and splashes into fx.currentRtv(); drops on the lens
    // (fx.current() -> fx.next(), then fx.swap()).
    void draw(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame, const tmshaders::Settings& s);
    // Once per frame on the render thread (sound: rain, thunder, wind).
    void tick(const tmshaders::Settings& s, double time);
};

} // namespace tmvs_mp
