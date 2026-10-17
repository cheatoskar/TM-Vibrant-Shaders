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

extern "C" IMAGE_DOS_HEADER __ImageBase; // this DLL's base = its HMODULE
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

// Sharing the mouse with the other mod that hooks this window (100% TMX + Bingo). The property's
// value is the HMODULE of the mod that wants the mouse right now; whoever finds it taken by a mod
// that is still loaded leaves mouse messages alone.
const wchar_t* const kMouseOwner = L"TMMods.MouseOwner";
HANDLE selfHandle() {
    return reinterpret_cast<HANDLE>(&__ImageBase);
}
unsigned g_drawFrame = 0; // draw() calls: the answer below is kept for one frame
bool foreignOwnsMouse(HWND hwnd) {
    HANDLE owner = GetPropW(hwnd, kMouseOwner);
    if (!owner || owner == selfHandle()) return false;
    // Still loaded? A mod that went away with the property set must not leave us deaf. Asked on
    // every mouse message while the other mod holds the property, and GetModuleHandleExW takes the
    // loader lock: the answer holds for this frame (and at most 100 ms, should draw() stop).
    static HANDLE s_owner = nullptr;
    static unsigned s_frame = 0;
    static DWORD s_tick = 0;
    static bool s_loaded = false;
    const DWORD now = GetTickCount();
    if (owner == s_owner && s_frame == g_drawFrame && now - s_tick < 100) return s_loaded;
    HMODULE module = nullptr;
    s_loaded = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(owner), &module) != 0;
    s_owner = owner;
    s_frame = g_drawFrame;
    s_tick = now;
    return s_loaded;
}
// Z-order between the two mods' overlays: the one that drew last is on top and gets the mouse when
// both menus lie under the pointer. After our ImGui render we write our HMODULE here; at the start
// of the next frame the property still holds the last writer, so "it is ours" = we were on top.
const wchar_t* const kTopOverlay = L"TMMods.TopOverlay";
bool g_weAreTop = false;
void updateTop() {
    g_weAreTop = g_hwnd && GetPropW(g_hwnd, kTopOverlay) == selfHandle();
}
void removeTop() {
    g_weAreTop = false;
    if (g_hwnd && GetPropW(g_hwnd, kTopOverlay) == selfHandle()) RemovePropW(g_hwnd, kTopOverlay);
}
// Every frame: take the property when we want the mouse and it is free (or the other mod has it but
// we are on top, so our menu is what the pointer is over), give it back when we don't want it.
void publishMouseOwner(bool want) {
    if (!g_hwnd) return;
    HANDLE owner = GetPropW(g_hwnd, kMouseOwner);
    if (want && owner != selfHandle() && (!owner || g_weAreTop || !foreignOwnsMouse(g_hwnd))) SetPropW(g_hwnd, kMouseOwner, selfHandle());
    else if (!want && owner == selfHandle()) RemovePropW(g_hwnd, kMouseOwner);
}

// Which button a mouse message is about (0 left, 1 right, 2 middle, 3/4 X1/X2), -1 for others;
// up = it is the release.
int mouseButton(UINT msg, WPARAM wparam, bool& up) {
    up = false;
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: return 0;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: return 1;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: return 2;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: return GET_XBUTTON_WPARAM(wparam) == XBUTTON2 ? 4 : 3;
    case WM_LBUTTONUP: up = true; return 0;
    case WM_RBUTTONUP: up = true; return 1;
    case WM_MBUTTONUP: up = true; return 2;
    case WM_XBUTTONUP: up = true; return GET_XBUTTON_WPARAM(wparam) == XBUTTON2 ? 4 : 3;
    }
    return -1;
}
// A press we swallowed: its release is swallowed too, and only then (a release the game never
// saw the press of is harmless, a press it saw whose release is eaten is a stuck button).
bool g_swallowedButton[5] = {};

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

// Why the menu opened or closed this frame, for the log (null: its own close button).
const char* g_menuWhy = nullptr;

