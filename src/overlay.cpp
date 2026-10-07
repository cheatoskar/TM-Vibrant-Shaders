#include "overlay.h"
#include "autoquality.h"
#include "config.h"
#include "pipeline.h"
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "tm_shaders_version.h"
#include <cstdio>
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

const char* kSkyModes[] = {"Game sky", "Clear sky + clouds", "Starry night", "Black hole", "Aurora", "Ring world"};
const char* kPlanetTypes[] = {"Saturn", "Jupiter", "Ice giant", "Exotic"};
const char* kPlanetViews[] = {"Distant", "Next to the rings", "On the rings"};
const char* kDebugViews[] = {"Final image", "Depth", "Normals", "Ambient occlusion", "Sun shadows", "Light shafts", "Bloom", "Long shadows"};

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

// What each effect costs right now, measured on the GPU (timestamp queries).
void drawPerformance() {
    const Pipeline* pipeline = g_status.pipeline;
    if (!pipeline || !ImGui::CollapsingHeader("Performance")) return;
    struct Group {
        const char* label;
        const char* passes[3];
    };
    static const Group kGroups[] = {
        {"Depth & normals", {"LinearDepth", "Prepare", "DownsampleND"}},
        {"AO & sun shadows", {"OcclusionShadow", "BilateralBlur", nullptr}},
        {"Long-range shadows / rain map", {"HeightSplat", "HeightMerge", nullptr}},
        {"Volumetric light", {"ShadowHeight", "Volumetric", nullptr}},
        {"Bounce light (GI)", {"GI", "GITemporal", nullptr}},
        {"Neon trail", {"TrailPoint", "Trail", nullptr}},
        {"Custom sky", {"Sky*", "AuroraHalf", nullptr}},
        {"Volumetric clouds", {"Clouds", nullptr, nullptr}},
        {"Reflections", {"Reflect", nullptr, nullptr}},
        {"Neon light", {"SpillDown", "SpillBlur", nullptr}},
        {"Lighting, grass, weather, fog", {"Lighting", nullptr, nullptr}},
        {"Rain particles", {"RainDrop", "RainSplash", nullptr}},
        {"Depth of field, motion blur", {"Focus", "DofBlur", "Cinematic"}},
        {"Light shafts", {"RayMask", "RayBlur", nullptr}},
        {"Bloom", {"BloomDown", "BloomUp", nullptr}},
        {"Exposure, colour grading", {"Luminance", "Adapt", "Final"}},
        {"FXAA", {"FXAA", nullptr, nullptr}},
        {"Temporal AA", {"TAA", nullptr, nullptr}},
        {"Sharpening", {"Sharpen", "Copy", nullptr}},
    };
    const float total = pipeline->totalTime();
    const float fps = ImGui::GetIO().Framerate;
    if (total <= 0.0f) {
        ImGui::TextDisabled("Measuring... (needs GPU timestamp queries)");
        return;
    }
    const float frameMs = fps > 1.0f ? 1000.0f / fps : 0.0f;
    ImGui::Text("Effects: %.2f ms per frame", total);
    if (frameMs > total) ImGui::TextDisabled("Without effects: about %.0f FPS (now %.0f)", 1000.0f / (frameMs - total), fps);
    for (const Group& g : kGroups) {
        float ms = 0.0f;
        for (const char* name : g.passes) {
            if (!name) continue;
            for (int p = 0; p < pipeline->passCount(); p++) {
                // "Name*" matches every pass starting with Name.
                const size_t n = strlen(name);
                const bool match = name[n - 1] == '*' ? !strncmp(Pipeline::passName(p), name, n - 1) : !strcmp(Pipeline::passName(p), name);
                if (match) ms += pipeline->passTime(p);
            }
        }
        if (ms < 0.005f) continue;
        // FPS gained by switching this off, from the current frame time.
        const float gain = frameMs > ms ? 1000.0f / (frameMs - ms) - fps : 0.0f;
        ImGui::BulletText("%-30s %5.2f ms  (+%.0f FPS off)", g.label, ms, gain);
    }
    ImGui::TextDisabled("Measured on your GPU. Switching an effect off gains about its time.");
}

