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

bool supported(const Field& f) {
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
    const float total = pipeline->total();
    const float fps = ImGui::GetIO().Framerate;
    if (total <= 0.0f) {
        ImGui::TextDisabled("Measuring... (needs GPU timestamp queries)");
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
        {"Lighting", ImVec4(0.86f, 0.62f, 0.28f, 1.0f)},       {"Look", ImVec4(0.86f, 0.62f, 0.28f, 1.0f)},
        {"Sky", ImVec4(0.38f, 0.58f, 0.90f, 1.0f)},            {"Sky & Atmosphere", ImVec4(0.38f, 0.58f, 0.90f, 1.0f)},
        {"Bloom & Lens", ImVec4(0.64f, 0.50f, 0.88f, 1.0f)},   {"Colour", ImVec4(0.88f, 0.48f, 0.58f, 1.0f)},
        {"Weather", ImVec4(0.30f, 0.72f, 0.68f, 1.0f)},        {"Weather & Surfaces", ImVec4(0.30f, 0.72f, 0.68f, 1.0f)},
        {"Cinematic", ImVec4(0.58f, 0.62f, 0.70f, 1.0f)},      {"Image", ImVec4(0.50f, 0.70f, 0.50f, 1.0f)},
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
           !strcmp(f.key, "VideoExportBest") || !strcmp(f.key, "VideoCodec") || !strcmp(f.key, "VideoBitrate") || !strcmp(f.key, "TAAJitter") || !strcmp(f.key, "GameNight") || !strcmp(f.key, "EngineMoon") || !strcmp(f.key, "GameSkyClouds") ||
           !strcmp(f.key, "CleanHighlights") || !strcmp(f.key, "GameHdr") || !strcmp(f.key, "EngineLights") ||
           !strcmp(f.key, "CasterShadows") || !strcmp(f.key, "CleanWater") || !strcmp(f.key, "GameGBuffer") ||
           !strcmp(f.key, "ShadeMenuBars");
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
    if (is("FocusDistance") || is("BokehSize")) return s.depthOfField > 0.0f;
    if (is("DofAutofocus")) return s.depthOfField > 0.0f && s.focusDistance <= 0.0f; // a set distance overrides it
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
        s.skyEffectSize = 3.0f;
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

bool toggle(const char* id, bool* value); // below, with the other widgets

// What a setting does, shown when the mouse rests on it: the labels stay short, the detail is here.
// "(restart)" / "(next map)" say when a change takes effect.
const char* tooltipFor(const char* key) {
    static const struct {
        const char* key;
        const char* tip;
    } kTips[] = {
        {"AOStrength", "Darkens corners and creases where light would not reach."},
        {"AORadius", "How far around a point the occlusion looks. Large values darken whole areas, small ones only tight corners."},
        {"SunLight", "How strongly sunlit surfaces take on the sun colour."},
        {"AmbientTint", "How strongly surfaces in shade take on the sky colour."},
        {"SunColor", "The colour of the sunlight."},
        {"SkyColor", "The colour of shade and ambient light."},
        {"SkyBrightness", "Brightness of the mod's sky."},
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
        {"ToneCurve", "How bright colours roll off to white (TrackMania 2): 0 the game's curve, 1 the mod's, 2 ACES."},
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
        {"ShadowStrength", "Sun shadows drawn by the mod. 0 leaves the game's own."},
        {"ShadowLength", "How far a shadow is traced from each point. Longer is more accurate but slower."},
        {"LongShadows", "Shadows from far-away objects, such as hills and buildings."},
        {"LongShadowRange", "How far the long shadows reach, in metres."},
        {"LongShadowQuality", "Stepped is fast. Every cell checks the whole map grid, so thin walls leave no gaps."},
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
        {"LampGlare", "Part of the lamps' light leaves the bright core and spreads wide and soft around them: the bulbs stay apart, the light reaches farther. 0 = off. Needs Game light sources."},
        {"LampHaze", "The light the haze takes from the lamps glows in the air around them; stronger with more haze and for far lamps. 1 = all of it, 0 = off. Needs Game light sources."},
        {"LampSkyHalos", "At night the lamps' halos also shine over the sky instead of ending at the edge of the lamp's frame. Needs Game light sources."},
        {"Floodlights", "Brightness and glow of stadium floodlights, start lights and other lamps, and the light they throw around them. The neon borders and Light sources stay as they are. 1 = as before. Needs Game light sources."},
        {"ToneKnee", "Where bright colours start to roll off instead of clipping."},
        {"NightSurfaces", "How bright floodlit surfaces get at night. 1 keeps the game's white."},
        {"Rain", "0 is dry, 2 is a downpour."},
        {"Snow", "0 is none, 2 is a blizzard."},
        {"LensDrops", "Drops and flakes run down the camera lens."},
        {"WeatherShelter", "No rain or snow under roofs and bridges."},
        {"WeatherSound", "Volume of rain and thunder."},
        {"WaterSurfaces", "Reflections on pools and the sea."},
        {"Reflections", "Reflections on the dry track. Above 1 it turns into a mirror."},
        {"Spray", "Spray behind the car on wet roads and snow."},
        {"CinematicOnlyInReplays", "Motion blur and depth of field only in replays and video export."},
        {"VideoExportBest", "Renders each video frame with full quality and the effects on the video's own clock."},
        {"VideoCodec", "What the replay editor's Shoot video writes. MP4 is encoded by the graphics card and skips the game's codec dialog; the file lands where the .avi would."},
        {"VideoBitrate", "Mbit/s. 0 picks one: 50 for 4K 30 fps H.264, 30 for HEVC."},
        {"FocusDistance", "Metres to the sharp point. 0 focuses automatically."},
        {"DofAutofocus", "Where the automatic focus sits. Middle: what is in the middle of the screen. Car: your car, wherever the replay camera puts it; the middle again while the car is off screen. A click in the photo mode still wins."},
        {"TAA", "Smooths edges by blending frames. Costs a little sharpness."},
        {"TAAJitter", "Shifts the image by sub-pixels each frame for finer edges."},
        {"Sharpen", "Contrast-adaptive sharpening (CAS)."},
        {"AutoQuality", "Below the target FPS it switches off the effects that cost most for least look, and back on when there is room. Your settings stay as they are."},
        {"TargetFPS", "The frame rate auto quality aims for."},
        {"DisableGameMSAA", "Turns off the game's own MSAA, which the mod's effects do not need. (restart)"},
        {"ReadableGameDepth", "Lets the effects work in replays and video export. (restart)"},
        {"GameMaterials", "Reads the game's material ids. Used for the TMUF sea and the debug view."},
        {"DisableGameFX", "Turns off the game's own bloom and colour filter. (next map)"},
        {"GameNight", "Under night skies the game lights day maps as at midnight."},
        {"EngineMoon", "Under the black hole: the game's moonlight (baked light and shadows, glints, the car) comes from the black hole, stays as the game has it, or is left out. (next map)"},
        {"GameSkyClouds", "Keeps the game's sunset clouds, and the sea mirroring them, under night skies."},
        {"GameHdr", "The game draws into a float image, so bright light keeps its texture. The capture key saves it too."},
        {"CleanHighlights", "Floodlit white at night stays even. Off is the 1.4.0 look."},
        {"EngineLights", "Lamps and glows glow as the game draws them instead of being guessed from the image. Needs Game HDR."},
        {"CasterShadows", "Long shadows from the game's own meshes, cast from the sun or black hole. Needs Long shadows."},
        {"CleanWater", "Pools mirror only what they can see, sharp. Off shows blotches of the ramps over them."},
        {"GameGBuffer", "Surface normals from the game's shaders, so AO, shadows, reflections and light follow bump maps and the car's real shape. Needs Game HDR. (restart the first time)"},
        {"ShadeMenuBars", "Shades the image under the replay menu's bars. Off shows leftovers of earlier frames."},
        {"NeonTrail", "A glowing trail behind the car. 0 is off."},
        {"TrailTyres", "Two lines from the rear tyres instead of one."},
        {"TrailDuration", "Seconds until the trail fades. 0 keeps the whole run."},
        {"TrailKeepRuns", "How many earlier runs' trails stay after a restart, dimmer the older they are. 0 clears the trail on every restart."},
        // Sharing presets as text (Shaders page).
        {"ShareCopy", "Puts the current look on the clipboard as one line of text (TMVS1:...) to paste into a chat. Only the look's settings are in it, not your installation's (auto quality, sound volume, neon trail, ...). The same code works in TrackMania Nations/United and TrackMania 2."},
        {"SharePaste", "Reads a share code from the clipboard and makes it your look: unsaved, on top of the built-in preset it is closest to, so Save as preset keeps it. Nothing saved is replaced. Settings this version doesn't know are ignored and counted; values out of range are moved into range."},
        // Photo mode (General, not a look setting).
        {"SplitPhotoLabels", "Photos of the compare split get both sides' names burnt in at the bottom, at the line (the list on the screen is never in them)."},
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
    switch (f.kind) {
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
            } else if (!strcmp(f.key, "ToneCurve")) {
                static const char* kCurves[] = {"The game's", "TMVS (keeps hue)", "ACES (film)"};
                changed |= ImGui::Combo(label, value, kCurves, IM_ARRAYSIZE(kCurves));
            } else if (!strcmp(f.key, "VideoCodec")) {
                static const char* kCodecs[] = {"Game's AVI", "MP4 (H.264)", "MP4 (HEVC)"};
                changed |= ImGui::Combo(label, value, kCodecs, IM_ARRAYSIZE(kCodecs));
            } else if (!strcmp(f.key, "DofAutofocus")) {
                static const char* kFocusModes[] = {"Middle", "Car"};
                changed |= ImGui::Combo(label, value, kFocusModes, IM_ARRAYSIZE(kFocusModes));
            } else if (!strcmp(f.key, "EngineMoon")) {
                static const char* kMoons[] = {"Game's", "Black hole", "Off"};
                changed |= ImGui::Combo(label, value, kMoons, IM_ARRAYSIZE(kMoons));
            } else if (!strcmp(f.key, "LongShadowQuality")) {
                static const char* kLongQualities[] = {"Stepped", "Every cell"};
                changed |= ImGui::Combo(label, value, kLongQualities, IM_ARRAYSIZE(kLongQualities));
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
    if (f.kind != Field::Bool) hovered = ImGui::IsItemHovered();
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

enum Page { kShaders, kCustomize, kStudio, kMaps, kPerformance, kSettings, kAbout, kPageCount };

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
// (--bench, Stadium start gate, 1080p: light < 7 ms, medium < 9 ms, heavy above).
struct BuiltInInfo {
    const char* name;
    const char* blurb;
    int cost;
};
const BuiltInInfo kBuiltIn[] = {
    {"Vibrant", "The default. Warm sun, rich colours, glowing borders, light shafts.", 3},
    {"Realistic", "The game's own colours and sky; only the light is new.", 2},
    {"Golden Hour", "Clear sky, low warm sun, long light shafts.", 3},
    {"Dreamy", "Soft pastel bloom, pink and teal.", 2},
    {"Neon", "Starry night, bright neon that lights up the whole track.", 2},
    {"Horizon", "A glowing black hole and a ringed planet over a dark stadium.", 3},
    {"Aurora", "Northern lights over the track, green and teal.", 3},
    {"Competition", "Clarity first: shadows and depth, no haze or lens effects.", 1},
    {"Performance", "The Vibrant look for weaker GPUs: fewer samples, no shafts.", 1},
    {"Rainy", "Overcast, wet track, puddles and rain.", 2},
    {"Replay Cinema", "Film look with motion blur and depth of field, for replays.", 3},
    {"Storm", "Low clouds, pouring rain, deep puddles, lightning and thunder.", 3},
    {"Snowstorm", "A blizzard: driving snow and thick haze close around you.", 2},
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
            if (!active && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Apply the preset first: the picture is taken from the game view.");
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
    if (ImGui::Button("Open the presets folder")) {
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
                          if (ImGui::Button("Open the LUT folder")) {
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
    panels.push_back(settingsPanel("Light and shadows", s,
                                   {{"ShadowStrength", "Shadows"},
                                    {"LongShadows", "Long shadows"},
                                    {"AOStrength", "Ambient occlusion"},
                                    {"GodRays", "Light shafts"},
                                    {"NeonLight", "Neon light spill"},
                                    {"Exposure", "Brightness"},
                                    {"Contrast", "Contrast"},
                                    {"Saturation", "Colour"}}));
    panels.push_back(settingsPanel("Bloom and lens", s,
                                   {{"Bloom", "Glow"},
                                    {"BloomRadius", "Glow radius"},
                                    {"HighlightBoost", "Light sources"},
                                    {"BorderGlow", "Neon borders"},
                                    {"Floodlights", "Floodlights"},
                                    {"LensFlare", "Lens flare"},
                                    {"ChromaticAberration", "Colour fringes"},
                                    {"Vignette", "Vignette"},
                                    {"FilmGrain", "Film grain"}}));
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
                                    {"Reflections", "Reflections"},
                                    {"ReflectionBlur", "Reflection blur"}}));
    panels.push_back(settingsPanel("Neon trail", s,
                                   {{"NeonTrail", "Neon trail"},
                                    {"TrailColor", "Colour"},
                                    {"TrailWidth", "Width"},
                                    {"TrailTyres", "Tyre lines"},
                                    {"TrailDuration", "Trail fade (s)"},
                                    {"TrailKeepRuns", "Earlier runs"}}));
    panels.push_back(settingsPanel("Cinematic", s,
                                   {{"MotionBlur", "Motion blur"},
                                    {"DepthOfField", "Depth of field"},
                                    {"DofAutofocus", "Focus on"},
                                    {"CinematicOnlyInReplays", "Cinematic in replays only"}}));
    panels.push_back(settingsPanel("Image quality", s,
                                   {{"TAA", "Temporal AA"},
                                    {"FXAA", "FXAA"},
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
    ImGui::TextDisabled("Sun: %s (%.2f %.2f %.2f)", g_host->sunKnown ? "from game" : "unknown", g_host->sunDirection[0], g_host->sunDirection[1],
                        g_host->sunDirection[2]);
    ImGui::Spacing();

    // One panel per category. Searching shows the matches of every category; a match that
    // does nothing with the current choices is greyed out.
    std::vector<PanelDef> panels;
    const bool searching = query[0] != 0;
    for (const Field& f : fields()) {
        const bool match = !searching || containsNoCase(f.label, query) || containsNoCase(f.key, query) || containsNoCase(f.category, query);
        if (!match || (!searching && !isRelevant(f, s))) continue;
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
                                      if (strcmp(g.category, category) != 0) continue;
                                      const bool hit = !searching || containsNoCase(g.label, query) || containsNoCase(g.key, query) ||
                                                       containsNoCase(g.category, query);
                                      const bool relevant = isRelevant(g, s);
                                      if (!hit || (!searching && !relevant)) continue;
                                      ImGui::BeginDisabled(!relevant);
                                      l |= drawField(g, s);
                                      ImGui::EndDisabled();
                                      if (!relevant && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                                          ImGui::SetTooltip("No effect with the current settings");
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
    pageTitle("Maps", "Presets for day, sunset and night maps. The map type is detected from its lighting.");
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
        ImGui::TextWrapped("Your pick (%s) holds on every map until you choose a preset for a map type below.", config.preset.c_str());
        if (ImGui::Button("Use the presets per map type again")) config.resumeMoodPresets();
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
    panels.push_back({"Performance", 5.0f, [&s] {
                          bool l = drawKey(s, "AutoQuality");
                          l |= drawKey(s, "TargetFPS");
                          l |= drawKey(s, "Quality");
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
                                        "Day, sunset and night maps each use their own preset (see Maps). Off: the same preset everywhere.\n"
                                        "A preset you pick holds on every map until you choose one for a map type again.") &&
                              config.autoMood)
                              config.resumeMoodPresets();
                          ImGui::BeginDisabled(!config.autoMood);
                          switchRow("Remember preset picks", &config.rememberMoodPick,
                                    "On: picking a preset on a night map makes it the preset for all night maps (same for day and sunset).\nOff: only the Maps page sets these.");
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
    // Its own panel: in the long Game panel the owner didn't find it.
    panels.push_back({"Video export", 3.0f, [&s] {
                          bool l = drawKey(s, "VideoCodec");
                          l |= drawKey(s, "VideoBitrate");
                          l |= drawKey(s, "VideoExportBest");
                          return l;
                      }});
    panels.push_back({"Game", 8.0f, [&s] {
                          bool l = drawKey(s, "CinematicOnlyInReplays");
                          l |= drawKey(s, "WeatherSound");
                          l |= drawKey(s, "TAA");
                          l |= drawKey(s, "TAAJitter");
                          l |= drawKey(s, "DisableGameMSAA");
                          l |= drawKey(s, "ReadableGameDepth");
                          l |= drawKey(s, "GameMaterials");
                          l |= drawKey(s, "DisableGameFX");
                          l |= drawKey(s, "GameNight");
                          l |= drawKey(s, "EngineMoon");
                          l |= drawKey(s, "GameSkyClouds");
                          l |= drawKey(s, "GameHdr");
                          l |= drawKey(s, "CleanHighlights");
                          l |= drawKey(s, "EngineLights");
                          l |= drawKey(s, "CasterShadows");
                          l |= drawKey(s, "CleanWater");
                          l |= drawKey(s, "GameGBuffer");
                          l |= drawKey(s, "ShadeMenuBars");
                          return l;
                      }});
    panels.push_back({"Photo mode", g_host->splitOn ? (g_host->splitPreset ? 13.0f : 11.0f) : 8.0f, [&config] {
                          const std::string capKey = keyText(keys::Action::Capture), photoKey = keyText(keys::Action::Photo);
                          const std::string splitKey = keyText(keys::Action::PhotoSplit);
                          switchRow("Auto photo mode", &config.photoAuto,
                                    ("Photo mode turns on by itself with the free camera (cam 7): no HUD, full quality, far-reaching effects.\n" + photoKey +
                                     " turns it on or off anywhere.").c_str());
                          switchRow("Hide HUD", &config.photoHideHud, "Off: the game's HUD stays on screen in photo mode (photos never have it).");
                          ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
                          char factor[48];
                          snprintf(factor, sizeof(factor), "%d x %d tiles = %.1fx", config.photoTiles, config.photoTiles, config.photoTiles * 0.84f);
                          if (ImGui::SliderInt("Hi-res photo", &config.photoTiles, 2, 4, factor)) config.markDirty();
                          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shift+%s: a photo this many times the screen size. Pause the replay while the tiles are taken.", capKey.c_str());
                          ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
                          char aperture[32];
                          snprintf(aperture, sizeof(aperture), "f/%.1f", (g_host->fNumber ? g_host->fNumber(config.photoDof) : 0.0f));
                          if (ImGui::SliderFloat("Aperture", &config.photoDof, 0.0f, 1.0f, aperture)) config.markDirty();
                          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Like a camera lens: wide open (f/1.4) blurs most around the clicked focus point, f/16 keeps all sharp (Ctrl+wheel in the game).");
                          if (g_host->splitOn) {
                              bool& split = *g_host->splitOn;
                              if (switchRow("Compare split", &split,
                                            ("One side of a line shows the game's image without the mod's effects (or another preset's look), the other yours (" + splitKey +
                                             ").\n" +
                                             // TMNF (the one with a preset side): hi-res photos and the aperture on both sides too.
                                             (g_host->splitPreset ? "In photos too, hi-res ones included; the aperture blurs both sides alike."
                                                                  : "In photos too, not in hi-res ones.") +
                                             " The game's own changes stay on both sides (its bloom off, real night).").c_str())) {
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
                                      ImGui::SetTooltip("What the other side of the line shows: the game's own image, or a preset's look next to yours.\n"
                                                        "A preset costs a second run of the effects every frame (photo mode only).");
                                  switchRow("Photo labels", &config.splitPhotoLabels, tooltipFor("SplitPhotoLabels"));
                              }
                              ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
                              ImGui::SliderFloat("Line", g_host->splitPosition, 0.0f, 1.0f, "%.2f");
                              if (ImGui::IsItemHovered()) ImGui::SetTooltip("Where the line sits, from the left (Ctrl+Shift+wheel in the game).");
                              ImGui::Checkbox("Swap sides", g_host->splitSwapped);
                              if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shows the game's image on the right of the line instead of the left.");
                              ImGui::EndDisabled();
                          }
                          ImGui::TextDisabled("%s photo  |  Shift+%s hi-res  |  middle click: focus", capKey.c_str(), capKey.c_str());
                          ImGui::TextDisabled("Ctrl+middle click: no focus  |  Ctrl+wheel: aperture  |  Shift+wheel: zoom  |  %s: on/off", photoKey.c_str());
                          if (g_host->splitOn) ImGui::TextDisabled("%s: compare split  |  Ctrl+Shift+wheel: move its line", splitKey.c_str());
                          return false;
                      }});
    panels.push_back({"Files", 3.0f, [&config] {
                          if (ImGui::Button("Open the TMVS folder")) ShellExecuteW(nullptr, L"open", log::dataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                          if (ImGui::IsItemHovered()) ImGui::SetTooltip("settings.ini, tmvs.log, the F12 captures and the photos.");
                          ImGui::SameLine();
                          if (ImGui::Button("Open the presets folder")) {
                              CreateDirectoryW(config.presetDir().c_str(), nullptr);
                              ShellExecuteW(nullptr, L"open", config.presetDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                          }
                          ImGui::TextDisabled("All changes are saved automatically.");
                          return false;
                      }});
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
        if (line.rfind("![", 0) == 0) continue; // pictures: only on the release page
        // Links: [text](url) -> text.
        for (size_t open; (open = line.find('[')) != std::string::npos;) {
            const size_t close = line.find("](", open);
            const size_t end = close == std::string::npos ? std::string::npos : line.find(')', close);
            if (end == std::string::npos) break;
            line = line.substr(0, open) + line.substr(open + 1, close - open - 1) + line.substr(end + 1);
        }
        if (line.empty()) {
            ImGui::Spacing();
        } else if (line[0] == '#') {
            ImGui::TextColored(kAccent, "%s", line.substr(line.find_first_not_of("# ")).c_str());
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
    panels.push_back({"Updates", info.state == update::State::Available ? 14.0f : 4.0f, [&config, info] {
                          switch (info.state) {
                              case update::State::Checking: ImGui::TextDisabled("Checking for updates..."); break;
                              case update::State::UpToDate: ImGui::Text("You're up to date (%s).", TM_SHADERS_VERSION_A); break;
                              case update::State::Failed: ImGui::TextDisabled("Couldn't check for updates. Are you offline?"); break;
                              case update::State::Off: ImGui::TextDisabled("Not checked."); break;
                              case update::State::Available: {
                                  ImGui::PushFont(g_fontTitle);
                                  ImGui::TextColored(kAccent, "Update available: %s", info.version.c_str());
                                  ImGui::PopFont();
                                  ImGui::TextDisabled("Installed: %s. Download the new version and run its Setup.", TM_SHADERS_VERSION_A);
                                  if (ImGui::Button("Download")) openLink(info.url);
                                  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opens the release page in your browser. In fullscreen the game may minimise.");
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
                          if (ImGui::Button("Open the TMVS folder")) {
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
                          ImGui::TextWrapped("Built with Dear ImGui, minimp3 and stb. Sharpening: AMD FidelityFX CAS. Anti-aliasing: FXAA 3.11.");
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

void drawNavigation(Config& config) {
    static const char* const kPages[] = {"Shaders", "Customize", "Studio", "Maps", "Performance", "Settings", "About"};
    static const char* const kHints[] = {"Browse and apply presets", "Main settings", "All settings", "Presets for day, sunset and night maps",
                                         "Frame rate and effect costs", "General options", "Version, updates, help"};
    const bool updateOut = update::latest().state == update::State::Available;
    const float h = 38.0f * g_scale;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (int i = 0; i < kPageCount; i++) {
        const bool active = config.menuPage == i;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        if (ImGui::InvisibleButton(kPages[i], ImVec2(w, h)) && !active) {
            config.menuPage = i;
            config.markDirty();
        }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetTooltip("%s", kHints[i]);
        if (active || hovered) draw->AddRectFilled(p, ImVec2(p.x + w, p.y + h), active ? colour(kAccent, 0.16f) : IM_COL32(255, 255, 255, 14), 6.0f * g_scale);
        if (active) draw->AddRectFilled(ImVec2(p.x, p.y + h * 0.22f), ImVec2(p.x + 3.0f * g_scale, p.y + h * 0.78f), colour(kAccent, 1.0f), 2.0f);
        const float size = g_fontTitle->FontSize * 0.82f;
        draw->AddText(g_fontTitle, size, ImVec2(p.x + 14.0f * g_scale, p.y + (h - size) * 0.5f),
                      active ? IM_COL32_WHITE : ImGui::GetColorU32(ImGuiCol_Text, 0.7f), kPages[i]);
        // A new version: a dot on About.
        if (i == kAbout && updateOut) draw->AddCircleFilled(ImVec2(p.x + w - 14.0f * g_scale, p.y + h * 0.5f), 4.0f * g_scale, colour(kAccent, 1.0f));
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

void drawMenu() {
    Config& config = Config::get();
    Settings& s = config.settings;
    // Shown in the game only while the pointer rests on a card (set again by the card).
    config.previewPreset.clear();
    if (g_cards.hovered != g_cards.resting) {
        g_cards.resting = g_cards.hovered;
        g_cards.restingSince = ImGui::GetTime();
    }
    g_cards.hovered.clear();

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
    ImGui::PushItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.55f, 360.0f * g_scale));
    const std::vector<std::string> names = config.presetNames();
    bool look = false;
    switch (config.menuPage) {
        case kCustomize: look = drawCustomize(config, s); break;
        case kStudio: look = drawStudio(config, s); break;
        case kMaps: drawMaps(config, names); break;
        case kPerformance: look = drawPerformancePage(s); break;
        case kSettings: look = drawSettings(config, s); break;
        case kAbout: drawAbout(config); break;
        default: drawShaders(config); break;
    }
    ImGui::PopItemWidth();
    ImGui::EndChild();

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
    ImGui::End();
}

} // namespace

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
    drawMenu();
    if (keys::capturing() && !g_keysShown) keys::cancelCapture();
    g_host = nullptr;
}

bool consumeReloadRequest() {
    const bool requested = g_reloadRequested;
    g_reloadRequested = false;
    return requested;
}

} // namespace menu
} // namespace tmshaders
