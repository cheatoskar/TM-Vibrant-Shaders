#include "overlay.h"
#include "autoquality.h"
#include "config.h"
#include "engine.h"
#include "pipeline.h"
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "tm_shaders_version.h"
#include <cctype>
#include <cmath>
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
const char* kPlanetChoices[] = {"None", "Saturn", "Jupiter", "Ice giant", "Exotic"};
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
    style.WindowRounding = 8.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.FramePadding = ImVec2(7.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.WindowBorderSize = 1.0f;
    style.GrabMinSize = 9.0f;
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.07f, 0.09f, 0.94f);
    colors[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.09f, 0.11f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.10f, 0.09f, 1.0f);
    colors[ImGuiCol_Separator] = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.21f, 0.26f, 0.95f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.25f, 0.31f, 1.0f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.09f, 0.11f, 0.98f);
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
        {"Rain and snow particles", {"RainDrop", "RainSplash", "SnowFlake"}},
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

// Each part of the menu has its own muted colour (header, slider grab, check mark), so you
// find your way around; the orange accent stays for everything else.
ImVec4 sectionColour(const char* name) {
    struct Entry {
        const char* name;
        ImVec4 colour;
    };
    static const Entry kColours[] = {
        {"Lighting", ImVec4(0.86f, 0.62f, 0.28f, 1.0f)},       {"Look", ImVec4(0.86f, 0.62f, 0.28f, 1.0f)},
        {"Sky", ImVec4(0.38f, 0.58f, 0.90f, 1.0f)},            {"Sky & Atmosphere", ImVec4(0.38f, 0.58f, 0.90f, 1.0f)},
        {"Bloom & Lens", ImVec4(0.64f, 0.50f, 0.88f, 1.0f)},   {"Colour", ImVec4(0.88f, 0.48f, 0.58f, 1.0f)},
        {"Weather", ImVec4(0.30f, 0.72f, 0.68f, 1.0f)},        {"Weather & Surfaces", ImVec4(0.30f, 0.72f, 0.68f, 1.0f)},
        {"Cinematic", ImVec4(0.58f, 0.62f, 0.70f, 1.0f)},      {"Image", ImVec4(0.50f, 0.70f, 0.50f, 1.0f)},
        {"Performance", ImVec4(0.50f, 0.70f, 0.50f, 1.0f)},    {"Neon trail", ImVec4(0.26f, 0.74f, 0.90f, 1.0f)},
        {"Author's Shader", ImVec4(0.88f, 0.76f, 0.36f, 1.0f)},
    };
    for (const Entry& e : kColours) {
        if (!strcmp(e.name, name)) return e.colour;
    }
    return ImVec4(0.85f, 0.55f, 0.20f, 1.0f);
}

