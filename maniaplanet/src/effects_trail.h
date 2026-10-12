#pragma once
// Neon trail behind the car (the TMNF mod's): the car's position from the engine (hooks.cpp reads
// it from the car's ObjectV at its G-buffer draw), kept 30 times a second (and every 2 m) in a ring
// buffer, drawn as a glowing ribbon into the HDR image. Shaders: shaders/trail.hlsl.
#include <d3d11.h>
#include <vector>

namespace tmshaders {
struct Settings;
}

namespace tmvs_mp {

class Effects;
struct SceneFrame;

class TrailStage {
public:
    bool init(ID3D11Device* device);
    void release();
    void releaseTargets() {}
    static bool supports(const char* key);
    // After the composite (with the weather's particles): records the car, draws the trail into
    // fx.currentRtv().
    void draw(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame, const tmshaders::Settings& s);

private:
    static constexpr UINT kPoints = 8192; // 4.5 min at 30 points a second, less at speed
    ID3D11VertexShader* m_vs = nullptr;
    ID3D11PixelShader* m_ps = nullptr;
    ID3D11Texture2D* m_pointsTex = nullptr;
    ID3D11ShaderResourceView* m_pointsSrv = nullptr;
    ID3D11Buffer* m_cb = nullptr;
    ID3D11BlendState* m_add = nullptr;
    std::vector<float> m_points; // kPoints x (xyz, stamp)
    UINT m_head = 0, m_count = 0;
    float m_clock = 0.0f, m_time = 0.0f;
    float m_kept[3] = {};
    bool m_keptValid = false;
    bool m_break = true;
    double m_lastTime = -1.0;
    double m_carSeenAt = -1.0;
    bool m_ready = false;
};

} // namespace tmvs_mp
