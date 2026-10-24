#include "misc/internal.h"
#include "config.h"
#include "inifile.h"
#include "log.h"
#include "menu.h"
#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace tmshaders {
namespace misc {
namespace {

Settings g_settings;
const wchar_t* const kSection = L"Misc";

float iniFloat(const wchar_t* ini, const wchar_t* key, float fallback, float lo, float hi) {
    wchar_t text[32] = {};
    if (!GetPrivateProfileStringW(kSection, key, L"", text, 32, ini)) return fallback;
    return std::clamp(static_cast<float>(_wtof(text)), lo, hi);
}

int iniInt(const wchar_t* ini, const wchar_t* key, int fallback, int lo, int hi) {
    return std::clamp(static_cast<int>(GetPrivateProfileIntW(kSection, key, fallback, ini)), lo, hi);
}

void setFloat(IniFile& ini, const wchar_t* key, float value) {
    wchar_t text[32];
    swprintf(text, 32, L"%.3g", value);
    ini.set(kSection, key, text);
}

void setInt(IniFile& ini, const wchar_t* key, int value) {
    wchar_t text[16];
    swprintf(text, 16, L"%d", value);
    ini.set(kSection, key, text);
}

} // namespace

Settings& settings() {
    return g_settings;
}

void settingsChanged() {
    Config::get().markDirty();
}

std::wstring dataFolder(const wchar_t* sub) {
    const std::wstring folder = log::dataDir() + L"\\" + sub;
    CreateDirectoryW(folder.c_str(), nullptr);
    return folder;
}

std::string utf8(const std::wstring& text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring wide(const std::string& text) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], n);
    return out;
}

void boot() {
    const std::wstring path = log::dataDir() + L"\\settings.ini";
    const wchar_t* ini = path.c_str();
    Settings& s = g_settings;
    s.minimap = iniInt(ini, L"Minimap", 0, 0, 1) != 0;
    s.mapX = iniFloat(ini, L"MinimapX", -1.0f, -1.0f, 16384.0f);
    s.mapY = iniFloat(ini, L"MinimapY", -1.0f, -1.0f, 16384.0f);
    s.mapSize = iniFloat(ini, L"MinimapSize", 260.0f, 120.0f, 1200.0f);
    s.mapOpacity = iniFloat(ini, L"MinimapOpacity", 0.85f, 0.2f, 1.0f);
    s.mapZoom = iniFloat(ini, L"MinimapZoom", 1.0f, 1.0f, 8.0f);
    s.mapRotate = iniInt(ini, L"MinimapRotate", 0, 0, 1) != 0;
    s.splits = iniInt(ini, L"SplitTimes", 0, 0, 1) != 0;
    s.splitX = iniFloat(ini, L"SplitTimesX", -1.0f, -1.0f, 16384.0f);
    s.splitY = iniFloat(ini, L"SplitTimesY", -1.0f, -1.0f, 16384.0f);
    s.splitReference = iniInt(ini, L"SplitReference", 0, 0, 1);
    s.splitBar = iniInt(ini, L"SplitBar", 1, 0, 1) != 0;
    s.finishEffect = iniInt(ini, L"FinishEffect", 0, 0, 2);
    Config::get().saveExtra = &save;
    TMVS_LOG("misc: minimap %s, split times %s, finish effect %d", s.minimap ? "on" : "off", s.splits ? "on" : "off", s.finishEffect);
}

void save(IniFile& ini) {
    const Settings& s = g_settings;
    setInt(ini, L"Minimap", s.minimap ? 1 : 0);
    setFloat(ini, L"MinimapX", s.mapX);
    setFloat(ini, L"MinimapY", s.mapY);
    setFloat(ini, L"MinimapSize", s.mapSize);
    setFloat(ini, L"MinimapOpacity", s.mapOpacity);
    setFloat(ini, L"MinimapZoom", s.mapZoom);
    setInt(ini, L"MinimapRotate", s.mapRotate ? 1 : 0);
    setInt(ini, L"SplitTimes", s.splits ? 1 : 0);
    setFloat(ini, L"SplitTimesX", s.splitX);
    setFloat(ini, L"SplitTimesY", s.splitY);
    setInt(ini, L"SplitReference", s.splitReference);
    setInt(ini, L"SplitBar", s.splitBar ? 1 : 0);
    setInt(ini, L"FinishEffect", s.finishEffect);
}

void drawPage() {
    Settings& s = g_settings;
    bool changed = false;
    ImGui::TextWrapped("Small tools for driving. They only show the track's shape and your times: no checkpoint or trigger "
                       "positions, no ghost's path. Move and resize their windows while this menu is open.");
    ImGui::Spacing();

    ImGui::SeparatorText("Minimap");
    changed |= ImGui::Checkbox("Show the minimap", &s.minimap);
    if (s.minimap) {
        changed |= ImGui::SliderFloat("Size", &s.mapSize, 120.0f, 800.0f, "%.0f px");
        changed |= ImGui::SliderFloat("Opacity", &s.mapOpacity, 0.2f, 1.0f, "%.2f");
        changed |= ImGui::SliderFloat("Zoom", &s.mapZoom, 1.0f, 8.0f, "%.1fx");
        changed |= ImGui::Checkbox("Turn with the car (heading up)", &s.mapRotate);
        mapview::drawPageInfo();
    }

    ImGui::SeparatorText("Split times");
    changed |= ImGui::Checkbox("Show split times", &s.splits);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("At each checkpoint, when the game registers it: your time against your best run or a replay.\n"
                          "After the finish: the time won or lost in each sector.");
    }
    if (s.splits) {
        changed |= ImGui::Checkbox("Sector clock under the delta", &s.splitBar);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How much of the reference's time for this sector has passed (red once over it).");
        splits::drawPage();
    }

    ImGui::SeparatorText("Finish effects");
    static const char* const kEffects[] = {"Off", "Subtle", "Big"};
    changed |= ImGui::Combo("At the finish", &s.finishEffect, kEffects, 3);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Confetti when you cross the finish, fireworks too for a new best run.\nNever in photos or the video export.");
    if (s.finishEffect > 0) {
        ImGui::SameLine();
        if (ImGui::Button("Try it")) confetti::start(s.finishEffect == 2, true);
    }
    if (changed) settingsChanged();
}

bool wantsOverlay(const Frame& frame) {
    // The race is followed also while nothing shows (the finish of a run, your best), but only for
    // the split times and the finish effect.
    if (g_settings.splits || g_settings.finishEffect > 0) splits::update();
    int time = 0;
    bool best = false;
    // A finish while hidden (photo mode, a screenshot) has no effect later.
    if (splits::takeFinish(time, best) && g_settings.finishEffect > 0 && !frame.hide) confetti::start(g_settings.finishEffect == 2, best);
    if (frame.hide) return false;
    return mapview::wanted() || splits::wanted() || confetti::active();
}

void drawOverlay(const Frame& frame) {
    if (frame.hide) return;
    if (mapview::wanted()) mapview::draw(frame);
    if (splits::wanted()) splits::draw(frame);
    confetti::draw(frame);
}

} // namespace misc
} // namespace tmshaders
