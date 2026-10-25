#pragma once
// Weather stage: the TMNF mod's wet roads, puddles, reflections (screen space), rain, snow, splashes,
// the spray behind the car, lightning and drops on the lens. Shaders: shaders/weather.hlsl (passes, particles),
// shaders/weather.hlsli (what PS_Lighting calls per surface and per pixel).
#include "engine_frame.h"
#include <d3d11.h>

namespace tmshaders {
struct Settings;
}

namespace tmvs_mp {

class Effects;
struct SceneFrame;

// Which of the car's model axes is ahead, as the spray's check from the car's motion found it
// (WeatherStage::spray): +1 its z, -1 -z. `decided` stays false until enough moves showed it
// (+1 until then). The headlights read it too.
float carForwardSign(bool& decided);

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
    // After the composite: rain, snow, splashes and the spray into fx.currentRtv(); drops on the lens
    // (fx.current() -> fx.next(), then fx.swap()).
    void draw(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame, const tmshaders::Settings& s);
    // Once per frame on the render thread (sound: rain, thunder, wind).
    void tick(const tmshaders::Settings& s, double time);

private:
    // The spray's constants (weather.hlsl w_Car .. w_Spray) for this frame; returns its amount
    // (0: none). Keeps the car's velocity and decides which way its model faces.
    float spray(const SceneFrame& frame, const tmshaders::Settings& s, float out[6][4]);

    ID3D11PixelShader* m_reflectPs = nullptr;
    ID3D11VertexShader* m_dropVs = nullptr;
    ID3D11PixelShader* m_dropPs = nullptr;
    ID3D11VertexShader* m_splashVs = nullptr;
    ID3D11PixelShader* m_splashPs = nullptr;
    ID3D11VertexShader* m_snowVs = nullptr;
    ID3D11PixelShader* m_snowPs = nullptr;
    ID3D11VertexShader* m_sprayVs = nullptr;
    ID3D11PixelShader* m_sprayPs = nullptr;
    ID3D11PixelShader* m_lensPs = nullptr;
    ID3D11Buffer* m_cb = nullptr;
    ID3D11BlendState* m_add = nullptr;  // rain: added (light caught by the drops)
    ID3D11BlendState* m_over = nullptr; // snow: premultiplied, over the image
    // The reflections (half res). Not a Target: effects.h includes this header before it
    // defines Target.
    ID3D11Texture2D* m_reflectTex = nullptr;
    ID3D11RenderTargetView* m_reflectRtv = nullptr;
    ID3D11ShaderResourceView* m_reflectSrv = nullptr;
    UINT m_reflectW = 0, m_reflectH = 0;
    bool m_ready = false;

    // The camera's velocity (smoothed, world m/s): drops streak along their motion relative to it.
    float m_cameraVelocity[3] = {};
    double m_lastTime = -1.0;

    // The spray's car: its velocity (smoothed, world m/s) from its position frame to frame, and the
    // car last seen, held for a few frames without it (else the spray jumped to the depth search).
    float m_carVelocity[3] = {};
    float m_carPrev[3] = {};
    double m_carPrevTime = -1.0;
    unsigned m_carTrack = 0;
    EngineCar m_held;
    double m_heldAt = -1.0;
    CarWheels m_wheels; // the last wheels its bones gave (kept for the car while a frame shows none)
    int m_sprayFrom = -1; // what the last log line said (once per change)
    int m_sprayLogged = 0;
};

} // namespace tmvs_mp
