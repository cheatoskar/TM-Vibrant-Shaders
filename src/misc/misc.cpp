#include "misc/internal.h"
#include "config.h"
#include "inifile.h"
#include "log.h"
#include "menu.h"
#include <windows.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
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
    swprintf(text, 32, L"%.4g", value);
    ini.set(kSection, key, text);
}

void setInt(IniFile& ini, const wchar_t* key, int value) {
    wchar_t text[16];
    swprintf(text, 16, L"%d", value);
    ini.set(kSection, key, text);
}

// The height of a window that follows its content (WindowPlace::autoHeight).
void contentHeightOnly(ImGuiSizeCallbackData* data) {
    const WindowPlace* place = static_cast<const WindowPlace*>(data->UserData);
    if (place->contentHeight > 0.0f) data->DesiredSize.y = place->contentHeight;
}

bool apart(ImVec2 a, ImVec2 b) {
    return fabsf(a.x - b.x) > 0.5f || fabsf(a.y - b.y) > 0.5f;
}

} // namespace

Settings& settings() {
    return g_settings;
}

void settingsChanged() {
    Config::get().markDirty();
}

float screenScale() {
    // The game window's client area against 2560 x 1440: by its height, or by its width when it is
    // narrower than 16:9 (a 4:3 or a tall window), so nothing grows past the window's edges.
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    if (size.x < 100.0f || size.y < 100.0f) return 1.0f;
    return std::min(size.y / 1440.0f, size.x / 2560.0f);
}

float textScale() {
    // The menu's font is 17 px at 1080p and grows with the screen (22.7 px at 1440p).
    const float font = ImGui::GetFont() ? ImGui::GetFont()->FontSize : 0.0f;
    return font > 1.0f ? std::clamp(screenScale() * 17.0f * 1440.0f / 1080.0f / font, 0.4f, 2.0f) : 1.0f;
}

PanelStyle::PanelStyle() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float r = textScale();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x * r, style.WindowPadding.y * r));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x * r, style.ItemSpacing.y * r));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(style.CellPadding.x * r, style.CellPadding.y * r));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x * r, style.FramePadding.y * r));
}

PanelStyle::~PanelStyle() {
    ImGui::PopStyleVar(4);
}

void WindowPlace::before(const Frame& frame, ImVec2 pos, ImVec2 size) {
    autoHeight = size.y <= 0.0f;
    // Kept on the screen (another resolution, a window dragged away).
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    pos.x = std::clamp(pos.x, 0.0f, std::max(0.0f, display.x - 40.0f));
    pos.y = std::clamp(pos.y, 0.0f, std::max(0.0f, display.y - 40.0f));
    // Placed from the settings: with the menu closed, when it opens, and when they changed (the
    // page's size slider): the window's own size never overrides a setting the slider just made.
    const bool changed = apart(pos, applied) || fabsf(size.x - appliedSize.x) > 0.5f || (!autoHeight && fabsf(size.y - appliedSize.y) > 0.5f);
    if (!frame.menuOpen || !wasOpen || changed) {
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(size.x, autoHeight ? std::max(contentHeight, 1.0f) : size.y), ImGuiCond_Always);
        applied = pos;
        appliedSize = size;
    }
    if (autoHeight) ImGui::SetNextWindowSizeConstraints(ImVec2(80.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX), contentHeightOnly, this);
    else if (lockHeight) ImGui::SetNextWindowSizeConstraints(ImVec2(80.0f, size.y), ImVec2(FLT_MAX, size.y));
    wasOpen = frame.menuOpen;
}

bool WindowPlace::after(const Frame& frame, ImVec2& pos, ImVec2& size) {
    if (!frame.menuOpen || ImGui::IsMouseDown(ImGuiMouseButton_Left)) return false;
    pos = ImGui::GetWindowPos();
    size = ImGui::GetWindowSize();
    const bool resized = fabsf(size.x - appliedSize.x) > 0.5f || (!autoHeight && fabsf(size.y - appliedSize.y) > 0.5f);
    if (!apart(pos, applied) && !resized) return false;
    applied = pos;
    appliedSize = ImVec2(size.x, autoHeight ? appliedSize.y : size.y);
    return true;
}

void WindowPlace::end() {
    if (autoHeight) contentHeight = ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + ImGui::GetStyle().WindowPadding.y;
}

