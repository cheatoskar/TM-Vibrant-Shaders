// The F8 menu in ManiaPlanet: the TMNF mod's menu (src/menu.cpp) on ImGui's D3D11 backend.
// Window messages arrive on the game's window thread, ImGui runs on the render thread: they are
// queued and handed over at Present.
#include "overlay.h"
#include "capture.h"
#include "config.h"
#include "engine_frame.h"
#include "hooks.h"
#include "log.h"
#include "menu.h"
#include "options.h"
#include "renderer.h"
#include "shaderdb.h"
#include "thumbnails11.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <shellapi.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace tmvs_mp {
namespace overlay {
namespace {

using tmshaders::Config;

HWND g_window = nullptr;
WNDPROC g_originalProc = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
bool g_ready = false;
std::atomic<bool> g_open{false};
std::atomic<bool> g_wantMouse{false};
std::atomic<bool> g_wantKeyboard{false};
ULONGLONG g_toastUntil = 0;
std::string g_toast;

struct Message {
    UINT msg;
    WPARAM w;
    LPARAM l;
};
std::mutex g_queueLock;
std::vector<Message> g_queue;

bool isMouse(UINT msg) {
    return msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;
}

bool isKeyboard(UINT msg) {
    return (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || msg == WM_CHAR;
}

bool isHotkey(WPARAM w) {
    return w == VK_F7 || w == VK_F8 || w == VK_F12;
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (g_open && (isMouse(msg) || isKeyboard(msg) || msg == WM_SETFOCUS || msg == WM_KILLFOCUS)) {
        {
            std::lock_guard<std::mutex> lock(g_queueLock);
            if (g_queue.size() < 512) g_queue.push_back({msg, w, l});
        }
        // Keep clicks and typing in the menu away from the game (its window messages; the game may
        // also read the mouse directly).
        if ((isMouse(msg) && g_wantMouse) || (isKeyboard(msg) && g_wantKeyboard && !isHotkey(w))) return 0;
    }
    return CallWindowProcW(g_originalProc, hwnd, msg, w, l);
}

void toast(const std::string& text, ULONGLONG ms = 3000) {
    g_toast = text;
    g_toastUntil = GetTickCount64() + ms;
}

bool pressed(int key, bool& was) {
    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool edge = down && !was;
    was = down;
    return edge;
}

// The same keys as in TMNF: F8 menu, F7 effects on/off, F12 capture.
void hotkeys() {
    static bool f7 = false, f8 = false, f12 = false;
    if (GetForegroundWindow() != g_window) return;
    Config& config = Config::get();
    if (pressed(VK_F7, f7)) {
        config.settings.enabled = !config.settings.enabled;
        config.markDirty();
        toast(config.settings.enabled ? "TM Vibrant Shaders: on" : "TM Vibrant Shaders: off (the game's own image)");
    }
    if (pressed(VK_F12, f12)) {
        capture::request();
        toast("Frame captured (Documents\\ManiaPlanet\\TMVS\\captures)");
    }
    if (pressed(VK_F8, f8)) config.showOverlay = !config.showOverlay;
}

// What the ManiaPlanet build can do so far; the menu shows only these.
bool supports(const tmshaders::Field& f) {
    static const char* const kKeys[] = {"Exposure",  "Contrast", "ToneKnee", "ToneCurve",   "Saturation",          "Vibrance",
                                        "Temperature", "Tint",   "Lift",     "Gamma",       "Gain",                "SplitToning",
                                        "Bloom",     "BloomRadius", "Vignette", "FilmGrain", "ChromaticAberration", "Sharpen"};
    for (const char* key : kKeys)
        if (!strcmp(f.key, key)) return true;
    return false;
}

const char* placedText(hooks::Placed p) {
    switch (p) {
    case hooks::Placed::BeforeHud: return "before the interface";
    case hooks::Placed::EndOfFrame: return "at the end of the frame (interface included)";
    case hooks::Placed::Fallback: return "at the end of the frame: interface start not found";
    default: return "nowhere";
    }
}

// ManiaPlanet's own options and what the mod found in the engine (Settings page).
void gamePanel() {
    Options& o = options::current();
    const hooks::Stats& st = hooks::stats();
    const ImVec4 good(0.45f, 0.85f, 0.45f, 1), warn(0.95f, 0.75f, 0.3f, 1), bad(0.95f, 0.4f, 0.35f, 1);
    bool changed = ImGui::Checkbox("HDR stage (the look on the engine's HDR image)", &o.hdrStage);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("On: the mod replaces the engine's tone map; bright lights and sky keep colour and detail.\n"
                          "Off: the engine's tone map stays and the look goes on its finished image.");
    int placement = static_cast<int>(o.placement);
    changed |= ImGui::RadioButton("Before the interface", &placement, 0);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("End of frame (interface too)", &placement, 1);
    o.placement = static_cast<Placement>(placement);
    changed |= ImGui::Checkbox("End of frame when the interface start isn't found", &o.fallbackAtEnd);
    if (changed) options::save();
    int view = static_cast<int>(o.view);
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
    if (ImGui::Combo("View", &view,
                     "The mod\0Compare with the game (split)\0Check: depth\0Check: normals\0Check: motion\0Check: albedo\0"
                     "Check: world grid\0"))
        o.view = static_cast<View>(view);
    if (o.view == View::Split) {
        ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
        ImGui::SliderFloat("Split", &o.split, 0.0f, 1.0f);
    }
    if (ImGui::Button("Capture this frame (F12)")) capture::request();
    if (!capture::lastFolder().empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Open capture")) ShellExecuteW(nullptr, L"open", capture::lastFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::Separator();
    if (st.toneMapSeen && st.toneMapped) ImGui::TextColored(good, "HDR stage replaces the engine's tone map");
    else if (st.toneMapSeen) ImGui::TextColored(good, "Scene found (the engine's tone map)");
    else ImGui::TextColored(warn, "No scene in this frame (menu)");
    const ImVec4 placedColour = st.placed == hooks::Placed::BeforeHud || st.placed == hooks::Placed::EndOfFrame ? good
                                : st.placed == hooks::Placed::Fallback                                         ? warn
                                                                                                              : bad;
    ImGui::TextColored(placedColour, "Finish %s", placedText(st.placed));
    if (st.cameraFound) ImGui::TextColored(good, "Camera found (eye %.0f %.0f %.0f)", st.eye[0], st.eye[1], st.eye[2]);
    else ImGui::TextDisabled("Camera not found in this frame");
    std::string found;
    for (int i = 0; i < kTexCount; i++) {
        if (!(st.textures & (1u << i))) continue;
        if (!found.empty()) found += ", ";
        found += engineTextureName(i);
    }
    ImGui::TextDisabled("Engine textures: %s", found.empty() ? "none" : found.c_str());
    ImGui::TextDisabled("Shaders recognised: %u of %u, %u draws, %ux%u", st.shadersKnown, st.shadersCreated, st.drawsLastFrame,
                        st.width, st.height);
}

} // namespace

void init(HWND window, ID3D11Device* device, ID3D11DeviceContext* ctx) {
    if (g_ready || !window) return;
    g_window = window;
    g_device = device;
    g_ctx = ctx;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    tmshaders::menu::setupStyle(window);
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(device, ctx);
    g_originalProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&windowProc)));
    g_ready = true;
    toast("TM Vibrant Shaders " TMVS_MP_VERSION " is running - F8 menu, F7 on/off", 8000);
    TMVS_LOG("overlay: ready on window %p", static_cast<void*>(window));
}

