// The F8 menu: presets, the gallery, every setting, maps, performance, about. Game-neutral: what
// depends on the game (preset pictures, GPU timings, map comments, photo mode, which settings it
// can do) comes in through menu::Host. The platform part (window messages, the ImGui renderer)
// is overlay.cpp for TMNF and maniaplanet/src/overlay.cpp for ManiaPlanet.
#include "menu.h"
#include "config.h"
#include "keys.h"
#include "log.h"
#include "lut.h"
#include "update.h"
#include "imgui_internal.h"
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

namespace tmshaders {
namespace menu {
namespace {

const Host* g_host = nullptr;
bool g_reloadRequested = false;
ImFont* g_fontTitle = nullptr; // headings, preset names
float g_scale = 1.0f;          // 1 at 1080p, more on larger screens

const char* kSkyModes[] = {"Game sky", "Clear sky + clouds", "Starry night", "Black hole", "Aurora", "Ring world"};
const char* kPlanetChoices[] = {"None", "Saturn", "Jupiter", "Ice giant", "Exotic"};
const char* kPlanetViews[] = {"Distant", "Next to the rings", "On the rings"};
const char* kDebugViews[] = {"Final image", "Depth", "Normals", "Ambient occlusion", "Sun shadows", "Light shafts", "Bloom", "Long shadows", "Materials (game draws)"};

ImTextureID picture(const std::string& preset) {
    return g_host->picture ? g_host->picture(preset) : ImTextureID{};
}

// Settings that work from settings.ini but are not in the menu (no page, no search): Night on
// game light is an experiment that needs a game restart and changes little, and players took it
// for a switch for the night presets. GameLight=1 in settings.ini still turns it on.
bool hiddenFromMenu(const Field& f) {
    return !strcmp(f.key, "GameLight");
}

bool supported(const Field& f) {
    if (hiddenFromMenu(f)) return false;
    return !g_host || !g_host->supports || g_host->supports(f);
}

// What each effect costs right now, measured on the GPU (timestamp queries).
void drawPerformance() {
    const PassTimings* pipeline = g_host->timings;
    if (!pipeline) {
        ImGui::TextDisabled("Not measured in this game yet.");
        return;
    }
    struct Group {
        const char* label;
        const char* passes[4];
    };
    static const Group kGroups[] = {
        {"Depth & normals", {"LinearDepth", "Prepare*", "DownsampleND"}},
        {"AO & sun shadows", {"OcclusionShadow", "BilateralBlur", nullptr}},
        {"Long-range shadows / rain map", {"HeightSplat", "HeightMerge", "LightGrid*", "Casters"}},
        {"Volumetric light", {"ShadowHeight", "Volumetric", nullptr}},
        {"Bounce light (GI)", {"GI", "GITemporal", nullptr}},
        {"Neon trail", {"TrailPoint", "Trail", nullptr}},
        {"Custom sky", {"Sky*", "AuroraHalf", nullptr}},
        {"Volumetric clouds", {"Clouds", nullptr, nullptr}},
        {"Reflections", {"Reflect", nullptr, nullptr}},
        {"Neon light", {"Spill*", nullptr, nullptr}},
        {"Lamp glare and haze", {"Lamp*", nullptr, nullptr}},
        {"Lighting, grass, weather, fog", {"Lighting*", nullptr, nullptr}},
        {"Rain and snow particles", {"RainDrop", "RainSplash", "SnowFlake", "WaterSplash"}},
        {"Clear water", {"ClearWater", nullptr, nullptr}},
        {"Depth of field, motion blur", {"Focus", "DofBlur", "Cinematic"}},
        {"Light shafts", {"RayMask", "RayBlur", nullptr}},
        {"Bloom", {"BloomDown", "BloomUp", nullptr}},
        {"Exposure, colour grading", {"Luminance", "Adapt", "Final"}},
        {"Edge smoothing (FXAA, SMAA)", {"FXAA", "Smaa*", nullptr}},
        {"Temporal AA", {"TAA", nullptr, nullptr}},
        {"Sharpening", {"Sharpen", "Copy", nullptr}},
    };
    const float total = pipeline->total();
    const float fps = ImGui::GetIO().Framerate;
    if (total <= 0.0f) {
        ImGui::TextDisabled("Measuring...");
        return;
    }
    const float frameMs = fps > 1.0f ? 1000.0f / fps : 0.0f;
    ImGui::Text("Effects: %.2f ms per frame", total);
    if (frameMs > total) ImGui::TextDisabled("Without effects: about %.0f FPS (now %.0f)", 1000.0f / (frameMs - total), fps);
    float times[IM_ARRAYSIZE(kGroups)] = {};
    float most = 0.01f;
    for (int i = 0; i < IM_ARRAYSIZE(kGroups); i++) {
        for (const char* name : kGroups[i].passes) {
            if (!name) continue;
            for (int p = 0; p < pipeline->count(); p++) {
                // "Name*" matches every pass starting with Name.
                const size_t n = strlen(name);
                const bool match = name[n - 1] == '*' ? !strncmp(pipeline->name(p), name, n - 1) : !strcmp(pipeline->name(p), name);
                if (match) times[i] += pipeline->time(p);
            }
        }
        most = std::max(most, times[i]);
    }
    if (ImGui::BeginTable("costs", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Effect", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("off", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("+000 FPS").x);
        for (int i = 0; i < IM_ARRAYSIZE(kGroups); i++) {
            const float ms = times[i];
            if (ms < 0.005f) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kGroups[i].label);
            ImGui::TableNextColumn();
            // A bar as long as its share of the most expensive effect, the time on it.
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w * ms / most, p.y + h), ImGui::GetColorU32(ImVec4(1.0f, 0.62f, 0.22f, 0.35f + 0.5f * ms / most)), 3.0f); // the accent
            ImGui::Text(" %.2f ms", ms);
            ImGui::TableNextColumn();
            // FPS gained by switching this off, from the current frame time.
            const float gain = frameMs > ms ? 1000.0f / (frameMs - ms) - fps : 0.0f;
            ImGui::TextDisabled("+%.0f FPS", gain);
        }
        ImGui::EndTable();
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
        {"Lighting", ImVec4(0.86f, 0.62f, 0.28f, 1.0f)},       {"Light and shadows", ImVec4(0.86f, 0.62f, 0.28f, 1.0f)},
        {"Sky", ImVec4(0.38f, 0.58f, 0.90f, 1.0f)},            {"Haze and light shafts", ImVec4(0.52f, 0.68f, 0.84f, 1.0f)},
        {"Bloom and lens", ImVec4(0.64f, 0.50f, 0.88f, 1.0f)}, {"Colour", ImVec4(0.88f, 0.48f, 0.58f, 1.0f)},
        {"Weather", ImVec4(0.30f, 0.72f, 0.68f, 1.0f)},        {"Weather and surfaces", ImVec4(0.30f, 0.72f, 0.68f, 1.0f)},
        {"Cinematic", ImVec4(0.58f, 0.62f, 0.70f, 1.0f)},      {"Image quality", ImVec4(0.50f, 0.70f, 0.50f, 1.0f)},
        {"Game integration", ImVec4(0.70f, 0.62f, 0.50f, 1.0f)}, {"Look", ImVec4(0.86f, 0.62f, 0.28f, 1.0f)},
        {"Performance", ImVec4(0.50f, 0.70f, 0.50f, 1.0f)},    {"Neon trail", ImVec4(0.26f, 0.74f, 0.90f, 1.0f)},
        {"Author's Shader", ImVec4(0.88f, 0.76f, 0.36f, 1.0f)}, {"Cost per effect", ImVec4(0.50f, 0.70f, 0.50f, 1.0f)},
        {"Updates", ImVec4(0.86f, 0.62f, 0.28f, 1.0f)},         {"Help", ImVec4(0.38f, 0.58f, 0.90f, 1.0f)},
        {"Credits", ImVec4(0.58f, 0.62f, 0.70f, 1.0f)},         {"Presets and maps", ImVec4(0.88f, 0.76f, 0.36f, 1.0f)},
        {"Menu", ImVec4(0.64f, 0.50f, 0.88f, 1.0f)},           {"Game", ImVec4(0.30f, 0.72f, 0.68f, 1.0f)},
        {"Files", ImVec4(0.58f, 0.62f, 0.70f, 1.0f)},
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
           !strcmp(f.key, "ReadableGameDepth") || !strcmp(f.key, "DisableGameFX") || !strcmp(f.key, "GameMaterials") || !strcmp(f.key, "WeatherSound") || !strcmp(f.key, "CinematicOnlyInReplays") ||
           !strncmp(f.key, "Video", 5) || !strcmp(f.key, "TAAJitter") || !strcmp(f.key, "GameNight") || !strcmp(f.key, "EngineMoon") || !strcmp(f.key, "GameSkyClouds") ||
           !strcmp(f.key, "CleanHighlights") || !strcmp(f.key, "GameHdr") || !strcmp(f.key, "EngineLights") ||
           !strcmp(f.key, "CasterShadows") || !strcmp(f.key, "CasterAlphaTest") || !strcmp(f.key, "CleanWater") || !strcmp(f.key, "WaterFarReflections") || !strcmp(f.key, "OverlapFix") || !strcmp(f.key, "GameGBuffer") ||
           !strcmp(f.key, "GameLight") || !strcmp(f.key, "ShadeMenuBars") || !strcmp(f.key, "RenderScale") ||
           !strcmp(f.key, "FsrSharpness");
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
    if (is("LongShadowRange") || is("LongShadowQuality")) return s.longShadows > 0.0f;
    if (is("ShadowLength")) return s.shadowStrength > 0.0f;
    if (is("GodRayDecay")) return s.godRays > 0.0f;
    if (is("Puddles")) return s.wetness > 0.0f;
    if (is("Spray")) return s.wetness > 0.0f || s.snowCover > 0.0f;
    if (is("LensDrops")) return s.rain > 0.0f || s.snow > 0.0f;
    if (is("ReflectionBlur")) return s.reflections > 0.0f;
    if (is("WaterFarReflections")) return s.cleanWater && s.waterSurfaces > 0.0f;
    if (is("WaterClarity")) {
        // TrackMania 2 has no Water setting (the game's own surface stays): clarity stands alone.
        const Field* surfaces = findField("WaterSurfaces");
        return s.waterSurfaces > 0.0f || !surfaces || !supported(*surfaces);
    }
    if (is("CarHeadlightRange") || is("CarHeadlightBeam")) return s.carHeadlights > 0.0f;
    if (is("FocusDistance") || is("BokehSize")) return s.depthOfField > 0.0f;
    if (is("DofAutofocus")) return s.depthOfField > 0.0f && s.focusDistance <= 0.0f; // a set distance overrides it
    // The video export: what the chosen format and encoder use (the game's AVI: none of it).
    const bool mp4 = s.videoCodec >= 1;
    if (is("VideoQuality")) return mp4 && s.videoBitrate <= 0.0f;
    if (is("Video10Bit")) return s.videoCodec >= 2;
    if (is("VideoEncoder") || is("VideoBitrate") || is("VideoAspect") || is("VideoOwnFrames")) return mp4;
    if (is("VideoFfmpegPreset")) return mp4 && s.videoEncoder == 1;
    if (is("VideoShutter") || is("VideoLensDof") || is("VideoGrain")) return mp4 && s.videoOwnFrames;
    if (is("VideoHdr")) return mp4; // the Dynamic range dropdown (with Blend samples in HDR)
    if (is("VideoHdrPeak") || is("VideoHdrWhite")) return s.videoCodec >= 2 && s.video10Bit && s.videoOwnFrames && s.videoHdr;
    if (is("TargetFPS")) return s.autoQuality;
    if (is("FsrSharpness")) return s.renderScale < 0.99f;
    if (is("Sharpen")) return s.renderScale >= 0.99f; // FSR sharpening takes its place while scaled
    if (is("TAAJitter")) return s.taa;
    if (is("SMAA")) return s.fxaa;
    if (!strncmp(f.key, "Trail", 5)) return s.neonTrail > 0.0f;
    if (is("WeatherSound")) return s.rain > 0.0f || s.lightning > 0.0f || s.snow > 0.0f;
    if (is("SunAzimuth")) return s.sunElevationOverride >= 0.0f;
    return true;
}

// Choosing a sky brings the settings that make it look right (the planet with the space
// skies, the black hole's light, a darker night for the stars).
void applySkyDefaults(Settings& s, int sky) {
    if (sky == 3 || sky == 5) {
        s.skyEffectSize = 3.0f;
        s.godRays = 1.35f;
        s.godRayDecay = 0.9f;
        if (s.planetSize <= 0.0f) s.planetSize = 1.0f;
        if (sky == 5) {
            s.planetView = 1; // next to the rings
            s.skyNight = 0.5f;
        }
    } else if (sky == 2) {
        s.skyNight = 0.5f;
        s.skyBrightness = 1.0f;
        s.reflections = 1.1f;
    } else if (sky == 4) {
        s.skyRotation = 150.0f;
    }
}

// Ring world seen from on the rings: the planet and the black hole placed for that view (my test,
// 2026-10-17).
void applyOnTheRings(Settings& s) {
    s.skyEffectSize = 4.5f;
    s.planetSize = 1.15f;
    s.planetAzimuth = 20.0f;
    s.planetElevation = -4.0f;
    s.skyRotation = 150.0f;
    s.blackHoleElevation = 25.0f;
    s.skyNight = 0.5f;
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

bool toggle(const char* id, bool* value); // below, with the other widgets

// What a setting does, shown when the mouse rests on it: the labels stay short, the detail is here.
// "(restart)" / "(next map)" say when a change takes effect.
const char* tooltipFor(const char* key) {
    if (g_host)
        for (const auto& t : g_host->tips)
            if (!strcmp(t.first, key)) return t.second;
    static const struct {
        const char* key;
        const char* tip;
    } kTips[] = {
        {"AOStrength", "Darkens corners and creases where light would not reach."},
        {"AORadius", "Reach of the occlusion. Large values darken whole areas, small ones only tight corners."},
        {"SunLight", "How strongly sunlit surfaces take on the sun colour."},
        {"AmbientTint", "How strongly surfaces in shade take on the sky colour."},
        {"SunColor", "The colour of the sunlight."},
        {"SkyColor", "The colour of shade and ambient light."},
        {"SkyBrightness", "Brightness of the custom sky."},
        {"CloudCoverage", "How much of the sky the clouds cover."},
        {"CloudHeight", "Height of the cloud layer. Lower clouds look bigger and closer."},
        {"SkyEnhance", "A richer colour gradient on the game's own sky."},
        {"SunGlow", "The glow and disc around the sun."},
        {"FogDensity", "How thick the haze is."},
        {"FogHeightFalloff", "How fast the haze thins out with height. 0 is the same everywhere."},
        {"FogSunScatter", "How much the haze glows around the sun."},
        {"HazeDistance", "Where the haze closes in. 1 is the default, lower brings it nearer."},
        {"GodRayDecay", "How far the light shafts reach from the sun."},
        {"AutoExposure", "How much the brightness adapts to the scene, like an eye. 0 keeps it fixed."},
        {"ToneCurve", "How bright colours roll off to white."},
        {"Vibrance", "Boosts weak colours more than strong ones. Below 0 mutes them."},
        {"Temperature", "Warmer (orange) or cooler (blue) image."},
        {"Tint", "Shifts the image towards green or magenta."},
        {"Lift", "Brightness of the darkest parts."},
        {"Gamma", "Brightness of the mid-tones."},
        {"Gain", "Brightness of the highlights."},
        {"SplitToning", "Cool shadows, warm highlights."},
        {"Puddles", "Standing water on flat ground. Needs wet roads."},
        {"GrassDetail", "Grass patches and blades close to the camera."},
        {"MowingStripes", "Light and dark mowing stripes on grass."},
        {"Wind", "Slant of the rain, movement of grass and clouds."},
        {"BokehSize", "The largest blur size of depth of field, at 1080p."},
        {"ShadowStrength", "Strength of the added sun shadows. 0 keeps the game's own."},
        {"ShadowLength", "How far shadows reach from each point. Longer is more accurate but slower."},
        {"LongShadows", "Shadows from far-away objects, such as hills and buildings."},
        {"LongShadowRange", "How far the long shadows reach, in metres."},
        {"LongShadowQuality", "Stepped is fast. Every cell is slower but leaves no gaps behind thin walls."},
        {"NeonLight", "Neon blocks light the road and walls around them."},
        {"GlobalIllumination", "Sunlit surfaces light their surroundings."},
        {"GameSunColor", "How much of the sun's colour comes from the map instead of the preset."},
        {"SunElevation", "Overrides the sun's height in degrees. -1 uses the game's sun."},
        {"SunAzimuth", "Overrides the compass direction of the sun."},
        {"SkyNight", "How dark the sky gets at night. -1 picks it from the map."},
        {"SkyEffectSize", "0 turns the black hole off. Above 3 the camera is close to it."},
        {"PlanetSize", "0 turns the planet off."},
        {"PlanetView", "Where the camera sits relative to the planet's rings."},
        {"CloudAmount", "Clouds in the clear sky."},
        {"VolumetricLight", "Haze that shows the sun's shadows as beams."},
        {"HighlightBoost", "How strongly lamps and bright spots glow."},
        {"BorderGlow", "Glow of the neon strips along the road. Needs Game light sources."},
        {"CarHeadlights", "Two spotlights on the front of your car that light the road and show as beams in haze. 1 is full, 0 is off. Works in races and replays."},
        {"CarHeadlightRange", "How far the headlights reach."},
        {"CarHeadlightBeam", "How strongly the light cone shows in the air."},
        {"LampGlare", "Soft glare around lamps. 0 is off. Needs Game light sources."},
        {"LampHaze", "Lamps glow in the haze around them, more with more haze. 1 is full, 0 is off. Needs Game light sources."},
        {"LampSkyHalos", "Lamp halos also shine over the sky at night. Needs Game light sources."},
        {"Floodlights", "Brightness and glow of stadium floodlights, start lights and other lamps. 1 is the default. Needs Game light sources."},
        {"ToneKnee", "Where bright colours start to roll off instead of clipping."},
        {"NightSurfaces", "How bright floodlit surfaces get at night. 1 keeps the game's white."},
        {"Rain", "0 is dry, 2 is a downpour."},
        {"Snow", "0 is none, 2 is a blizzard."},
        {"LensDrops", "Drops and flakes run down the camera lens."},
        {"WeatherShelter", "No rain or snow under roofs and bridges."},
        {"WeatherSound", "Volume of rain and thunder."},
        {"WaterSurfaces", "Reflections on pools and the sea."},
        {"WaterClarity", "Clear water: you see the bottom, deeper water turns blue-green. 0 is the game's murky water."},
        {"WaterSplash", "Spray, ripples and foam where the car enters water, plus a bow wave and drips. 0 is off."},
        {"Reflections", "Reflections on the dry track. Above 1 it turns into a mirror."},
        {"Spray", "Spray from the car's tyres on wet roads and snow."},
        {"CinematicOnlyInReplays", "Motion blur and depth of field only in replays and video export."},
        {"VideoExportBest", "During export: full effect quality, no auto quality, and effects timed to the video so rain and exposure match the picture."},
        {"VideoCodec", "File format of Shoot video. H.264 plays everywhere; HEVC and AV1 make smaller files but are harder to edit. Falls back to H.264 if the encoder is missing."},
        {"VideoQuality", "Picture quality. 80 is close to the source, 92 suits editing, 62 is small. Ignored when a bitrate is set."},
        {"Video10Bit", "10-bit HEVC and AV1 avoid colour bands in skies, fog and glows. H.264 stays 8-bit and gets a fine dither."},
        {"VideoBitrate", "Fixed average bitrate in Mbit/s, for upload limits (Discord: 8 Mbit/s fits 10 s in 10 MB). 0 lets Video quality decide (recommended)."},
        {"VideoEncoder", "Graphics card: fast, and on NVIDIA (driver R570 or newer) the best quality per megabyte. ffmpeg: software encoding, better on AMD or Intel; needs ffmpeg.exe in your TMVS folder or on PATH and can slow the export."},
        {"VideoFfmpegPreset", "Encoder effort. Fast keeps up with any export; Best (slow) packs the most quality per megabyte but the export waits for it."},
        {"VideoOwnFrames", "Blends the export's samples in HDR before tone curve, bloom and grading. Lights streak like on a real camera and the file gets 10 bits or dither. Needed for HDR10 and lens depth of field. Off: the game blends 8-bit frames."},
        {"VideoShutter", "How much of each frame the motion blur covers. 360 blurs the whole frame (smoothest); 180 is the film look, crisper and faster to export. Needs motion blur in Shoot video."},
        {"VideoLensDof", "Depth of field from the samples themselves: true bokeh, no halos at edges. Needs 16 or more samples per frame (high quality 4x4 or more in Shoot video); with fewer, a normal blur."},
        {"VideoGrain", "How much film grain goes into the video (1 = as on screen). Grain costs file size: 0.5 keeps the feel, 0 is smallest."},
        {"VideoAspect", "Crops the video to a wider picture from the centre; the file has no black bars. Pick a larger size in Shoot video for the width you want."},
        {"VideoHdr", "SDR (classic): the game blends the samples itself.\nSDR (HDR blend): the mod blends them before colours are finished: cleaner light streaks, no colour bands. Recommended.\nHDR10: for HDR screens; lights stay brighter than white. Uses 10-bit HEVC. YouTube makes an SDR copy."},
        {"VideoHdrPeak", "Brightest light in nits. 1000 suits most HDR monitors and YouTube."},
        {"VideoHdrWhite", "Brightness of the normal picture's white on an HDR screen, in nits. 203 is the broadcast reference. If the video looks darker than the game, raise it towards your Windows SDR brightness (often 250-300)."},

        {"FocusDistance", "Metres to the sharp point. 0 focuses automatically."},
        {"DofAutofocus", "Where automatic focus sits. Middle: the screen centre. Car: your car, or the centre while it is off screen. A click in photo mode still wins."},
        {"TAA", "Smooths edges by blending frames. Costs a little sharpness."},
        {"TAAJitter", "Shifts the image by sub-pixels each frame for finer edges."},
        {"Sharpen", "Contrast-adaptive sharpening (CAS). Replaced by FSR sharpening when Render scale is below 1.00."},
        {"AutoQuality", "Switches off the costliest effects when FPS falls below the target, and back on when there is room. Light shafts, neon light and long shadows stay. Your settings are kept."},
        {"TargetFPS", "The frame rate auto quality aims for."},
        {"RenderScale", "Renders at this fraction of your screen resolution and scales up with AMD FSR 1; the HUD stays sharp. 1.00 is off. 0.77 costs a little sharpness, 0.67 more, for clearly more FPS. Photos, captures and video export use full resolution. Needs Game HDR."},
        {"FsrSharpness", "FSR sharpening of the scaled image: 0 none, 1 strongest. Replaces Sharpening while Render scale is below 1."},
        {"DisableGameMSAA", "Turns off the game's own MSAA, which the effects don't need. (restart)"},
        {"ReadableGameDepth", "Lets effects work in replays and video export. (restart)"},
        {"GameMaterials", "Reads the game's material ids, for the TMUF sea and the debug view."},
        {"DisableGameFX", "Turns off the game's own bloom and colour filter. (next map)"},
        {"GameNight", "Lights day maps like midnight under night skies."},
        {"EngineMoon", "Moonlight under the black hole sky: from the black hole, the game's own, or off. (next map)"},
        {"GameSkyClouds", "Keeps the game's sunset clouds, and their reflection in the sea, under night skies."},
        {"GameHdr", "Draws in high dynamic range so bright lights keep detail. Captures include it."},
        {"CleanHighlights", "Evens out floodlit white at night. Off: the old look with patchy highlights."},
        {"EngineLights", "Lamps and glows use the game's own light sources. Needs Game HDR."},
        {"CasterShadows", "Long shadows from the game's own models, lit by the sun or black hole. Needs Long shadows."},
        {"CasterAlphaTest", "Fences, grids and lattice towers cast patterned shadows. Off: they cast none."},
        {"CleanWater", "Sharp reflections in pools. Off: blotchy ramp reflections."},
        {"WaterFarReflections", "Pools and the sea also mirror distant stands and masts. Off: near objects only."},
        {"OverlapFix", "TMUnlimiter maps: overlapping blocks at the same height no longer flicker. Engine off shows on top, then boosters, then start, finish and checkpoints, then the block placed last. Works with shaders off too."},
        {"GameGBuffer", "Real surface normals, so AO, shadows and reflections follow bump maps and the car's shape. Needs Game HDR. (restart the first time)"},
        {"GameLight", "Darkens night by dimming the game's own lighting, not the whole image: paint keeps its colour and lamp-lit areas stay bright. Needs Game HDR and Game light sources. (restart the first time)"},
        {"ShadeMenuBars", "Shows the mirrored replay under the replay menu bars. Off: black bars."},
        {"NeonTrail", "A glowing trail behind the car. 0 is off."},
        {"TrailTyres", "Two lines from the rear tyres instead of one."},
        {"TrailDuration", "Seconds until the trail fades. 0 keeps the whole run."},
        {"TrailKeepRuns", "How many earlier runs' trails stay after a restart, dimmer the older they are. 0 clears the trail on every restart."},
        // The plainer ones too, so every setting says something when the pointer rests on it.
        {"SkyMode", "Game sky keeps the game's own sky, the others replace it. Picking one also sets what suits it (planet, black hole light, night darkness)."},
        {"SkyRotation", "Turns stars, black hole, planet and aurora around you."},
        {"StarAmount", "Number and brightness of the stars, and how often shooting stars cross."},
        {"AuroraSpeed", "How fast the aurora's curtains move."},
        {"BlackHoleAzimuth", "Compass direction of the black hole in the sky."},
        {"BlackHoleElevation", "Height of the black hole in the sky."},
        {"PlanetType", "Which planet the sky shows. None turns it off."},
        {"PlanetAzimuth", "Compass direction of the planet in the sky."},
        {"PlanetElevation", "Height of the planet in the sky."},
        {"VolumetricClouds", "3D clouds over any sky, lit by the sun. 0 is off."},
        {"GodRays", "Rays of light around the visible sun."},
        {"Bloom", "Soft glow around the bright parts of the image."},
        {"BloomRadius", "Wider or tighter glow."},
        {"LensFlare", "Ghost reflections of the sun, as in a camera lens."},
        {"ChromaticAberration", "Colour fringes towards the edges of the screen."},
        {"Vignette", "Darker corners."},
        {"FilmGrain", "Fine film noise."},
        {"Exposure", "Overall brightness, in camera stops (EV)."},
        {"Contrast", "The difference between dark and bright parts."},
        {"Saturation", "Strength of all colours. 0 takes them out."},
        {"Wetness", "A darker, glossy, reflective track, as after rain."},
        {"SnowCover", "Snow on everything that faces up. Cars keep their paint."},
        {"Lightning", "How often lightning strikes: a bolt, a flash over the track, thunder."},
        {"ReflectionBlur", "Blurs the dry track's reflection into a soft sheen."},
        {"TrailColor", "The colour of the neon trail."},
        {"TrailWidth", "Width of the trail, in metres."},
        {"MotionBlur", "Blur from camera movement. 1 is one full frame of movement."},
        {"DepthOfField", "Blurs what is in front of and behind the point in focus."},
        {"FXAA", "Smooths edges within one frame: SMAA, or FXAA (a little softer, a little faster)."},
        {"SMAA", "Smoother long edges and sharper textures than FXAA. About 0.2 ms more at 1440p. Off: FXAA."},
        {"Quality", "Samples for AO, shadows, clouds, volumetric light and bounce light. Low is clearly faster."},
        // Sharing presets as text (Shaders page).
        {"ShareCopy", "Copies the current look as a line of text (TMVS1:...) to share. Machine settings (auto quality, sound volume, neon trail) are not included. Works in TrackMania Nations/United and TrackMania 2."},
        {"SharePaste", "Applies a share code from the clipboard as an unsaved look; Save as preset keeps it. Saved presets are not changed. Unknown settings are ignored, out-of-range values are clamped."},
        // Photo mode (General, not a look setting).
        {"SplitPhotoLabels", "Adds both sides' names to the bottom of split photos."},
        // TrackMania 2 (maniaplanet.ini, the host's own panel).
        {"BlackHoleMoon", "Brightness of the black hole's light on night maps, with Game moonlight set to Black hole. 1 is as bright as the moon was. Shadows: the game's moon shadows where the black hole stands near the moon, otherwise only short contact shadows."},
    };
    for (const auto& t : kTips)
        if (!strcmp(t.key, key)) return t.tip;
    return nullptr;
}

// A tooltip that wraps instead of running off the screen.
void showTip(const char* text) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// One setting as a widget. Returns true when the look changed (-> preset becomes Custom).
bool drawField(const Field& f, Settings& s, const char* label = nullptr) {
    if (!supported(f)) return false;
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
    bool hovered = false;
    switch (!strcmp(f.key, "VideoHdr") ? Field::Int : f.kind) {
        case Field::Bool:
            // A switch, like the rest of the menu.
            changed |= toggle("##on", reinterpret_cast<bool*>(base + f.offset));
            hovered = ImGui::IsItemHovered();
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            hovered |= ImGui::IsItemHovered();
            break;
        case Field::Int: {
            int* value = reinterpret_cast<int*>(base + f.offset);
            if (!strcmp(f.key, "VideoHdr")) {
                // One dropdown for two switches (Blend samples in HDR, HDR10 video): which picture the file gets.
                static const char* kRanges[] = {"SDR (classic)", "SDR (HDR blend)", "HDR10"};
                int choice = !s.videoOwnFrames ? 0 : (s.videoHdr && s.videoCodec >= 2 && s.video10Bit) ? 2 : 1;
                if (ImGui::Combo(label, &choice, kRanges, IM_ARRAYSIZE(kRanges))) {
                    s.videoOwnFrames = choice > 0;
                    s.videoHdr = choice == 2;
                    if (choice == 2) {
                        if (s.videoCodec < 2) s.videoCodec = 2; // HDR10 needs HEVC or AV1
                        s.video10Bit = true;
                    }
                    changed = true;
                }
            } else if (!strcmp(f.key, "Quality")) {
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
            } else if (!strcmp(f.key, "ToneCurve")) {
                static const char* kCurves[] = {"The game's", "TMVS (keeps hue)", "ACES (film)"};
                changed |= ImGui::Combo(label, value, kCurves, IM_ARRAYSIZE(kCurves));
            } else if (!strcmp(f.key, "VideoCodec")) {
                static const char* kCodecs[] = {"Game's AVI", "MP4 (H.264)", "MP4 (HEVC)", "MP4 (AV1)"};
                changed |= ImGui::Combo(label, value, kCodecs, IM_ARRAYSIZE(kCodecs));
            } else if (!strcmp(f.key, "VideoEncoder")) {
                static const char* kEncoders[] = {"Graphics card", "ffmpeg (software)"};
                changed |= ImGui::Combo(label, value, kEncoders, IM_ARRAYSIZE(kEncoders));
            } else if (!strcmp(f.key, "VideoFfmpegPreset")) {
                static const char* kSpeeds[] = {"Fast", "Balanced", "Best (slow)"};
                changed |= ImGui::Combo(label, value, kSpeeds, IM_ARRAYSIZE(kSpeeds));
            } else if (!strcmp(f.key, "VideoAspect")) {
                static const char* kAspects[] = {"As shot", "2:1", "2.39:1 (cinema)"};
                changed |= ImGui::Combo(label, value, kAspects, IM_ARRAYSIZE(kAspects));
            } else if (!strcmp(f.key, "DofAutofocus")) {
                static const char* kFocusModes[] = {"Middle", "Car"};
                changed |= ImGui::Combo(label, value, kFocusModes, IM_ARRAYSIZE(kFocusModes));
            } else if (!strcmp(f.key, "EngineMoon") && g_host && g_host->engineMoon) {
                // TrackMania 2: what its choices can do there (maniaplanet/src/overlay.cpp).
                changed |= g_host->engineMoon(s, label);
            } else if (!strcmp(f.key, "EngineMoon")) {
                static const char* kMoons[] = {"Game's", "Black hole", "Off"};
                changed |= ImGui::Combo(label, value, kMoons, IM_ARRAYSIZE(kMoons));
            } else if (!strcmp(f.key, "LongShadowQuality")) {
                static const char* kLongQualities[] = {"Stepped", "Every cell"};
                changed |= ImGui::Combo(label, value, kLongQualities, IM_ARRAYSIZE(kLongQualities));
            } else if (!strcmp(f.key, "PlanetView")) {
                const bool picked = ImGui::Combo(label, value, kPlanetViews, IM_ARRAYSIZE(kPlanetViews));
                if (picked && *value == 2 && s.skyMode == 5) applyOnTheRings(s);
                changed |= picked;
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
    if (f.kind != Field::Bool || !strcmp(f.key, "VideoHdr")) hovered = ImGui::IsItemHovered();
    if (hovered) {
        if (const char* tip = tooltipFor(f.key)) showTip(tip);
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

bool containsNoCase(const char* text, const char* query) {
    for (; *text; text++) {
        size_t i = 0;
        while (query[i] && text[i] && tolower(static_cast<unsigned char>(text[i])) == tolower(static_cast<unsigned char>(query[i]))) i++;
        if (!query[i]) return true;
    }
    return false;
}

// --- Widgets -------------------------------------------------------------------

// New pages last: the page's number is saved (MenuPage); the bar shows Video and Resource packs
// before Settings.
enum Page { kShaders, kCustomize, kStudio, kMaps, kPerformance, kSettings, kAbout, kVideo, kPacks, kMisc, kPageCount };

const ImVec4 kAccent(1.00f, 0.62f, 0.22f, 1.0f);

ImU32 colour(const ImVec4& c, float alpha) {
    return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
}

// An on/off switch: a pill with a knob that slides over.
bool toggle(const char* id, bool* value) {
    const float h = ImGui::GetFrameHeight(), w = h * 1.8f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    if (clicked) *value = !*value;
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetItemID();
    float t = storage->GetFloat(key, *value ? 1.0f : 0.0f);
    t += ((*value ? 1.0f : 0.0f) - t) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
    storage->SetFloat(key, t);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec4 off(0.25f, 0.26f, 0.31f, 1.0f);
    const ImVec4 track(off.x + (kAccent.x - off.x) * t, off.y + (kAccent.y - off.y) * t, off.z + (kAccent.z - off.z) * t, 1.0f);
    draw->AddRectFilled(p, ImVec2(p.x + w, p.y + h), colour(track, ImGui::IsItemHovered() ? 1.0f : 0.85f), h * 0.5f);
    draw->AddCircleFilled(ImVec2(p.x + h * 0.5f + t * (w - h), p.y + h * 0.5f), h * 0.5f - 3.0f, IM_COL32(255, 255, 255, 240));
    return clicked;
}

// Keeps the next button on this line when it fits, else starts a new line.
void sameLineIfRoom(const char* nextLabel) {
    const float need = ImGui::CalcTextSize(nextLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < need) ImGui::NewLine();
}

// A switch with its label in front and a tooltip. Returns true when flipped.
// A hotkey's current name for texts ("F8", "Shift+F6"), "no key" if the player turned it off.
std::string keyText(keys::Action a) {
    const keys::Binding b = keys::get(a);
    return b.vk ? keys::name(b) : std::string("no key");
}

// Set while the Hotkeys panel is drawn: a key capture whose panel is gone (other page, menu
// closed) is cancelled at the end of the frame, else the keyboard would stay captured.
bool g_keysShown = false;

// The Hotkeys panel: one row per action, Change waits for the next key press (Esc cancels).
void drawHotkeys() {
    g_keysShown = true;
    ImGui::TextDisabled("Press the new key (Esc cancels). Letters need Ctrl or Alt.");
    if (ImGui::BeginTable("hotkeys", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthFixed, 80.0f * g_scale);
        for (int i = 0; i < static_cast<int>(keys::Action::Count); i++) {
            const keys::Action a = static_cast<keys::Action>(i);
            if (!keys::info(a).available) continue;
            const bool mine = keys::capturing() && keys::captureTarget() == a;
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(keys::info(a).label);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (mine) ImGui::TextColored(kAccent, "Press a key...");
            else ImGui::TextUnformatted(keyText(a).c_str());
            ImGui::TableNextColumn();
            if (ImGui::Button(mine ? "Cancel" : "Change", ImVec2(-FLT_MIN, 0.0f))) {
                if (mine) keys::cancelCapture();
                else keys::beginCapture(a);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!keys::note().empty()) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "%s", keys::note().c_str());
    ImGui::BeginDisabled(keys::capturing());
    if (ImGui::Button("Reset to defaults")) keys::resetDefaults();
    ImGui::EndDisabled();
}

bool switchRow(const char* label, bool* value, const char* tooltip) {
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight() * 1.8f));
    const bool flipped = toggle("switch", value);
    if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    if (flipped) Config::get().markDirty();
    return flipped;
}

// A page title with a line of explanation under it.
void pageTitle(const char* title, const char* hint) {
    ImGui::PushFont(g_fontTitle);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::TextDisabled("%s", hint);
    ImGui::Spacing();
}

// A rounded chip that is either on or off (the filters of the shader page).
bool chip(const char* label, bool on) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight() * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.85f) : ImVec4(0.16f, 0.17f, 0.21f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? kAccent : ImVec4(0.22f, 0.23f, 0.28f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, on ? ImVec4(0.08f, 0.06f, 0.04f, 1.0f) : ImGui::GetStyle().Colors[ImGuiCol_Text]);
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    return clicked;
}

// A yes/no question in the middle of the screen, opened with ImGui::OpenPopup(title).
// True when answered with `yes`.
bool confirm(const char* title, const char* question, const char* yes) {
    bool answered = false;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * g_scale, 14.0f * g_scale));
    if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextUnformatted(question);
        ImGui::Spacing();
        if (ImGui::Button(yes, ImVec2(120.0f * g_scale, 0.0f))) {
            answered = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.23f, 0.28f, 1.0f));
        if (ImGui::Button("Cancel", ImVec2(120.0f * g_scale, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    return answered;
}

// --- Presets as a gallery --------------------------------------------------------

// What the menu shows about a preset: its look's GPU load and what kind of look it is,
// worked out from its values (works for your own presets too).
struct LookInfo {
    int cost = 2; // 1 light, 2 medium, 3 heavy
    bool night = false;
    bool weather = false;
    ImVec4 sky, sun; // the picture's stand-in when there is none
    std::string description; // your preset's own line ("" = none)
    DWORD read = 0;          // when it was worked out
};

LookInfo describeLook(const Settings& s) {
    LookInfo info;
    // Your own presets: the expensive effects, roughly by what they cost (light shafts and
    // the volumetrics most). The built-in presets have their measured load.
    float c = 0.0f;
    if (s.godRays > 0.0f) c += 3.0f;
    if (s.volumetricLight > 0.0f) c += 2.0f;
    if (s.volumetricClouds > 0.0f) c += 2.0f;
    if (s.globalIllumination > 0.0f) c += 1.5f;
    if (s.reflections > 0.0f) c += 1.5f;
    if (s.rain > 0.0f || s.snow > 0.0f) c += 1.5f;
    if (s.longShadows > 0.0f) c += 1.0f;
    if (s.depthOfField > 0.0f || s.motionBlur > 0.0f) c += 1.0f;
    if (s.quality >= 2) c += 1.0f;
    info.cost = c < 4.0f ? 1 : (c < 8.0f ? 2 : 3);
    info.night = (s.skyMode >= 2 && s.skyMode <= 5) || s.skyNight > 0.3f;
    info.weather = s.rain > 0.0f || s.snow > 0.0f || s.wetness > 0.5f;
    info.sky = ImVec4(s.skyColor[0] * 0.6f, s.skyColor[1] * 0.6f, s.skyColor[2] * 0.6f, 1.0f);
    info.sun = ImVec4(s.sunColor[0] * 0.45f, s.sunColor[1] * 0.45f, s.sunColor[2] * 0.45f, 1.0f);
    if (info.night) info.sky = ImVec4(0.05f, 0.06f, 0.14f, 1.0f);
    return info;
}

// The built-in presets: a line about each, and its GPU load as measured with the previewer
// (--bench, two race captures at 2560x1440 on a GTX 1080 Ti, 1.4.1: light < 5 ms, medium < 6.3 ms,
// heavy above; docs/performance.md).
struct BuiltInInfo {
    const char* name;
    const char* blurb;
    int cost;
};
const BuiltInInfo kBuiltIn[] = {
    {"Vibrant", "The default. Warm sun, rich colours, glowing borders, light shafts.", 2},
    {"Realistic", "The game's own colours and sky; only the light is new.", 2},
    {"Golden Hour", "Clear sky, low warm sun, long light shafts.", 3},
    {"Dreamy", "Soft pastel bloom, pink and teal.", 2},
    {"Neon", "Starry night, bright neon that lights up the whole track.", 2},
    {"Horizon", "A glowing black hole and a ringed planet over a dark stadium.", 3},
    {"Aurora", "Northern lights over the track, green and teal.", 3},
    {"Competition", "Clarity first: shadows and depth, no haze or lens effects.", 1},
    {"Performance", "The Vibrant look for weaker GPUs: fewer samples, no shafts.", 1},
    {"Rainy", "Overcast, wet track, puddles and rain.", 3},
    {"Replay Cinema", "Film look with motion blur and depth of field, for replays.", 3},
    {"Storm", "Low clouds, pouring rain, deep puddles, lightning and thunder.", 3},
    {"Snowstorm", "A blizzard: driving snow and thick haze close around you.", 2},
    {"Horror", "Black night in thick fog. Only your headlights light the road.", 2},
};

const BuiltInInfo* builtInInfo(const std::string& name) {
    for (const BuiltInInfo& b : kBuiltIn) {
        if (name == b.name) return &b;
    }
    return nullptr;
}

const char* presetBlurb(const std::string& name, const std::string& description) {
    if (const BuiltInInfo* b = builtInInfo(name)) return b->blurb;
    return description.empty() ? "Right-click to add a description." : description.c_str();
}

// Looks of the presets, worked out once. Your presets are read from their files again every
// few seconds: they can be changed or dropped into the folder while the game runs.
std::map<std::string, LookInfo> g_looks;

const LookInfo& lookInfo(const std::string& name) {
    Config& config = Config::get();
    auto it = g_looks.find(name);
    const bool own = config.isUserPreset(name);
    if (it != g_looks.end() && (!own || GetTickCount() - it->second.read < 3000)) return it->second;
    Settings s = config.settings;
    config.presetSettings(name, s);
    LookInfo info = describeLook(s);
    if (const BuiltInInfo* b = builtInInfo(name)) info.cost = b->cost;
    info.description = config.presetDescription(name);
    info.read = GetTickCount();
    return g_looks[name] = info;
}

struct CardState {
    std::string hovered;   // the card under the pointer this frame
    std::string resting;   // ... and since when it has been there
    double restingSince = 0.0;
};
CardState g_cards;

// One preset as a card: its picture with the name on it, a line about it underneath.
// Returns true when clicked.
bool presetCard(const std::string& name, bool active, float width) {
    Config& config = Config::get();
    const LookInfo& info = lookInfo(name);
    ImGuiStyle& style = ImGui::GetStyle();
    const float imageH = width * 9.0f / 16.0f;
    const float pad = 8.0f * g_scale;
    const float textH = ImGui::GetFontSize() * 2.0f + pad * 2.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 q(p.x + width, p.y + imageH + textH);
    ImGui::PushID(name.c_str());
    const bool clicked = ImGui::InvisibleButton("card", ImVec2(width, imageH + textH));
    const bool hovered = ImGui::IsItemHovered();
    const float rounding = 8.0f * g_scale;
    ImDrawList* draw = ImGui::GetWindowDrawList();

    draw->AddRectFilled(p, q, hovered ? IM_COL32(38, 40, 50, 255) : IM_COL32(26, 28, 35, 255), rounding);
    const ImVec2 imageEnd(q.x, p.y + imageH);
    if (const ImTextureID texture = picture(name)) {
        draw->AddImageRounded(texture, p, imageEnd, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, rounding,
                              ImDrawFlags_RoundCornersTop);
    } else {
        // No picture (your preset before its first one): its sky over its sun light.
        draw->AddRectFilledMultiColor(ImVec2(p.x, p.y + rounding), imageEnd, colour(info.sky, 1.0f), colour(info.sky, 1.0f),
                                      colour(info.sun, 1.0f), colour(info.sun, 1.0f));
        draw->AddRectFilled(p, ImVec2(q.x, p.y + rounding + 1.0f), colour(info.sky, 1.0f), rounding, ImDrawFlags_RoundCornersTop);
        const char* none = "No picture yet";
        const ImVec2 size = ImGui::CalcTextSize(none);
        draw->AddText(ImVec2(p.x + (width - size.x) * 0.5f, p.y + (imageH - size.y) * 0.5f), IM_COL32(255, 255, 255, 110), none);
    }
    // The name on a dark fade at the bottom of the picture.
    const float fade = imageH * 0.45f;
    draw->AddRectFilledMultiColor(ImVec2(p.x, imageEnd.y - fade), imageEnd, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 190),
                                  IM_COL32(0, 0, 0, 190));
    ImGui::PushFont(g_fontTitle);
    draw->AddText(ImVec2(p.x + pad, imageEnd.y - ImGui::GetFontSize() - pad * 0.7f), IM_COL32_WHITE, name.c_str());
    ImGui::PopFont();

    // GPU load: three dots in the top right corner.
    const float dot = 3.2f * g_scale, gap = 9.0f * g_scale;
    const ImVec2 badge(q.x - pad - gap * 3.0f - 4.0f * g_scale, p.y + pad);
    draw->AddRectFilled(badge, ImVec2(q.x - pad, badge.y + gap + 6.0f * g_scale), IM_COL32(0, 0, 0, 150), gap);
    for (int i = 0; i < 3; i++) {
        const ImVec2 c(badge.x + 2.0f * g_scale + gap * (i + 0.5f) + 2.0f * g_scale, badge.y + (gap + 6.0f * g_scale) * 0.5f);
        draw->AddCircleFilled(c, dot, i < info.cost ? colour(kAccent, 1.0f) : IM_COL32(255, 255, 255, 85));
    }

    if (hovered) g_cards.hovered = name;
    // Resting on a card for a moment shows it in the game (passing over doesn't flicker).
    if (hovered && !active && config.hoverPreview && g_cards.resting == name && ImGui::GetTime() - g_cards.restingSince > 0.3) {
        config.previewPreset = name;
    }

    // Active, or shown in the game right now.
    const bool live = config.previewPreset == name;
    if (active || live) {
        const char* tag = active ? "ACTIVE" : "PREVIEW";
        const ImVec2 size = ImGui::CalcTextSize(tag);
        const ImVec2 a(p.x + pad, p.y + pad), b(a.x + size.x + 12.0f * g_scale, a.y + size.y + 4.0f * g_scale);
        draw->AddRectFilled(a, b, active ? colour(kAccent, 1.0f) : IM_COL32(60, 170, 255, 230), (b.y - a.y) * 0.5f);
        draw->AddText(ImVec2(a.x + 6.0f * g_scale, a.y + 2.0f * g_scale), IM_COL32(15, 12, 8, 255), tag);
    }

    // Two lines about it.
    const ImVec4 clip(p.x + pad, imageEnd.y + pad, q.x - pad, q.y - pad * 0.5f);
    draw->AddText(nullptr, 0.0f, ImVec2(clip.x, clip.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), presetBlurb(name, info.description), nullptr,
                  width - pad * 2.0f, &clip);

    if (active) {
        draw->AddRect(p, q, colour(kAccent, 1.0f), rounding, 0, 2.0f * g_scale);
    } else if (hovered) {
        draw->AddRect(p, q, IM_COL32(255, 255, 255, 90), rounding, 0, 1.5f * g_scale);
    }
    (void)style;

    // Right click: which maps it is for, and your preset's line and picture.
    bool editDescription = false, askDelete = false;
    if (ImGui::BeginPopupContextItem("card")) {
        static const char* const kMoods[] = {"Use on day maps", "Use on sunset maps", "Use on night maps"};
        for (int m = 0; m < static_cast<int>(Mood::Count); m++) {
            if (ImGui::MenuItem(kMoods[m], nullptr, config.moodPreset[m] == name)) {
                config.moodPreset[m] = name;
                config.autoMood = true;
                if (static_cast<int>(config.mood) == m) config.selectPreset(name);
                config.resumeMoodPresets();
                config.markDirty();
            }
        }
        if (config.isUserPreset(name)) {
            ImGui::Separator();
            if (ImGui::MenuItem("Edit description")) editDescription = true;
            if (ImGui::MenuItem("Update picture", nullptr, false, active)) config.requestPicture(name);
            if (!active && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Apply the preset first to update its picture.");
            if (ImGui::MenuItem("Delete")) askDelete = true;
        }
        ImGui::EndPopup();
    }
    if (askDelete) ImGui::OpenPopup("Delete preset?");
    if (confirm("Delete preset?", ("Delete \"" + name + "\"? This can't be undone.").c_str(), "Delete")) config.deleteUserPreset(name);
    // Opened after the context menu closed (a popup opened from inside it would close with it).
    static char s_description[160] = "";
    if (editDescription) {
        snprintf(s_description, sizeof(s_description), "%s", info.description.c_str());
        ImGui::OpenPopup("describe");
    }
    if (ImGui::BeginPopup("describe")) {
        ImGui::Text("Description of %s", name.c_str());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(360.0f * g_scale);
        const bool enter = ImGui::InputTextWithHint("##description", "Description", s_description, 141,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Save") || enter) {
            config.setPresetDescription(name, s_description);
            g_looks.erase(name);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return clicked;
}

// "Save the current look as a preset": the name, then saved (the picture follows).
void savePresetPopup(Config& config) {
    static char name[48] = "";
    static char description[160] = "";
    if (ImGui::BeginPopup("save preset")) {
        ImGui::TextUnformatted("Save preset");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(360.0f * g_scale);
        bool enter = ImGui::InputTextWithHint("##name", "name", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetNextItemWidth(360.0f * g_scale);
        enter |= ImGui::InputTextWithHint("##description", "Description (optional)", description, 141, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextDisabled("The current game view becomes its picture.");
        if ((ImGui::Button("Save") || enter) && config.saveUserPreset(name, description)) {
            g_looks.erase(name);
            name[0] = '\0';
            description[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// The "+" card at the end of your presets.
void addCard(float width) {
    const float pad = 8.0f * g_scale;
    const float h = width * 9.0f / 16.0f + ImGui::GetFontSize() * 2.0f + pad * 2.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("add", ImVec2(width, h))) ImGui::OpenPopup("save preset");
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 line = hovered ? colour(kAccent, 1.0f) : IM_COL32(255, 255, 255, 70);
    draw->AddRect(p, ImVec2(p.x + width, p.y + h), line, 8.0f * g_scale, 0, 1.5f * g_scale);
    const ImVec2 c(p.x + width * 0.5f, p.y + h * 0.42f);
    const float arm = 14.0f * g_scale;
    draw->AddLine(ImVec2(c.x - arm, c.y), ImVec2(c.x + arm, c.y), line, 3.0f * g_scale);
    draw->AddLine(ImVec2(c.x, c.y - arm), ImVec2(c.x, c.y + arm), line, 3.0f * g_scale);
    const char* label = "New preset";
    const ImVec2 size = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(c.x - size.x * 0.5f, c.y + arm + 10.0f * g_scale), line, label);
}

// Cards in rows that fill the page.
template <typename Draw>
void cardGrid(int count, Draw drawCard) {
    const float spacing = 12.0f * g_scale;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>((avail + spacing) / (215.0f * g_scale + spacing)));
    const float width = std::floor((avail - spacing * (columns - 1)) / columns);
    for (int i = 0; i < count; i++) {
        if (i % columns != 0) ImGui::SameLine(0.0f, spacing);
        drawCard(i, width);
        if (i % columns == columns - 1 || i == count - 1) ImGui::Dummy(ImVec2(0.0f, spacing * 0.25f));
    }
}

enum Filter { kAll, kDay, kNight, kWeather, kLight, kFilterCount };

bool passes(int filter, const LookInfo& info) {
    switch (filter) {
        case kDay: return !info.night;
        case kNight: return info.night;
        case kWeather: return info.weather;
        case kLight: return info.cost == 1;
        default: return true;
    }
}

void drawShaders(Config& config) {
    // Presets put into the folder (or deleted there) show up while the menu is open.
    static DWORD s_scanned = 0;
    if (GetTickCount() - s_scanned > 2000) {
        s_scanned = GetTickCount();
        config.rescanUserPresets();
    }
    pageTitle("Shaders", "Hover to preview, click to apply, right-click for more.");
    static int s_filter = kAll;
    static const char* const kFilters[] = {"All", "Day", "Night", "Weather", "Lightweight"};
    for (int f = 0; f < kFilterCount; f++) {
        if (f) ImGui::SameLine();
        if (chip(kFilters[f], s_filter == f)) s_filter = f;
    }
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight() * 1.8f - ImGui::CalcTextSize("Live preview").x - 12.0f * g_scale);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Live preview");
    ImGui::SameLine();
    if (toggle("live", &config.hoverPreview)) config.markDirty();
    ImGui::Spacing();

    // Your changes on top of a preset: offer to keep them.
    if (config.preset == kCustomPreset) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.12f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * g_scale);
        ImGui::BeginChild("custom", ImVec2(0.0f, ImGui::GetFrameHeight() + 16.0f * g_scale), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Unsaved changes to %s.", config.basePreset().c_str());
        if (config.isUserPreset(config.basePreset())) {
            ImGui::SameLine();
            if (ImGui::Button("Save changes")) config.saveUserPreset(config.basePreset());
        }
        ImGui::SameLine();
        if (ImGui::Button("Save as preset")) ImGui::OpenPopup("save preset");
        savePresetPopup(config);
        ImGui::SameLine();
        if (ImGui::Button("Discard")) ImGui::OpenPopup("Discard changes?");
        if (confirm("Discard changes?", ("Discard all changes to " + config.basePreset() + "?").c_str(), "Discard")) {
            config.selectPreset(config.basePreset());
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    std::vector<std::string> builtIn;
    for (int i = 0; i < static_cast<int>(Preset::Custom); i++) {
#ifdef TMVS_MANIAPLANET
        if (i == static_cast<int>(Preset::Horror)) continue; // its headlights are TrackMania Nations/United's so far
#endif
        const std::string name = presetName(static_cast<Preset>(i));
        if (passes(s_filter, lookInfo(name))) builtIn.push_back(name);
    }
    const std::string current = config.mapLookActive() ? std::string() : config.preset;
    cardGrid(static_cast<int>(builtIn.size()), [&](int i, float width) {
        if (presetCard(builtIn[i], builtIn[i] == current, width)) config.selectPreset(builtIn[i]);
    });
    if (builtIn.empty()) ImGui::TextDisabled("No presets match this filter.");

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::SeparatorText("Your presets");
    ImGui::PopStyleColor();
    std::vector<std::string> own;
    for (const std::string& name : config.userPresets()) {
        if (passes(s_filter, lookInfo(name))) own.push_back(name);
    }
    cardGrid(static_cast<int>(own.size()) + 1, [&](int i, float width) {
        if (i == static_cast<int>(own.size())) {
            addCard(width);
        } else if (presetCard(own[i], own[i] == current, width)) {
            config.selectPreset(own[i]);
        }
    });
    savePresetPopup(config);
    // Sharing: a preset is its .ini (and .jpg) in this folder; dropped-in ones appear by themselves.
    if (ImGui::Button("Open presets folder")) {
        CreateDirectoryW(config.presetDir().c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", config.presetDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opens Explorer. In fullscreen the game may minimise.");
    ImGui::SameLine();
    ImGui::TextDisabled("Each preset is a .ini file plus a .jpg picture. Copy both to share it.");

    // Sharing as text: one line to paste into a chat (the same code reads in both games).
    static std::string s_shareStatus;
    static bool s_shareOk = true;
    if (ImGui::Button("Copy share code")) {
        s_shareOk = copyTextToClipboard(config.shareCode());
        s_shareStatus = s_shareOk ? "Share code copied." : "The clipboard is busy. Try again.";
    }
    if (ImGui::IsItemHovered()) showTip(tooltipFor("ShareCopy"));
    ImGui::SameLine();
    if (ImGui::Button("Paste share code")) {
        std::string text;
        s_shareOk = textFromClipboard(text, s_shareStatus) && config.applyShareCode(text, s_shareStatus);
    }
    if (ImGui::IsItemHovered()) showTip(tooltipFor("SharePaste"));
    if (!s_shareStatus.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, s_shareOk ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : ImVec4(1.0f, 0.5f, 0.4f, 1.0f));
        ImGui::TextUnformatted(s_shareStatus.c_str());
        ImGui::PopStyleColor();
    }
}

// --- Panels: settings side by side ---------------------------------------------------

// A group of settings in a rounded box with its title in the group's colour. Returns
// whether the look changed. `rows` = about how many settings it shows (for the layout).
struct PanelDef {
    std::string title;
    float rows;
    std::function<bool()> body;
    bool foldable = false; // the title opens and closes it (closed at first)
    bool marked = false;   // something in it differs from the preset (a dot on the title)
};

// Which foldable panels are open (by title), for this game session.
std::map<std::string, bool> g_panelOpen;

bool panelOpen(const PanelDef& def) {
    if (!def.foldable) return true;
    auto it = g_panelOpen.find(def.title);
    return it != g_panelOpen.end() && it->second;
}

bool panel(const PanelDef& def) {
    SectionStyle style(def.title.c_str());
    const ImVec4 c = sectionColour(def.title.c_str());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.13f, 0.165f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.07f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * g_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * g_scale, 10.0f * g_scale));
    ImGui::BeginChild(def.title.c_str(), ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollWithMouse); // the wheel scrolls the page, not the (never scrolling) panel
    // Title with a short bar in the group's colour; a foldable panel opens and closes on it.
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = g_fontTitle->FontSize * 0.8f;
    const bool open = panelOpen(def);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (def.foldable) {
        const float w = ImGui::GetContentRegionAvail().x;
        if (ImGui::InvisibleButton("fold", ImVec2(w, h + 2.0f * g_scale))) g_panelOpen[def.title] = !open;
        if (ImGui::IsItemHovered()) draw->AddRectFilled(ImVec2(p.x - 6.0f * g_scale, p.y - 3.0f * g_scale), ImVec2(p.x + w + 6.0f * g_scale, p.y + h + 5.0f * g_scale),
                                                        IM_COL32(255, 255, 255, 12), 5.0f * g_scale);
        // A chevron on the right: pointing down when open.
        const ImVec2 m(p.x + w - 8.0f * g_scale, p.y + h * 0.55f);
        const float a = 4.5f * g_scale;
        const ImU32 chevron = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        if (open) {
            draw->AddTriangleFilled(ImVec2(m.x - a, m.y - a * 0.5f), ImVec2(m.x + a, m.y - a * 0.5f), ImVec2(m.x, m.y + a * 0.6f), chevron);
        } else {
            draw->AddTriangleFilled(ImVec2(m.x - a * 0.5f, m.y - a), ImVec2(m.x - a * 0.5f, m.y + a), ImVec2(m.x + a * 0.6f, m.y), chevron);
        }
    } else {
        ImGui::Dummy(ImVec2(0.0f, h + 2.0f * g_scale));
    }
    draw->AddRectFilled(ImVec2(p.x, p.y + h * 0.1f), ImVec2(p.x + 3.0f * g_scale, p.y + h * 0.95f), colour(c, 1.0f), 2.0f);
    draw->AddText(g_fontTitle, h, ImVec2(p.x + 10.0f * g_scale, p.y), colour(c, 1.0f), def.title.c_str());
    if (def.marked) {
        // Changed settings inside: a dot after the title.
        const float tw = g_fontTitle->CalcTextSizeA(h, FLT_MAX, 0.0f, def.title.c_str()).x;
        draw->AddCircleFilled(ImVec2(p.x + 10.0f * g_scale + tw + 8.0f * g_scale, p.y + h * 0.55f), 3.0f * g_scale, colour(kAccent, 1.0f));
    }
    bool changed = false;
    if (open) {
        ImGui::Dummy(ImVec2(0.0f, 2.0f * g_scale));
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.52f);
        changed = def.body();
        ImGui::PopItemWidth();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    return changed;
}

// Panels in as many columns as fit (at least `minWidth` each). Each panel goes to the column
// that is shortest so far, so the columns end at about the same height.
bool panelColumns(const std::vector<PanelDef>& panels, float minWidth) {
    if (panels.empty()) return false;
    const float spacing = 12.0f * g_scale;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int columns = std::clamp(static_cast<int>((avail + spacing) / (minWidth * g_scale + spacing)), 1, static_cast<int>(panels.size()));
    std::vector<std::vector<int>> stacks(columns);
    std::vector<float> heights(columns, 0.0f);
    for (int i = 0; i < static_cast<int>(panels.size()); i++) {
        const int c = static_cast<int>(std::min_element(heights.begin(), heights.end()) - heights.begin());
        stacks[c].push_back(i);
        heights[c] += (panelOpen(panels[i]) ? panels[i].rows : 0.0f) + 2.5f;
    }
    bool changed = false;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(spacing * 0.5f, 0.0f));
    if (ImGui::BeginTable("panels", columns, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        for (int c = 0; c < columns; c++) {
            ImGui::TableSetColumnIndex(c);
            for (int i : stacks[c]) {
                changed |= panel(panels[i]);
                ImGui::Dummy(ImVec2(0.0f, spacing * 0.5f));
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    return changed;
}

// A panel of settings by key, with the menu's own labels; only what the current choices use.
using Entries = std::vector<std::pair<const char*, const char*>>;

PanelDef settingsPanel(const char* title, Settings& s, const Entries& entries) {
    float rows = 0.0f;
    for (const auto& e : entries) {
        const Field* f = findField(e.first);
        // Only what this game can do (ManiaPlanet has no long shadows, FXAA, water ...): the
        // panel's height had room for rows that were never drawn.
        if (f && supported(*f) && isRelevant(*f, s)) rows += 1.0f;
    }
    return {title, rows, [&s, entries] {
                bool l = false;
                for (const auto& e : entries) l |= drawKey(s, e.first, e.second);
                return l;
            }};
}

// --- The other pages ---------------------------------------------------------------

// On top of every preset (not part of one): a colour look from Resolve, Lightroom, ...
PanelDef lutPanel(Config& config) {
    return {"Colour look (LUT)", 3.0f, [&config] {
                          ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
                          if (ImGui::BeginCombo("Look", config.lutFile.empty() ? "None" : config.lutFile.c_str())) {
                              if (ImGui::Selectable("None", config.lutFile.empty())) {
                                  config.lutFile.clear();
                                  config.markDirty();
                              }
                              for (const std::string& name : lut::list()) {
                                  if (ImGui::Selectable(name.c_str(), name == config.lutFile)) {
                                      config.lutFile = name;
                                      config.markDirty();
                                  }
                              }
                              ImGui::EndCombo();
                          }
                          if (ImGui::IsItemHovered()) ImGui::SetTooltip("3D .cube files in the LUT folder, applied after the preset's own colours.");
                          if (!config.lutFile.empty()) {
                              ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
                              if (ImGui::SliderFloat("Amount", &config.lutAmount, 0.0f, 1.0f, "%.2f")) config.markDirty();
                          }
                          if (ImGui::Button("Open LUT folder")) {
                              CreateDirectoryW(lut::folder().c_str(), nullptr);
                              ShellExecuteW(nullptr, L"open", lut::folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                          }
                          return false;
                      }};
}

// What a player wants to change; every setting is in Studio.
bool drawCustomize(Config& config, Settings& s) {
    pageTitle("Customize", "The settings most players want. Every setting is in Studio.");
    std::vector<PanelDef> panels;
    // In the order of Studio's groups, so the two pages read alike.
    panels.push_back(settingsPanel("Light and shadows", s,
                                   {{"ShadowStrength", "Shadows"},
                                    {"LongShadows", "Long shadows"},
                                    {"AOStrength", "Ambient occlusion"},
                                    {"NeonLight", "Neon light spill"},
                                    {"CarHeadlights", "Car headlights"},
                                    {"GodRays", "Light shafts"}}));
    panels.push_back(settingsPanel("Sky", s,
                                   {{"SkyMode", "Sky"},
                                    {"SkyRotation", "Turn the sky"},
                                    {"StarAmount", "Stars"},
                                    {"AuroraSpeed", "Aurora movement"},
                                    {"PlanetType", "Planet"},
                                    {"PlanetSize", "Planet size"},
                                    {"PlanetView", "Planet view"},
                                    {"PlanetAzimuth", "Planet direction"},
                                    {"PlanetElevation", "Planet height"},
                                    {"SkyEffectSize", "Black hole size"},
                                    {"BlackHoleAzimuth", "Black hole direction"},
                                    {"BlackHoleElevation", "Black hole height"},
                                    {"VolumetricClouds", "Clouds"}}));
    panels.push_back(settingsPanel("Bloom and lens", s,
                                   {{"Bloom", "Glow"},
                                    {"BloomRadius", "Glow radius"},
                                    {"HighlightBoost", "Light intensity"},
                                    {"BorderGlow", "Neon borders"},
                                    {"Floodlights", "Floodlights"},
                                    {"LensFlare", "Lens flare"},
                                    {"ChromaticAberration", "Colour fringes"},
                                    {"Vignette", "Vignette"},
                                    {"FilmGrain", "Film grain"}}));
    panels.push_back(settingsPanel("Colour", s,
                                   {{"Exposure", "Brightness"},
                                    {"Contrast", "Contrast"},
                                    {"Saturation", "Saturation"}}));
    panels.push_back(settingsPanel("Weather", s,
                                   {{"Rain", "Rain"},
                                    {"Snow", "Snow"},
                                    {"SnowCover", "Snow on the ground"},
                                    {"Wetness", "Wet roads"},
                                    {"Lightning", "Lightning"},
                                    {"WeatherShelter", "Dry under roofs"},
                                    {"LensDrops", "Drops on the lens"},
                                    {"Spray", "Car spray"},
                                    {"WeatherSound", "Weather volume"},
                                    {"WaterSurfaces", "Water"},
                                    {"WaterClarity", "Water clarity"},
                                    {"WaterSplash", "Water splash"},
                                    {"Reflections", "Reflections"},
                                    {"ReflectionBlur", "Reflection blur"}}));
    panels.push_back(settingsPanel("Neon trail", s,
                                   {{"NeonTrail", "Neon trail"},
                                    {"TrailColor", "Colour"},
                                    {"TrailWidth", "Width (m)"},
                                    {"TrailTyres", "Tyre lines"},
                                    {"TrailDuration", "Trail fade (s)"},
                                    {"TrailKeepRuns", "Earlier runs"}}));
    panels.push_back(settingsPanel("Cinematic", s,
                                   {{"MotionBlur", "Motion blur"},
                                    {"DepthOfField", "Depth of field"},
                                    {"DofAutofocus", "Focus on"},
                                    {"CinematicOnlyInReplays", nullptr}}));
    panels.push_back(settingsPanel("Image quality", s,
                                   {{"TAA", "Temporal AA"},
                                    {"FXAA", "Edge smoothing"},
                                    {"SMAA", "SMAA instead of FXAA"},
                                    {"Sharpen", "Sharpening"},
                                    {"Quality", "Effect quality"},
                                    {"AutoQuality", "Auto quality"}}));
    // A panel with nothing this game can do (or nothing the current choices use) is left out.
    panels.erase(std::remove_if(panels.begin(), panels.end(), [](const PanelDef& p) { return p.rows <= 0.0f; }), panels.end());
    for (PanelDef& p : panels) p.foldable = true; // closed at first, the title opens them
    const bool look = panelColumns(panels, 330.0f);
    if (config.preset == kCustomPreset) {
        if (ImGui::Button("Save as preset")) ImGui::OpenPopup("save preset");
        savePresetPopup(config);
        ImGui::SameLine();
        if (ImGui::Button("Discard changes")) ImGui::OpenPopup("Discard changes?");
        if (confirm("Discard changes?", ("Discard all changes to " + config.basePreset() + "?").c_str(), "Discard")) {
            config.selectPreset(config.basePreset());
        }
    }
    return look;
}

// Everything: every setting, your own presets, debug views, shader reload.
bool drawStudio(Config& config, Settings& s) {
    pageTitle("Studio", "All settings of the current preset.");
    // What you are working on.
    const std::string& base = config.basePreset();
    const bool changed = config.preset == kCustomPreset;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Preset:");
    ImGui::SameLine();
    ImGui::TextColored(kAccent, "%s", config.mapLookActive() ? "Author's Shader" : base.c_str());
    if (changed) {
        ImGui::SameLine();
        ImGui::TextDisabled("(unsaved changes)");
    }
    if (config.isUserPreset(base) && !config.mapLookActive()) {
        ImGui::SameLine();
        ImGui::BeginDisabled(!changed);
        if (ImGui::Button("Save changes")) config.saveUserPreset(base);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(changed ? "Overwrites \"%s\". Picture and description are kept." : "No changes to \"%s\" yet.", base.c_str());
        }
    }
    if (ImGui::Button(config.isUserPreset(base) ? "Save as new preset" : "Save as preset")) ImGui::OpenPopup("save preset");
    savePresetPopup(config);
    sameLineIfRoom("Reset preset");
    if (ImGui::Button("Reset preset")) ImGui::OpenPopup("Reset preset?");
    if (confirm("Reset preset?", ("Reset all settings to " + config.basePreset() + "?").c_str(), "Reset")) {
        // Back to the preset your changes started from (a map's look: its own look again).
        if (config.basePreset() == kMapLookPreset) {
            const Settings look = config.baseline();
            const bool enabled = s.enabled;
            s = look;
            s.enabled = enabled;
            config.preset = kMapLookPreset;
            config.markDirty();
        } else {
            config.selectPreset(config.basePreset());
        }
    }
    if (g_host->reloadShaders) {
        const std::string reload = "Reload shaders (" + keyText(keys::Action::Reload) + ")";
        sameLineIfRoom(reload.c_str());
        if (ImGui::Button(reload.c_str())) g_reloadRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < 260.0f * g_scale) ImGui::NewLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Debug view");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170.0f * g_scale);
    if (g_host->debugViews.empty()) {
        ImGui::Combo("##debug", &s.debugView, kDebugViews, IM_ARRAYSIZE(kDebugViews));
    } else {
        // The game's own list: (value, name) pairs.
        const char* current = "Final image";
        for (const auto& v : g_host->debugViews)
            if (v.first == s.debugView) current = v.second;
        if (ImGui::BeginCombo("##debug", current)) {
            for (const auto& v : g_host->debugViews)
                if (ImGui::Selectable(v.second, v.first == s.debugView)) s.debugView = v.first;
            ImGui::EndCombo();
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Debug view");
    // The search box: on the same line when there is room, else on its own.
    static char query[48] = "";
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < 220.0f * g_scale) ImGui::NewLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", "Search settings", query, sizeof(query));
    // For working on the shaders, not for players: only while a debug view is on, and only
    // where the game reports the sun (TrackMania 2 doesn't pass it to the menu).
    if (s.debugView != 0 && g_host->sunKnown) {
        ImGui::TextDisabled("Sun: from game (%.2f %.2f %.2f)", g_host->sunDirection[0], g_host->sunDirection[1], g_host->sunDirection[2]);
    }
    ImGui::Spacing();

    // One panel per category. Searching shows the matches of every category; a match that
    // does nothing with the current choices is greyed out.
    std::vector<PanelDef> panels;
    const bool searching = query[0] != 0;
    for (const Field& f : fields()) {
        const bool match = !searching || containsNoCase(f.label, query) || containsNoCase(f.key, query) || containsNoCase(f.category, query);
        // A hidden setting found by the search would open an empty panel (so would one this game
        // can't do: "FSR" in TrackMania 2).
        if (!match || !supported(f) || (!searching && !isRelevant(f, s))) continue;
        if (panels.empty() || panels.back().title != f.category) {
            const char* category = f.category;
            // Changed settings in this group (a dot on its title).
            bool marked = false;
            for (const Field& g : fields()) {
                if (!strcmp(g.category, category) && !isMachineSetting(g) && differsFromBase(g, s, config.baseline())) marked = true;
            }
            panels.push_back({category, 0.0f, [&s, category, searching] {
                                  bool l = false;
                                  for (const Field& g : fields()) {
                                      if (strcmp(g.category, category) != 0 || hiddenFromMenu(g)) continue;
                                      const bool hit = !searching || containsNoCase(g.label, query) || containsNoCase(g.key, query) ||
                                                       containsNoCase(g.category, query);
                                      const bool relevant = isRelevant(g, s);
                                      if (!hit || (!searching && !relevant)) continue;
                                      ImGui::BeginDisabled(!relevant);
                                      l |= drawField(g, s);
                                      ImGui::EndDisabled();
                                      if (!relevant && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                                          ImGui::SetTooltip("%s", !strcmp(g.key, "FsrSharpness")
                                                                      ? "FSR runs only with Render scale (FSR) below 1.00: set that first"
                                                                  : !strcmp(g.key, "Sharpen")
                                                                      ? "Render scale (FSR) is below 1.00: FSR sharpening sharpens instead"
                                                                      : "No effect with the current settings");
                                      }
                                  }
                                  return l;
                              },
                              !searching, marked});
        }
        panels.back().rows += 1.0f;
    }
    // Searching opens the groups with matches; else they open and close on their titles.
    if (!searching && !panels.empty()) {
        bool anyOpen = false;
        for (const PanelDef& d : panels) anyOpen |= panelOpen(d);
        if (ImGui::SmallButton(anyOpen ? "Collapse all" : "Expand all")) {
            for (const PanelDef& d : panels) g_panelOpen[d.title] = !anyOpen;
        }
        ImGui::Spacing();
    }
    if (!searching || containsNoCase("Colour look LUT", query)) {
        panels.insert(panels.begin(), lutPanel(config));
        panels.front().foldable = !searching;
    }
    if (panels.empty()) ImGui::TextDisabled("Nothing found.");
    return panelColumns(panels, 360.0f);
}

// Which look on which map: a preset per map mood, and the looks map authors ship.
void drawMaps(Config& config, const std::vector<std::string>& names) {
    pageTitle("Maps", "A preset for day, sunset and night maps. The type is detected from the map's lighting.");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset per map type");
    ImGui::SameLine();
    if (toggle("mood", &config.autoMood)) {
        if (config.autoMood) config.resumeMoodPresets();
        config.markDirty();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("this map: %s", moodName(config.mood));
    if (config.autoMood && config.manualPick) {
        ImGui::TextWrapped("%s is used on every map until you pick a preset for a map type below.", config.preset.c_str());
        if (ImGui::Button("Use map type presets")) config.resumeMoodPresets();
    }
    ImGui::Spacing();

    // The three moods as big cards side by side (one under the other when narrow).
    ImGui::BeginDisabled(!config.autoMood);
    static const char* const kLabels[] = {"Day maps", "Sunset maps", "Night maps"};
    const float spacing = 12.0f * g_scale;
    const int columns = ImGui::GetContentRegionAvail().x >= 3.0f * 230.0f * g_scale ? 3 : 1;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(spacing * 0.5f, spacing * 0.5f));
    if (ImGui::BeginTable("moods", columns, ImGuiTableFlags_SizingStretchSame)) {
        for (int m = 0; m < static_cast<int>(Mood::Count); m++) {
            ImGui::TableNextColumn();
            std::string& mp = config.moodPreset[m];
            ImGui::PushID(m);
            const float w = std::min(ImGui::GetContentRegionAvail().x, 420.0f * g_scale), h = w * 9.0f / 16.0f;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(w, h));
            ImDrawList* draw = ImGui::GetWindowDrawList();
            if (const ImTextureID texture = mp.empty() ? ImTextureID{} : picture(mp)) {
                draw->AddImageRounded(texture, p, ImVec2(p.x + w, p.y + h), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 8.0f * g_scale);
            } else {
                draw->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(30, 32, 40, 255), 8.0f * g_scale);
                const char* keep = "keeps the current look";
                const ImVec2 size = ImGui::CalcTextSize(keep);
                draw->AddText(ImVec2(p.x + (w - size.x) * 0.5f, p.y + (h - size.y) * 0.5f), IM_COL32(255, 255, 255, 110), keep);
            }
            draw->AddRectFilledMultiColor(ImVec2(p.x, p.y + h * 0.6f), ImVec2(p.x + w, p.y + h), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                          IM_COL32(0, 0, 0, 190), IM_COL32(0, 0, 0, 190));
            draw->AddText(g_fontTitle, g_fontTitle->FontSize, ImVec2(p.x + 10.0f * g_scale, p.y + h - g_fontTitle->FontSize - 8.0f * g_scale), IM_COL32_WHITE,
                          kLabels[m]);
            if (static_cast<int>(config.mood) == m) {
                draw->AddRect(p, ImVec2(p.x + w, p.y + h), colour(kAccent, 1.0f), 8.0f * g_scale, 0, 2.0f * g_scale);
                const char* tag = "THIS MAP";
                const ImVec2 size = ImGui::CalcTextSize(tag);
                const ImVec2 a(p.x + 8.0f * g_scale, p.y + 8.0f * g_scale), b(a.x + size.x + 12.0f * g_scale, a.y + size.y + 4.0f * g_scale);
                draw->AddRectFilled(a, b, colour(kAccent, 1.0f), (b.y - a.y) * 0.5f);
                draw->AddText(ImVec2(a.x + 6.0f * g_scale, a.y + 2.0f * g_scale), IM_COL32(15, 12, 8, 255), tag);
            }
            ImGui::SetNextItemWidth(w);
            if (ImGui::BeginCombo("##preset", mp.empty() ? "Keep current" : mp.c_str())) {
                if (ImGui::Selectable("Keep current", mp.empty())) {
                    mp.clear();
                    config.resumeMoodPresets();
                    config.markDirty();
                }
                for (const auto& name : names) {
                    if (ImGui::Selectable(name.c_str(), mp == name)) {
                        mp = name;
                        if (static_cast<int>(config.mood) == m) config.selectPreset(name);
                        config.resumeMoodPresets();
                        config.markDirty();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::EndDisabled();
    ImGui::Spacing();

    // Author's Shader: a look stored in a map (its comments), for everyone with the mod.
    std::vector<PanelDef> panels;
    panels.push_back({"Author's Shader", 4.0f, [&config] {
                          ImGui::TextWrapped("A preset stored inside a map. Players with the mod see the map with it.");
                          bool mapLooks = config.useMapLooks;
                          ImGui::AlignTextToFramePadding();
                          ImGui::TextUnformatted("Author's Shaders");
                          ImGui::SameLine();
                          if (toggle("maplooks", &mapLooks)) config.setUseMapLooks(mapLooks);
                          if (config.mapLookActive()) {
                              ImGui::SameLine();
                              ImGui::TextColored(sectionColour("Author's Shader"), "active on this map");
                          }
                          static std::string result;
                          if (ImGui::Button("Set as Author's Shader")) {
                              std::string comments;
                              const bool ok = g_host->mapComments && g_host->setMapComments && g_host->mapComments(comments) &&
                                              g_host->setMapComments(Config::withMapTag(comments, config.mapTag()));
                              result = ok ? "Set. Save the map to keep it." : "No map open in the editor.";
                          }
                          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stores the current preset in the map open in the editor.");
                          ImGui::SameLine();
                          if (ImGui::Button("Copy code")) {
                              ImGui::SetClipboardText(config.mapTag().c_str());
                              result = "Copied.";
                          }
                          if (!result.empty()) ImGui::TextDisabled("%s", result.c_str());
                          return false;
                      }});
    panelColumns(panels, 360.0f);
}

bool drawPerformancePage(Settings& s) {
    pageTitle("Performance", "Frame rate target and the cost of each effect on your GPU.");
    std::vector<PanelDef> panels;
    panels.push_back({"Performance", 6.0f, [&s] {
                          bool l = drawKey(s, "AutoQuality");
                          l |= drawKey(s, "TargetFPS");
                          l |= drawKey(s, "Quality");
                          l |= drawKey(s, "RenderScale");
                          l |= drawKey(s, "FsrSharpness");
                          if (s.autoQuality && g_host->autoQualityReduced) {
                              if (const char* reduced = g_host->autoQualityReduced()) {
                                  ImGui::TextWrapped("Turned off by auto quality: %s", reduced);
                              } else {
                                  ImGui::TextDisabled("All effects on.");
                              }
                          }
                          return l;
                      }});
    panels.push_back({"Cost per effect", 14.0f, [] {
                          drawPerformance();
                          return false;
                      }});
    return panelColumns(panels, 420.0f);
}

// --- Resource packs -----------------------------------------------------------------

// One pack as a card, like a preset's: its picture (the mod's load screen) with its name on it,
// a line about it underneath. Returns true when clicked. With `action` a small button sits on the
// picture's corner (a click on it sets `*actionClicked`, not the card's).
bool packCard(const std::string& title, const std::string& line, ImTextureID texture, int texW, int texH, const char* stand, bool active,
              bool usable, float width, const char* action = nullptr, bool* actionClicked = nullptr) {
    const float imageH = width * 9.0f / 16.0f;
    const float pad = 8.0f * g_scale;
    const float textH = ImGui::GetFontSize() * 2.0f + pad * 2.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 q(p.x + width, p.y + imageH + textH);
    bool clicked = ImGui::InvisibleButton("card", ImVec2(width, imageH + textH));
    const bool hovered = ImGui::IsItemHovered();
    const float rounding = 8.0f * g_scale;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p, q, hovered ? IM_COL32(38, 40, 50, 255) : IM_COL32(26, 28, 35, 255), rounding);
    const ImVec2 imageEnd(q.x, p.y + imageH);
    if (texture && texW > 0 && texH > 0) {
        // A load screen is stretched over the screen by the game (most are square): so here too.
        // Only pictures wider than 16:9 or taller than square are cropped around the middle.
        const float aspect = static_cast<float>(texW) / static_cast<float>(texH), wide = 16.0f / 9.0f;
        ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 1.0f);
        if (aspect > wide) {
            uv0.x = (1.0f - wide / aspect) * 0.5f;
            uv1.x = 1.0f - uv0.x;
        } else if (aspect < 1.0f) {
            uv0.y = (1.0f - aspect) * 0.5f;
            uv1.y = 1.0f - uv0.y;
        }
        draw->AddImageRounded(texture, p, imageEnd, uv0, uv1, usable ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 90), rounding,
                              ImDrawFlags_RoundCornersTop);
    } else {
        draw->AddRectFilledMultiColor(ImVec2(p.x, p.y + rounding), imageEnd, IM_COL32(44, 48, 62, 255), IM_COL32(44, 48, 62, 255),
                                      IM_COL32(22, 24, 31, 255), IM_COL32(22, 24, 31, 255));
        draw->AddRectFilled(p, ImVec2(q.x, p.y + rounding + 1.0f), IM_COL32(44, 48, 62, 255), rounding, ImDrawFlags_RoundCornersTop);
        const ImVec2 size = ImGui::CalcTextSize(stand);
        draw->AddText(ImVec2(p.x + (width - size.x) * 0.5f, p.y + (imageH - size.y) * 0.45f), IM_COL32(255, 255, 255, 110), stand);
    }
    const float fade = imageH * 0.45f;
    draw->AddRectFilledMultiColor(ImVec2(p.x, imageEnd.y - fade), imageEnd, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 190),
                                  IM_COL32(0, 0, 0, 190));
    ImGui::PushFont(g_fontTitle);
    const ImVec4 titleClip(p.x + pad, p.y, q.x - pad, imageEnd.y);
    draw->AddText(g_fontTitle, g_fontTitle->FontSize, ImVec2(p.x + pad, imageEnd.y - ImGui::GetFontSize() - pad * 0.7f), IM_COL32_WHITE, title.c_str(),
                  nullptr, 0.0f, &titleClip);
    ImGui::PopFont();
    if (active) {
        const char* tag = "IN USE";
        const ImVec2 size = ImGui::CalcTextSize(tag);
        const ImVec2 a(p.x + pad, p.y + pad), b(a.x + size.x + 12.0f * g_scale, a.y + size.y + 4.0f * g_scale);
        draw->AddRectFilled(a, b, colour(kAccent, 1.0f), (b.y - a.y) * 0.5f);
        draw->AddText(ImVec2(a.x + 6.0f * g_scale, a.y + 2.0f * g_scale), IM_COL32(15, 12, 8, 255), tag);
    }
    if (action) {
        const ImVec2 size = ImGui::CalcTextSize(action);
        const ImVec2 b(q.x - pad, p.y + pad + size.y + 8.0f * g_scale), a(b.x - size.x - 20.0f * g_scale, p.y + pad);
        const bool over = hovered && ImGui::IsMouseHoveringRect(a, b);
        draw->AddRectFilled(a, b, over ? colour(kAccent, 1.0f) : IM_COL32(15, 12, 8, 235), (b.y - a.y) * 0.5f);
        draw->AddRect(a, b, colour(kAccent, 1.0f), (b.y - a.y) * 0.5f, 0, 1.5f * g_scale);
        draw->AddText(ImVec2(a.x + 10.0f * g_scale, a.y + 4.0f * g_scale), over ? IM_COL32(15, 12, 8, 255) : IM_COL32_WHITE, action);
        if (over && clicked) {
            clicked = false;
            if (actionClicked) *actionClicked = true;
        }
    }
    const ImVec4 clip(p.x + pad, imageEnd.y + pad, q.x - pad, q.y - pad * 0.5f);
    draw->AddText(nullptr, 0.0f, ImVec2(clip.x, clip.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), line.c_str(), nullptr, width - pad * 2.0f, &clip);
    if (active) {
        draw->AddRect(p, q, colour(kAccent, 1.0f), rounding, 0, 2.0f * g_scale);
    } else if (hovered) {
        draw->AddRect(p, q, IM_COL32(255, 255, 255, 90), rounding, 0, 1.5f * g_scale);
    }
    return clicked;
}

// Resource packs: the game's environment mods on every map, one choice per environment (and per
// map mood). The choice is used when a map loads; nothing of it goes into maps, replays or online.
void drawPacks(Config& config) {
    ResourcePacks* rp = g_host->resourcePacks;
    pageTitle("Resource packs", "Texture mods from Skins\\<Environment>\\Mod, on every map.");
    if (!rp->hooked()) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "This game build can't use them (tmvs.log says why).");
    if (ImGui::Button("Refresh")) rp->refresh();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Finds new packs. New ones work after a restart.");
    ImGui::SameLine(0.0f, 20.0f * g_scale);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Also replace a map's own mod");
    ImGui::SameLine();
    if (toggle("replace", &config.packReplaceMapMod)) config.markDirty();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: maps with their own mod keep it.");
    const std::string status = rp->status();
    ImGui::TextDisabled("Last map: %s", status.empty() ? "none loaded yet" : status.c_str());
    // The loaded map: switch its textures now instead of at the next map.
    static std::string s_applied, s_appliedEnv;
    const ResourcePacks::Live live = rp->live();
    if (live.possible) {
        ImGui::BeginDisabled(!live.differs);
        if (ImGui::Button("Apply now")) {
            s_appliedEnv = live.environment;
            s_applied = rp->applyNow();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Switches the textures on this map.");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (live.differs) {
            ImGui::TextColored(kAccent, "Shown: %s. Selected: %s.", live.shown.c_str(), live.wanted.c_str());
        } else {
            ImGui::TextDisabled("Shown: %s.", live.shown.c_str());
        }
    }
    ImGui::Spacing();

    static const char* const kSlots[] = {"All moods", "Day", "Sunset", "Night"};
    static std::map<std::string, int> s_slot; // per environment: the choice the cards set (-1 all moods)
    const std::vector<ResourcePacks::Pack>& packs = rp->packs();
    for (const std::string& env : rp->environments()) {
        ImGui::PushID(env.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::SeparatorText(env.c_str());
        ImGui::PopStyleColor();
        const Config::PackChoice* found = config.findPackChoice(env);
        const Config::PackChoice choice = found ? *found : Config::PackChoice();
        bool perMood = choice.perMood;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("One per map mood");
        ImGui::SameLine();
        if (toggle("permood", &perMood)) {
            config.packChoice(env).perMood = perMood;
            config.markDirty();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Separate packs for day, sunset and night maps.");
        int& slot = s_slot.emplace(env, -1).first->second;
        if (!perMood) slot = -1;
        if (perMood) {
            for (int i = -1; i < static_cast<int>(Mood::Count); i++) {
                ImGui::SameLine();
                if (chip(kSlots[i + 1], slot == i)) slot = i;
            }
        }
        ImGui::SameLine(0.0f, 20.0f * g_scale);
        if (ImGui::SmallButton("Open folder")) rp->openFolder(env);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opens Skins\\%s\\Mod.", env.c_str());
        const std::string& selected = slot < 0 ? choice.all : choice.mood[slot];

        std::vector<size_t> mine;
        for (size_t i = 0; i < packs.size(); i++) {
            if (packs[i].environment == env) mine.push_back(i);
        }
        cardGrid(static_cast<int>(mine.size()) + 2, [&](int i, float width) {
            std::string value, title, line;
            ImTextureID texture{};
            int tw = 0, th = 0;
            bool usable = true;
            if (i == 0) {
                title = "Map's own";
                line = "The map's own mod.";
            } else if (i == 1) {
                value = "None";
                title = "None";
                line = "The game's textures.";
                texture = picture("Realistic"); // the preset closest to the game's own look
                tw = 16;
                th = 9;
            } else {
                const ResourcePacks::Pack& pack = packs[mine[i - 2]];
                value = pack.file;
                title = pack.name;
                usable = pack.usable;
                line = usable ? pack.detail : "Restart TrackMania to use it.";
                texture = rp->preview(mine[i - 2], tw, th);
            }
            // With one per mood: which moods use it.
            if (perMood) {
                std::string uses;
                for (int m = 0; m < static_cast<int>(Mood::Count); m++) {
                    if (choice.mood[m] == value) uses += std::string(uses.empty() ? "" : ", ") + kSlots[m + 1];
                }
                if (!uses.empty()) line = uses + ". " + line;
            }
            // The chosen card: an Apply button when the loaded map shows something else, afterwards how it went.
            const bool chosen = value == selected;
            const bool offer = chosen && live.possible && live.differs && live.environment == env;
            if (chosen && !offer && !s_applied.empty() && s_appliedEnv == env) line = s_applied;
            bool apply = false;
            ImGui::PushID(i);
            if (packCard(title, line, texture, tw, th, i < 2 ? "" : "No picture", chosen, usable, width, offer ? "Apply" : nullptr, &apply)) {
                Config::PackChoice& c = config.packChoice(env);
                (slot < 0 ? c.all : c.mood[slot]) = value;
                config.markDirty();
                s_applied.clear();
            }
            if (apply) {
                s_appliedEnv = env;
                s_applied = rp->applyNow();
            }
            ImGui::PopID();
        });
        ImGui::PopID();
        ImGui::Spacing();
    }
}

// The replay editor's Shoot video: presets for what the video is for, the game's own dialog in a
// few lines, then every setting. Only where the game can export (not TrackMania 2).
bool videoPageShown() {
    const Field* f = findField("VideoCodec");
    return f && supported(*f);
}

struct VideoPreset {
    const char* name;
    const char* tip;
    void (*apply)(Settings& s);
    bool (*matches)(const Settings& s);
};

const VideoPreset kVideoPresets[] = {
    {"YouTube", "H.264, quality 80, half grain: close to the source and clean after YouTube's encode. Shoot at 1440p or more for a higher YouTube bitrate.",
     [](Settings& s) {
         s.videoCodec = 1;
         s.videoQuality = 80;
         s.video10Bit = false;
         s.videoBitrate = 0.0f;
         s.videoGrain = 0.5f;
         s.videoHdr = false;
         s.videoEncoder = 0;
     },
     [](const Settings& s) { return s.videoCodec == 1 && s.videoQuality == 80 && s.videoBitrate <= 0.0f && s.videoGrain == 0.5f; }},
    {"Best", "H.264, quality 92, half grain: the most detail at a sensible file size. Presets use the graphics card's encoder; with AMD or Intel set Video encoder to ffmpeg afterwards.",
     [](Settings& s) {
         s.videoCodec = 1;
         s.videoQuality = 92;
         s.video10Bit = false;
         s.videoBitrate = 0.0f;
         s.videoGrain = 0.5f;
         s.videoHdr = false;
         s.videoEncoder = 0;
     },
     [](const Settings& s) { return s.videoCodec == 1 && s.videoQuality == 92 && s.videoBitrate <= 0.0f && s.videoGrain == 0.5f; }},
    {"Editing", "H.264, quality 92, full grain: smooth to edit and grade. Large files.",
     [](Settings& s) {
         s.videoCodec = 1;
         s.videoQuality = 92;
         s.video10Bit = false;
         s.videoBitrate = 0.0f;
         s.videoGrain = 1.0f;
         s.videoHdr = false;
         s.videoEncoder = 0;
     },
     [](const Settings& s) { return s.videoCodec == 1 && s.videoQuality == 92 && s.videoBitrate <= 0.0f && s.videoGrain == 1.0f; }},
    {"Small", "HEVC 10-bit, quality 70: about two thirds of the YouTube file size, no bands in the sky. Harder to edit.",
     [](Settings& s) {
         s.videoCodec = 2;
         s.videoQuality = 70;
         s.video10Bit = true;
         s.videoBitrate = 0.0f;
         s.videoGrain = 0.3f;
         s.videoHdr = false;
         s.videoEncoder = 0;
     },
     [](const Settings& s) { return s.videoCodec == 2 && s.video10Bit && s.videoQuality == 70 && s.videoBitrate <= 0.0f && !s.videoHdr; }},
    {"Discord", "H.264 at 8 Mbit/s, little grain: 10 seconds fit Discord's 10 MB. Shoot at 1080p or less.",
     [](Settings& s) {
         s.videoCodec = 1;
         s.videoQuality = 62;
         s.video10Bit = false;
         s.videoBitrate = 8.0f;
         s.videoGrain = 0.2f;
         s.videoHdr = false;
         s.videoEncoder = 0;
     },
     [](const Settings& s) { return s.videoCodec == 1 && s.videoBitrate > 7.9f && s.videoBitrate < 8.1f; }},
    {"HDR10", "HEVC 10-bit HDR10, quality 85: lights above white stay bright on an HDR screen.",
     [](Settings& s) {
         s.videoCodec = 2;
         s.videoQuality = 85;
         s.video10Bit = true;
         s.videoBitrate = 0.0f;
         s.videoGrain = 0.3f;
         s.videoOwnFrames = true;
         s.videoHdr = true;
         s.videoEncoder = 0;
     },
     [](const Settings& s) { return s.videoCodec >= 2 && s.video10Bit && s.videoHdr && s.videoOwnFrames; }},
};

const VideoPreset kVideoLooks[] = {
    {"Replay", "The game's look: motion blur over the whole frame (360), no crop.",
     [](Settings& s) {
         s.videoShutter = 360.0f;
         s.videoAspect = 0;
     },
     [](const Settings& s) { return s.videoShutter >= 359.0f && s.videoAspect == 0; }},
    {"Cinema", "Film shutter (180), lens depth of field and a 2.39:1 picture. Raise Depth of field in Customize for the blur.",
     [](Settings& s) {
         s.videoOwnFrames = true;
         s.videoShutter = 180.0f;
         s.videoLensDof = true;
         s.videoAspect = 2;
     },
     [](const Settings& s) { return s.videoShutter > 179.0f && s.videoShutter < 181.0f && s.videoAspect == 2 && s.videoLensDof; }},
};

// A row of preset buttons; the one the settings match is lit.
void videoPresetRow(Config& config, Settings& s, const VideoPreset* presets, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (i) ImGui::SameLine();
        if (chip(presets[i].name, presets[i].matches(s))) {
            presets[i].apply(s);
            config.markDirty();
        }
        if (ImGui::IsItemHovered()) showTip(presets[i].tip);
    }
}

bool drawVideoPage(Config& config, Settings& s) {
    pageTitle("Video export", "Settings for Shoot video in the replay editor.");
    std::vector<PanelDef> panels;
    panels.push_back({"For", 4.0f, [&config, &s] {
                          videoPresetRow(config, s, kVideoPresets, sizeof(kVideoPresets) / sizeof(kVideoPresets[0]));
                          ImGui::Spacing();
                          ImGui::TextDisabled("Look");
                          videoPresetRow(config, s, kVideoLooks, sizeof(kVideoLooks) / sizeof(kVideoLooks[0]));
                          return false;
                      }});
    panels.push_back({"Recommended dialog settings", 3.0f, [] {
                          ImGui::BulletText("Size 1440p or 4K, 60 fps");
                          ImGui::BulletText("High quality on, 4 samples, or 9 with motion blur");
                          return false;
                      }});
    panels.push_back(settingsPanel("Format", s,
                                   {{"VideoCodec", nullptr}, {"VideoHdr", "Dynamic range"}, {"VideoQuality", nullptr}, {"Video10Bit", nullptr},
                                    {"VideoBitrate", nullptr}, {"VideoEncoder", nullptr}, {"VideoFfmpegPreset", nullptr}, {"VideoHdrPeak", nullptr},
                                    {"VideoHdrWhite", nullptr}}));
    panels.push_back(settingsPanel("Rendering", s,
                                   {{"VideoExportBest", nullptr}, {"VideoShutter", nullptr}, {"VideoLensDof", nullptr},
                                    {"VideoGrain", nullptr}, {"VideoAspect", nullptr}}));
    return panelColumns(panels, 380.0f);
}

// How the mod behaves: switches that aren't part of a look.
bool drawSettings(Config& config, Settings& s) {
    pageTitle("Settings", "General options. They are not part of presets.");
    std::vector<PanelDef> panels;
    if (g_host->gamePanel) {
        panels.push_back({g_host->gamePanelTitle ? g_host->gamePanelTitle : "Game", 6.0f, [] {
                              g_host->gamePanel();
                              return false;
                          }});
    }
    panels.push_back({"Presets and maps", 4.0f, [&config] {
                          if (switchRow("Preset per map type", &config.autoMood,
                                        "Day, sunset and night maps each use their own preset (see Maps). Off: one preset everywhere.\n"
                                        "A preset you pick applies to all maps until you pick one for a map type.") &&
                              config.autoMood)
                              config.resumeMoodPresets();
                          ImGui::BeginDisabled(!config.autoMood);
                          switchRow("Remember preset picks", &config.rememberMoodPick,
                                    "On: a preset picked on a night map becomes the night preset (same for day and sunset).\nOff: only the Maps page sets them.");
                          ImGui::EndDisabled();
                          bool mapLooks = config.useMapLooks;
                          if (switchRow("Author's Shaders", &mapLooks, "Use the preset a map's author stored in the map.")) {
                              config.setUseMapLooks(mapLooks);
                          }
                          return false;
                      }});
    panels.push_back({"Menu", 3.0f, [&config] {
                          switchRow("Live preview", &config.hoverPreview, "Hovering over a preset shows it in the game.");
                          switchRow("Full-screen menu", &config.menuFull, "Off: the right part of the game stays visible.");
                          switchRow("Check for updates", &config.checkUpdates, "Checks GitHub for a new version when the game starts.");
                          return false;
                      }});
    panels.push_back({"Hotkeys", 11.0f, [] {
                          drawHotkeys();
                          return false;
                      }});
    // The video export has its own page; TrackMania 2 (no page) keeps its one switch here.
    if (!videoPageShown()) {
        panels.push_back(settingsPanel("Video export", s, {{"VideoExportBest", nullptr}}));
        if (panels.back().rows <= 0.0f) panels.pop_back();
    }
    panels.push_back(settingsPanel("Effects", s, {{"CinematicOnlyInReplays", nullptr}, {"WeatherSound", nullptr}, {"TAA", nullptr}, {"TAAJitter", nullptr}}));
    // How the mod works with the game itself, as in Studio's group of the same name (same order).
    Entries game;
    for (const Field& f : fields()) {
        if (!strcmp(f.category, "Game integration")) game.push_back({f.key, nullptr});
    }
    panels.push_back(settingsPanel("Game integration", s, game));
    panels.push_back({"Photo mode", (g_host->splitOn ? (g_host->splitPreset ? 13.0f : 11.0f) : 8.0f) + (g_host->photoHideCar ? 2.0f : 0.0f), [&config] {
                          const std::string capKey = keyText(keys::Action::Capture), photoKey = keyText(keys::Action::Photo);
                          const std::string splitKey = keyText(keys::Action::PhotoSplit);
                          switchRow("Auto photo mode", &config.photoAuto,
                                    ("Photo mode starts with the free camera (cam 7): no HUD, full quality.\n" + photoKey +
                                     " toggles it anywhere.").c_str());
                          switchRow("Hide HUD", &config.photoHideHud, "Off: the HUD stays visible in photo mode. Photos never include it.");
                          if (g_host->photoHideCar)
                              switchRow("Hide editor", &config.photoHideEditor, "In the replay editor (MediaTracker): photo mode hides the editor's panels, as its Tab key does, and the 3D view fills the screen. They are back when photo mode ends.");
                          if (g_host->photoHideCar)
                              switchRow("Hide car", &config.photoHideCar, "Hides cars and their shadows in photo mode and photos. Smoke and tyre marks stay.");
                          ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
                          char factor[48];
                          snprintf(factor, sizeof(factor), "%d x %d tiles = %.1fx", config.photoTiles, config.photoTiles, config.photoTiles * 0.84f);
                          if (ImGui::SliderInt("Hi-res photo", &config.photoTiles, 2, 4, factor)) config.markDirty();
                          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shift+%s takes a photo this many times the screen size. Pause the replay while it renders.", capKey.c_str());
                          ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
                          char aperture[32];
                          snprintf(aperture, sizeof(aperture), "f/%.1f", (g_host->fNumber ? g_host->fNumber(config.photoDof) : 0.0f));
                          if (ImGui::SliderFloat("Aperture", &config.photoDof, 0.0f, 1.0f, aperture)) config.markDirty();
                          if (ImGui::IsItemHovered()) ImGui::SetTooltip("f/1.4 blurs most around the focus point, f/16 keeps everything sharp. Ctrl+wheel in the game.");
                          if (g_host->splitOn) {
                              bool& split = *g_host->splitOn;
                              if (switchRow("Compare split", &split,
                                            ("The game's image (or another preset) on one side of a line, your look on the other (" + splitKey +
                                             ").\n" +
                                             (g_host->splitNote ? std::string(g_host->splitNote)
                                                                // TMNF (the one with a preset side): hi-res photos and the aperture on both sides too.
                                                                : std::string(g_host->splitPreset ? "Also in photos, hi-res ones too. The aperture applies to both sides."
                                                                                                  : "Also in photos, not in hi-res ones.") +
                                                                      " Game bloom off and real night apply to both sides.")).c_str())) {
                                  if (split && g_host->photoOn) g_host->photoOn();
                              }
                              ImGui::BeginDisabled(!split);
                              if (std::string* other = g_host->splitPreset) {
                                  // The other side: the game's image or a preset (your look stays on the mod's side;
                                  // to compare two presets, pick one as your look and the other here).
                                  ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
                                  if (ImGui::BeginCombo("Other side", other->empty() ? "The game's image" : other->c_str())) {
                                      if (ImGui::Selectable("The game's image", other->empty())) other->clear();
                                      for (const std::string& name : config.presetNames()) {
                                          if (ImGui::Selectable(name.c_str(), *other == name)) *other = name;
                                      }
                                      ImGui::EndCombo();
                                  }
                                  if (ImGui::IsItemHovered())
                                      ImGui::SetTooltip("The game's image or a preset on the other side.\n"
                                                        "A preset runs the effects twice (photo mode only).");
                                  switchRow("Photo labels", &config.splitPhotoLabels, tooltipFor("SplitPhotoLabels"));
                              }
                              ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
                              ImGui::SliderFloat("Line", g_host->splitPosition, 0.0f, 1.0f, "%.2f");
                              if (ImGui::IsItemHovered()) ImGui::SetTooltip("Line position from the left. Ctrl+Shift+wheel in the game.");
                              ImGui::Checkbox("Swap sides", g_host->splitSwapped);
                              if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shows the game's image on the right.");
                              ImGui::EndDisabled();
                          }
                          ImGui::TextDisabled("%s photo  |  Shift+%s hi-res  |  middle click: focus", capKey.c_str(), capKey.c_str());
                          ImGui::TextDisabled("Ctrl+middle click: no focus  |  Ctrl+wheel: aperture  |  Shift+wheel: zoom  |  %s: on/off", photoKey.c_str());
                          if (g_host->splitOn) ImGui::TextDisabled("%s: compare split  |  Ctrl+Shift+wheel: move its line", splitKey.c_str());
                          return false;
                      }});
    panels.push_back({"Files", 3.0f, [&config] {
                          if (ImGui::Button("Open TMVS folder")) ShellExecuteW(nullptr, L"open", log::dataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                          if (ImGui::IsItemHovered()) ImGui::SetTooltip("settings.ini, tmvs.log, the F12 captures and the photos.");
                          ImGui::SameLine();
                          if (ImGui::Button("Open presets folder")) {
                              CreateDirectoryW(config.presetDir().c_str(), nullptr);
                              ShellExecuteW(nullptr, L"open", config.presetDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                          }
                          ImGui::TextDisabled("All changes are saved automatically.");
                          return false;
                      }});
    // A settings panel with nothing this game can do is left out.
    panels.erase(std::remove_if(panels.begin(), panels.end(), [](const PanelDef& p) { return p.rows <= 0.0f; }), panels.end());
    return panelColumns(panels, 380.0f);
}

// Release notes (Markdown) as menu text: headings in the accent colour, list items as bullets.
void markdown(const std::string& text) {
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        for (const char* mark : {"**", "`"}) {
            for (size_t at; (at = line.find(mark)) != std::string::npos;) line.erase(at, strlen(mark));
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("![", 0) == 0) continue; // pictures: only on the release page
        // HTML (<details>), tables (the file hashes) and rules: only on the release page too.
        if (line.rfind("<", 0) == 0 || line.rfind("|", 0) == 0 || line.rfind("---", 0) == 0) continue;
        // Links: [text](url) -> text.
        for (size_t open; (open = line.find('[')) != std::string::npos;) {
            const size_t close = line.find("](", open);
            const size_t shut = close == std::string::npos ? std::string::npos : line.find(')', close);
            if (shut == std::string::npos) break;
            line = line.substr(0, open) + line.substr(open + 1, close - open - 1) + line.substr(shut + 1);
        }
        if (line.empty()) {
            ImGui::Spacing();
        } else if (line[0] == '#') {
            const size_t title = line.find_first_not_of("# ");
            if (title != std::string::npos) ImGui::TextColored(kAccent, "%s", line.c_str() + title);
        } else if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", line.c_str() + 2);
        } else {
            ImGui::TextWrapped("%s", line.c_str());
        }
    }
}

void openLink(const std::string& url) {
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Version, updates, help and credits.
void drawAbout(Config& config) {
    pageTitle("About", "TM Vibrant Shaders " TM_SHADERS_VERSION_A " - updates, help and credits.");
    const update::Info info = update::latest();
    std::vector<PanelDef> panels;
    const update::Install install = update::install();
    const float updatesHeight =
        info.state != update::State::Available ? 4.0f : (install == update::Install::ModLoader ? 16.0f : 14.0f);
    panels.push_back({"Updates", updatesHeight, [&config, info, install] {
                          switch (info.state) {
                              case update::State::Checking: ImGui::TextDisabled("Checking for updates..."); break;
                              case update::State::UpToDate: ImGui::Text("You're up to date (%s).", TM_SHADERS_VERSION_A); break;
                              case update::State::Failed: ImGui::TextDisabled("Couldn't check for updates. Are you offline?"); break;
                              case update::State::Off: ImGui::TextDisabled("Not checked."); break;
                              case update::State::Available: {
                                  ImGui::PushFont(g_fontTitle);
                                  ImGui::TextColored(kAccent, "Update available: %s", info.version.c_str());
                                  ImGui::PopFont();
                                  if (install == update::Install::ModLoaderServer) {
                                      // The ModLoader's own auto update fetches it from our server.
                                      ImGui::TextDisabled("Installed: %s. The ModLoader installs the update by itself.", TM_SHADERS_VERSION_A);
                                      if (ImGui::Button("Release page")) openLink(info.url);
                                  } else {
                                      ImGui::TextDisabled("Installed: %s. Download the new version and run its Setup.", TM_SHADERS_VERSION_A);
                                      if (ImGui::Button("Download")) openLink(info.url);
                                  }
                                  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opens the release page in your browser. In fullscreen the game may minimise.");
                                  if (install == update::Install::ModLoader) {
                                      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                                      ImGui::TextWrapped("To update through the ModLoader, add our server under servers: in its config.yaml.");
                                      ImGui::PopStyleColor();
                                      if (ImGui::Button("Copy server address")) ImGui::SetClipboardText(update::kModLoaderServer);
                                      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", update::kModLoaderServer);
                                  }
                                  if (!info.notes.empty()) {
                                      ImGui::Spacing();
                                      ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.25f));
                                      ImGui::BeginChild("notes", ImVec2(0.0f, 260.0f * g_scale), ImGuiChildFlags_AlwaysUseWindowPadding);
                                      markdown(info.notes);
                                      ImGui::EndChild();
                                      ImGui::PopStyleColor();
                                  }
                                  break;
                              }
                          }
                          if (info.state != update::State::Checking && ImGui::Button("Check now")) update::check();
                          ImGui::AlignTextToFramePadding();
                          ImGui::TextUnformatted("Check on startup");
                          ImGui::SameLine();
                          if (toggle("autocheck", &config.checkUpdates)) config.markDirty();
                          if (ImGui::IsItemHovered()) ImGui::SetTooltip("One request to api.github.com per game start. No personal data is sent.");
                          return false;
                      }});
    panels.push_back({"Help", 7.0f, [] {
                          const std::string repo = std::string("https://github.com/") + update::kRepository;
                          if (ImGui::Button("Project page")) openLink(repo);
                          ImGui::SameLine();
                          if (ImGui::Button("Report a problem")) openLink(repo + "/issues");
                          ImGui::SameLine();
                          if (ImGui::Button("All releases")) openLink(repo + "/releases");
                          if (ImGui::Button("Open TMVS folder")) {
                              ShellExecuteW(nullptr, L"open", log::dataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                          }
                          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Settings, presets, tmvs.log and F12 captures. Attach tmvs.log to bug reports.");
                          ImGui::Spacing();
                          ImGui::TextDisabled("%s  menu      %s  effects on/off", keyText(keys::Action::Menu).c_str(), keyText(keys::Action::Effects).c_str());
                          if (g_host->reloadShaders)
                              ImGui::TextDisabled("%s  reload shaders      %s  frame capture", keyText(keys::Action::Reload).c_str(),
                                                  keyText(keys::Action::Capture).c_str());
                          else ImGui::TextDisabled("%s  frame capture", keyText(keys::Action::Capture).c_str());
                          return false;
                      }});
    panels.push_back({"Credits", 6.0f, [] {
                          ImGui::TextWrapped("Built with Dear ImGui, minimp3 and stb. Sharpening: AMD FidelityFX CAS. Upscaling: AMD FidelityFX FSR 1. Anti-aliasing: FXAA 3.11, SMAA (Jimenez, Echevarria, Masia, Navarro, Gutierrez).");
                          ImGui::TextWrapped("Rain and thunder: CC0 recordings from freesound.org by Rubaoliva, loganzsound, seth-m, Fission9, TRP, "
                                             "Kinoton, Martineerok, kingsrow and bastipictures. The snow storm wind is made by the mod itself.");
                          ImGui::TextDisabled("Licences: see THIRD_PARTY_NOTICES.txt in the download.");
                          return false;
                      }});
    panelColumns(panels, 380.0f);
}

// --- The window ---------------------------------------------------------------------

// The window's size: most of the screen's height and as narrow as the navigation and one
// column of settings allow, leaving most of the game in view (the live preview). Full: the
// whole screen.
void placeWindow(bool full, ImGuiCond cond) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float margin = 24.0f * g_scale;
    ImVec2 size(display.x - margin * 2.0f, display.y - margin * 2.0f);
    if (!full) size.x = std::clamp(display.x * 0.3f, std::min(660.0f * g_scale, size.x), std::min(1500.0f * g_scale, size.x));
    ImGui::SetNextWindowPos(ImVec2(margin, margin), cond);
    ImGui::SetNextWindowSize(size, cond);
}

// The full screen / side button: a frame with a smaller one inside (side) or filling it.
bool layoutButton(bool full) {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("layout", ImVec2(h, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p, ImVec2(p.x + h, p.y + h), hovered ? IM_COL32(70, 130, 230, 255) : IM_COL32(45, 95, 185, 230), 5.0f * g_scale);
    const ImU32 c = IM_COL32_WHITE;
    const float i = h * 0.24f;
    draw->AddRect(ImVec2(p.x + i, p.y + i), ImVec2(p.x + h - i, p.y + h - i), c, 2.0f, 0, 1.5f);
    if (full) {
        draw->AddRectFilled(ImVec2(p.x + i, p.y + i), ImVec2(p.x + h * 0.55f, p.y + h - i), c, 2.0f);
    } else {
        draw->AddRectFilled(ImVec2(p.x + i + 2.0f, p.y + i + 2.0f), ImVec2(p.x + h - i - 2.0f, p.y + h - i - 2.0f), c, 1.0f);
    }
    if (hovered) ImGui::SetTooltip(full ? "Smaller menu (game visible on the right)" : "Full-screen menu");
    return clicked;
}

void drawHeader(Config& config, Settings& s) {
    // The mod's icon left of the name, as tall as the title font. It is made at exactly that many
    // pixels and drawn on whole pixels, texel for pixel: sharp at every menu size.
    const int iconSize = static_cast<int>(g_fontTitle->FontSize + 0.5f);
    const ImTextureID icon = g_host->icon ? g_host->icon(iconSize) : ImTextureID{};
    if (icon) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 at(std::floor(p.x), std::floor(p.y));
        ImGui::Dummy(ImVec2(static_cast<float>(iconSize), static_cast<float>(iconSize)));
        ImGui::GetWindowDrawList()->AddImageRounded(icon, at, ImVec2(at.x + iconSize, at.y + iconSize), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                                    IM_COL32_WHITE, iconSize * 0.18f);
        ImGui::SameLine(0.0f, 8.0f * g_scale);
    }
    ImGui::PushFont(g_fontTitle);
    ImGui::TextColored(kAccent, "TM VIBRANT SHADERS");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", TM_SHADERS_VERSION_A);
    const update::Info info = update::latest();
    if (info.state == update::State::Available) {
        ImGui::SameLine();
        char label[64];
        snprintf(label, sizeof(label), "Update: %s", info.version.c_str());
        if (chip(label, true)) config.menuPage = kAbout;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A new version is available. Click for details.");
    }

    // Right side: frame rate, the on/off switch, layout, close.
    char fps[96];
    const char* reduced = g_host->autoQualityReduced ? g_host->autoQualityReduced() : nullptr;
    if (s.autoQuality && reduced) {
        snprintf(fps, sizeof(fps), "%.0f FPS  effects lowered", ImGui::GetIO().Framerate);
    } else {
        snprintf(fps, sizeof(fps), "%.0f FPS", ImGui::GetIO().Framerate);
    }
    const float h = ImGui::GetFrameHeight();
    const float right = ImGui::GetContentRegionMax().x;
    const float switchW = h * 1.8f, buttonW = h;
    char effectsLabel[32];
    snprintf(effectsLabel, sizeof(effectsLabel), "Effects (%s)", keyText(keys::Action::Effects).c_str());
    const float effectsW = ImGui::CalcTextSize(effectsLabel).x;
    ImGui::SameLine(right - buttonW * 2.0f - switchW - effectsW - ImGui::CalcTextSize(fps).x - 48.0f * g_scale);
    ImGui::TextDisabled("%s", fps);
    if (reduced && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Turned off by auto quality: %s", reduced);
    }
    ImGui::SameLine(0.0f, 20.0f * g_scale);
    ImGui::TextUnformatted(effectsLabel);
    ImGui::SameLine();
    if (toggle("enabled", &s.enabled)) config.markDirty();
    ImGui::SameLine(right - buttonW * 2.0f - 4.0f * g_scale);
    if (layoutButton(config.menuFull)) {
        config.menuFull = !config.menuFull;
        config.markDirty();
    }
    ImGui::SameLine(right - buttonW);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.20f, 0.20f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.28f, 0.26f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button("\xC3\x97", ImVec2(buttonW, h))) config.showOverlay = false; // U+00D7
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close (%s)", keyText(keys::Action::Menu).c_str());
    ImGui::PopStyleColor(4);

    if (!g_host->depthAvailable) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Depth buffer unavailable - restart the game with MSAA disabled.");
    }
    if (!g_host->engineHooks) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Unknown game build: effects also apply to the HUD.");
    if (g_host->shaderError) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Shader error, see tmvs.log");
    ImGui::Spacing();
}

// --- The pages: one table for the tab bar and for detached windows -------------------

using Names = std::vector<std::string>;

// A page of the menu. Every page is in the table below, in the order of the tab bar; that is all a
// new page needs: it gets its tab, its window ("detach") and its saved state ([Windows], by `key`).
struct PageDef {
    int id;            // the Page enum (saved as MenuPage)
    const char* key;   // saved in [Windows]; never change it
    const char* title;
    const char* hint;
    bool (*available)(); // null: always; false: not shown (e.g. a page this game doesn't have)
    // Draws the page's body (no window of its own, no heading) in the menu or in a window; true:
    // a look setting changed (the preset becomes Custom).
    bool (*draw)(Config&, Settings&, const Names&);
};

const PageDef kPageTable[] = {
    {kShaders, "shaders", "Shaders", "Browse and apply presets", nullptr, [](Config& c, Settings&, const Names&) { drawShaders(c); return false; }},
    {kCustomize, "customize", "Customize", "Main settings", nullptr, [](Config& c, Settings& s, const Names&) { return drawCustomize(c, s); }},
    {kStudio, "studio", "Studio", "All settings", nullptr, [](Config& c, Settings& s, const Names&) { return drawStudio(c, s); }},
    {kMaps, "maps", "Maps", "Presets for day, sunset and night maps", nullptr, [](Config& c, Settings&, const Names& n) { drawMaps(c, n); return false; }},
    {kPerformance, "performance", "Performance", "Frame rate and effect costs", nullptr, [](Config&, Settings& s, const Names&) { return drawPerformancePage(s); }},
    {kVideo, "video", "Video", "Shoot video: presets, quality, the camera's look", videoPageShown, [](Config& c, Settings& s, const Names&) { return drawVideoPage(c, s); }},
    {kPacks, "packs", "Resource packs", "Texture mods on every map", [] { return g_host->resourcePacks != nullptr; }, [](Config& c, Settings&, const Names&) { drawPacks(c); return false; }},
    {kMisc, "misc", "Misc", "Minimap, split times, finish effects", [] { return static_cast<bool>(g_host->miscPage); }, [](Config&, Settings&, const Names&) { g_host->miscPage(); return false; }},
    {kSettings, "settings", "Settings", "General options", nullptr, [](Config& c, Settings& s, const Names&) { return drawSettings(c, s); }},
    {kAbout, "about", "About", "Version, updates, help", nullptr, [](Config& c, Settings&, const Names&) { drawAbout(c); return false; }},
};

bool pageShown(const PageDef& page) {
    return !page.available || page.available();
}

const PageDef* findPage(int id) {
    for (const PageDef& page : kPageTable) {
        if (page.id == id) return &page;
    }
    return nullptr;
}

// The page a saved number stands for; one this game doesn't have (or an unknown number): Settings.
const PageDef& pageFor(int id) {
    const PageDef* page = findPage(id);
    if (page && pageShown(*page)) return *page;
    return *findPage(kSettings);
}

Config::MenuWindow& windowOf(Config& config, const PageDef& page) {
    return config.menuWindows[page.key];
}

bool isDetached(Config& config, const PageDef& page) {
    const auto it = config.menuWindows.find(page.key);
    return it != config.menuWindows.end() && it->second.detached;
}

std::string windowName(const PageDef& page) {
    return std::string(page.title) + "##tmvsWindow_" + page.key;
}

// The first page in the bar that is shown and still in the menu (the Settings page if none).
int firstAttached(Config& config) {
    for (const PageDef& page : kPageTable) {
        if (pageShown(page) && !isDetached(config, page)) return page.id;
    }
    return kSettings;
}

void detachPage(Config& config, const PageDef& page) {
    Config::MenuWindow& w = windowOf(config, page);
    w.detached = true;
    if (config.menuPage == page.id) config.menuPage = firstAttached(config);
    config.markDirty();
}

void attachPage(Config& config, const PageDef& page) {
    windowOf(config, page).detached = false;
    config.menuPage = page.id;
    config.markDirty();
}

// A page's pointer-sized "pop out" symbol: a frame with an arrow leaving its corner.
void drawPopOutIcon(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 c) {
    const float w = max.x - min.x, t = 1.5f * g_scale;
    const ImVec2 a(min.x + w * 0.12f, min.y + w * 0.30f), b(min.x + w * 0.70f, min.y + w * 0.88f);
    draw->AddRect(a, b, c, 2.0f, 0, t);
    draw->AddLine(ImVec2(min.x + w * 0.45f, min.y + w * 0.55f), ImVec2(min.x + w * 0.90f, min.y + w * 0.10f), c, t);
    draw->AddLine(ImVec2(min.x + w * 0.58f, min.y + w * 0.10f), ImVec2(min.x + w * 0.90f, min.y + w * 0.10f), c, t);
    draw->AddLine(ImVec2(min.x + w * 0.90f, min.y + w * 0.10f), ImVec2(min.x + w * 0.90f, min.y + w * 0.42f), c, t);
}

void drawNavigation(Config& config) {
    const bool updateOut = update::latest().state == update::State::Available;
    const float h = 38.0f * g_scale;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (const PageDef& page : kPageTable) {
        if (!pageShown(page)) continue;
        const bool detached = isDetached(config, page);
        const bool active = config.menuPage == page.id && !detached;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const bool clicked = ImGui::InvisibleButton(page.title, ImVec2(w, h));
        const bool hovered = ImGui::IsItemHovered();
        // The pop-out symbol at the right end of the tab: a click on it makes the page a window.
        const float iconSize = 18.0f * g_scale;
        const ImVec2 iconMin(p.x + w - iconSize - 8.0f * g_scale, p.y + (h - iconSize) * 0.5f), iconMax(iconMin.x + iconSize, iconMin.y + iconSize);
        const bool onIcon = hovered && !detached && ImGui::IsMouseHoveringRect(iconMin, iconMax);
        if (clicked) {
            if (detached) {
                // Its window: in front (and open if it was minimized).
                windowOf(config, page).minimized = false;
                ImGui::SetWindowFocus(windowName(page).c_str());
            } else if (onIcon) {
                detachPage(config, page);
            } else if (!active) {
                config.menuPage = page.id;
                config.markDirty();
            }
        }
        if (hovered) {
            if (detached) ImGui::SetTooltip("%s is in its own window. Click to bring the window to the front.", page.title);
            else if (onIcon) ImGui::SetTooltip("Open %s in its own window", page.title);
            else ImGui::SetTooltip("%s", page.hint);
        }
        if (active || hovered) draw->AddRectFilled(p, ImVec2(p.x + w, p.y + h), active ? colour(kAccent, 0.16f) : IM_COL32(255, 255, 255, 14), 6.0f * g_scale);
        if (active) draw->AddRectFilled(ImVec2(p.x, p.y + h * 0.22f), ImVec2(p.x + 3.0f * g_scale, p.y + h * 0.78f), colour(kAccent, 1.0f), 2.0f);
        const float size = g_fontTitle->FontSize * 0.82f;
        draw->AddText(g_fontTitle, size, ImVec2(p.x + 14.0f * g_scale, p.y + (h - size) * 0.5f),
                      active ? IM_COL32_WHITE : ImGui::GetColorU32(ImGuiCol_Text, detached ? 0.4f : 0.7f), page.title);
        if (detached) {
            // Right-aligned, measured in its own size; left out where the title would run into it (the
            // dimmed title and the tooltip say it then).
            const char* mark = "(detached)";
            const float ms = g_fontTitle->FontSize * 0.6f;
            const float markX = p.x + w - g_fontTitle->CalcTextSizeA(ms, FLT_MAX, 0.0f, mark).x - 10.0f * g_scale;
            const float titleEnd = p.x + 14.0f * g_scale + g_fontTitle->CalcTextSizeA(size, FLT_MAX, 0.0f, page.title).x;
            if (markX > titleEnd + 8.0f * g_scale)
                draw->AddText(g_fontTitle, ms, ImVec2(markX, p.y + (h - ms) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text, 0.4f), mark);
        } else if (hovered || active) {
            drawPopOutIcon(draw, iconMin, iconMax, onIcon ? colour(kAccent, 1.0f) : ImGui::GetColorU32(ImGuiCol_Text, 0.45f));
        }
        // A new version: a dot on About, left of the pop-out symbol.
        if (page.id == kAbout && updateOut) draw->AddCircleFilled(ImVec2(p.x + w - 36.0f * g_scale, p.y + h * 0.5f), 4.0f * g_scale, colour(kAccent, 1.0f));
    }
    // Keys at the bottom of the bar.
    const float lines = ImGui::GetTextLineHeightWithSpacing() * 4.0f;
    const float space = ImGui::GetContentRegionAvail().y - lines;
    if (space > 0.0f) ImGui::Dummy(ImVec2(0.0f, space));
    ImGui::TextDisabled("%s  menu", keyText(keys::Action::Menu).c_str());
    ImGui::TextDisabled("%s  effects on/off", keyText(keys::Action::Effects).c_str());
    ImGui::TextDisabled("%s  frame capture", keyText(keys::Action::Capture).c_str());
    ImGui::TextDisabled("Changes save automatically");
}

// A page's body where it is shown: in the menu's page area or in a window of its own. The look
// settings it changed make the preset Custom (or the base preset again when all are back).
void drawPageBody(const PageDef& page, Config& config, Settings& s, const Names& names) {
    ImGui::PushItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.55f, 360.0f * g_scale));
    const bool look = page.draw(config, s, names);
    ImGui::PopItemWidth();
    if (look) lookEdited(config, s);
}

void drawMenu() {
    Config& config = Config::get();
    Settings& s = config.settings;
    // Placed again when the screen size or the layout changes; otherwise where you left it.
    static ImVec2 s_display(0.0f, 0.0f);
    static bool s_full = false;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const bool replace = display.x != s_display.x || display.y != s_display.y || config.menuFull != s_full;
    s_display = display;
    s_full = config.menuFull;
    placeWindow(config.menuFull, replace ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(620.0f * g_scale, display.x), std::min(480.0f * g_scale, display.y)), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * g_scale, 14.0f * g_scale));
    const bool open = ImGui::Begin("TM Vibrant Shaders", &config.showOverlay, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::End();
        return;
    }
    drawHeader(config, s);

    ImGui::BeginChild("navigation", ImVec2(170.0f * g_scale, 0.0f));
    drawNavigation(config);
    ImGui::EndChild();
    ImGui::SameLine(0.0f, 14.0f * g_scale);

    ImGui::BeginChild("page", ImVec2(0.0f, 0.0f));
    const PageDef& page = pageFor(config.menuPage);
    if (isDetached(config, page)) {
        // The page is in a window (it was the open one when saved, or About from the update note).
        ImGui::TextDisabled("%s is in its own window.", page.title);
        if (ImGui::Button("Show window")) {
            windowOf(config, page).minimized = false;
            ImGui::SetWindowFocus(windowName(page).c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Move back")) attachPage(config, page);
    } else {
        drawPageBody(page, config, s, config.presetNames());
    }
    ImGui::EndChild();
    ImGui::End();
}

// --- Detached windows ----------------------------------------------------------------

bool windowKept(const Config::MenuWindow& w) {
    return w.detached && w.keepOpen;
}

// One page in a window of its own. Position, size, minimized state and the options are saved
// ([Windows]); closing it (X) puts the page back in the menu's tab bar.
void drawDetachedWindow(const PageDef& page, Config& config, bool menuOpen) {
    Config::MenuWindow& w = windowOf(config, page);
    if (!menuOpen && !w.keepOpen) return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float sc = g_scale;
    ImVec2 size(w.w * sc, w.h * sc), pos(w.x * sc, w.y * sc);
    if (w.w <= 0.0f) {
        // First time: beside the menu, each new window a step further.
        static int s_placed = 0;
        size = ImVec2(std::min(560.0f * sc, display.x - 40.0f * sc), std::min(560.0f * sc, display.y - 40.0f * sc));
        pos = ImVec2(display.x * 0.35f + 36.0f * sc * (s_placed % 6), 60.0f * sc + 36.0f * sc * (s_placed % 6));
        s_placed++;
        w.x = pos.x / sc, w.y = pos.y / sc, w.w = size.x / sc, w.h = size.y / sc;
    }
    size.x = std::clamp(size.x, std::min(320.0f * sc, display.x), display.x);
    size.y = std::clamp(size.y, std::min(160.0f * sc, display.y), display.y);
    // Never off the screen (another resolution than when it was saved).
    pos.x = std::clamp(pos.x, 0.0f, std::max(0.0f, display.x - size.x));
    pos.y = std::clamp(pos.y, 0.0f, std::max(0.0f, display.y - std::min(size.y, 40.0f * sc)));
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(320.0f * sc, display.x), std::min(160.0f * sc, display.y)), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::SetNextWindowCollapsed(w.minimized, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w * (static_cast<float>(w.opacity) / 100.0f));
    bool open = true;
    const std::string name = windowName(page);
    const bool visible = ImGui::Begin(name.c_str(), &open, ImGuiWindowFlags_NoSavedSettings);
    const bool collapsed = ImGui::IsWindowCollapsed();
    // Remember where it is, once the mouse is up (a drag is many positions).
    const ImVec2 at = ImGui::GetWindowPos(), extent = ImGui::GetWindowSize();
    if (!ImGui::IsMouseDown(0) || collapsed != w.minimized) {
        const bool changed = collapsed != w.minimized || (!collapsed && (std::fabs(at.x / sc - w.x) > 0.5f || std::fabs(at.y / sc - w.y) > 0.5f ||
                                                                          std::fabs(extent.x / sc - w.w) > 0.5f || std::fabs(extent.y / sc - w.h) > 0.5f));
        if (changed) {
            w.minimized = collapsed;
            if (!collapsed) {
                w.x = std::round(at.x / sc), w.y = std::round(at.y / sc);
                w.w = std::round(extent.x / sc), w.h = std::round(extent.y / sc);
            }
            config.markDirty();
        }
    }
    if (visible && !collapsed) {
        if (ImGui::SmallButton("Window options")) ImGui::OpenPopup("windowOptions");
        if (ImGui::BeginPopup("windowOptions")) {
            if (ImGui::Checkbox("Keep open when the menu closes", &w.keepOpen)) config.markDirty();
            ImGui::SetNextItemWidth(180.0f * sc);
            if (ImGui::SliderInt("Opacity", &w.opacity, 30, 100, "%d%%")) config.markDirty();
            if (ImGui::Button("Back to menu")) {
                open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::Separator();
        // Its own scroll area under the options, like the menu's page area.
        ImGui::BeginChild("body", ImVec2(0.0f, 0.0f));
        drawPageBody(page, config, config.settings, config.presetNames());
        ImGui::EndChild();
    }
    ImGui::End();
    if (!open) attachPage(config, page);
}

void drawDetachedWindows(Config& config, bool menuOpen) {
    for (const PageDef& page : kPageTable) {
        if (pageShown(page) && isDetached(config, page)) drawDetachedWindow(page, config, menuOpen);
    }
}

} // namespace

const char* tooltip(const char* key) {
    return tooltipFor(key);
}

void lookEdited(Config& config, Settings& s) {
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

void setupStyle(HWND window) {
    ImGuiIO& io = ImGui::GetIO();
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
    colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    // Segoe UI (every Windows has it) reads much better than the built-in pixel font; sizes
    // grow with the screen.
    RECT client{};
    GetClientRect(window, &client);
    g_scale = std::clamp(static_cast<float>(client.bottom - client.top) / 1080.0f, 1.0f, 2.0f);
    style.ScaleAllSizes(g_scale);
    char fonts[MAX_PATH] = {};
    GetWindowsDirectoryA(fonts, MAX_PATH);
    const std::string regular = std::string(fonts) + "\\Fonts\\segoeui.ttf", semibold = std::string(fonts) + "\\Fonts\\seguisb.ttf";
    if (GetFileAttributesA(regular.c_str()) != INVALID_FILE_ATTRIBUTES) {
        io.Fonts->AddFontFromFileTTF(regular.c_str(), 17.0f * g_scale);
        g_fontTitle = io.Fonts->AddFontFromFileTTF(GetFileAttributesA(semibold.c_str()) != INVALID_FILE_ATTRIBUTES ? semibold.c_str() : regular.c_str(),
                                                   22.0f * g_scale);
    }
    if (!g_fontTitle) g_fontTitle = io.Fonts->AddFontDefault();
}

float scale() {
    return g_scale;
}

void draw(const Host& host) {
    g_host = &host;
    g_keysShown = false;
    Config& config = Config::get();
    // Shown in the game only while the pointer rests on a card (set again by the card).
    config.previewPreset.clear();
    if (g_cards.hovered != g_cards.resting) {
        g_cards.resting = g_cards.hovered;
        g_cards.restingSince = ImGui::GetTime();
    }
    g_cards.hovered.clear();
    const bool menuOpen = config.showOverlay;
    if (menuOpen) drawMenu();
    // After the menu: in front of it.
    if (!host.hideDetached) drawDetachedWindows(config, menuOpen);
    if (keys::capturing() && !g_keysShown) keys::cancelCapture();
    g_host = nullptr;
}

bool pinnedWindows() {
    for (const auto& entry : Config::get().menuWindows) {
        if (windowKept(entry.second)) return true;
    }
    return false;
}

bool consumeReloadRequest() {
    const bool requested = g_reloadRequested;
    g_reloadRequested = false;
    return requested;
}

void drawSplitBar(const std::string& mine, std::string& other, float position, bool mineLeft, const std::string& note, float bottom) {
    Config& config = Config::get();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float x = position * size.x, y = size.y - bottom * g_scale, pad = 8.0f * g_scale;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
    const char* kGame = "The game's image";
    // Your side.
    ImGui::SetNextWindowPos(ImVec2(mineLeft ? x - pad : x + pad, y), ImGuiCond_Always, ImVec2(mineLeft ? 1.0f : 0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##splitMine", nullptr, flags | ImGuiWindowFlags_NoInputs);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(mine.c_str());
    ImGui::End();
    // The other side.
    const std::vector<std::string> names = config.presetNames();
    float widest = ImGui::CalcTextSize(kGame).x;
    for (const std::string& name : names) widest = std::max(widest, ImGui::CalcTextSize(name.c_str()).x);
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

} // namespace menu
} // namespace tmshaders
