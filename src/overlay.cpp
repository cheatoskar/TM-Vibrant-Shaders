#include "overlay.h"
#include "menu.h"
#include "autoquality.h"
#include "config.h"
#include "engine.h"
#include "keys.h"
#include "pipeline.h"
#include "log.h"
#include "lut.h"
#include "photo.h"
#include "thumbnails.h"
#include "update.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "tm_shaders_version.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace tmshaders {
namespace overlay {
namespace {

bool g_initialized = false;
HWND g_hwnd = nullptr;
WNDPROC g_originalWndProc = nullptr;
Status g_status;
// The compare split's bar at the bottom (the two looks, the other side switchable) was drawn last
// frame: it takes the mouse over it, also with the menu closed.
bool g_splitBar = false;

// The mouse for the split's bar with the menu closed: moves always (hover), presses only over it
// (or while ImGui holds one: its release), so the game keeps every other click and its mouse
// capture (the backend takes and releases the capture with each press).
bool splitBarMouse(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!g_splitBar || msg < WM_MOUSEFIRST || msg > WM_MOUSELAST || !ImGui::GetCurrentContext()) return false;
    ImGuiIO& io = ImGui::GetIO();
    const bool held = io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2];
    if (msg != WM_MOUSEMOVE && !io.WantCaptureMouse && !held) return false;
    ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
    return msg != WM_MOUSEMOVE;
}


LRESULT CALLBACK hookedWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    Config& config = Config::get();
    if (keys::capturing() && !config.showOverlay) keys::cancelCapture(); // the menu was closed meanwhile
    // The menu's "Change" button waits for the next key: nothing else sees it (not the hotkeys,
    // not the menu, not the game's window procedure). Esc cancels; the game's Esc is held back
    // the way it is for closing the menu.
    if (keys::capturing() && (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_CHAR ||
                              msg == WM_SYSCHAR || msg == WM_DEADCHAR)) {
        if (wparam == VK_ESCAPE && (msg == WM_KEYDOWN || msg == WM_KEYUP)) engine::swallowEscape();
        if (keys::feed(msg, wparam, lparam)) return 0;
    }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lparam & (1 << 30))) {
        // The hotkeys that need no per-frame look (Capture, Reload and Trace are polled in plugin.cpp).
        // With a text field in the menu focused only F keys count, so typing can't trigger them.
        const bool typing = config.showOverlay && ImGui::GetIO().WantTextInput;
        switch (keys::resolve(static_cast<unsigned>(wparam), typing)) {
        case keys::Action::Menu:
            config.showOverlay = !config.showOverlay;
            return 0;
        case keys::Action::Effects:
            config.settings.enabled = !config.settings.enabled;
            config.markDirty();
            return 0;
        case keys::Action::PhotoSplit:
            photo::toggleSplit();
            return 0;
        case keys::Action::Photo:
            photo::toggle();
            return 0;
        default:
            break;
        }
        // Esc closes the menu first, then leaves photo mode; only then does the game get it.
        if (wparam == VK_ESCAPE && (config.showOverlay || photo::active())) {
            if (config.showOverlay) config.showOverlay = false;
            else photo::toggle();
            engine::swallowEscape();
            return 0;
        }
    }
    if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && wparam == VK_ESCAPE && GetAsyncKeyState(VK_ESCAPE) < 0 && engine::escapeSwallowed()) return 0;
    if (config.showOverlay) {
        // The game reads its keys itself and never turns key presses into characters, so the
        // menu's text fields stayed empty: while one has the focus, make the WM_CHAR here
        // (unless the game's own loop already did).
        if (msg == WM_KEYDOWN && ImGui::GetIO().WantTextInput) {
            MSG queued;
            if (!PeekMessageW(&queued, hwnd, WM_CHAR, WM_CHAR, PM_NOREMOVE)) {
                MSG key{hwnd, msg, wparam, lparam, 0, {}};
                TranslateMessage(&key);
            }
        }
        if (msg == WM_MOUSEWHEEL) {
            // Owner report: the wheel didn't scroll the menu. Once: does it get here at all?
            static bool logged = false;
            if (!logged) {
                logged = true;
                const ImGuiWindow* hovered = ImGui::GetCurrentContext() ? ImGui::GetCurrentContext()->HoveredWindow : nullptr;
                TMVS_LOG("menu: mouse wheel %d over %s", static_cast<int>(GET_WHEEL_DELTA_WPARAM(wparam)), hovered ? hovered->Name : "nothing");
            }
        }
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return 1;
        // Only swallow the mouse over the menu itself, so the game's own menus stay clickable.
        if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && ImGui::GetIO().WantCaptureMouse) return 1;
        if ((msg == WM_KEYDOWN || msg == WM_CHAR) && ImGui::GetIO().WantCaptureKeyboard) return 1;
    }
    if (!config.showOverlay && splitBarMouse(hwnd, msg, wparam, lparam)) return 0;
    // Photo mode: focus click and depth of field on the wheel (the game doesn't see them).
    if (photo::handleMessage(hwnd, msg, wparam, lparam)) return 0;
    return CallWindowProcW(g_originalWndProc, hwnd, msg, wparam, lparam);
}

void init(IDirect3DDevice9* device) {
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (FAILED(device->GetCreationParameters(&cp))) return;
    g_hwnd = cp.hFocusWindow;
    if (!g_hwnd) return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    // The game drives its own (hardware) cursor and hides it in many states, so the menu
    // draws its own pointer and never touches the OS cursor.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    menu::setupStyle(g_hwnd);

    // Is there a newer version on GitHub? (Background; the answer shows in the menu.)
    if (Config::get().checkUpdates) update::check();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX9_Init(device);
    g_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hookedWndProc)));
    g_initialized = true;
}

} // namespace


