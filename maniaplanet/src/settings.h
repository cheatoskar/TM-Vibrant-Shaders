#pragma once
#include <string>
#include <vector>

namespace tmvs_mp {

// Where the mod's pass goes in the frame.
enum class Placement : int {
    BeforeHud = 0, // after the engine's tone map and post effects, before the interface (default)
    EndOfFrame = 1 // on the finished image at Present: the interface is graded too
};

enum class View : int { Normal = 0, Split = 1, Depth = 2 };

struct Settings {
    bool enabled = true;
    std::string preset = "Vibrant";

    // Grading of the engine's finished (tone-mapped) image.
    float exposure = 0.0f;   // stops
    float contrast = 0.2f;   // S-curve, 0 = none
    float saturation = 1.08f;
    float vibrance = 0.35f;  // saturation that spares already saturated colours
    float warmth = 0.0f;     // -1 cold .. 1 warm
    float tint = 0.0f;       // -1 green .. 1 magenta
    float shadows = 0.0f;    // -1 crush .. 1 lift
    float highlights = 0.25f; // 0 = off; rolls the top end off so whites aren't washed out
    float clarity = 0.35f;   // local contrast
    float sharpen = 0.3f;
    float vignette = 0.15f;
    std::string lut;         // .cube file name in the luts folder, empty = none
    float lutMix = 1.0f;

    Placement placement = Placement::BeforeHud;
    // When the interface draw can't be found in a frame, still grade at the end of it.
    bool fallbackAtEnd = true;
    View view = View::Normal;
    float split = 0.5f;
};

namespace settings {

Settings& current();
void load();
void save();

struct Preset {
    const char* name;
    const char* description;
};
const std::vector<Preset>& presets();
// Sets the grading values of a built-in preset (placement, view and LUT stay).
void applyPreset(Settings& s, const std::string& name);

} // namespace settings
} // namespace tmvs_mp
