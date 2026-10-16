#pragma once
#include <string>

namespace tmvs_mp {

// The look (presets, every setting, the F8 menu) is the TMNF mod's: tmshaders::Config and
// tmshaders::Settings, shared. What only ManiaPlanet has lives here, in maniaplanet.ini.

// Where the mod's grading pass goes in the frame.
enum class Placement : int {
    BeforeHud = 0, // after the engine's tone map and post effects, before the interface (default)
    EndOfFrame = 1 // on the finished image at Present: the interface is graded too
};

// Normal and Split show the mod; the rest show what it reads from the engine (checks).
enum class View : int { Normal = 0, Split = 1, Depth = 2, Normals = 3, Motion = 4, Albedo = 5, WorldGrid = 6 };

struct Options {
    // The look on the engine's HDR image in place of its tone map (else on its finished image).
    bool hdrStage = true;
    Placement placement = Placement::BeforeHud;
    // When the interface start can't be found in a frame, still grade at the end of it.
    bool fallbackAtEnd = true;
    View view = View::Normal;
    float split = 0.5f; // Split: the game's own image left of this
    // Soft sun shadows: the light's angular diameter in degrees (the sun is 0.53) for the mod's
    // PCSS shader in place of the engine's sun shadow pass (default 1, the owner's pick); 0 keeps the engine's.
    float softShadows = 1.0f;
    // The light sources (lamps, screens, halos, the car's lights) from what the engine draws as
    // light, not guessed from the image's colours (engine_lights.cpp). On since the owner's test
    // (2026-10-16, captures 031/032): grass no longer glows, the road borders still do.
    bool engineLightDraws = true;
    // Night maps under the black hole: the engine's moon (its diffuse light and its blue glint) is
    // taken out of the image and the black hole lights the scene instead, at this strength (1 = as
    // bright as the game's moon; 0 = off, the default). Shader side only (scene.hlsl).
    float blackHoleMoon = 0.0f;
};

namespace options {

Options& current();
void load();
void save();

} // namespace options

// Sets a look setting (a settings.ini key: Exposure, Bloom, ToneCurve ...; also Preset, Enabled,
// Lut, LutAmount) or a ManiaPlanet option (Placement, FallbackAtEnd, View, Split, SoftShadows). False for an
// unknown key. For the offline tools and the test.
bool setValue(const std::string& key, const std::string& value);

} // namespace tmvs_mp