// A hotkey's window message (handleHotkeys does the action when polling sees the key). One nobody
// pressed (scan code 0: dev.ini's key actions, posted) is never seen by polling: it is done from
// here. For the log: how old the message is and when polling saw the key, so a late message
// pump shows (the owner's report: in the replay viewer the menu came seconds late).
void hotkeyMessage(unsigned vk, keys::Action action, LPARAM lparam) {
    const bool posted = ((lparam >> 16) & 0xFF) == 0;
    if (posted) keys::push(action);
    static int s_lines = 0;
    if (s_lines >= 100) return;
    s_lines++;
    const DWORD now = GetTickCount();
    const long age = static_cast<long>(now - static_cast<DWORD>(GetMessageTime()));
    const DWORD polled = keys::polledAt(vk);
    char seen[64] = "not yet";
    if (polled && now - polled < 60000) snprintf(seen, sizeof(seen), "%lu ms before", now - polled);
    TMVS_LOG("keys: window message for %s (%s)%s, %ld ms old; polling saw the key %s", keys::name(keys::get(action)).c_str(),
             keys::info(action).name, posted ? " nobody pressed (done from the message)" : "", age, seen);
}

// Once a frame (draw): the hotkeys polled, the menu, effects and photo ones done here; Capture,
// Reload and Trace wait for plugin.cpp's take().
void handleHotkeys() {
    Config& config = Config::get();
    const bool typing = g_initialized && config.showOverlay && ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput;
    keys::poll(typing);
    if (keys::take(keys::Action::Menu)) {
        config.showOverlay = !config.showOverlay;
        g_menuWhy = "its key";
    }
    if (keys::take(keys::Action::Effects)) {
        config.settings.enabled = !config.settings.enabled;
        config.markDirty();
        TMVS_LOG("effects: %s (its key)", config.settings.enabled ? "on" : "off");
    }
    if (keys::take(keys::Action::PhotoSplit)) photo::toggleSplit();
    if (keys::take(keys::Action::Photo)) photo::toggle();
}


// Our part of a message: true = we take it, the game must not see it.
bool ours(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    Config& config = Config::get();
    bool releaseUp = false;
    const int button = mouseButton(msg, wparam, releaseUp);
    // The other mod has the mouse: its menu is under the pointer. Not ours, not photo mode's.
    // (A release of a press ImGui already took is still shown to it, so nothing sticks.)
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && foreignOwnsMouse(hwnd)) {
        if (ImGui::GetCurrentContext()) {
            // On top of the other mod's window we keep hovering (moves only, no buttons), so we can take
            // the mouse next frame when the pointer is over our menu. Underneath we stay out of sight.
            if (msg == WM_MOUSEMOVE && g_weAreTop) ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
            else if (msg == WM_MOUSEMOVE) ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            else if (releaseUp && ImGui::GetIO().MouseDown[button]) ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
        }
        return false;
    }
    if (keys::capturing() && !config.showOverlay) keys::cancelCapture(); // the menu was closed meanwhile
    // The menu's "Change" button waits for the next key: nothing else sees it (not the hotkeys,
    // not the menu, not the game's window procedure). Esc cancels; the game's Esc is held back
    // the way it is for closing the menu.
    if (keys::capturing() && (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_CHAR ||
                              msg == WM_SYSCHAR || msg == WM_DEADCHAR)) {
        if (wparam == VK_ESCAPE && (msg == WM_KEYDOWN || msg == WM_KEYUP)) engine::swallowEscape();
        if (keys::feed(msg, wparam, lparam)) return true;
    }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lparam & (1 << 30))) {
        // The menu, effects and photo keys: done once a frame by polling (handleHotkeys), as Capture,
        // Reload and Trace are; their messages are only kept from the game here. In the replay viewer
        // the messages came seconds late (the owner's report), polling sees the key at the next frame.
        // With a text field in the menu focused only F keys count, so typing can't trigger them.
        const bool typing = config.showOverlay && ImGui::GetIO().WantTextInput;
        const keys::Action action = keys::resolve(static_cast<unsigned>(wparam), typing);
        switch (action) {
        case keys::Action::Menu:
        case keys::Action::Effects:
        case keys::Action::PhotoSplit:
        case keys::Action::Photo:
            hotkeyMessage(static_cast<unsigned>(wparam), action, lparam);
            return true;
        default:
            break;
        }
        // Esc closes the menu first, then leaves photo mode; only then does the game get it.
        if (wparam == VK_ESCAPE && (config.showOverlay || photo::active())) {
            if (config.showOverlay) {
                config.showOverlay = false;
                g_menuWhy = "Esc";
            } else {
                photo::toggle();
            }
            engine::swallowEscape();
            return true;
        }
    }
    if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && wparam == VK_ESCAPE && GetAsyncKeyState(VK_ESCAPE) < 0 && engine::escapeSwallowed()) return true;
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
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return true;
        // Only swallow the mouse over the menu itself, so the game's own menus stay clickable.
        if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && ImGui::GetIO().WantCaptureMouse) return true;
        if ((msg == WM_KEYDOWN || msg == WM_CHAR) && ImGui::GetIO().WantCaptureKeyboard) return true;
    }
    if (!config.showOverlay && splitBarMouse(hwnd, msg, wparam, lparam)) return true;
    // Photo mode: focus click and depth of field on the wheel (the game doesn't see them).
    if (photo::handleMessage(hwnd, msg, wparam, lparam)) return true;
    return false;
}

