#include "overlay.h"
#include "config.h"
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace tmshaders {
namespace overlay {
namespace {

bool g_initialized = false;
HWND g_hwnd = nullptr;
WNDPROC g_originalWndProc = nullptr;

LRESULT CALLBACK hookedWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_KEYDOWN) {
        if (wparam == Config::get().toggleKey) { // Default F8
            toggle();
            return 0;
        } else if (wparam == VK_F7) { // Quick toggle shaders on/off
            Config::get().settings.enabled = !Config::get().settings.enabled;
            return 0;
        }
    }

    if (Config::get().showOverlay) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam))
            return 1;
        // Suppress mouse input to game while menu is open
        if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)
            return 1;
    }

    return CallWindowProc(g_originalWndProc, hwnd, msg, wparam, lparam);
}

} // namespace

void init(IDirect3DDevice9* device) {
    if (g_initialized) return;

    D3DDEVICE_CREATION_PARAMETERS cp;
    if (FAILED(device->GetCreationParameters(&cp))) return;
    g_hwnd = cp.hFocusWindow;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // Don't create default imgui.ini

    // Sildur/BSL-inspired sleek dark theme
    ImGui::StyleColorsDark();
    ImVec4* colors = ImGui::GetStyle().Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.12f, 0.94f);
    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.35f, 0.60f, 0.70f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.45f, 0.75f, 0.80f);
    colors[ImGuiCol_Button] = ImVec4(0.18f, 0.32f, 0.55f, 0.80f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.46f, 0.75f, 0.90f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.12f, 0.24f, 0.42f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.16f, 0.22f, 0.80f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.35f, 0.60f, 0.95f, 0.90f);

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX9_Init(device);

    g_originalWndProc = (WNDPROC)SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hookedWndProc);
    g_initialized = true;
}

void preReset() {
    if (g_initialized) {
        ImGui_ImplDX9_InvalidateDeviceObjects();
    }
}

void postReset() {
    if (g_initialized) {
        ImGui_ImplDX9_CreateDeviceObjects();
    }
}

void toggle() {
    Config::get().showOverlay = !Config::get().showOverlay;
}

bool isVisible() {
    return Config::get().showOverlay;
}

void draw(IDirect3DDevice9* device) {
    if (!g_initialized) {
        init(device);
        if (!g_initialized) return;
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (Config::get().showOverlay) {
        ImGui::SetNextWindowSize(ImVec2(480, 540), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("TM Vibrant Shaders", &Config::get().showOverlay, ImGuiWindowFlags_NoCollapse)) {
            
            ShaderSettings& s = Config::get().settings;

            ImGui::Checkbox("Enable Shaders (F7)", &s.enabled);
            ImGui::SameLine();
            ImGui::TextDisabled("| F8: Toggle Menu");

            ImGui::Separator();
            ImGui::Text("Presets:");

            if (ImGui::RadioButton("Stadium 2020 (PBR Clarity & Modern Tarmac)", s.activePreset == Preset::Stadium2020)) {
                s.applyPreset(Preset::Stadium2020);
            }
            if (ImGui::RadioButton("Golden Hour (Warm Sunlight & Saturated)", s.activePreset == Preset::GoldenHour)) {
                s.applyPreset(Preset::GoldenHour);
            }
            if (ImGui::RadioButton("Clear Daylight (Neutral Contrast & Visibility)", s.activePreset == Preset::ClearDaylight)) {
                s.applyPreset(Preset::ClearDaylight);
            }
            if (ImGui::RadioButton("Grand Prix (Anamorphic Flares & Film Curve)", s.activePreset == Preset::GrandPrixCinematic)) {
                s.applyPreset(Preset::GrandPrixCinematic);
            }
            if (ImGui::RadioButton("Custom (Manual Controls)", s.activePreset == Preset::Custom)) {
                s.activePreset = Preset::Custom;
            }

            ImGui::Separator();

            if (ImGui::CollapsingHeader("Texture & Geometry (FidelityFX CAS & AO)", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::SliderFloat("FidelityFX CAS Sharpness", &s.sharpness, 0.0f, 1.0f);
                ImGui::SliderFloat("Contact Shading (Micro-AO)", &s.clarity, 0.0f, 1.0f);
                ImGui::SliderFloat("Road Specular Sheen", &s.roadSheen, 0.0f, 1.0f);
                ImGui::SliderFloat("Lens Vignette", &s.vignette, 0.0f, 0.6f);
            }

            if (ImGui::CollapsingHeader("Color Grading & Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::SliderFloat("Exposure", &s.exposure, 0.7f, 1.4f);
                ImGui::SliderFloat("Contrast", &s.contrast, 0.8f, 1.4f);
                ImGui::SliderFloat("Saturation", &s.saturation, 0.7f, 1.5f);
                ImGui::SliderFloat("Color Warmth", &s.warmth, -0.2f, 0.2f);
                ImGui::SliderFloat("Sky Blue Boost", &s.skyBoost, 0.0f, 0.6f);
                ImGui::SliderFloat("Foliage Boost", &s.foliageBoost, 0.0f, 0.6f);
            }

            if (ImGui::CollapsingHeader("Emissive Bloom & Lens Flares")) {
                ImGui::Checkbox("Enable Bloom", &s.enableBloom);
                if (s.enableBloom) {
                    ImGui::SliderFloat("Bloom Intensity", &s.bloomIntensity, 0.0f, 1.5f);
                    ImGui::SliderFloat("Bloom Threshold", &s.bloomThreshold, 0.70f, 0.98f);
                }
                ImGui::Checkbox("Enable Anamorphic Flares", &s.enableFlares);
                if (s.enableFlares) {
                    ImGui::SliderFloat("Flare Streak Intensity", &s.flareIntensity, 0.0f, 1.5f);
                }
            }

            if (ImGui::CollapsingHeader("Volumetric Sun Rays")) {
                ImGui::Checkbox("Enable Sun Rays", &s.enableSunRays);
                if (s.enableSunRays) {
                    ImGui::SliderFloat("Ray Intensity", &s.sunRayIntensity, 0.0f, 0.5f);
                    ImGui::SliderFloat("Ray Falloff Decay", &s.sunRayDecay, 0.85f, 0.99f);
                }
            }

            ImGui::Separator();
            if (ImGui::Button("Reset to Preset Defaults", ImVec2(180, 24))) {
                s.applyPreset(s.activePreset);
            }
            ImGui::SameLine();
            if (ImGui::Button("Close Menu (F8)", ImVec2(140, 24))) {
                Config::get().showOverlay = false;
            }
        }
        ImGui::End();
    }

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

} // namespace overlay
} // namespace tmshaders