int windowFlags(const Frame& frame, bool resizable) {
    int flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (!frame.menuOpen) flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    else if (!resizable) flags |= ImGuiWindowFlags_NoResize;
    return flags;
}

float windowChrome(const Frame& frame, const char* title, bool resizable) {
    ImGui::SetWindowFontScale(textScale());
    if (!frame.menuOpen) return 0.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    // Sizes from the text's height, so the strip keeps its proportions on every screen.
    const float t = ImGui::GetTextLineHeight(), h = t * 1.25f, w = std::max(1.0f, t * 0.08f);
    const ImU32 strip = IM_COL32(255, 160, 60, 70), line = IM_COL32(255, 210, 160, 230);
    draw->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + h), strip, ImGui::GetStyle().WindowRounding, ImDrawFlags_RoundCornersTop);
    // The move symbol: a cross with four arrow heads.
    const float c = h * 0.5f, r = h * 0.32f, a = h * 0.12f;
    const ImVec2 m(wp.x + t * 0.35f + r, wp.y + c);
    draw->AddLine(ImVec2(m.x - r, m.y), ImVec2(m.x + r, m.y), line, w);
    draw->AddLine(ImVec2(m.x, m.y - r), ImVec2(m.x, m.y + r), line, w);
    draw->AddTriangleFilled(ImVec2(m.x - r - a, m.y), ImVec2(m.x - r + a, m.y - a), ImVec2(m.x - r + a, m.y + a), line);
    draw->AddTriangleFilled(ImVec2(m.x + r + a, m.y), ImVec2(m.x + r - a, m.y - a), ImVec2(m.x + r - a, m.y + a), line);
    draw->AddTriangleFilled(ImVec2(m.x, m.y - r - a), ImVec2(m.x - a, m.y - r + a), ImVec2(m.x + a, m.y - r + a), line);
    draw->AddTriangleFilled(ImVec2(m.x, m.y + r + a), ImVec2(m.x - a, m.y + r - a), ImVec2(m.x + a, m.y + r - a), line);
    draw->AddText(ImVec2(m.x + r + t * 0.45f, wp.y + (h - t) * 0.5f), line, title);
    if (resizable) {
        // The grip: three diagonal lines in the lower right corner.
        const ImVec2 e(wp.x + ws.x - t * 0.18f, wp.y + ws.y - t * 0.18f);
        for (int k = 1; k <= 3; k++) {
            const float d = k * t * 0.24f;
            draw->AddLine(ImVec2(e.x - d, e.y), ImVec2(e.x, e.y - d), line, w);
        }
    }
    ImGui::Dummy(ImVec2(0.0f, h - ImGui::GetStyle().WindowPadding.y));
    return h;
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
    s.mapSize = iniFloat(ini, L"MinimapSize", 260.0f, 120.0f, 800.0f);
    s.mapOpacity = iniFloat(ini, L"MinimapOpacity", 0.85f, 0.2f, 1.0f);
    s.mapZoom = iniFloat(ini, L"MinimapZoom", 1.0f, 1.0f, 8.0f);
    s.mapRotate = iniInt(ini, L"MinimapRotate", 0, 0, 1) != 0;
    s.splits = iniInt(ini, L"SplitTimes", 0, 0, 1) != 0;
    s.splitAttached = iniInt(ini, L"SplitTimesAttached", 1, 0, 1) != 0;
    s.splitX = iniFloat(ini, L"SplitTimesX", -1.0f, -1.0f, 16384.0f);
    s.splitY = iniFloat(ini, L"SplitTimesY", -1.0f, -1.0f, 16384.0f);
    s.splitWidth = iniFloat(ini, L"SplitTimesWidth", 260.0f, 160.0f, 800.0f);
    s.splitCompare = iniInt(ini, L"SplitCompare", kCompareBest, 0, 3);
    s.splitBar = iniInt(ini, L"SplitBar", 1, 0, 1) != 0;
    s.session = iniInt(ini, L"SessionPanel", 1, 0, 1) != 0;
    s.sessionAttached = iniInt(ini, L"SessionAttached", 1, 0, 1) != 0;
    s.sessionX = iniFloat(ini, L"SessionX", -1.0f, -1.0f, 16384.0f);
    s.sessionY = iniFloat(ini, L"SessionY", -1.0f, -1.0f, 16384.0f);
    s.finishEffect = iniInt(ini, L"FinishEffect", 0, 0, 2);
    Config::get().saveExtra = &save;
    TMVS_LOG("misc: minimap %s, split times %s (compare with %d), finish effect %d", s.minimap ? "on" : "off", s.splits ? "on" : "off",
             s.splitCompare, s.finishEffect);
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
    setInt(ini, L"SplitTimesAttached", s.splitAttached ? 1 : 0);
    setFloat(ini, L"SplitTimesX", s.splitX);
    setFloat(ini, L"SplitTimesY", s.splitY);
    setFloat(ini, L"SplitTimesWidth", s.splitWidth);
    setInt(ini, L"SplitCompare", s.splitCompare);
    setInt(ini, L"SplitBar", s.splitBar ? 1 : 0);
    setInt(ini, L"SessionPanel", s.session ? 1 : 0);
    setInt(ini, L"SessionAttached", s.sessionAttached ? 1 : 0);
    setFloat(ini, L"SessionX", s.sessionX);
    setFloat(ini, L"SessionY", s.sessionY);
    setInt(ini, L"FinishEffect", s.finishEffect);
}

