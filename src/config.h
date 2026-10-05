#pragma once
#include <string>

namespace tmshaders {

enum class Preset {
    SildursVibrant,
    BSLClean,
    IterationTCinematic,
    Custom
};

struct ShaderSettings {
    bool enabled = true;
    Preset activePreset = Preset::SildursVibrant;

    // TM_VibrantColor
    float exposure = 1.10f;
    float contrast = 1.18f;
    float colorTemp = 0.12f;
    float vibrance = 0.45f;
    float skyVibrance = 0.32f;
    float foliageBoost = 0.35f;
    int tonemapMode = 1; // ACES
    float sunTint[3] = {1.05f, 1.02f, 0.90f};
    float shadowTint[3] = {0.94f, 0.96f, 1.02f};

    // TM_SunRays
    bool enableSunRays = true;
    float sunPos[2] = {0.50f, 0.15f};
    float rayDensity = 1.15f;
    float rayDecay = 0.965f;
    float rayWeight = 0.42f;
    float rayExposure = 1.40f;
    float rayColor[3] = {1.0f, 0.85f, 0.58f};

    // TM_CinematicBloom
    bool enableBloom = true;
    float bloomThreshold = 0.72f;
    float bloomIntensity = 0.80f;
    float bloomRadius = 2.6f;
    float anamorphicIntensity = 0.35f;
    float flareTint[3] = {0.85f, 0.90f, 1.00f};

    // TM_AtmosphericFog
    bool enableFog = true;
    float fogDensity = 0.38f;
    float fogStart = 0.10f;
    float fogCurve = 1.8f;
    float sunScatterPower = 0.50f;
    float horizonColor[3] = {0.85f, 0.90f, 0.98f};
    float sunFogColor[3] = {1.00f, 0.90f, 0.75f};

    // TM_DepthShading
    bool enableDepthShading = true;
    float aoIntensity = 0.60f;
    float aoRadius = 2.5f;

    void applyPreset(Preset preset);
};

class Config {
public:
    static Config& get();

    void load();
    void save();
    const std::wstring& getModulePath() const { return m_modulePath; }
    void setModulePath(const std::wstring& path) { m_modulePath = path; }

    ShaderSettings settings;
    bool showOverlay = false;
    unsigned int toggleKey = 0x77; // VK_F8

private:
    Config() = default;
    std::wstring m_modulePath;
    std::wstring m_iniPath;
};

} // namespace tmshaders
