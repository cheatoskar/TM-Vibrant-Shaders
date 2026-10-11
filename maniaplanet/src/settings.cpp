#include "settings.h"
#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>

namespace tmvs_mp {
namespace settings {
namespace {

Settings g_settings;

std::wstring file() {
    return tmshaders::log::dataDir() + L"\\settings.ini";
}

std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

bool readString(const wchar_t* key, std::string& value) {
    wchar_t buffer[512] = {};
    GetPrivateProfileStringW(L"TMVS", key, L"\x1", buffer, 512, file().c_str());
    if (buffer[0] == L'\x1') return false;
    value = narrow(buffer);
    return true;
}

void readFloat(const wchar_t* key, float& value) {
    std::string s;
    if (readString(key, s)) value = static_cast<float>(atof(s.c_str()));
}

void readInt(const wchar_t* key, int& value) {
    std::string s;
    if (readString(key, s)) value = atoi(s.c_str());
}

void writeString(const wchar_t* key, const std::string& value) {
    WritePrivateProfileStringW(L"TMVS", key, widen(value).c_str(), file().c_str());
}

void writeFloat(const wchar_t* key, float value) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.3f", value);
    writeString(key, buffer);
}

void writeInt(const wchar_t* key, int value) {
    writeString(key, std::to_string(value));
}

// Grading values per preset: exposure, contrast, saturation, vibrance, warmth, tint, shadows,
// highlights, clarity, sharpen, vignette.
struct Values {
    float v[11];
};

struct PresetDef {
    Preset info;
    Values values;
};

const std::vector<PresetDef>& definitions() {
    static const std::vector<PresetDef> defs = {
        {{"Vibrant", "Richer colour and depth, the engine's light kept."},
         {{0.0f, 0.2f, 1.08f, 0.35f, 0.0f, 0.0f, 0.0f, 0.25f, 0.35f, 0.3f, 0.15f}}},
        {{"Natural", "A light touch: crisper, a little more colour."},
         {{0.0f, 0.08f, 1.0f, 0.15f, 0.0f, 0.0f, 0.0f, 0.15f, 0.15f, 0.2f, 0.05f}}},
        {{"Punchy", "Strong contrast and colour, deep shadows."},
         {{0.0f, 0.4f, 1.15f, 0.55f, 0.05f, 0.0f, -0.15f, 0.35f, 0.55f, 0.4f, 0.25f}}},
        {{"Golden Hour", "Warm, soft highlights, gentle contrast."},
         {{0.1f, 0.15f, 1.05f, 0.3f, 0.45f, 0.05f, 0.05f, 0.4f, 0.25f, 0.25f, 0.2f}}},
        {{"Cold Night", "Cool and contrasty, darker shadows."},
         {{-0.15f, 0.3f, 0.95f, 0.25f, -0.4f, -0.05f, -0.2f, 0.3f, 0.35f, 0.3f, 0.3f}}},
        {{"Clean", "No grading, only sharpening (to compare with the game)."},
         {{0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.3f, 0.0f}}},
    };
    return defs;
}

} // namespace

Settings& current() {
    return g_settings;
}

const std::vector<Preset>& presets() {
    static const std::vector<Preset> list = [] {
        std::vector<Preset> out;
        for (const auto& d : definitions()) out.push_back(d.info);
        return out;
    }();
    return list;
}

void applyPreset(Settings& s, const std::string& name) {
    for (const auto& d : definitions()) {
        if (name != d.info.name) continue;
        const float* v = d.values.v;
        s.exposure = v[0];
        s.contrast = v[1];
        s.saturation = v[2];
        s.vibrance = v[3];
        s.warmth = v[4];
        s.tint = v[5];
        s.shadows = v[6];
        s.highlights = v[7];
        s.clarity = v[8];
        s.sharpen = v[9];
        s.vignette = v[10];
        s.preset = name;
        return;
    }
}

bool set(Settings& s, const std::string& key, const std::string& value) {
    const struct {
        const char* key;
        float* value;
    } floats[] = {{"Exposure", &s.exposure},     {"Contrast", &s.contrast}, {"Saturation", &s.saturation},
                  {"Vibrance", &s.vibrance},     {"Warmth", &s.warmth},     {"Tint", &s.tint},
                  {"Shadows", &s.shadows},       {"Highlights", &s.highlights}, {"Clarity", &s.clarity},
                  {"Sharpen", &s.sharpen},       {"Vignette", &s.vignette}, {"LutMix", &s.lutMix},
                  {"Split", &s.split},           {"ToneKnee", &s.toneKnee}, {"ToneHue", &s.toneHue},
                  {"HdrExposure", &s.hdrExposure}, {"Bloom", &s.bloom}};
    for (const auto& f : floats) {
        if (_stricmp(key.c_str(), f.key) == 0) {
            *f.value = static_cast<float>(atof(value.c_str()));
            return true;
        }
    }
    if (_stricmp(key.c_str(), "Lut") == 0) s.lut = value;
    else if (_stricmp(key.c_str(), "View") == 0) s.view = static_cast<View>(atoi(value.c_str()));
    else if (_stricmp(key.c_str(), "ToneCurve") == 0) s.toneCurve = static_cast<ToneCurve>(atoi(value.c_str()));
    else if (_stricmp(key.c_str(), "Preset") == 0) applyPreset(s, value);
    else return false;
    return true;
}

void load() {
    Settings& s = g_settings;
    int enabled = s.enabled, placement = static_cast<int>(s.placement), fallback = s.fallbackAtEnd;
    int curve = static_cast<int>(s.toneCurve);
    readInt(L"Enabled", enabled);
    readString(L"Preset", s.preset);
    applyPreset(s, s.preset);
    readFloat(L"Exposure", s.exposure);
    readFloat(L"Contrast", s.contrast);
    readFloat(L"Saturation", s.saturation);
    readFloat(L"Vibrance", s.vibrance);
    readFloat(L"Warmth", s.warmth);
    readFloat(L"Tint", s.tint);
    readFloat(L"Shadows", s.shadows);
    readFloat(L"Highlights", s.highlights);
    readFloat(L"Clarity", s.clarity);
    readFloat(L"Sharpen", s.sharpen);
    readFloat(L"Vignette", s.vignette);
    readString(L"Lut", s.lut);
    readFloat(L"LutMix", s.lutMix);
    readInt(L"Placement", placement);
    readInt(L"ToneCurve", curve);
    readFloat(L"ToneKnee", s.toneKnee);
    readFloat(L"ToneHue", s.toneHue);
    readFloat(L"HdrExposure", s.hdrExposure);
    readFloat(L"Bloom", s.bloom);
    s.toneCurve = static_cast<ToneCurve>(curve < 0 || curve > 2 ? 1 : curve);
    readInt(L"FallbackAtEnd", fallback);
    s.enabled = enabled != 0;
    s.placement = placement == 1 ? Placement::EndOfFrame : Placement::BeforeHud;
    s.fallbackAtEnd = fallback != 0;
    TMVS_LOG("settings: loaded (preset %s, enabled %d)", s.preset.c_str(), s.enabled ? 1 : 0);
}

void save() {
    const Settings& s = g_settings;
    writeInt(L"Enabled", s.enabled ? 1 : 0);
    writeString(L"Preset", s.preset);
    writeFloat(L"Exposure", s.exposure);
    writeFloat(L"Contrast", s.contrast);
    writeFloat(L"Saturation", s.saturation);
    writeFloat(L"Vibrance", s.vibrance);
    writeFloat(L"Warmth", s.warmth);
    writeFloat(L"Tint", s.tint);
    writeFloat(L"Shadows", s.shadows);
    writeFloat(L"Highlights", s.highlights);
    writeFloat(L"Clarity", s.clarity);
    writeFloat(L"Sharpen", s.sharpen);
    writeFloat(L"Vignette", s.vignette);
    writeString(L"Lut", s.lut);
    writeFloat(L"LutMix", s.lutMix);
    writeInt(L"Placement", static_cast<int>(s.placement));
    writeInt(L"ToneCurve", static_cast<int>(s.toneCurve));
    writeFloat(L"ToneKnee", s.toneKnee);
    writeFloat(L"ToneHue", s.toneHue);
    writeFloat(L"HdrExposure", s.hdrExposure);
    writeFloat(L"Bloom", s.bloom);
    writeInt(L"FallbackAtEnd", s.fallbackAtEnd ? 1 : 0);
}

} // namespace settings
} // namespace tmvs_mp
