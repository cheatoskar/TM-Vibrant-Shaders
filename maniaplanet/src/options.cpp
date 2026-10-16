#include "options.h"
#include "config.h"
#include "log.h"
#include "photo11.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tmvs_mp {
namespace options {
namespace {

Options g_options;

std::wstring file() {
    return tmshaders::log::dataDir() + L"\\maniaplanet.ini";
}

} // namespace

Options& current() {
    return g_options;
}

void load() {
    const std::wstring ini = file();
    Options& o = g_options;
    o.placement = GetPrivateProfileIntW(L"ManiaPlanet", L"Placement", 0, ini.c_str()) == 1 ? Placement::EndOfFrame
                                                                                          : Placement::BeforeHud;
    o.fallbackAtEnd = GetPrivateProfileIntW(L"ManiaPlanet", L"FallbackAtEnd", 1, ini.c_str()) != 0;
    o.hdrStage = GetPrivateProfileIntW(L"ManiaPlanet", L"HdrStage", 1, ini.c_str()) != 0;
    o.engineLightDraws = GetPrivateProfileIntW(L"ManiaPlanet", L"EngineLightDraws", 0, ini.c_str()) != 0;
}

void save() {
    const std::wstring ini = file();
    const Options& o = g_options;
    WritePrivateProfileStringW(L"ManiaPlanet", L"Placement", o.placement == Placement::EndOfFrame ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"ManiaPlanet", L"FallbackAtEnd", o.fallbackAtEnd ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"ManiaPlanet", L"HdrStage", o.hdrStage ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"ManiaPlanet", L"EngineLightDraws", o.engineLightDraws ? L"1" : L"0", ini.c_str());
}

} // namespace options

bool setValue(const std::string& key, const std::string& value) {
    using namespace tmshaders;
    Options& o = options::current();
    Config& config = Config::get();
    Settings& s = config.settings;
    const char* k = key.c_str();
    if (!_stricmp(k, "Placement")) o.placement = atoi(value.c_str()) == 1 ? Placement::EndOfFrame : Placement::BeforeHud;
    else if (!_stricmp(k, "FallbackAtEnd")) o.fallbackAtEnd = atoi(value.c_str()) != 0;
    else if (!_stricmp(k, "HdrStage")) o.hdrStage = atoi(value.c_str()) != 0;
    else if (!_stricmp(k, "EngineLightDraws")) o.engineLightDraws = atoi(value.c_str()) != 0;
    else if (!_stricmp(k, "View")) o.view = static_cast<View>(atoi(value.c_str()));
    else if (!_stricmp(k, "Split")) o.split = static_cast<float>(atof(value.c_str()));
    else if (!_stricmp(k, "Enabled")) s.enabled = atoi(value.c_str()) != 0;
    else if (!_stricmp(k, "ShowMenu")) config.showOverlay = atoi(value.c_str()) != 0; // F8, for the test
    else if (!_stricmp(k, "MenuPage")) config.menuPage = atoi(value.c_str());
    else if (!_stricmp(k, "PhotoMode")) { // F6, for the test
        if ((atoi(value.c_str()) != 0) != photo::active()) photo::toggle();
    }
    else if (!_stricmp(k, "PhotoShot")) photo::request(); // F12 in photo mode, for the test
    else if (!_stricmp(k, "Preset")) return config.selectPreset(value);
    else if (!_stricmp(k, "Lut")) config.lutFile = value;
    else if (!_stricmp(k, "LutAmount")) config.lutAmount = static_cast<float>(atof(value.c_str()));
    else {
        for (const Field& f : fields()) {
            if (_stricmp(k, f.key)) continue;
            char* p = reinterpret_cast<char*>(&s) + f.offset;
            switch (f.kind) {
                case Field::Float: *reinterpret_cast<float*>(p) = static_cast<float>(atof(value.c_str())); break;
                case Field::Int: *reinterpret_cast<int*>(p) = atoi(value.c_str()); break;
                case Field::Bool: *reinterpret_cast<bool*>(p) = atoi(value.c_str()) != 0; break;
                case Field::Color: {
                    float* c = reinterpret_cast<float*>(p);
                    if (sscanf(value.c_str(), "%f,%f,%f", &c[0], &c[1], &c[2]) != 3) return false;
                    break;
                }
            }
            config.preset = kCustomPreset;
            return true;
        }
        return false;
    }
    return true;
}

} // namespace tmvs_mp