const Field* findField(const char* key) {
    for (const Field& f : fields()) {
        if (!strcmp(f.key, key)) return &f;
    }
    return nullptr;
}

// Settings of the installation (not of a look): changing them keeps the preset.
bool isMachineSetting(const Field& f) {
    return !strcmp(f.key, "NeonTrail") || !strncmp(f.key, "Trail", 5) || !strcmp(f.key, "AutoQuality") || !strcmp(f.key, "TargetFPS") || !strcmp(f.key, "DisableGameMSAA") ||
           !strcmp(f.key, "ReadableGameDepth") || !strcmp(f.key, "WeatherSound") || !strcmp(f.key, "CinematicOnlyInReplays") ||
           !strcmp(f.key, "TAAJitter");
}

// Only show what does something with the current choices (planet settings only for the
// skies that have a planet, cloud settings only with clouds on, ...).
bool isRelevant(const Field& f, const Settings& s) {
    auto is = [&f](const char* key) { return !strcmp(f.key, key); };
    const int sky = s.skyMode;
    if (is("SkyRotation")) return sky >= 2;
    if (is("CloudAmount")) return sky == 1;
    if (is("StarAmount")) return sky >= 1;
    if (is("SkyBrightness")) return sky >= 1;
    if (is("SkyEnhance")) return sky == 0;
    if (is("SkyEffectSize")) return sky == 3 || sky == 5;
    if (is("PlanetSize") || is("PlanetType")) return sky == 2 || sky == 3 || sky == 5;
    if (is("PlanetView") || is("PlanetAzimuth") || is("PlanetElevation")) return sky == 5;
    if (is("CloudCoverage") || is("CloudHeight")) return s.volumetricClouds > 0.0f;
    if (is("LongShadowRange")) return s.longShadows > 0.0f;
    if (is("ShadowLength")) return s.shadowStrength > 0.0f;
    if (is("GodRayDecay")) return s.godRays > 0.0f;
    if (is("Puddles") || is("Spray")) return s.wetness > 0.0f;
    if (is("LensDrops")) return s.rain > 0.0f;
    if (is("FocusDistance") || is("BokehSize")) return s.depthOfField > 0.0f;
    if (is("TargetFPS")) return s.autoQuality;
    if (is("TAAJitter")) return s.taa;
    if (!strncmp(f.key, "Trail", 5)) return s.neonTrail > 0.0f;
    if (is("WeatherSound")) return s.rain > 0.0f || s.lightning > 0.0f;
    if (is("SunAzimuth")) return s.sunElevationOverride >= 0.0f;
    return true;
}

// One setting as a widget. Returns true when the look changed (-> preset becomes Custom).
bool drawField(const Field& f, Settings& s, const char* label = nullptr) {
    char* base = reinterpret_cast<char*>(&s);
    if (!label) label = f.label;
    bool changed = false;
    switch (f.kind) {
        case Field::Bool:
            changed = ImGui::Checkbox(label, reinterpret_cast<bool*>(base + f.offset));
            break;
        case Field::Int: {
            int* value = reinterpret_cast<int*>(base + f.offset);
            if (!strcmp(f.key, "Quality")) {
                static const char* kQualities[] = {"Low (fast)", "Medium", "High"};
                changed = ImGui::Combo(label, value, kQualities, IM_ARRAYSIZE(kQualities));
            } else if (!strcmp(f.key, "SkyMode")) {
                changed = ImGui::Combo(label, value, kSkyModes, IM_ARRAYSIZE(kSkyModes));
            } else if (!strcmp(f.key, "PlanetType")) {
                changed = ImGui::Combo(label, value, kPlanetTypes, IM_ARRAYSIZE(kPlanetTypes));
            } else if (!strcmp(f.key, "PlanetView")) {
                changed = ImGui::Combo(label, value, kPlanetViews, IM_ARRAYSIZE(kPlanetViews));
            } else {
                changed = ImGui::SliderInt(label, value, static_cast<int>(f.min), static_cast<int>(f.max));
            }
            break;
        }
        case Field::Color:
            changed = ImGui::ColorEdit3(label, reinterpret_cast<float*>(base + f.offset), ImGuiColorEditFlags_NoInputs);
            break;
        default:
            changed = ImGui::SliderFloat(label, reinterpret_cast<float*>(base + f.offset), f.min, f.max,
                                         !strcmp(f.key, "TargetFPS") ? "%.0f" : "%.2f");
            break;
    }
    if (!changed) return false;
    Config::get().markDirty();
    return !isMachineSetting(f);
}

