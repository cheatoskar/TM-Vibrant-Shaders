#include "overlay.h"
#include "capture.h"
#include "engine_frame.h"
#include "hooks.h"
#include "log.h"
#include "lut.h"
#include "renderer.h"
#include "settings.h"
#include "shaderdb.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <shellapi.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace tmvs_mp {
namespace overlay {
namespace {

HWND g_window = nullptr;
WNDPROC g_originalProc = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
bool g_ready = false;
std::atomic<bool> g_open{false};
std::atomic<bool> g_wantMouse{false};
std::atomic<bool> g_wantKeyboard{false};
ULONGLONG g_toastUntil = 0;
std::string g_toast;
bool g_dirty = false;
std::vector<std::string> g_luts;

// Window messages arrive on the game's window thread, ImGui runs on the render thread: queue them.
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

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (g_open && (isMouse(msg) || isKeyboard(msg) || msg == WM_SETFOCUS || msg == WM_KILLFOCUS)) {
        {
            std::lock_guard<std::mutex> lock(g_queueLock);
            if (g_queue.size() < 512) g_queue.push_back({msg, w, l});
        }
        // Keep clicks and typing in the menu away from the game (its window messages; the game may
        // also read the mouse directly).
        if ((isMouse(msg) && g_wantMouse) || (isKeyboard(msg) && g_wantKeyboard && w != VK_F7 && w != VK_F8 && w != VK_F12)) return 0;
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

// The same keys as the TMNF mod: F8 menu, F7 effects on/off, F12 capture.
void hotkeys() {
    static bool f7 = false, f8 = false, f12 = false;
    if (GetForegroundWindow() != g_window) return;
    Settings& s = settings::current();
    if (pressed(VK_F7, f7)) {
        s.enabled = !s.enabled;
        g_dirty = true;
        toast(s.enabled ? "TM Vibrant Shaders: on" : "TM Vibrant Shaders: off (the game's own image)");
    }
    if (pressed(VK_F12, f12)) {
        capture::request();
        toast("Frame captured (Documents\\ManiaPlanet\\TMVS\\captures)");
    }
    if (pressed(VK_F8, f8)) {
        g_open = !g_open;
        if (!g_open && g_dirty) {
            settings::save();
            g_dirty = false;
        }
        if (g_open) g_luts = tmshaders::lut::list();
    }
}

const char* placedText(hooks::Placed p) {
    switch (p) {
    case hooks::Placed::BeforeHud: return "before the interface";
    case hooks::Placed::EndOfFrame: return "at the end of the frame (interface included)";
    case hooks::Placed::Fallback: return "at the end of the frame: interface start not found";
    default: return "nowhere";
    }
}

bool slider(const char* label, float* v, float lo, float hi, const char* fmt = "%.2f") {
    const bool changed = ImGui::SliderFloat(label, v, lo, hi, fmt);
    if (changed) g_dirty = true;
    return changed;
}

void status() {
    const hooks::Stats& st = hooks::stats();
    const ImVec4 good(0.45f, 0.85f, 0.45f, 1), warn(0.95f, 0.75f, 0.3f, 1), bad(0.95f, 0.4f, 0.35f, 1);
    if (st.toneMapSeen && st.toneMapped) ImGui::TextColored(good, "Scene found; HDR stage replaces the engine's tone map");
    else if (st.toneMapSeen) ImGui::TextColored(good, "Scene found (the engine's tone map)");
    else ImGui::TextColored(warn, "Scene not found in this frame (menu or unknown shaders)");
    const ImVec4 placedColour = st.placed == hooks::Placed::BeforeHud || st.placed == hooks::Placed::EndOfFrame ? good
                                : st.placed == hooks::Placed::Fallback                                         ? warn
                                                                                                              : bad;
    ImGui::TextColored(placedColour, "Effects applied %s", placedText(st.placed));
    if (st.placed == hooks::Placed::BeforeHud && st.hudShader) ImGui::TextDisabled("  interface starts with %s", st.hudShader);
    if (st.depthFound) ImGui::TextColored(good, "Depth found (format %d)", static_cast<int>(st.depthFormat));
    else ImGui::TextDisabled("Depth not found yet");
    if (st.cameraFound) ImGui::TextColored(good, "Camera found (eye %.0f %.0f %.0f)", st.eye[0], st.eye[1], st.eye[2]);
    else ImGui::TextColored(warn, "Camera not found in this frame");
    std::string found;
    for (int i = 0; i < kTexCount; i++) {
        if (!(st.textures & (1u << i))) continue;
        if (!found.empty()) found += ", ";
        found += engineTextureName(i);
    }
    ImGui::TextDisabled("Engine textures: %s", found.empty() ? "none" : found.c_str());
    ImGui::TextDisabled("Shaders recognised: %u of %u created, %u draws, %ux%u", st.shadersKnown, st.shadersCreated,
                        st.drawsLastFrame, st.width, st.height);
    if (st.deferredContexts) ImGui::TextColored(warn, "The game records on %u deferred contexts", st.deferredContexts);
}

void menu() {
    Settings& s = settings::current();
    ImGui::SetNextWindowPos(ImVec2(30, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(470, 0), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin("TM Vibrant Shaders for ManiaPlanet " TMVS_MP_VERSION, &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    if (ImGui::Checkbox("Effects on (F7)", &s.enabled)) g_dirty = true;
    ImGui::SameLine();
    int view = static_cast<int>(s.view);
    ImGui::SetNextItemWidth(150);
    if (ImGui::Combo("##view", &view,
                     "Normal view\0Compare (split)\0Check: depth\0Check: normals\0Check: motion\0Check: albedo\0"
                     "Check: world grid\0"))
        s.view = static_cast<View>(view);
    if (s.view == View::Split) slider("Split", &s.split, 0.0f, 1.0f);

    if (ImGui::BeginCombo("Preset", s.preset.c_str())) {
        for (const auto& p : settings::presets()) {
            if (ImGui::Selectable(p.name, s.preset == p.name)) {
                settings::applyPreset(s, p.name);
                g_dirty = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.description);
        }
        ImGui::EndCombo();
    }

    if (ImGui::CollapsingHeader("Tone mapping and bloom", ImGuiTreeNodeFlags_DefaultOpen)) {
        int curve = static_cast<int>(s.toneCurve);
        if (ImGui::Combo("Curve", &curve, "Engine (the game as it is)\0TMVS (colours keep their hue)\0ACES (film)\0")) {
            s.toneCurve = static_cast<ToneCurve>(curve);
            g_dirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How the engine's HDR image becomes the picture. TMVS and ACES keep the game's mid-tones;\n"
                              "bright lights and sky keep colour and detail instead of washing out.");
        if (s.toneCurve == ToneCurve::Tmvs) {
            slider("Shoulder start", &s.toneKnee, 0.3f, 0.9f);
            slider("Hue in highlights", &s.toneHue, 0.0f, 1.0f);
        }
        slider("HDR exposure", &s.hdrExposure, -2.0f, 2.0f, "%.2f stops");
        slider("Bloom", &s.bloom, 0.0f, 0.6f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The mod's bloom on the HDR image (the engine's own bloom stays).");
    }
    if (ImGui::CollapsingHeader("Colour and light", ImGuiTreeNodeFlags_DefaultOpen)) {
        slider("Exposure", &s.exposure, -1.0f, 1.0f, "%.2f stops");
        slider("Contrast", &s.contrast, 0.0f, 1.0f);
        slider("Saturation", &s.saturation, 0.0f, 2.0f);
        slider("Vibrance", &s.vibrance, 0.0f, 1.0f);
        slider("Warmth", &s.warmth, -1.0f, 1.0f);
        slider("Tint", &s.tint, -1.0f, 1.0f);
        slider("Shadows", &s.shadows, -1.0f, 1.0f);
        slider("Highlights", &s.highlights, 0.0f, 1.0f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pulls bright areas down so they keep detail; white stays white.");
    }
    if (ImGui::CollapsingHeader("Detail", ImGuiTreeNodeFlags_DefaultOpen)) {
        slider("Clarity", &s.clarity, 0.0f, 1.0f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Local contrast: shapes and depth stand out more.");
        slider("Sharpen", &s.sharpen, 0.0f, 1.0f);
        slider("Vignette", &s.vignette, 0.0f, 1.0f);
    }
    if (ImGui::CollapsingHeader("Look (LUT)")) {
        if (ImGui::BeginCombo("Look", s.lut.empty() ? "None" : s.lut.c_str())) {
            if (ImGui::Selectable("None", s.lut.empty())) {
                s.lut.clear();
                g_dirty = true;
            }
            for (const auto& name : g_luts) {
                if (ImGui::Selectable(name.c_str(), s.lut == name)) {
                    s.lut = name;
                    g_dirty = true;
                }
            }
            ImGui::EndCombo();
        }
        if (!s.lut.empty()) slider("Look strength", &s.lutMix, 0.0f, 1.0f);
        ImGui::TextDisabled(".cube files in Documents\\ManiaPlanet\\TMVS\\luts");
    }
    if (ImGui::CollapsingHeader("Placement and diagnostics")) {
        int placement = static_cast<int>(s.placement);
        if (ImGui::RadioButton("Before the interface", &placement, 0)) g_dirty = true;
        ImGui::SameLine();
        if (ImGui::RadioButton("End of frame (interface too)", &placement, 1)) g_dirty = true;
        s.placement = static_cast<Placement>(placement);
        if (ImGui::Checkbox("End of frame when the interface start isn't found", &s.fallbackAtEnd)) g_dirty = true;
        if (ImGui::Button("Capture this frame (F12)")) capture::request();
        if (!capture::lastFolder().empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Open folder")) ShellExecuteW(nullptr, L"open", capture::lastFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::TextDisabled("Log and captures: Documents\\ManiaPlanet\\TMVS");
    }
    ImGui::Separator();
    status();
    ImGui::End();
    if (!open) {
        g_open = false;
        if (g_dirty) {
            settings::save();
            g_dirty = false;
        }
    }
}

} // namespace

void init(HWND window, ID3D11Device* device, ID3D11DeviceContext* ctx) {
    if (g_ready || !window) return;
    g_window = window;
    g_ctx = ctx;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    char fonts[MAX_PATH] = {};
    GetWindowsDirectoryA(fonts, MAX_PATH);
    const std::string font = std::string(fonts) + "\\Fonts\\segoeui.ttf";
    if (GetFileAttributesA(font.c_str()) != INVALID_FILE_ATTRIBUTES) io.Fonts->AddFontFromFileTTF(font.c_str(), 18.0f);
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowPadding = ImVec2(14, 12);
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(device, ctx);
    g_originalProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&windowProc)));
    g_ready = true;
    toast("TM Vibrant Shaders " TMVS_MP_VERSION " is running - F8 menu, F7 on/off", 8000);
    TMVS_LOG("overlay: ready on window %p", static_cast<void*>(window));
}

void frame(ID3D11RenderTargetView* backBuffer) {
    if (!g_ready || !backBuffer) return;
    hotkeys();
    const bool showToast = GetTickCount64() < g_toastUntil;
    if (!g_open && !showToast) return;

    std::vector<Message> messages;
    {
        std::lock_guard<std::mutex> lock(g_queueLock);
        messages.swap(g_queue);
    }
    for (const auto& m : messages) ImGui_ImplWin32_WndProcHandler(g_window, m.msg, m.w, m.l);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = g_open;
    ImGui::NewFrame();
    if (g_open) menu();
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
