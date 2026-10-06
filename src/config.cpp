#include "config.h"
#include "log.h"
#include <windows.h>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cwctype>

namespace tmshaders {

#define TMVS_FIELD(key, label, cat, kind, member, mn, mx) {key, label, cat, Field::kind, offsetof(Settings, member), mn, mx}

const std::vector<Field>& fields() {
    static const std::vector<Field> table = {
        TMVS_FIELD("AOStrength", "Ambient occlusion", "Lighting", Float, aoStrength, 0.0f, 2.0f),
        TMVS_FIELD("AORadius", "AO radius (m)", "Lighting", Float, aoRadius, 0.3f, 5.0f),
        TMVS_FIELD("ShadowStrength", "Sun shadows", "Lighting", Float, shadowStrength, 0.0f, 1.0f),
        TMVS_FIELD("ShadowLength", "Shadow ray length (m)", "Lighting", Float, shadowLength, 1.0f, 30.0f),
        TMVS_FIELD("LongShadows", "Long-range shadows", "Lighting", Float, longShadows, 0.0f, 1.0f),
        TMVS_FIELD("LongShadowRange", "Long shadow range (m)", "Lighting", Float, longShadowRange, 30.0f, 250.0f),
        TMVS_FIELD("NeonLight", "Neon light on surroundings", "Lighting", Float, neonLight, 0.0f, 2.0f),
        TMVS_FIELD("GlobalIllumination", "Bounce light (global illumination)", "Lighting", Float, globalIllumination, 0.0f, 2.0f),
        TMVS_FIELD("SunLight", "Sunlight warmth", "Lighting", Float, sunLight, 0.0f, 1.5f),
        TMVS_FIELD("AmbientTint", "Sky ambient tint", "Lighting", Float, ambientTint, 0.0f, 1.5f),
        TMVS_FIELD("SunColor", "Sun colour", "Lighting", Color, sunColor, 0.0f, 1.0f),
        TMVS_FIELD("GameSunColor", "Use game's sun colour", "Lighting", Float, gameSunColor, 0.0f, 1.0f),
        TMVS_FIELD("SkyColor", "Sky colour", "Lighting", Color, skyColor, 0.0f, 1.0f),
        TMVS_FIELD("SunElevation", "Sun elevation override (-1 = game)", "Lighting", Float, sunElevationOverride, -1.0f, 90.0f),
        TMVS_FIELD("SunAzimuth", "Sun azimuth override", "Lighting", Float, sunAzimuthOverride, 0.0f, 360.0f),

        TMVS_FIELD("SkyMode", "Sky", "Sky & Atmosphere", Int, skyMode, 0.0f, 5.0f),
        TMVS_FIELD("SkyNight", "Night darkening (-1 = auto)", "Sky & Atmosphere", Float, skyNight, -1.0f, 1.0f),
        TMVS_FIELD("SkyRotation", "Sky rotation", "Sky & Atmosphere", Float, skyRotation, 0.0f, 360.0f),
        TMVS_FIELD("SkyBrightness", "Sky brightness", "Sky & Atmosphere", Float, skyBrightness, 0.2f, 3.0f),
        TMVS_FIELD("CloudAmount", "Clouds (clear sky)", "Sky & Atmosphere", Float, cloudAmount, 0.0f, 1.0f),
        TMVS_FIELD("StarAmount", "Stars", "Sky & Atmosphere", Float, starAmount, 0.0f, 3.0f),
        TMVS_FIELD("SkyEffectSize", "Black hole size (> 3: up close)", "Sky & Atmosphere", Float, skyEffectSize, 0.3f, 6.0f),
        TMVS_FIELD("PlanetSize", "Ringed planet (0 = off)", "Sky & Atmosphere", Float, planetSize, 0.0f, 2.5f),
        TMVS_FIELD("PlanetType", "Ring world planet", "Sky & Atmosphere", Int, planetType, 0.0f, 3.0f),
        TMVS_FIELD("PlanetView", "Ring world view", "Sky & Atmosphere", Int, planetView, 0.0f, 2.0f),
        TMVS_FIELD("PlanetAzimuth", "Planet direction", "Sky & Atmosphere", Float, planetAzimuth, 0.0f, 360.0f),
        TMVS_FIELD("PlanetElevation", "Planet height", "Sky & Atmosphere", Float, planetElevation, -10.0f, 60.0f),
        TMVS_FIELD("VolumetricClouds", "Volumetric clouds", "Sky & Atmosphere", Float, volumetricClouds, 0.0f, 1.0f),
        TMVS_FIELD("CloudCoverage", "Cloud coverage", "Sky & Atmosphere", Float, cloudCoverage, 0.05f, 1.0f),
        TMVS_FIELD("CloudHeight", "Cloud height (m)", "Sky & Atmosphere", Float, cloudHeight, 300.0f, 4000.0f),
        TMVS_FIELD("SkyEnhance", "Sky enhancement", "Sky & Atmosphere", Float, skyEnhance, 0.0f, 1.0f),
        TMVS_FIELD("SunGlow", "Sun glow", "Sky & Atmosphere", Float, sunGlow, 0.0f, 2.0f),
        TMVS_FIELD("FogDensity", "Haze density", "Sky & Atmosphere", Float, fogDensity, 0.0f, 3.0f),
        TMVS_FIELD("FogHeightFalloff", "Haze height falloff", "Sky & Atmosphere", Float, fogHeightFalloff, 0.0f, 3.0f),
        TMVS_FIELD("FogSunScatter", "Haze sun scattering", "Sky & Atmosphere", Float, fogSunScatter, 0.0f, 2.0f),
        TMVS_FIELD("GodRays", "Light shafts", "Sky & Atmosphere", Float, godRays, 0.0f, 2.0f),
        TMVS_FIELD("VolumetricLight", "Volumetric light (shadowed haze)", "Sky & Atmosphere", Float, volumetricLight, 0.0f, 2.0f),
        TMVS_FIELD("GodRayDecay", "Light shaft length", "Sky & Atmosphere", Float, godRayDecay, 0.9f, 0.995f),

        TMVS_FIELD("Bloom", "Bloom", "Bloom & Lens", Float, bloom, 0.0f, 0.4f),
        TMVS_FIELD("BloomRadius", "Bloom radius", "Bloom & Lens", Float, bloomRadius, 0.3f, 1.2f),
        TMVS_FIELD("HighlightBoost", "Light source intensity", "Bloom & Lens", Float, highlightBoost, 0.0f, 8.0f),
        TMVS_FIELD("LensFlare", "Lens flare", "Bloom & Lens", Float, lensFlare, 0.0f, 1.5f),
        TMVS_FIELD("ChromaticAberration", "Chromatic aberration", "Bloom & Lens", Float, chromaticAberration, 0.0f, 1.0f),
        TMVS_FIELD("Vignette", "Vignette", "Bloom & Lens", Float, vignette, 0.0f, 1.0f),
        TMVS_FIELD("FilmGrain", "Film grain", "Bloom & Lens", Float, filmGrain, 0.0f, 0.15f),

        TMVS_FIELD("Exposure", "Exposure (EV)", "Colour", Float, exposure, -2.0f, 2.0f),
        TMVS_FIELD("AutoExposure", "Auto exposure", "Colour", Float, autoExposure, 0.0f, 1.0f),
        TMVS_FIELD("Contrast", "Contrast", "Colour", Float, contrast, 0.7f, 1.5f),
        TMVS_FIELD("Saturation", "Saturation", "Colour", Float, saturation, 0.0f, 2.0f),
        TMVS_FIELD("Vibrance", "Vibrance", "Colour", Float, vibrance, -0.5f, 1.0f),
        TMVS_FIELD("Temperature", "Temperature", "Colour", Float, temperature, -1.0f, 1.0f),
        TMVS_FIELD("Tint", "Tint", "Colour", Float, tint, -1.0f, 1.0f),
        TMVS_FIELD("Lift", "Lift", "Colour", Float, lift, -0.1f, 0.1f),
        TMVS_FIELD("Gamma", "Gamma", "Colour", Float, gamma, 0.6f, 1.6f),
        TMVS_FIELD("Gain", "Gain", "Colour", Float, gain, 0.6f, 1.6f),
        TMVS_FIELD("SplitToning", "Split toning", "Colour", Float, shadowTint, 0.0f, 1.0f),

        TMVS_FIELD("Wetness", "Wet roads", "Weather & Surfaces", Float, wetness, 0.0f, 1.0f),
        TMVS_FIELD("Rain", "Rain (2 = downpour)", "Weather & Surfaces", Float, rain, 0.0f, 2.0f),
        TMVS_FIELD("Puddles", "Puddles", "Weather & Surfaces", Float, puddles, 0.0f, 1.0f),
        TMVS_FIELD("Lightning", "Lightning", "Weather & Surfaces", Float, lightning, 0.0f, 1.0f),
        TMVS_FIELD("LensDrops", "Rain drops on the lens", "Weather & Surfaces", Bool, lensDrops, 0.0f, 1.0f),
        TMVS_FIELD("WeatherSound", "Rain and thunder sound", "Weather & Surfaces", Float, weatherSound, 0.0f, 1.0f),
        TMVS_FIELD("WaterSurfaces", "Water (pools, sea)", "Weather & Surfaces", Float, waterSurfaces, 0.0f, 1.0f),
        TMVS_FIELD("Reflections", "Track reflections (dry, >1 = mirror)", "Weather & Surfaces", Float, reflections, 0.0f, 2.0f),
        TMVS_FIELD("GrassDetail", "Grass detail", "Weather & Surfaces", Float, grassDetail, 0.0f, 1.0f),
        TMVS_FIELD("MowingStripes", "Mowing stripes", "Weather & Surfaces", Float, mowingStripes, 0.0f, 1.0f),
        TMVS_FIELD("Wind", "Wind", "Weather & Surfaces", Float, wind, 0.0f, 1.0f),

        TMVS_FIELD("MotionBlur", "Motion blur", "Cinematic", Float, motionBlur, 0.0f, 1.5f),
        TMVS_FIELD("DepthOfField", "Depth of field", "Cinematic", Float, depthOfField, 0.0f, 1.0f),
        TMVS_FIELD("CinematicOnlyInReplays", "Only in replays and video export", "Cinematic", Bool, cinematicOnlyInReplays, 0.0f, 1.0f),
        TMVS_FIELD("FocusDistance", "Focus distance (m, 0 = auto)", "Cinematic", Float, focusDistance, 0.0f, 200.0f),
        TMVS_FIELD("BokehSize", "Max blur (px)", "Cinematic", Float, bokehSize, 2.0f, 24.0f),

        TMVS_FIELD("FXAA", "FXAA anti-aliasing", "Image", Bool, fxaa, 0.0f, 1.0f),
        TMVS_FIELD("TAA", "Temporal anti-aliasing", "Image", Bool, taa, 0.0f, 1.0f),
        TMVS_FIELD("TAAJitter", "TAA: sub-pixel jitter", "Image", Bool, taaJitter, 0.0f, 1.0f),
        TMVS_FIELD("Sharpen", "Sharpening (CAS)", "Image", Float, sharpen, 0.0f, 1.0f),
        TMVS_FIELD("Quality", "Effect quality", "Image", Int, quality, 0.0f, 2.0f),
        TMVS_FIELD("AutoQuality", "Auto quality (adapts to your GPU)", "Image", Bool, autoQuality, 0.0f, 1.0f),
        TMVS_FIELD("TargetFPS", "Auto quality: target FPS", "Image", Float, targetFps, 30.0f, 240.0f),
        TMVS_FIELD("DisableGameMSAA", "Disable game MSAA (restart)", "Image", Bool, disableGameMSAA, 0.0f, 1.0f),
        TMVS_FIELD("ReadableGameDepth", "Effects in replay/video export (restart)", "Image", Bool, readableGameDepth, 0.0f, 1.0f),
    };
    return table;
}

#undef TMVS_FIELD

const char* presetName(Preset preset) {
    switch (preset) {
        case Preset::Vibrant: return "Vibrant";
        case Preset::Cinematic: return "Cinematic";
        case Preset::GoldenHour: return "Golden Hour";
        case Preset::Dreamy: return "Dreamy";
        case Preset::NeonNight: return "Neon Night";
        case Preset::EventHorizon: return "Event Horizon";
        case Preset::Aurora: return "Aurora";
        case Preset::Competition: return "Competition";
        case Preset::Performance: return "Performance";
        case Preset::RainyDay: return "Rainy Day";
        case Preset::ReplayCinema: return "Replay Cinema";
        case Preset::Thunderstorm: return "Thunderstorm";
        case Preset::Custom: return "Custom";
        default: return "?";
    }
}

const char* moodName(Mood mood) {
    switch (mood) {
        case Mood::Day: return "Day";
        case Mood::Dusk: return "Sunrise / Sunset";
        case Mood::Night: return "Night";
        default: return "Unknown";
    }
}

Mood classifyMood(const float c[3]) {
    // Stadium moods: day ~white, sunset ~(1, .56, .22), night ~(.33, .30, .44).
    const float luma = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    if (luma < 0.5f) return Mood::Night;
    if (c[2] < 0.5f * c[0]) return Mood::Dusk;
    return Mood::Day;
}

namespace {

const wchar_t* const kMoodKeys[] = {L"PresetDay", L"PresetDusk", L"PresetNight"};

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &out[0], n);
    return out;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &out[0], n, nullptr, nullptr);
    return out;
}