void drawPage() {
    Settings& s = g_settings;
    bool changed = false;
    ImGui::TextWrapped("Only the track's shape and your times: no checkpoint positions, no ghost paths. "
                       "Drag and resize the windows while this menu is open.");
    ImGui::Spacing();

    ImGui::SeparatorText("Minimap");
    changed |= ImGui::Checkbox("Minimap", &s.minimap);
    if (s.minimap) {
        changed |= ImGui::SliderFloat("Size", &s.mapSize, 120.0f, 800.0f, "%.0f px");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("At 1440p; smaller screens scale it down.");
        changed |= ImGui::SliderFloat("Opacity", &s.mapOpacity, 0.2f, 1.0f, "%.2f");
        changed |= ImGui::SliderFloat("Zoom", &s.mapZoom, 1.0f, 8.0f, "%.1fx");
        changed |= ImGui::Checkbox("Rotate with car", &s.mapRotate);
        if (ImGui::Button("Reset position##map")) {
            s.mapX = s.mapY = -1.0f;
            changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Top right; attached panels follow.");
        mapview::drawPageInfo();
    }

    ImGui::SeparatorText("Split times");
    changed |= ImGui::Checkbox("Split times", &s.splits);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Your time at each checkpoint against a reference, and each sector after the finish.");
    if (s.splits) {
        if (s.splitAttached && s.minimap) {
            ImGui::TextDisabled("Width: the minimap's");
        } else {
            changed |= ImGui::SliderFloat("Width", &s.splitWidth, 160.0f, 800.0f, "%.0f px");
        }
        changed |= ImGui::Checkbox("Sector clock", &s.splitBar);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Time in the current sector against the reference's sector time.");
        if (ImGui::Checkbox("Attach to minimap", &s.splitAttached)) {
            s.splitX = s.splitY = -1.0f; // unticked: it stays where it is
            changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sits under the minimap, as wide as it, and moves with it.");
        changed |= ImGui::Checkbox("Session panel", &s.session);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Attempts, best, median and last times, theoretical best, sector speeds.");
        if (s.session) {
            if (ImGui::Checkbox("Attach to split times", &s.sessionAttached)) {
                s.sessionX = s.sessionY = -1.0f;
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sits under the split times and moves with them.");
        }
        if (ImGui::Button("Reset position##splits")) {
            s.splitAttached = s.sessionAttached = true;
            s.splitX = s.splitY = s.sessionX = s.sessionY = -1.0f;
            changed = true;
        }
        splits::drawPage();
    }

    ImGui::SeparatorText("Finish effects");
    static const char* const kEffects[] = {"Off", "Subtle", "Big"};
    changed |= ImGui::Combo("Finish effect", &s.finishEffect, kEffects, 3);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Confetti at the finish, fireworks for a new best. Never in photos or videos.");
    if (s.finishEffect > 0) {
        ImGui::SameLine();
        if (ImGui::Button("Test")) confetti::start(s.finishEffect == 2, true);
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