struct SectionStyle {
    explicit SectionStyle(const char* name) {
        const ImVec4 c = sectionColour(name);
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(c.x, c.y, c.z, 0.28f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(c.x, c.y, c.z, 0.45f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(c.x, c.y, c.z, 0.60f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(c.x, c.y, c.z, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, c);
        ImGui::PushStyleColor(ImGuiCol_CheckMark, c);
    }
    ~SectionStyle() { ImGui::PopStyleColor(6); }
    SectionStyle(const SectionStyle&) = delete;
    SectionStyle& operator=(const SectionStyle&) = delete;
};

// A small round "reset" arrow in front of a setting, shown only while it differs from the
// preset it started from. Without it, the same space stays empty (the rows stay aligned).
bool resetButton(bool show, const char* tooltipPreset) {
    const float size = ImGui::GetFrameHeight();
    if (!show) {
        ImGui::Dummy(ImVec2(size, size));
        ImGui::SameLine(0.0f, 4.0f);
        return false;
    }
    const bool clicked = ImGui::InvisibleButton("reset", ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 corner = ImGui::GetItemRectMin();
    const ImVec2 centre(corner.x + size * 0.5f, corner.y + size * 0.5f);
    if (hovered) draw->AddCircleFilled(centre, size * 0.46f, ImGui::GetColorU32(ImGuiCol_FrameBgHovered));
    const ImU32 colour = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    // A circular arrow, turning clockwise.
    const float r = size * 0.24f, a0 = -2.2f, a1 = a0 + 4.9f;
    draw->PathArcTo(centre, r, a0, a1, 18);
    draw->PathStroke(colour, 0, 1.7f);
    const ImVec2 end(centre.x + cosf(a1) * r, centre.y + sinf(a1) * r);
    const ImVec2 along(-sinf(a1), cosf(a1)), out(cosf(a1), sinf(a1));
    const float head = size * 0.15f;
    draw->AddTriangleFilled(ImVec2(end.x + along.x * head, end.y + along.y * head),
                            ImVec2(end.x + out.x * head * 0.9f - along.x * head * 0.3f, end.y + out.y * head * 0.9f - along.y * head * 0.3f),
                            ImVec2(end.x - out.x * head * 0.9f - along.x * head * 0.3f, end.y - out.y * head * 0.9f - along.y * head * 0.3f), colour);
    if (hovered) ImGui::SetTooltip("Reset to %s", tooltipPreset);
    ImGui::SameLine(0.0f, 4.0f);
    return clicked;
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
    if (is("BlackHoleAzimuth") || is("BlackHoleElevation")) return (sky == 3 || sky == 5) && s.skyEffectSize > 0.0f;
    if (is("AuroraSpeed")) return sky == 4;
    if (is("PlanetType")) return sky == 2 || sky == 3 || sky == 5; // "None" switches the planet off
    if (is("PlanetSize")) return (sky == 2 || sky == 3 || sky == 5) && s.planetSize > 0.0f;
    if (is("PlanetAzimuth") || is("PlanetElevation")) return (sky == 3 || sky == 5) && s.planetSize > 0.0f;
    if (is("PlanetView")) return sky == 5 && s.planetSize > 0.0f;
    if (is("CloudCoverage") || is("CloudHeight")) return s.volumetricClouds > 0.0f;
    if (is("LongShadowRange")) return s.longShadows > 0.0f;
    if (is("ShadowLength")) return s.shadowStrength > 0.0f;
    if (is("GodRayDecay")) return s.godRays > 0.0f;
    if (is("Puddles")) return s.wetness > 0.0f;
    if (is("Spray")) return s.wetness > 0.0f || s.snowCover > 0.0f;
    if (is("LensDrops")) return s.rain > 0.0f || s.snow > 0.0f;
    if (is("ReflectionBlur")) return s.reflections > 0.0f;
    if (is("FocusDistance") || is("BokehSize")) return s.depthOfField > 0.0f;
    if (is("TargetFPS")) return s.autoQuality;
    if (is("TAAJitter")) return s.taa;
    if (!strncmp(f.key, "Trail", 5)) return s.neonTrail > 0.0f;
    if (is("WeatherSound")) return s.rain > 0.0f || s.lightning > 0.0f || s.snow > 0.0f;
    if (is("SunAzimuth")) return s.sunElevationOverride >= 0.0f;
    return true;
}

// Choosing a sky brings the settings that make it look right (the planet with the space
// skies, the black hole's light, a darker night for the stars).
void applySkyDefaults(Settings& s, int sky) {
    if (sky == 3 || sky == 5) {
        s.skyEffectSize = 4.0f;
        s.godRays = 1.35f;
        s.godRayDecay = 0.9f;
        if (s.planetSize <= 0.0f) s.planetSize = 1.0f;
        if (sky == 5) s.planetView = 1; // next to the rings
    } else if (sky == 2) {
        s.skyNight = 0.5f;
        s.skyBrightness = 1.0f;
        s.reflections = 1.1f;
    } else if (sky == 4) {
        s.skyRotation = 150.0f;
    }
}

size_t fieldSize(const Field& f) {
    return f.kind == Field::Bool ? sizeof(bool) : (f.kind == Field::Color ? sizeof(float) * 3 : sizeof(float));
}

// Whether a setting differs from the preset it started from.
bool differsFromBase(const Field& f, const Settings& s, const Settings& base) {
    const char* a = reinterpret_cast<const char*>(&s) + f.offset;
    const char* b = reinterpret_cast<const char*>(&base) + f.offset;
    if (!strcmp(f.key, "PlanetType") && (s.planetSize > 0.0f) != (base.planetSize > 0.0f)) return true;
    if (f.kind == Field::Float || f.kind == Field::Color) {
        for (int i = 0; i < (f.kind == Field::Color ? 3 : 1); i++) {
            const float x = reinterpret_cast<const float*>(a)[i], y = reinterpret_cast<const float*>(b)[i];
            if (fabsf(x - y) > 1e-4f * (fabsf(y) + 1.0f)) return true;
        }
        return false;
    }
    return memcmp(a, b, fieldSize(f)) != 0;
}

// One setting as a widget. Returns true when the look changed (-> preset becomes Custom).
bool drawField(const Field& f, Settings& s, const char* label = nullptr) {
    char* base = reinterpret_cast<char*>(&s);
    if (!label) label = f.label;
    bool changed = false;
    ImGui::PushID(f.key);
    // Machine settings (FPS target, sound volume, ...) are not part of a preset: no reset.
    const Config& config = Config::get();
    if (!isMachineSetting(f)) {
        if (resetButton(differsFromBase(f, s, config.baseline()), config.basePreset().c_str())) {
            memcpy(base + f.offset, reinterpret_cast<const char*>(&config.baseline()) + f.offset, fieldSize(f));
            if (!strcmp(f.key, "PlanetType")) s.planetSize = config.baseline().planetSize;
            changed = true;
        }
    } else {
        resetButton(false, "");
    }
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - ImGui::GetFrameHeight() - 4.0f);
    switch (f.kind) {
        case Field::Bool:
            changed |= ImGui::Checkbox(label, reinterpret_cast<bool*>(base + f.offset));
            break;
        case Field::Int: {
            int* value = reinterpret_cast<int*>(base + f.offset);
            if (!strcmp(f.key, "Quality")) {
                static const char* kQualities[] = {"Low (fast)", "Medium", "High"};
                changed |= ImGui::Combo(label, value, kQualities, IM_ARRAYSIZE(kQualities));
            } else if (!strcmp(f.key, "SkyMode")) {
                if (ImGui::Combo(label, value, kSkyModes, IM_ARRAYSIZE(kSkyModes))) {
                    applySkyDefaults(s, *value);
                    changed = true;
                }
            } else if (!strcmp(f.key, "PlanetType")) {
                int choice = s.planetSize > 0.0f ? *value + 1 : 0;
                if (ImGui::Combo(label, &choice, kPlanetChoices, IM_ARRAYSIZE(kPlanetChoices))) {
                    if (choice == 0) {
                        s.planetSize = 0.0f;
                    } else {
                        *value = choice - 1;
                        if (s.planetSize <= 0.0f) s.planetSize = 1.0f;
                    }
                    changed = true;
                }
            } else if (!strcmp(f.key, "PlanetView")) {
                changed |= ImGui::Combo(label, value, kPlanetViews, IM_ARRAYSIZE(kPlanetViews));
            } else {
                changed |= ImGui::SliderInt(label, value, static_cast<int>(f.min), static_cast<int>(f.max));
            }
            break;
        }
        case Field::Color:
            changed |= ImGui::ColorEdit3(label, reinterpret_cast<float*>(base + f.offset), ImGuiColorEditFlags_NoInputs);
            break;
        default:
            changed |= ImGui::SliderFloat(label, reinterpret_cast<float*>(base + f.offset), f.min, f.max,
                                          !strcmp(f.key, "TargetFPS") ? "%.0f" : "%.2f");
            break;
    }
    ImGui::PopID();
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

// A section of the simple menu in its colour; `body` draws its settings (returns whether
// the look changed).
template <typename Body>
bool simpleSection(Config& config, const char* label, int bit, Body body) {
    SectionStyle style(label);
    return section(config, label, bit) && body();
}

// The everyday menu: the handful of things a player wants.
bool drawSimple(Config& config, Settings& s) {
    bool look = false;
    look |= simpleSection(config, "Sky", 0, [&s] {
        bool l = drawKey(s, "SkyMode", "Sky");
        l |= drawKey(s, "SkyRotation", "Turn the sky");
        l |= drawKey(s, "StarAmount", "Stars");
        l |= drawKey(s, "AuroraSpeed", "Aurora movement");
        l |= drawKey(s, "PlanetType", "Planet");
        l |= drawKey(s, "PlanetSize", "Planet size");
        l |= drawKey(s, "PlanetView", "View");
        l |= drawKey(s, "PlanetAzimuth", "Planet direction");
        l |= drawKey(s, "PlanetElevation", "Planet height");
        l |= drawKey(s, "SkyEffectSize", "Black hole size (0 = off)");
        l |= drawKey(s, "BlackHoleAzimuth", "Black hole direction");
        l |= drawKey(s, "BlackHoleElevation", "Black hole height");
        return l;
    });
    look |= simpleSection(config, "Look", 1, [&s] {
        bool l = drawKey(s, "ShadowStrength", "Shadows");
        l |= drawKey(s, "GodRays", "Light shafts");
        l |= drawKey(s, "Bloom", "Glow");
        l |= drawKey(s, "NeonLight", "Neon light");
        l |= drawKey(s, "Exposure", "Brightness");
        l |= drawKey(s, "Saturation", "Colour");
        l |= drawKey(s, "MotionBlur", "Motion blur");
        return l;
    });
    look |= simpleSection(config, "Weather", 2, [&s] {
        bool l = drawKey(s, "Rain", "Rain");
        l |= drawKey(s, "Snow", "Snow");
        l |= drawKey(s, "SnowCover", "Snow on the ground");
        l |= drawKey(s, "Wetness", "Wet roads");
        l |= drawKey(s, "VolumetricClouds", "Clouds");
        l |= drawKey(s, "Lightning", "Lightning");
        l |= drawKey(s, "LensDrops", "Drops on the lens");
        l |= drawKey(s, "Spray", "Spray behind the car");
        l |= drawKey(s, "WeatherSound", "Weather sound");
        l |= drawKey(s, "WaterSurfaces", "Water");
        l |= drawKey(s, "Reflections", "Reflections (dry track)");
        l |= drawKey(s, "ReflectionBlur", "Reflection blur");
        return l;
    });
    look |= simpleSection(config, "Performance", 3, [&s] {
        bool l = drawKey(s, "AutoQuality", "Adapt quality to my GPU");
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Turns effects down when the frame rate drops below the target, and back up when there is room.\n"
                              "Your settings and presets stay as they are.");
        }
        l |= drawKey(s, "TargetFPS", "Target FPS");
        l |= drawKey(s, "Quality", "Effect quality");
        return l;
    });
    return look;
}

bool containsNoCase(const char* text, const char* query) {
    for (; *text; text++) {
        size_t i = 0;
        while (query[i] && text[i] && tolower(static_cast<unsigned char>(text[i])) == tolower(static_cast<unsigned char>(query[i]))) i++;
        if (!query[i]) return true;
    }
    return false;
}

// Search over every setting: the matches of all sections in one list. A match that does
// nothing with the current choices is shown greyed out.
bool drawSearchResults(Settings& s, const char* query) {
    bool look = false;
    const char* lastCategory = nullptr;
    int matches = 0;
    for (const Field& f : fields()) {
        if (!containsNoCase(f.label, query) && !containsNoCase(f.key, query) && !containsNoCase(f.category, query)) continue;
        matches++;
        if (!lastCategory || strcmp(lastCategory, f.category) != 0) {
            lastCategory = f.category;
            ImGui::PushStyleColor(ImGuiCol_Text, sectionColour(f.category));
            ImGui::SeparatorText(f.category);
            ImGui::PopStyleColor();
        }
        SectionStyle style(f.category);
        const bool relevant = isRelevant(f, s);
        ImGui::BeginDisabled(!relevant);
        look |= drawField(f, s);
        ImGui::EndDisabled();
        if (!relevant && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("No effect with the current settings");
    }
    if (!matches) ImGui::TextDisabled("Nothing found.");
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

    static char query[48] = "";
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", "Search settings", query, sizeof(query));

    bool look = false;
    if (query[0]) {
        look = drawSearchResults(s, query);
    } else {
        const char* openCategory = nullptr;
        bool categoryOpen = false;
        for (const Field& f : fields()) {
            if (!openCategory || strcmp(openCategory, f.category) != 0) {
                openCategory = f.category;
                SectionStyle style(f.category);
                categoryOpen = ImGui::CollapsingHeader(f.category);
            }
            if (categoryOpen && isRelevant(f, s)) {
                SectionStyle style(f.category);
                look |= drawField(f, s);
            }
        }
    }
    {
        SectionStyle style("Image");
        drawPerformance();
    }

    // Author's Shader: a look stored in a map (its comments), for everyone with the mod.
    SectionStyle authorStyle("Author's Shader");
    if (ImGui::CollapsingHeader("Author's Shader")) {
        bool mapLooks = config.useMapLooks;
        if (ImGui::Checkbox("Load Author's Shaders", &mapLooks)) config.setUseMapLooks(mapLooks);
        static std::string result;
        if (ImGui::Button("Set as Author's Shader")) {
            std::string comments;
            const bool ok = engine::currentMapComments(comments) &&
                            engine::setCurrentMapComments(Config::withMapTag(comments, config.mapTag()));
            result = ok ? "Set. Save the map to keep it." : "No map open.";
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stores this look in the map open in the editor.");
        ImGui::SameLine();
        if (ImGui::Button("Copy code")) {
            ImGui::SetClipboardText(config.mapTag().c_str());
            result = "Copied.";
        }
        if (!result.empty()) ImGui::TextDisabled("%s", result.c_str());
    }

    ImGui::Separator();
    if (ImGui::Button("Reset preset")) {
        // Back to the preset your changes started from (a map's look: its own look again).
        if (config.basePreset() == kMapLookPreset) {
            const Settings base = config.baseline();
            const bool enabled = s.enabled;
            s = base;
            s.enabled = enabled;
            config.preset = kMapLookPreset;
            config.markDirty();
        } else {
            config.selectPreset(config.basePreset());
        }
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
    const char* current = custom ? "Custom (your changes)" : (config.mapLookActive() ? "Author's Shader" : config.preset.c_str());
    if (ImGui::BeginCombo("Preset", current)) {
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
    if (look) {
        config.preset = kCustomPreset;
        // Every setting reset by hand: it is the preset again.
        bool same = true;
        for (const Field& f : fields()) {
            if (!isMachineSetting(f) && differsFromBase(f, s, config.baseline())) {
                same = false;
                break;
            }
        }
        if (same) config.preset = config.basePreset();
    }

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
