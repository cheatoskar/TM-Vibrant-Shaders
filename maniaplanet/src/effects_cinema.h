#pragma once
// Cinema stage: the TMNF mod's depth of field (auto focus, photo-mode focus), motion blur (on the
// engine's motion vectors) and temporal anti-aliasing, all on the HDR image before the tone map.
// Shaders: shaders/cinema.hlsl.
#include <d3d11.h>

namespace tmshaders {
struct Settings;
}

namespace tmvs_mp {

class Effects;
struct SceneFrame;

class CinemaStage {
public:
    bool init(ID3D11Device* device);
    void release();
    void releaseTargets();
    static bool supports(const char* key);

    // After the composite and the weather: depth of field, motion blur, TAA
    // (each fx.current() -> fx.next(), then fx.swap()).
    void run(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame, const tmshaders::Settings& s);
};

} // namespace tmvs_mp