// Older settings files stored presets by index.
std::string presetFromText(const std::wstring& text, const std::string& fallback) {
    if (text.empty()) return fallback;
    if (iswdigit(text[0])) {
        const int i = _wtoi(text.c_str());
        // Index 10 was "Custom" before more presets were added.
        if (i >= 10 || i >= static_cast<int>(Preset::Custom)) return presetName(Preset::Custom);
        return i >= 0 ? presetName(static_cast<Preset>(i)) : fallback;
    }
    if (!_wcsicmp(text.c_str(), L"Keep")) return {};
    return narrow(text);
}

// Machine settings that belong to the installation, not to a look.
bool isSystemField(const Field& f) {
    return !strcmp(f.key, "DisableGameMSAA") || !strcmp(f.key, "ReadableGameDepth") || !strcmp(f.key, "AutoQuality") ||
           !strcmp(f.key, "TargetFPS") || !strcmp(f.key, "WeatherSound") || !strcmp(f.key, "CinematicOnlyInReplays") ||
           !strcmp(f.key, "TAAJitter");
}

void readFields(Settings& settings, const wchar_t* ini, const wchar_t* section, bool includeSystem) {
    char* base = reinterpret_cast<char*>(&settings);
    for (const Field& f : fields()) {
        if (!includeSystem && isSystemField(f)) continue;
        wchar_t key[64], value[128];
        swprintf(key, 64, L"%hs", f.key);
        if (!GetPrivateProfileStringW(section, key, L"", value, 128, ini)) continue;
        if (f.kind == Field::Bool) {
            *reinterpret_cast<bool*>(base + f.offset) = _wtoi(value) != 0;
        } else if (f.kind == Field::Int) {
            *reinterpret_cast<int*>(base + f.offset) = _wtoi(value);
        } else if (f.kind == Field::Color) {
            float* c = reinterpret_cast<float*>(base + f.offset);
            swscanf(value, L"%f,%f,%f", &c[0], &c[1], &c[2]);
        } else {
            *reinterpret_cast<float*>(base + f.offset) = static_cast<float>(_wtof(value));
        }
    }
}