bool drawKey(Settings& s, const char* key, const char* label = nullptr) {
    const Field* f = findField(key);
    return f && isRelevant(*f, s) && drawField(*f, s, label);
}

// Which preset for which kind of map (day / sunset / night).
void drawMoodPresets(Config& config, const std::vector<std::string>& names) {
    for (int m = 0; m < static_cast<int>(Mood::Count); m++) {
        std::string& mp = config.moodPreset[m];
        static const char* const kLabels[] = {"Day maps", "Sunset maps", "Night maps"};
        const char* label = kLabels[m];
        if (ImGui::BeginCombo(label, mp.empty() ? "Keep current" : mp.c_str())) {
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
}

// A section of the menu that starts collapsed; whether it is open is remembered.
bool section(Config& config, const char* label, int bit, bool header = true) {
    const int flag = 1 << bit;
    ImGui::SetNextItemOpen((config.menuSections & flag) != 0, ImGuiCond_Once);
    // Own ID scope: a header may share its label with a widget inside it ("Sky").
    ImGui::PushID("section");
    const bool open = header ? ImGui::CollapsingHeader(label) : ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_NoTreePushOnOpen);
    ImGui::PopID();
    const int sections = open ? (config.menuSections | flag) : (config.menuSections & ~flag);
    if (sections != config.menuSections) {
        config.menuSections = sections;
        config.markDirty();
    }
    return open;
}

void drawStatus(const Settings& s) {
    if (!g_status.depthAvailable) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Depth buffer unavailable - restart the game with MSAA disabled.");
    }
    if (!g_status.engineHooks) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Unknown game build: effects also apply to the HUD.");
    }
    if (g_status.shaderError) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Shader error, see tmvs.log");
    if (s.autoQuality && g_status.autoQuality) {
        ImGui::TextDisabled("%.0f FPS   auto quality: %s", ImGui::GetIO().Framerate, AutoQuality::levelName(g_status.autoQuality->level()));
    } else {
        ImGui::TextDisabled("%.0f FPS", ImGui::GetIO().Framerate);
    }
}

// The everyday menu: the handful of things a player wants.
bool drawSimple(Config& config, Settings& s) {
    bool look = false;
    if (section(config, "Sky", 0)) {
        look |= drawKey(s, "SkyMode", "Sky");
        look |= drawKey(s, "PlanetType", "Planet");
        look |= drawKey(s, "PlanetView", "View");
        look |= drawKey(s, "PlanetAzimuth", "Planet direction");
        look |= drawKey(s, "SkyEffectSize", "Black hole size");
        look |= drawKey(s, "SkyRotation", "Turn the sky");
        look |= drawKey(s, "StarAmount", "Stars");
    }

    if (section(config, "Look", 1)) {
        look |= drawKey(s, "ShadowStrength", "Shadows");
        look |= drawKey(s, "GodRays", "Light shafts");
        look |= drawKey(s, "Bloom", "Glow");
        look |= drawKey(s, "NeonLight", "Neon light");
        look |= drawKey(s, "Exposure", "Brightness");
        look |= drawKey(s, "Saturation", "Colour");
        look |= drawKey(s, "MotionBlur", "Motion blur");
    }

    if (section(config, "Weather", 2)) {
        look |= drawKey(s, "Rain", "Rain");
        look |= drawKey(s, "Wetness", "Wet roads");
        look |= drawKey(s, "VolumetricClouds", "Clouds");
        look |= drawKey(s, "Lightning", "Lightning");
        look |= drawKey(s, "LensDrops", "Drops on the lens");
        look |= drawKey(s, "Spray", "Spray behind the car");
        if (s.rain > 0.0f || s.lightning > 0.0f) look |= drawKey(s, "WeatherSound", "Rain & thunder sound");
        look |= drawKey(s, "WaterSurfaces", "Water");
        look |= drawKey(s, "Reflections", "Reflections (dry track)");
    }

    if (section(config, "Performance", 3)) {
        look |= drawKey(s, "AutoQuality", "Adapt quality to my GPU");
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Turns effects down when the frame rate drops below the target, and back up when there is room.\n"
                              "Your settings and presets stay as they are.");
        }
        look |= drawKey(s, "TargetFPS", "Target FPS");
        look |= drawKey(s, "Quality", "Effect quality");
    }
    return look;
}

