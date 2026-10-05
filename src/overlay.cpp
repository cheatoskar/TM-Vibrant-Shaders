#include "overlay.h"
#include "config.h"
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "tm_shaders_version.h"
#include <cstring>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace tmshaders {
namespace overlay {
namespace {

bool g_initialized = false;
HWND g_hwnd = nullptr;
WNDPROC g_originalWndProc = nullptr;
Status g_status;
bool g_reloadRequested = false;

const char* kSkyModes[] = {"Game sky", "Clear sky + clouds", "Starry night", "Black hole", "Aurora"};
const char* kDebugViews[] = {"Final image", "Depth", "Normals", "Ambient occlusion", "Sun shadows", "Light shafts", "Bloom"};

LRESULT CALLBACK hookedWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    Config& config = Config::get();
    if (msg == WM_KEYDOWN && !(lparam & (1 << 30))) {
        if (wparam == VK_F8) {
            config.showOverlay = !config.showOverlay;
            return 0;
        }
        if (wparam == VK_F7) {
            config.settings.enabled = !config.settings.enabled;
            config.markDirty();
            return 0;
        }
    }
    if (config.showOverlay) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return 1;
        // Only swallow the mouse over the menu itself, so the game's own menus stay clickable.
        if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && ImGui::GetIO().WantCaptureMouse) return 1;
        if ((msg == WM_KEYDOWN || msg == WM_CHAR) && ImGui::GetIO().WantCaptureKeyboard) return 1;
    }
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

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.07f, 0.09f, 0.94f);
    colors[ImGuiCol_Header] = ImVec4(0.85f, 0.55f, 0.20f, 0.45f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.95f, 0.62f, 0.25f, 0.65f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.95f, 0.62f, 0.25f, 0.85f);
    colors[ImGuiCol_Button] = ImVec4(0.85f, 0.55f, 0.20f, 0.55f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.95f, 0.62f, 0.25f, 0.80f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.15f, 0.19f, 0.90f);
    colors[ImGuiCol_SliderGrab] = ImVec4(1.00f, 0.66f, 0.28f, 0.95f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 0.75f, 0.40f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(1.00f, 0.70f, 0.30f, 1.00f);

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX9_Init(device);
    g_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hookedWndProc)));
    g_initialized = true;
}