LRESULT CALLBACK hookedWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_ACTIVATEAPP) {
        // F keys pressed in another program must not count when the game is back (polling reads
        // the keyboard, not the game's messages). For the log: device resets follow these.
        if (!wparam) keys::focusLost();
        static int s_lines = 0;
        if (s_lines < 100) {
            s_lines++;
            TMVS_LOG("window: the game %s", wparam ? "is in front again" : "lost the focus");
        }
    }
    bool up = false;
    const int button = mouseButton(msg, wparam, up);
    bool taken = ours(hwnd, msg, wparam, lparam);
    if (button >= 0) {
        // A release goes where its press went: swallowed only if the press was.
        if (up) {
            taken = g_swallowedButton[button];
            g_swallowedButton[button] = false;
        } else {
            g_swallowedButton[button] = taken;
        }
    }
    if (taken) return 0;
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
    keys::setGameWindow(g_hwnd); // hotkeys only react while the game is in front
    g_initialized = true;
}

} // namespace


void shutdown() {
    if (g_hwnd) {
        publishMouseOwner(false); // never leave the other mod deaf
        removeTop();
    }
}

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
    g_drawFrame++;
    if (!g_initialized) init(device);
    handleHotkeys(); // also without the overlay (Capture, Reload and Trace)
    if (!g_initialized) return;
    {
        // The menu opening and closing, for the log (the owner's report: it closed by itself).
        static bool s_shown = false;
        if (Config::get().showOverlay != s_shown) {
            s_shown = Config::get().showOverlay;
            static int s_lines = 0;
            if (s_lines < 200) {
                s_lines++;
                TMVS_LOG("menu: %s (%s)", s_shown ? "open" : "closed", g_menuWhy ? g_menuWhy : "its close button");
            }
        }
        g_menuWhy = nullptr;
    }
    const bool menu = Config::get().showOverlay;
    const char* photoHint = photo::status();
    if (!menu) {
        Config::get().previewPreset.clear();
        if (!*photoHint) {
            g_splitBar = false;
            publishMouseOwner(false);
            removeTop(); // nothing of ours on screen: the other mod is on top
            return;
        }
    }

    updateTop(); // the previous frame's last writer, before anything of this frame
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // Draw ImGui's cursor over the menu or the split's bar (the game hides the OS cursor), the game's elsewhere.
    // Only while we own the mouse: with both mods' menus open only one pointer is drawn.
    ImGui::GetIO().MouseDrawCursor = (menu || g_splitBar) && ImGui::GetIO().WantCaptureMouse && !foreignOwnsMouse(g_hwnd);
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
    g_splitBar = photo::splitShown() && !photo::tiling() && *photoHint; // not over a hi-res photo's tiles
    if (g_splitBar) drawSplitBar();
    ImGui::EndFrame();
    // Does one of our windows want the mouse (hovered or held)? Then the other mod stays out of it.
    publishMouseOwner((menu || g_splitBar) && ImGui::GetIO().WantCaptureMouse);
    if (SUCCEEDED(device->BeginScene())) {
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        if (ImGui::GetDrawData()->TotalVtxCount > 0) SetPropW(g_hwnd, kTopOverlay, selfHandle()); // drawn last: on top
        device->EndScene();
    }
}

} // namespace overlay
} // namespace tmshaders
