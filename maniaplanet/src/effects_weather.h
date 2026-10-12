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

private:
    ID3D11PixelShader* m_reflectPs = nullptr;
    ID3D11VertexShader* m_dropVs = nullptr;
    ID3D11PixelShader* m_dropPs = nullptr;
    ID3D11VertexShader* m_splashVs = nullptr;
    ID3D11PixelShader* m_splashPs = nullptr;
    ID3D11VertexShader* m_snowVs = nullptr;
    ID3D11PixelShader* m_snowPs = nullptr;
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
};

} // namespace tmvs_mp