void frame(ID3D11RenderTargetView* backBuffer) {
    if (!g_ready || !backBuffer) return;
    Config& config = Config::get();
    config.tick(); // saves settings.ini a moment after a change
    hotkeys();
    g_open = config.showOverlay;
    const bool showToast = GetTickCount64() < g_toastUntil;
    if (!config.showOverlay) config.previewPreset.clear();
    if (!config.showOverlay && !showToast) return;

    std::vector<Message> messages;
    {
        std::lock_guard<std::mutex> lock(g_queueLock);
        messages.swap(g_queue);
    }
    for (const auto& m : messages) ImGui_ImplWin32_WndProcHandler(g_window, m.msg, m.w, m.l);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    // ImGui's pointer over the menu (the game hides the OS cursor), the game's elsewhere.
    io.MouseDrawCursor = config.showOverlay && io.WantCaptureMouse;
    ImGui::NewFrame();
    if (config.showOverlay) {
        static tmshaders::menu::Host host = [] {
            tmshaders::menu::Host h;
            h.picture = [](const std::string& preset) {
                return reinterpret_cast<ImTextureID>(thumbnails::get(g_device, preset));
            };
            h.supports = supports;
            h.gamePanelTitle = "ManiaPlanet";
            h.gamePanel = gamePanel;
            return h;
        }();
        tmshaders::menu::draw(host);
    }
    if (showToast) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 40), ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::SetNextWindowBgAlpha(0.8f);
        ImGui::Begin("##toast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextUnformatted(g_toast.c_str());
        ImGui::End();
    }
    ImGui::Render();
    g_wantMouse = io.WantCaptureMouse;
    g_wantKeyboard = io.WantCaptureKeyboard;

    StateBackup backup;
    backup.save(g_ctx);
    g_ctx->OMSetRenderTargets(1, &backBuffer, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    backup.restore(g_ctx);
}

} // namespace overlay
} // namespace tmvs_mp