void preReset() {
    if (g_initialized) ImGui_ImplDX9_InvalidateDeviceObjects();
}

void postReset() {
    if (g_initialized) ImGui_ImplDX9_CreateDeviceObjects();
}

void setStatus(const Status& status) {
    g_status = status;
}

bool consumeReloadRequest() {
    return menu::consumeReloadRequest();
}

// The compare split's bar: your look's name on its side of the line, the other side's (the game's
// image or a preset) as a list on the other. A line above the list when the frame, lit for your
// look, can't show the preset's light (photo::splitLightNote).
void drawSplitBar() {
    Config& config = Config::get();
    std::string& other = photo::splitPreset();
    const float scale = menu::scale();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float x = photo::splitPosition() * size.x, y = size.y - 24.0f * scale, pad = 8.0f * scale;
    const bool mineLeft = photo::splitSwapped();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
    const char* kGame = "The game's image";
    // Your side.
    ImGui::SetNextWindowPos(ImVec2(mineLeft ? x - pad : x + pad, y), ImGuiCond_Always, ImVec2(mineLeft ? 1.0f : 0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##splitMine", nullptr, flags | ImGuiWindowFlags_NoInputs);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(config.preset.empty() ? "Your look" : config.preset.c_str());
    ImGui::End();
    // The other side.
    const std::vector<std::string> names = config.presetNames();
    float widest = ImGui::CalcTextSize(kGame).x;
    for (const std::string& name : names) widest = std::max(widest, ImGui::CalcTextSize(name.c_str()).x);
    const std::string note = photo::splitLightNote();
    ImGui::SetNextWindowPos(ImVec2(mineLeft ? x + pad : x - pad, y), ImGuiCond_Always, ImVec2(mineLeft ? 0.0f : 1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##splitOther", nullptr, flags);
    if (!note.empty()) ImGui::TextDisabled("%s", note.c_str());
    ImGui::SetNextItemWidth(widest + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f);
    if (ImGui::BeginCombo("##splitOtherList", other.empty() ? kGame : other.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable(kGame, other.empty())) other.clear();
        for (const std::string& name : names) {
            if (ImGui::Selectable(name.c_str(), other == name)) other = name;
        }
        ImGui::EndCombo();
    }
    ImGui::End();
}

void draw(IDirect3DDevice9* device) {
    if (!g_initialized) {
        init(device);
        if (!g_initialized) return;
    }
    const bool menu = Config::get().showOverlay;
    const char* photoHint = photo::status();
    if (!menu) {
        Config::get().previewPreset.clear();
        if (!*photoHint) {
            g_splitBar = false;
            return;
        }
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // Draw ImGui's cursor over the menu or the split's bar (the game hides the OS cursor), the game's elsewhere.
    ImGui::GetIO().MouseDrawCursor = (menu || g_splitBar) && ImGui::GetIO().WantCaptureMouse;
    ImGui::NewFrame();
    if (menu) {
        // The pipeline's GPU timings for the Performance page.
        struct Timings : menu::PassTimings {
            const Pipeline* p;
            explicit Timings(const Pipeline* pipeline) : p(pipeline) {}
            int count() const override { return p->passCount(); }
            const char* name(int pass) const override { return Pipeline::passName(pass); }
            float time(int pass) const override { return p->passTime(pass); }
            float total() const override { return p->totalTime(); }
        } timings(g_status.pipeline);
        menu::Host host;
        host.depthAvailable = g_status.depthAvailable;
        host.engineHooks = g_status.engineHooks;
        host.sunKnown = g_status.sunKnown;
        memcpy(host.sunDirection, g_status.sunDirection, sizeof(host.sunDirection));
        host.shaderError = g_status.shaderError;
        host.timings = g_status.pipeline ? &timings : nullptr;
        host.picture = [device](const std::string& preset) {
            return static_cast<ImTextureID>(reinterpret_cast<intptr_t>(thumbnails::get(device, preset)));
        };
        const AutoQuality* aq = g_status.autoQuality;
        host.autoQualityReduced = [aq]() -> const char* { return aq && aq->reducedCount() ? aq->summary() : nullptr; };
        host.mapComments = [](std::string& comments) { return engine::currentMapComments(comments); };
        host.setMapComments = [](const std::string& comments) { return engine::setCurrentMapComments(comments); };
        host.fNumber = [](float dof) { return photo::fNumber(dof); };
        host.splitOn = &photo::splitOn();
        host.splitPosition = &photo::splitPosition();
        host.splitSwapped = &photo::splitSwapped();
        host.splitPreset = &photo::splitPreset();
        host.photoOn = [] {
            if (!photo::active()) photo::toggle();
        };
        host.supports = [](const Field& f) { return strcmp(f.key, "ToneCurve") != 0; }; // ManiaPlanet only
        menu::draw(host);
    }
    if (*photoHint) {
        // Photo mode line, top left. Drawn at Present: never in the photos.
        ImGui::SetNextWindowPos(ImVec2(16.0f * menu::scale(), 16.0f * menu::scale()));
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("##photo", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted(photoHint);
        ImGui::End();
    }
    // The compare split: which side is which, at the line near the bottom, the other side a list
    // to switch it without the menu. With photo mode's line on screen only (drawn at Present:
    // never in the photos).
    g_splitBar = photo::splitShown() && *photoHint;
    if (g_splitBar) drawSplitBar();
    ImGui::EndFrame();
    if (SUCCEEDED(device->BeginScene())) {
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        device->EndScene();
    }
}

} // namespace overlay
} // namespace tmshaders
