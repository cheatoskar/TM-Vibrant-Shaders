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
        if (ImGui::Begin("TrackMania Vibrant Shaders (TMModloader)", &Config::get().showOverlay, ImGuiWindowFlags_NoCollapse)) {
            
            ShaderSettings& s = Config::get().settings;

            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "Minecraft-Style Shaders for TrackMania Forever");
            ImGui::Checkbox("Enable Shaders (F7)", &s.enabled);
            ImGui::SameLine();
            ImGui::TextDisabled("(Hotkeys: F8 Menu | F7 Toggle)");

            ImGui::Separator();
            ImGui::Text("Shaderpacks / Presets:");

            if (ImGui::RadioButton("Sildur's Vibrant (Golden Sunlight & Saturated)", s.activePreset == Preset::SildursVibrant)) {
                s.applyPreset(Preset::SildursVibrant);
            }
            if (ImGui::RadioButton("BSL Clean (Filmic ACES & Distance Haze)", s.activePreset == Preset::BSLClean)) {
                s.applyPreset(Preset::BSLClean);
            }
            if (ImGui::RadioButton("IterationT Cinematic (Anamorphic Flares & Bloom)", s.activePreset == Preset::IterationTCinematic)) {
                s.applyPreset(Preset::IterationTCinematic);
            }
            if (ImGui::RadioButton("Custom / Manual Settings", s.activePreset == Preset::Custom)) {
                s.activePreset = Preset::Custom;
            }

            ImGui::Separator();

            if (ImGui::CollapsingHeader("Color Grading & Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::SliderFloat("Exposure", &s.exposure, 0.5f, 2.0f);
                ImGui::SliderFloat("Contrast", &s.contrast, 0.5f, 2.0f);
                ImGui::SliderFloat("Color Temperature (Warmth)", &s.colorTemp, -0.5f, 0.5f);
                ImGui::SliderFloat("Vibrance", &s.vibrance, -0.5f, 1.0f);
                ImGui::SliderFloat("Sky Blue Boost", &s.skyVibrance, 0.0f, 1.0f);
                ImGui::SliderFloat("Foliage / Grass Boost", &s.foliageBoost, 0.0f, 1.0f);
            }

            if (ImGui::CollapsingHeader("Volumetric God Rays (Light Shafts)", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox("Enable Sun Rays", &s.enableSunRays);
                if (s.enableSunRays) {
                    ImGui::SliderFloat("Ray Density", &s.rayDensity, 0.1f, 2.0f);
                    ImGui::SliderFloat("Ray Falloff Decay", &s.rayDecay, 0.85f, 0.99f);
                    ImGui::SliderFloat("Ray Exposure", &s.rayExposure, 0.2f, 3.0f);
                    ImGui::ColorEdit3("Sun Ray Color", s.rayColor);
                }
            }

            if (ImGui::CollapsingHeader("Cinematic Bloom & Anamorphic Flares", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox("Enable Bloom & Flares", &s.enableBloom);
                if (s.enableBloom) {
                    ImGui::SliderFloat("Bloom Intensity", &s.bloomIntensity, 0.0f, 2.5f);
                    ImGui::SliderFloat("Anamorphic Flare Streaks", &s.anamorphicIntensity, 0.0f, 3.0f);
                    ImGui::ColorEdit3("Flare Tint", s.flareTint);
                }
            }

            if (ImGui::CollapsingHeader("Atmospheric Distance Fog")) {
                ImGui::Checkbox("Enable Volumetric Fog", &s.enableFog);
                if (s.enableFog) {
                    ImGui::SliderFloat("Fog Density", &s.fogDensity, 0.0f, 2.0f);
                    ImGui::SliderFloat("Sun In-Scattering", &s.sunScatterPower, 0.0f, 1.0f);
                }
            }

            if (ImGui::CollapsingHeader("Ambient Occlusion / Depth Shading")) {
                ImGui::Checkbox("Enable Depth Contact Shading", &s.enableDepthShading);
                if (s.enableDepthShading) {
                    ImGui::SliderFloat("Contact Shading Intensity", &s.aoIntensity, 0.0f, 2.0f);
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