void writeFields(const Settings& settings, const wchar_t* ini, const wchar_t* section, bool includeSystem) {
    const char* base = reinterpret_cast<const char*>(&settings);
    for (const Field& f : fields()) {
        if (!includeSystem && isSystemField(f)) continue;
        wchar_t key[64], value[128];
        swprintf(key, 64, L"%hs", f.key);
        if (f.kind == Field::Bool) {
            swprintf(value, 128, L"%d", *reinterpret_cast<const bool*>(base + f.offset) ? 1 : 0);
        } else if (f.kind == Field::Int) {
            swprintf(value, 128, L"%d", *reinterpret_cast<const int*>(base + f.offset));
        } else if (f.kind == Field::Color) {
            const float* c = reinterpret_cast<const float*>(base + f.offset);
            swprintf(value, 128, L"%.3f,%.3f,%.3f", c[0], c[1], c[2]);
        } else {
            swprintf(value, 128, L"%.4f", *reinterpret_cast<const float*>(base + f.offset));
        }
        WritePrivateProfileStringW(section, key, value, ini);
    }
}

// Preset names become file names: keep them simple.
std::string sanitizeName(const std::string& name) {
    std::string out;
    for (char ch : name) {
        if (strchr("\\/:*?\"<>|", ch) || static_cast<unsigned char>(ch) < 32) continue;
        out += ch;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    if (out.size() > 40) out.resize(40);
    return out;
}

bool isBuiltIn(const std::string& name) {
    for (int i = 0; i <= static_cast<int>(Preset::Custom); i++) {
        if (!_stricmp(name.c_str(), presetName(static_cast<Preset>(i)))) return true;
    }
    return false;
}

} // namespace