void drawMenu() {
    Config& config = Config::get();
    Settings& s = config.settings;

    ImGui::SetNextWindowSize(ImVec2(470, 640), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("TM Vibrant Shaders " TM_SHADERS_VERSION_A, &config.showOverlay, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (ImGui::Checkbox("Enabled (F7)", &s.enabled)) config.markDirty();
    ImGui::SameLine();
    ImGui::TextDisabled("F8 menu");

    // Presets: built-in, then your own. Everything is saved automatically.
    const std::vector<std::string> names = config.presetNames();
    const bool custom = config.preset == kCustomPreset;
    if (ImGui::BeginCombo("Preset", custom ? "Custom (modified)" : config.preset.c_str())) {
        for (size_t i = 0; i < names.size(); i++) {
            if (i == static_cast<size_t>(Preset::Custom)) ImGui::Separator(); // user presets below
            if (ImGui::Selectable(names[i].c_str(), names[i] == config.preset)) config.selectPreset(names[i]);
        }
        ImGui::EndCombo();
    }
    static char presetNameBuffer[48] = "";
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##presetname", "name for your preset", presetNameBuffer, sizeof(presetNameBuffer));
    ImGui::SameLine();
    if (ImGui::Button("Save as preset") && config.saveUserPreset(presetNameBuffer)) presetNameBuffer[0] = '\0';
    if (config.isUserPreset(config.preset)) {
        ImGui::SameLine();
        if (ImGui::Button("Delete")) config.deleteUserPreset(config.preset);
    }

    if (ImGui::Checkbox("Preset per map mood", &config.autoMood)) config.markDirty();
    ImGui::SameLine();
    ImGui::TextDisabled("map: %s", moodName(config.mood));
    if (config.autoMood && ImGui::TreeNode("Mood presets")) {
        for (int m = 0; m < static_cast<int>(Mood::Count); m++) {
            std::string& mp = config.moodPreset[m];
            if (ImGui::BeginCombo(moodName(static_cast<Mood>(m)), mp.empty() ? "Keep current" : mp.c_str())) {
                if (ImGui::Selectable("Keep current", mp.empty())) {
                    mp.clear();
                    config.markDirty();
                }
                for (const auto& name : names) {
                    if (ImGui::Selectable(name.c_str(), mp == name)) {
                        mp = name;
                        if (static_cast<int>(config.mood) == m) config.selectPreset(name);
                        config.markDirty();
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::TreePop();
    }
    ImGui::Combo("View", &s.debugView, kDebugViews, IM_ARRAYSIZE(kDebugViews));

    // Status.
    if (!g_status.depthAvailable) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Depth buffer unavailable - restart the game with MSAA disabled.");
    }
    if (!g_status.engineHooks) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Unknown game build: effects also apply to the HUD.");
    }
    if (g_status.shaderError) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Shader error, see tmvs.log");
    ImGui::TextDisabled("Sun: %s (%.2f %.2f %.2f)   %.0f FPS", g_status.sunKnown ? "from game" : "unknown",
                        g_status.sunDirection[0], g_status.sunDirection[1], g_status.sunDirection[2], ImGui::GetIO().Framerate);
    ImGui::Separator();

    bool changed = false;
    char* base = reinterpret_cast<char*>(&s);
    const char* openCategory = nullptr;
    bool categoryOpen = false;
    for (const Field& f : fields()) {
        if (!openCategory || strcmp(openCategory, f.category) != 0) {
            openCategory = f.category;
            categoryOpen = ImGui::CollapsingHeader(f.category, strcmp(f.category, "Lighting") == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        }
        if (!categoryOpen) continue;
        switch (f.kind) {
            case Field::Bool:
                changed |= ImGui::Checkbox(f.label, reinterpret_cast<bool*>(base + f.offset));
                break;
            case Field::Int:
                if (strcmp(f.key, "Quality") == 0) {
                    static const char* kQualities[] = {"Low (fast)", "Medium", "High"};
                    changed |= ImGui::Combo(f.label, reinterpret_cast<int*>(base + f.offset), kQualities, IM_ARRAYSIZE(kQualities));
                } else if (strcmp(f.key, "SkyMode") == 0) {
                    changed |= ImGui::Combo(f.label, reinterpret_cast<int*>(base + f.offset), kSkyModes, IM_ARRAYSIZE(kSkyModes));
                } else {
                    changed |= ImGui::SliderInt(f.label, reinterpret_cast<int*>(base + f.offset), static_cast<int>(f.min), static_cast<int>(f.max));
                }
                break;
            case Field::Color:
                changed |= ImGui::ColorEdit3(f.label, reinterpret_cast<float*>(base + f.offset), ImGuiColorEditFlags_NoInputs);
                break;
            default:
                changed |= ImGui::SliderFloat(f.label, reinterpret_cast<float*>(base + f.offset), f.min, f.max, "%.3f");
                break;
        }
    }
    if (changed) {
        config.preset = kCustomPreset;
        config.markDirty();
    }

    ImGui::Separator();
    if (ImGui::Button("Reset preset")) {
        std::string base = config.preset;
        if (base == kCustomPreset) {
            base = config.mood != Mood::Unknown ? config.moodPreset[static_cast<int>(config.mood)] : std::string();
            if (base.empty()) base = "Vibrant";
        }
        config.selectPreset(base);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload shaders (F9)")) g_reloadRequested = true;
    ImGui::TextDisabled("Changes are saved automatically.   F12 frame capture");
    ImGui::End();
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
    bool requested = g_reloadRequested;
    g_reloadRequested = false;
    return requested;
}

void draw(IDirect3DDevice9* device) {
    if (!g_initialized) {
        init(device);
        if (!g_initialized) return;
    }
    if (!Config::get().showOverlay) return;

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // Draw ImGui's cursor over the menu (the game hides the OS cursor), the game's elsewhere.
    ImGui::GetIO().MouseDrawCursor = ImGui::GetIO().WantCaptureMouse;
    ImGui::NewFrame();
    drawMenu();
    ImGui::EndFrame();
    if (SUCCEEDED(device->BeginScene())) {
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        device->EndScene();
    }
}

} // namespace overlay
} // namespace tmshaders
