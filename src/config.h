#pragma once
#include <string>

namespace tmshaders {

enum class Preset {
    Stadium2020 = 0,
    GoldenHour,
    ClearDaylight,
    GrandPrixCinematic,
    Custom
};

struct ShaderSettings {
    bool enabled = true;
    Preset activePreset = Preset::Stadium2020;

    // Color & Tonemap
    float exposure = 1.00f;
    float contrast = 1.06f;
    float saturation = 1.05f;
    float warmth = 0.00f;       // Subtle Kelvin shift (-0.5 to 0.5)
    float skyBoost = 0.08f;
    float foliageBoost = 0.10f;

    // Next-Gen Texture & Geometry
    float sharpness = 0.70f;    // FidelityFX Contrast-Adaptive Sharpening
    float clarity = 0.50f;      // Contact Shading & Micro-AO
    float roadSheen = 0.35f;    // Tarmac Specular Sheen
    float vignette = 0.15f;     // Lens Vignette

    // Emissive Bloom
    bool enableBloom = true;
    float bloomIntensity = 0.25f;
    float bloomThreshold = 0.88f;

    // Anamorphic Flares
    bool enableFlares = false;
    float flareIntensity = 0.0f;

    // Volumetric Rays (bounded to sky)
    bool enableSunRays = false;
    float sunRayIntensity = 0.0f;
    float sunRayDecay = 0.94f;
    float sunPos[2] = {0.50f, 0.10f};

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