// Everything: own presets, mood presets, every setting, measured costs, debug views.
bool drawAdvanced(Config& config, Settings& s, const std::vector<std::string>& names) {
    static char presetNameBuffer[48] = "";
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##presetname", "name for your preset", presetNameBuffer, sizeof(presetNameBuffer));
    ImGui::SameLine();
    if (ImGui::Button("Save as preset") && config.saveUserPreset(presetNameBuffer)) presetNameBuffer[0] = '\0';
    if (config.isUserPreset(config.preset)) {
        ImGui::SameLine();
        if (ImGui::Button("Delete")) config.deleteUserPreset(config.preset);
    }
    ImGui::Combo("Debug view", &s.debugView, kDebugViews, IM_ARRAYSIZE(kDebugViews));
    ImGui::TextDisabled("Sun: %s (%.2f %.2f %.2f)", g_status.sunKnown ? "from game" : "unknown", g_status.sunDirection[0],
                        g_status.sunDirection[1], g_status.sunDirection[2]);

    bool look = false;
    const char* openCategory = nullptr;
    bool categoryOpen = false;
    for (const Field& f : fields()) {
        if (!openCategory || strcmp(openCategory, f.category) != 0) {
            openCategory = f.category;
            categoryOpen = ImGui::CollapsingHeader(f.category);
        }
        if (categoryOpen && isRelevant(f, s)) look |= drawField(f, s);
    }
    drawPerformance();

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
    return look;
}

void drawMenu() {
    Config& config = Config::get();
    Settings& s = config.settings;

    ImGui::SetNextWindowSize(ImVec2(460, 620), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("TM Vibrant Shaders " TM_SHADERS_VERSION_A, &config.showOverlay, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (ImGui::Checkbox("Enabled (F7)", &s.enabled)) config.markDirty();
    ImGui::SameLine(ImGui::GetWindowWidth() - 190.0f);
    if (ImGui::RadioButton("Simple", !config.advancedMenu)) {
        config.advancedMenu = false;
        config.markDirty();
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Advanced", config.advancedMenu)) {
        config.advancedMenu = true;
        config.markDirty();
    }

    // Presets: built-in, then your own. Everything is saved automatically.
    const std::vector<std::string> names = config.presetNames();
    const bool custom = config.preset == kCustomPreset;
    if (ImGui::BeginCombo("Preset", custom ? "Custom (your changes)" : config.preset.c_str())) {
        for (size_t i = 0; i < names.size(); i++) {
            if (i == static_cast<size_t>(Preset::Custom)) ImGui::Separator(); // user presets below
            if (ImGui::Selectable(names[i].c_str(), names[i] == config.preset)) config.selectPreset(names[i]);
        }
        ImGui::EndCombo();
    }
    if (ImGui::Checkbox("Preset per map mood", &config.autoMood)) config.markDirty();
    ImGui::SameLine();
    ImGui::TextDisabled("this map: %s", moodName(config.mood));
    if (config.autoMood && section(config, "Which preset for which maps", 4, false)) drawMoodPresets(config, names);
    drawStatus(s);

    const bool look = config.advancedMenu ? drawAdvanced(config, s, names) : drawSimple(config, s);
    if (look) config.preset = kCustomPreset;

    ImGui::Spacing();
    ImGui::TextDisabled(config.advancedMenu ? "Saved automatically.   F12 frame capture"
                                            : "Saved automatically.   More settings and your own presets: Advanced");
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