std::vector<std::string> Config::presetNames() const {
    std::vector<std::string> names;
    for (int i = 0; i < static_cast<int>(Preset::Custom); i++) names.push_back(presetName(static_cast<Preset>(i)));
    names.insert(names.end(), m_userPresets.begin(), m_userPresets.end());
    return names;
}

bool Config::isUserPreset(const std::string& name) const {
    for (const auto& user : m_userPresets) {
        if (user == name) return true;
    }
    return false;
}

std::wstring Config::presetFile(const std::string& name) const {
    return m_presetDir + L"\\" + widen(name) + L".ini";
}

void Config::scanUserPresets() {
    m_userPresets.clear();
    WIN32_FIND_DATAW found{};
    HANDLE find = FindFirstFileW((m_presetDir + L"\\*.ini").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring file = found.cFileName;
        std::string name = narrow(file.substr(0, file.size() - 4));
        if (!name.empty() && !isBuiltIn(name)) m_userPresets.push_back(name);
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

bool Config::applyNamed(const std::string& name) {
    for (int i = 0; i < static_cast<int>(Preset::Custom); i++) {
        if (name == presetName(static_cast<Preset>(i))) {
            applyPreset(settings, static_cast<Preset>(i));
            return true;
        }
    }
    if (!isUserPreset(name)) return false;
    applyPreset(settings, Preset::Vibrant); // anything the file doesn't set
    readFields(settings, presetFile(name).c_str(), L"Preset", false);
    return true;
}

bool Config::selectPreset(const std::string& name) {
    if (!applyNamed(name)) return false;
    preset = name;
    if (autoMood && mood != Mood::Unknown) moodPreset[static_cast<int>(mood)] = name;
    markDirty();
    return true;
}

bool Config::saveUserPreset(const std::string& requested) {
    const std::string name = sanitizeName(requested);
    if (name.empty() || isBuiltIn(name)) return false;
    CreateDirectoryW(m_presetDir.c_str(), nullptr);
    const std::wstring file = presetFile(name);
    DeleteFileW(file.c_str());
    writeFields(settings, file.c_str(), L"Preset", false);
    scanUserPresets();
    TMVS_LOG("config: saved preset \"%s\"", name.c_str());
    preset = name;
    if (autoMood && mood != Mood::Unknown) moodPreset[static_cast<int>(mood)] = name;
    markDirty();
    return true;
}

void Config::deleteUserPreset(const std::string& name) {
    if (!isUserPreset(name)) return;
    DeleteFileW(presetFile(name).c_str());
    scanUserPresets();
    if (preset == name) preset = kCustomPreset; // keep the current look
    for (auto& mp : moodPreset) {
        if (mp == name) mp.clear();
    }
    markDirty();
}

void Config::onMoodDetected(Mood m) {
    if (m == mood || m == Mood::Unknown) return;
    mood = m;
    markDirty();
    TMVS_LOG("config: map mood %s", moodName(m));
    // Your own unsaved tweaks win over the mood presets: a map change must not throw them away.
    if (preset == kCustomPreset) return;
    const std::string& p = moodPreset[static_cast<int>(m)];
    if (!autoMood || p.empty() || p == preset) return;
    if (applyNamed(p)) {
        preset = p;
        TMVS_LOG("config: map mood %s -> preset %s", moodName(m), p.c_str());
    }
}

void Config::markDirty() {
    if (!m_dirty) m_dirtySince = GetTickCount();
    m_dirty = true;
}

void Config::tick() {
    if (m_dirty && GetTickCount() - m_dirtySince > 1500) save();
}

void applyPreset(Settings& s, Preset preset) {
    const bool keepMSAA = s.disableGameMSAA;
    const bool keepDepth = s.readableGameDepth;
    const bool keepEnabled = s.enabled;
    const bool keepAuto = s.autoQuality;
    const float keepTarget = s.targetFps;
    const float keepSound = s.weatherSound;
    const bool keepReplayOnly = s.cinematicOnlyInReplays;
    const bool keepJitter = s.taaJitter;
    if (preset == Preset::Custom) return;
    s = Settings();
    s.disableGameMSAA = keepMSAA;
    s.readableGameDepth = keepDepth;
    s.enabled = keepEnabled;
    s.autoQuality = keepAuto;
    s.targetFps = keepTarget;
    s.weatherSound = keepSound;
    s.cinematicOnlyInReplays = keepReplayOnly;
    s.taaJitter = keepJitter;
    switch (preset) {
        case Preset::Vibrant:
            break; // Settings defaults are the Vibrant look.
        case Preset::Cinematic:
            s.volumetricLight = 1.0f;
            s.aoStrength = 1.2f;
            s.shadowStrength = 0.8f;
            s.sunLight = 0.6f;
            s.ambientTint = 0.7f;
            s.sunColor[0] = 1.0f; s.sunColor[1] = 0.82f; s.sunColor[2] = 0.58f;
            s.skyColor[0] = 0.48f; s.skyColor[1] = 0.66f; s.skyColor[2] = 1.0f;
            s.fogDensity = 1.1f;
            s.fogSunScatter = 1.3f;
            s.godRays = 1.5f;
            s.godRayDecay = 0.99f;
            s.sunGlow = 1.0f;
            s.bloom = 0.11f;
            s.lensFlare = 0.45f;
            s.vignette = 0.38f;
            s.contrast = 1.14f;
            s.saturation = 1.05f;
            s.vibrance = 0.2f;
            s.temperature = 0.12f;
            s.shadowTint = 0.45f;
            s.filmGrain = 0.02f;
            break;
        case Preset::GoldenHour:
            s.volumetricLight = 1.0f;
            s.skyMode = 1;
            s.cloudAmount = 0.6f;
            s.sunColor[0] = 1.0f; s.sunColor[1] = 0.74f; s.sunColor[2] = 0.45f;
            s.gameSunColor = 0.3f;
            s.skyColor[0] = 0.5f; s.skyColor[1] = 0.62f; s.skyColor[2] = 1.0f;
            s.sunLight = 0.75f;
            s.ambientTint = 0.65f;
            s.shadowStrength = 0.75f;
            s.fogDensity = 0.7f;
            s.fogSunScatter = 1.4f;
            s.godRays = 1.5f;
            s.godRayDecay = 0.99f;
            s.sunGlow = 1.0f;
            s.bloom = 0.09f;
            s.lensFlare = 0.4f;
            s.contrast = 1.1f;
            s.saturation = 1.12f;
            s.vibrance = 0.3f;
            s.temperature = 0.22f;
            s.shadowTint = 0.4f;
            s.vignette = 0.3f;
            break;
        case Preset::Dreamy:
            s.volumetricLight = 0.8f;
            s.sunColor[0] = 1.0f; s.sunColor[1] = 0.8f; s.sunColor[2] = 0.85f;
            s.skyColor[0] = 0.45f; s.skyColor[1] = 0.85f; s.skyColor[2] = 1.0f;
            s.gameSunColor = 0.3f;
            s.aoStrength = 0.7f;
            s.sunLight = 0.4f;
            s.ambientTint = 0.8f;
            s.fogDensity = 0.8f;
            s.fogSunScatter = 1.0f;
            s.skyEnhance = 0.8f;
            s.bloom = 0.2f;
            s.bloomRadius = 1.0f;
            s.godRays = 1.3f;
            s.contrast = 0.92f;
            s.saturation = 1.15f;
            s.vibrance = 0.35f;
            s.lift = 0.03f;
            s.temperature = 0.05f;
            s.tint = 0.25f;
            s.shadowTint = 0.8f;
            s.vignette = 0.2f;
            s.chromaticAberration = 0.08f;
            break;
        case Preset::NeonNight:
            s.skyMode = 2;
            s.starAmount = 1.3f;
            s.skyColor[0] = 0.35f; s.skyColor[1] = 0.5f; s.skyColor[2] = 1.0f;
            s.aoStrength = 1.3f;
            s.ambientTint = 0.8f;
            s.highlightBoost = 3.0f;
            s.bloom = 0.4f;
            s.godRays = 2.0f;
            s.exposure = 1.5f;
            s.bloomRadius = 0.9f;
            s.fogDensity = 0.6f;
            s.contrast = 1.08f;
            s.saturation = 1.08f;
            s.vibrance = 0.25f;
            s.temperature = -0.12f;
            s.shadowTint = 0.5f;
            s.vignette = 0.3f;
            s.chromaticAberration = 0.0f;
            s.filmGrain = 0.01f;
            s.neonLight = 2.0f;
            s.reflections = 1.3f;   // night: the lit stadium mirrors in the dry track
            break;
        case Preset::EventHorizon:
            s.skyMode = 5;      // the ring world sky: planet, rings and a black hole
            s.reflections = 1.3f;
            s.planetType = 0;
            s.planetView = 0;
            s.skyRotation = 0.0f;
            s.skyEffectSize = 1.2f;
            s.planetSize = 1.0f;
            s.starAmount = 1.5f;
            s.skyColor[0] = 0.3f; s.skyColor[1] = 0.48f; s.skyColor[2] = 1.0f;
            s.aoStrength = 1.4f;
            s.ambientTint = 0.7f;
            s.highlightBoost = 3.0f;
            s.fogDensity = 0.9f;
            s.bloom = 0.1f;
            s.contrast = 1.18f;
            s.saturation = 1.0f;
            s.vibrance = 0.15f;
            s.temperature = -0.05f;
            s.shadowTint = 0.6f;
            s.vignette = 0.45f;
            s.chromaticAberration = 0.0f;
            s.filmGrain = 0.015f;
            break;
        case Preset::Aurora:
            s.skyMode = 4;
            s.reflections = 1.3f;
            s.starAmount = 1.2f;
            s.skyColor[0] = 0.3f; s.skyColor[1] = 0.85f; s.skyColor[2] = 0.75f;
            s.ambientTint = 0.9f;
            s.highlightBoost = 4.0f;
            s.fogDensity = 0.5f;
            s.bloom = 0.11f;
            s.contrast = 1.08f;
            s.saturation = 1.15f;
            s.vibrance = 0.3f;
            s.temperature = -0.2f;
            s.tint = -0.15f;
            s.shadowTint = 0.6f;
            s.vignette = 0.3f;
            s.chromaticAberration = 0.0f;
            break;
        case Preset::Competition:
            s.volumetricLight = 0.0f;
            s.globalIllumination = 0.0f;
            s.aoStrength = 0.8f;
            s.shadowStrength = 0.4f;
            s.sunLight = 0.2f;
            s.ambientTint = 0.25f;
            s.fogDensity = 0.0f;
            s.godRays = 0.0f;
            s.sunGlow = 0.3f;
            s.bloom = 0.03f;
            s.lensFlare = 0.0f;
            s.chromaticAberration = 0.0f;
            s.vignette = 0.0f;
            s.filmGrain = 0.0f;
            s.autoExposure = 0.0f;
            s.contrast = 1.04f;
            s.saturation = 1.06f;
            s.vibrance = 0.2f;
            s.temperature = 0.0f;
            s.shadowTint = 0.0f;
            s.sharpen = 0.5f;
            break;
        case Preset::Performance:
            s.volumetricLight = 0.0f;
            s.globalIllumination = 0.0f;
            // Same look, cheaper: fewer AO / shadow samples, no shafts, flare or extra passes.
            s.quality = 0;
            s.godRays = 0.0f;
            s.lensFlare = 0.0f;
            s.chromaticAberration = 0.0f;
            s.filmGrain = 0.0f;
            s.sharpen = 0.0f;
            s.longShadows = 0.0f;
            s.neonLight = 0.0f;
            s.grassDetail = 0.0f;
            s.taa = false;
            break;
        case Preset::RainyDay:
            s.volumetricLight = 0.0f;
            s.volumetricClouds = 1.0f;
            s.cloudCoverage = 0.95f;
            s.cloudHeight = 900.0f;
            s.wetness = 1.0f;
            s.rain = 0.7f;
            s.puddles = 0.4f;
            s.wind = 0.6f;
            s.sunLight = 0.15f;
            s.shadowStrength = 0.3f;
            s.longShadows = 0.0f;
            s.ambientTint = 0.8f;
            s.skyColor[0] = 0.55f; s.skyColor[1] = 0.62f; s.skyColor[2] = 0.75f;
            s.fogDensity = 1.6f;
            s.fogSunScatter = 0.2f;
            s.godRays = 0.0f;
            s.sunGlow = 0.2f;
            s.lensFlare = 0.0f;
            s.highlightBoost = 4.0f;
            s.neonLight = 1.0f;
            s.bloom = 0.1f;
            s.exposure = -0.15f;
            s.contrast = 1.04f;
            s.saturation = 0.85f;
            s.vibrance = 0.1f;
            s.temperature = -0.15f;
            s.shadowTint = 0.3f;
            s.vignette = 0.35f;
            break;
        case Preset::ReplayCinema:
            s.volumetricLight = 1.0f;
            s.aoStrength = 1.2f;
            s.sunLight = 0.7f;
            s.ambientTint = 0.7f;
            s.sunColor[0] = 1.0f; s.sunColor[1] = 0.82f; s.sunColor[2] = 0.58f;
            s.fogDensity = 1.0f;
            s.fogSunScatter = 1.2f;
            s.godRays = 1.6f;
            s.godRayDecay = 0.985f;
            s.bloom = 0.1f;
            s.lensFlare = 0.35f;
            s.reflections = 0.35f;
            s.motionBlur = 0.1f;
            s.depthOfField = 0.08f; // meant for replays: while driving, the auto focus sits on your car
            s.bokehSize = 10.0f;
            s.vignette = 0.4f;
            s.contrast = 1.12f;
            s.temperature = 0.1f;
            s.shadowTint = 0.45f;
            s.filmGrain = 0.025f;
            s.chromaticAberration = 0.1f;
            break;
        case Preset::Thunderstorm:
            s.lensDrops = true;
            s.volumetricLight = 0.0f;
            // Overcast: the day map's sun is taken out like for a night sky, only darker grey.
            s.skyNight = 0.45f;
            s.volumetricClouds = 1.0f;
            s.cloudCoverage = 1.0f;
            s.cloudHeight = 650.0f;
            s.wetness = 1.0f;
            s.rain = 1.6f;
            s.puddles = 0.55f;
            s.lightning = 1.0f;
            s.wind = 0.9f;
            s.sunLight = 0.0f;
            s.shadowStrength = 0.1f;
            s.longShadows = 0.0f;
            s.ambientTint = 0.9f;
            s.skyColor[0] = 0.5f; s.skyColor[1] = 0.56f; s.skyColor[2] = 0.68f;
            s.fogDensity = 2.2f;
            s.fogSunScatter = 0.0f;
            s.godRays = 0.0f;
            s.sunGlow = 0.0f;
            s.lensFlare = 0.0f;
            s.highlightBoost = 4.5f;
            s.neonLight = 1.3f;
            s.bloom = 0.12f;
            s.exposure = -0.3f;
            s.contrast = 1.1f;
            s.saturation = 0.78f;
            s.vibrance = 0.05f;
            s.temperature = -0.25f;
            s.shadowTint = 0.4f;
            s.vignette = 0.45f;
            s.chromaticAberration = 0.0f;
            s.filmGrain = 0.02f;
            break;
        default:
            break;
    }
}

Config& Config::get() {
    static Config instance;
    return instance;
}

void Config::load() {
    m_path = log::dataDir() + L"\\settings.ini";
    m_presetDir = log::dataDir() + L"\\presets";
    scanUserPresets();
    const wchar_t* ini = m_path.c_str();
    wchar_t text[128];
    GetPrivateProfileStringW(L"General", L"Preset", L"", text, 128, ini);
    preset = presetFromText(text, "Vibrant");
    autoMood = GetPrivateProfileIntW(L"General", L"AutoMoodPresets", 1, ini) != 0;
    advancedMenu = GetPrivateProfileIntW(L"General", L"AdvancedMenu", 0, ini) != 0;
    for (int i = 0; i < static_cast<int>(Mood::Count); i++) {
        if (GetPrivateProfileStringW(L"General", kMoodKeys[i], L"", text, 128, ini)) moodPreset[i] = presetFromText(text, moodPreset[i]);
    }
    // The mood of the last map: restarting on the same kind of map keeps your current look.
    const int lastMood = static_cast<int>(GetPrivateProfileIntW(L"General", L"LastMood", -1, ini));
    mood = lastMood >= 0 && lastMood < static_cast<int>(Mood::Count) ? static_cast<Mood>(lastMood) : Mood::Unknown;

    settings.enabled = GetPrivateProfileIntW(L"General", L"Enabled", 1, ini) != 0;
    settings.disableGameMSAA = GetPrivateProfileIntW(L"Settings", L"DisableGameMSAA", 1, ini) != 0;
    settings.readableGameDepth = GetPrivateProfileIntW(L"Settings", L"ReadableGameDepth", 1, ini) != 0;
    settings.autoQuality = GetPrivateProfileIntW(L"Settings", L"AutoQuality", 1, ini) != 0;
    wchar_t target[32] = {};
    if (GetPrivateProfileStringW(L"Settings", L"TargetFPS", L"", target, 32, ini)) settings.targetFps = static_cast<float>(_wtof(target));
    if (GetPrivateProfileStringW(L"Settings", L"WeatherSound", L"", target, 32, ini)) settings.weatherSound = static_cast<float>(_wtof(target));
    settings.cinematicOnlyInReplays = GetPrivateProfileIntW(L"Settings", L"CinematicOnlyInReplays", 1, ini) != 0;
    settings.taaJitter = GetPrivateProfileIntW(L"Settings", L"TAAJitter", 1, ini) != 0;
    if (preset == kCustomPreset) {
        applyPreset(settings, Preset::Vibrant);
        readFields(settings, ini, L"Settings", false);
    } else if (!applyNamed(preset)) {
        preset = "Vibrant";
        applyNamed(preset);
    }
    m_dirty = false;
}

void Config::save() {
    if (m_path.empty()) m_path = log::dataDir() + L"\\settings.ini";
    const wchar_t* ini = m_path.c_str();
    WritePrivateProfileStringW(L"General", L"Preset", widen(preset).c_str(), ini);
    WritePrivateProfileStringW(L"General", L"Enabled", settings.enabled ? L"1" : L"0", ini);
    WritePrivateProfileStringW(L"General", L"AutoMoodPresets", autoMood ? L"1" : L"0", ini);
    WritePrivateProfileStringW(L"General", L"AdvancedMenu", advancedMenu ? L"1" : L"0", ini);
    for (int i = 0; i < static_cast<int>(Mood::Count); i++) {
        WritePrivateProfileStringW(L"General", kMoodKeys[i], moodPreset[i].empty() ? L"Keep" : widen(moodPreset[i]).c_str(), ini);
    }
    wchar_t value[16];
    swprintf(value, 16, L"%d", static_cast<int>(mood));
    WritePrivateProfileStringW(L"General", L"LastMood", value, ini);
    writeFields(settings, ini, L"Settings", true);
    m_dirty = false;
}

} // namespace tmshaders
