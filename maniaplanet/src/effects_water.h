#pragma once
// Water stage: what the car throws up where it meets the game's water (WaterSplash, the TMNF mod's:
// a fountain of droplets and mist with rings and foam where it goes in, a wake and a bow wave while
// it drives through, drips when it comes out). Clear water itself (WaterClarity) is drawn inside the
// engine's frame (water11.cpp); this stage finds the water's surface near the car with what that
// left (its fog buffer and the surfaces' planes) and draws the splash into the HDR image.
// Shaders: shaders/weather.hlsl (PS_WaterLevel, VS/PS_WaterSplash).
#include "engine_frame.h"
#include <d3d11.h>

namespace tmshaders {
struct Settings;
}

namespace tmvs_mp {

class Effects;
struct SceneFrame;

class WaterStage {
public:
    bool init(ID3D11Device* device);
    void release();
    static bool supports(const char* key);
    // True when the effects should run for the water this frame (water seen, WaterSplash on).
    static bool wanted(const tmshaders::Settings& s);

    // After the composite: finds the surface near the car (read back a few frames later), follows
    // the car in and out of it, draws the splash into fx.currentRtv().
    void draw(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame, const tmshaders::Settings& s);

    // The previewer (mp_preview --water y): a surface at this height near any car, without the engine's
    // water.
    void setTestLevel(float y) {
        m_test = true;
        m_testLevel = y;
    }

private:
    void measure(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame);
    void collect(ID3D11DeviceContext* ctx, double time, float bottom);
    void follow(const SceneFrame& frame, const tmshaders::Settings& s, float dt);

    ID3D11PixelShader* m_levelPs = nullptr;
    ID3D11VertexShader* m_splashVs = nullptr;
    ID3D11PixelShader* m_splashPs = nullptr;
    ID3D11Buffer* m_cb = nullptr;
    ID3D11BlendState* m_over = nullptr;
    ID3D11Texture2D* m_pictures = nullptr;
    ID3D11ShaderResourceView* m_picturesSrv = nullptr;
    // The surface near the car: one texel per plane, read back without waiting (a few frames late).
    ID3D11Texture2D* m_levelTex = nullptr;
    ID3D11RenderTargetView* m_levelRtv = nullptr;
    static constexpr int kReadbacks = 3;
    ID3D11Texture2D* m_staging[kReadbacks] = {};
    bool m_stagingPending[kReadbacks] = {};
    int m_stagingNext = 0;
    bool m_ready = false;

    // The water's height near the car (from the read-backs) and how long ago it was last seen.
    float m_level = 0.0f;
    bool m_levelKnown = false;
    double m_levelSeen = -1e9;
    bool m_test = false;
    float m_testLevel = 0.0f;

    // The car against the surface (TMNF Pipeline::updateWater). An entry: where the tyres went
    // under, its velocity, how hard, when.
    struct Entry {
        float position[3] = {};
        float velocity[3] = {};
        float strength = 0.0f;
        double time = -1e9;
    };
    Entry m_entries[4];
    int m_entryNext = 0;
    bool m_in = false;          // the tyres are under the surface
    bool m_valid = false;       // the last frame's car and level are known (an entry needs both)
    float m_prev[3] = {};       // the car's place then, and the height of its tyres' bottom
    float m_prevBottom = 0.0f;
    float m_wake = 0.0f;        // 0..1: how far in the water the car is (smoothed)
    float m_inFor = 0.0f;       // seconds in the water this time
    double m_exitTime = -1e9;   // when the car came out, for the drips, and how much it carried
    float m_drip = 0.0f;
    double m_logTime = -1e9;    // the log says one splash a second
    double m_lastTime = -1.0;
    float m_velocity[3] = {};   // the car's velocity (smoothed, world m/s), from its last place
    float m_carPrev[3] = {};
    double m_carPrevTime = -1.0;
    unsigned m_track = 0;
    int m_levelLogs = 0;
    float m_loggedLevel = -1e9f;
};

} // namespace tmvs_mp
