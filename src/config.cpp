#include "config.h"
#include <windows.h>
#include <shlobj.h>

namespace tmshaders {

void ShaderSettings::applyPreset(Preset preset) {
    activePreset = preset;
    switch (preset) {
        case Preset::SildursVibrant:
            exposure = 1.10f;
            contrast = 1.18f;
            colorTemp = 0.12f;
            vibrance = 0.45f;
            skyVibrance = 0.32f;
            foliageBoost = 0.35f;
            tonemapMode = 1;
            sunTint[0] = 1.05f; sunTint[1] = 1.02f; sunTint[2] = 0.90f;
            shadowTint[0] = 0.94f; shadowTint[1] = 0.96f; shadowTint[2] = 1.02f;

            enableSunRays = true;
            rayDensity = 1.15f;
            rayDecay = 0.965f;
            rayWeight = 0.42f;
            rayExposure = 1.40f;
            rayColor[0] = 1.00f; rayColor[1] = 0.85f; rayColor[2] = 0.58f;

            enableBloom = true;
            bloomThreshold = 0.72f;
            bloomIntensity = 0.80f;
            bloomRadius = 2.6f;
            anamorphicIntensity = 0.35f;
            flareTint[0] = 0.85f; flareTint[1] = 0.90f; flareTint[2] = 1.00f;

            enableFog = true;
            fogDensity = 0.38f;
            fogStart = 0.10f;
            fogCurve = 1.8f;
            sunScatterPower = 0.50f;

            enableDepthShading = true;
            aoIntensity = 0.60f;
            aoRadius = 2.5f;
            break;

        case Preset::BSLClean:
            exposure = 1.05f;
            contrast = 1.12f;
            colorTemp = 0.03f;
            vibrance = 0.25f;
            skyVibrance = 0.18f;
            foliageBoost = 0.18f;
            tonemapMode = 1;
            sunTint[0] = 1.02f; sunTint[1] = 1.01f; sunTint[2] = 0.96f;
            shadowTint[0] = 0.96f; shadowTint[1] = 0.98f; shadowTint[2] = 1.01f;

            enableSunRays = true;
            rayDensity = 0.85f;
            rayDecay = 0.950f;
            rayWeight = 0.28f;
            rayExposure = 0.95f;
            rayColor[0] = 1.00f; rayColor[1] = 0.92f; rayColor[2] = 0.80f;

            enableBloom = true;
            bloomThreshold = 0.80f;
            bloomIntensity = 0.45f;
            bloomRadius = 1.8f;
            anamorphicIntensity = 0.15f;
            flareTint[0] = 0.80f; flareTint[1] = 0.88f; flareTint[2] = 1.00f;

            enableFog = true;
            fogDensity = 0.52f;
            fogStart = 0.08f;
            fogCurve = 2.0f;
            sunScatterPower = 0.35f;

            enableDepthShading = true;
            aoIntensity = 0.75f;
            aoRadius = 2.8f;
            break;

        case Preset::IterationTCinematic:
            exposure = 1.12f;
            contrast = 1.24f;
            colorTemp = 0.06f;
            vibrance = 0.38f;
            skyVibrance = 0.28f;
            foliageBoost = 0.22f;
            tonemapMode = 1;
            sunTint[0] = 1.06f; sunTint[1] = 1.02f; sunTint[2] = 0.92f;
            shadowTint[0] = 0.92f; shadowTint[1] = 0.95f; shadowTint[2] = 1.04f;

            enableSunRays = true;
            rayDensity = 1.25f;
            rayDecay = 0.970f;
            rayWeight = 0.45f;
            rayExposure = 1.50f;
            rayColor[0] = 1.00f; rayColor[1] = 0.88f; rayColor[2] = 0.68f;

            enableBloom = true;
            bloomThreshold = 0.68f;
            bloomIntensity = 0.95f;
            bloomRadius = 3.0f;
            anamorphicIntensity = 1.25f; // Signature IterationT flare
            flareTint[0] = 0.65f; flareTint[1] = 0.85f; flareTint[2] = 1.00f;

            enableFog = true;
            fogDensity = 0.35f;
            fogStart = 0.12f;
            fogCurve = 1.6f;
            sunScatterPower = 0.55f;

            enableDepthShading = true;
            aoIntensity = 0.65f;
            aoRadius = 2.6f;
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
    // Set default preset
    settings.applyPreset(Preset::SildursVibrant);
}

void Config::save() {
    // Persist active settings if needed
}

} // namespace tmshaders
