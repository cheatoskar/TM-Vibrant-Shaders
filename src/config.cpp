#include "config.h"
#include <windows.h>
#include <shlobj.h>

namespace tmshaders {

void ShaderSettings::applyPreset(Preset preset) {
    activePreset = preset;
    switch (preset) {
        case Preset::Stadium2020:
            // Modern, razor-sharp, natural daylight, deep contact shadows, clean road
            exposure = 1.00f;
            contrast = 1.06f;
            saturation = 1.05f;
            warmth = 0.00f; // Neutral white balance
            skyBoost = 0.08f;
            foliageBoost = 0.12f;

            sharpness = 0.70f;  // High FidelityFX CAS sharpness
            clarity = 0.55f;    // Deep contact shadows on blocks & car
            roadSheen = 0.35f;  // Tarmac specular
            vignette = 0.15f;

            enableBloom = true;
            bloomIntensity = 0.25f;
            bloomThreshold = 0.88f; // Only genuine lights bloom

            enableFlares = false;
            flareIntensity = 0.0f;

            enableSunRays = false;
            sunRayIntensity = 0.0f;
            sunRayDecay = 0.94f;
            break;

        case Preset::GoldenHour:
            // Warm afternoon sun, saturated grass and sky, controlled god rays
            exposure = 1.02f;
            contrast = 1.10f;
            saturation = 1.20f;
            warmth = 0.06f; // Gentle golden warmth, not blinding yellow
            skyBoost = 0.25f;
            foliageBoost = 0.25f;

            sharpness = 0.50f;
            clarity = 0.45f;
            roadSheen = 0.30f;
            vignette = 0.20f;

            enableBloom = true;
            bloomIntensity = 0.40f;
            bloomThreshold = 0.84f;

            enableFlares = false;
            flareIntensity = 0.10f;

            enableSunRays = true;
            sunRayIntensity = 0.12f; // Soft, bounded to sky
            sunRayDecay = 0.95f;
            break;

        case Preset::ClearDaylight:
            // High contrast, crisp track clarity, competitive visibility
            exposure = 0.98f;
            contrast = 1.12f;
            saturation = 1.10f;
            warmth = -0.02f; // Cool, crisp daylight
            skyBoost = 0.15f;
            foliageBoost = 0.15f;

            sharpness = 0.80f; // Maximum edge sharpness
            clarity = 0.60f;
            roadSheen = 0.20f;
            vignette = 0.10f;

            enableBloom = true;
            bloomIntensity = 0.15f;
            bloomThreshold = 0.90f;

            enableFlares = false;
            flareIntensity = 0.0f;

            enableSunRays = false;
            sunRayIntensity = 0.0f;
            sunRayDecay = 0.94f;
            break;

        case Preset::GrandPrixCinematic:
            // Dramatic film contrast, anamorphic flares on taillights & stadium floods
            exposure = 1.00f;
            contrast = 1.18f;
            saturation = 1.12f;
            warmth = 0.02f;
            skyBoost = 0.15f;
            foliageBoost = 0.12f;

            sharpness = 0.60f;
            clarity = 0.50f;
            roadSheen = 0.45f;
            vignette = 0.30f;

            enableBloom = true;
            bloomIntensity = 0.50f;
            bloomThreshold = 0.82f;

            enableFlares = true;
            flareIntensity = 0.65f; // Horizontal anamorphic streaks

            enableSunRays = true;
            sunRayIntensity = 0.10f;
            sunRayDecay = 0.95f;
            break;

        case Preset::Custom:
            break;
    }
}

Config& Config::get() {
    static Config instance;
    return instance;
}

void Config::load() {
    if (m_iniPath.empty()) {
        WCHAR path[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_MYDOCUMENTS, NULL, 0, path))) {
            m_iniPath = std::wstring(path) + L"\\TrackMania\\Config\\tm_vibrant_shaders.ini";
        }
    }
    // Default to clean Stadium 2020 preset
    settings.applyPreset(Preset::Stadium2020);
}

void Config::save() {
}

} // namespace tmshaders
