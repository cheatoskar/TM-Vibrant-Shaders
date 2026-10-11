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
};

namespace options {

Options& current();
void load();
void save();

} // namespace options

// Sets a look setting (a settings.ini key: Exposure, Bloom, ToneCurve ...; also Preset, Enabled,
// Lut, LutAmount) or a ManiaPlanet option (Placement, FallbackAtEnd, View, Split). False for an
// unknown key. For the offline tools and the test.
bool setValue(const std::string& key, const std::string& value);

} // namespace tmvs_mp
