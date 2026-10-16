#pragma once
// Neon trail behind the car (the TMNF mod's): the car's position from the engine (hooks.cpp reads
// it from the car's ObjectV at its G-buffer draw), kept 60 times a second (and every metre) while
// it moves, in a ring buffer, drawn as a glowing ribbon into the HDR image. It stays until the run
// starts again (the car back at its start) or the map changes. Shaders: shaders/trail.hlsl.
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

    // The recording alone (also for the offline test): dt (s), the car's position (null: not seen
    // this frame). True: the live end (slot head()) is drawn after the kept points.
    bool record(float dt, const float* car);
    void startRecording() {
        if (m_points.empty()) m_points.assign(kPoints * 4, 0.0f);
        clear();
    }
    void tick(float dt) {
        m_time += dt;
        m_clock += dt;
    }
    UINT head() const { return m_head; }
    UINT count() const { return m_count; }
    const float* point(UINT i) const { return &m_points[(i % kPoints) * 4]; }

private:
    static constexpr UINT kPoints = 16384; // 4.5 min at 60 points a second, ~3 min at full speed
    void clear();
    static constexpr float kMinStep = 0.3f; // m: closer points are no line (the car shaking in place)
    static constexpr float kLift = 0.1f;    // m over the car's origin (on the road), see draw()
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
    int m_liveFrames = 0; // frames the live end may stay without the car
    double m_lastTime = -1.0;
    float m_start[3] = {};   // where the run began (the first point after a clear)
    bool m_startValid = false;
    int m_mapSerial = -1;
    bool m_ready = false;
};

} // namespace tmvs_mp
