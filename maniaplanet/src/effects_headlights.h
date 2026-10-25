#pragma once
// The car's headlights (CarHeadlights, the TMNF mod's): two spotlights on the front of the car the
// camera follows, lighting the engine's G-buffer and the haze. The car's pose comes from the
// engine's own constants (hooks.cpp reads its ObjectV at its G-buffer draw, as for the neon trail
// and the spray); nothing in the game changes. Shader: shaders/headlights.hlsl.
#include "engine_frame.h"
#include <d3d11.h>

namespace tmshaders {
struct Settings;
}

namespace tmvs_mp {

class Effects;
struct SceneFrame;

class HeadlightStage {
public:
    bool init(ID3D11Device* device);
    void release();
    static bool supports(const char* key);
    // After the lighting composite: the headlights added to fx.currentRtv().
    void draw(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame, const tmshaders::Settings& s);

    // Which of the car's model axes is ahead (+1 its z, -1 -z): from its motion once the spray's
    // check has decided (carForwardSign), until then from the chase camera, which looks along the
    // car. carForward: the model's z in the world; cameraForward: the camera's (world).
    // `from`: 0 not known yet (+1), 1 the camera, 2 the car's motion.
    static float forwardSign(const float carForward[3], const float carPosition[3], const float cameraEye[3],
                             const float cameraForward[3], double time, int* from = nullptr);

private:
    ID3D11PixelShader* m_ps = nullptr;
    ID3D11Buffer* m_cb = nullptr;
    ID3D11BlendState* m_add = nullptr;
    bool m_ready = false;
    // The car last seen and its velocity (world m/s): a frame without its draw moves it on for 0.15 s.
    EngineCar m_held;
    double m_heldAt = -1.0;
    float m_velocity[3] = {};
    int m_logged = -1; // what the last log line said (once per change)
    int m_logs = 0;
};

} // namespace tmvs_mp
