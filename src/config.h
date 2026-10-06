#pragma once
#include "settings.h"
#include <string>
#include <vector>

namespace tmshaders {

enum class Preset : int {
    Vibrant = 0,     // Sildur's Vibrant style: saturated, warm sun, strong shafts
    Cinematic,       // IterationT style: dense sunlit haze, deep shadows, film contrast
    GoldenHour,      // procedural clear sky, low warm sun, long shafts, warm film look
    Dreamy,          // soft pastel bloom, lifted shadows, pink/teal split toning
    Neon,            // starry night: day-for-night grading, glowing neon and floodlights
    Horizon,         // IterationT black hole over a dark, cold stadium
    Aurora,          // aurora borealis night, green-teal grade
    Competition,     // clarity first: AO + contact shadows, no haze or lens effects
    Performance,     // lighter passes for weak GPUs
    RainyDay,        // overcast, wet roads, puddles and falling rain
    ReplayCinema,    // film look with motion blur and depth of field for replays
    Storm,           // dark, overcast, pouring rain, soaked track, lightning
    Custom,
    Count
};

const char* presetName(Preset preset);
void applyPreset(Settings& settings, Preset preset);

// Time of day of the loaded map, read from the colour of the game's sun light.
enum class Mood : int { Unknown = -1, Day = 0, Dusk, Night, Count };
const char* moodName(Mood mood);
Mood classifyMood(const float sunColor[3]);

// Describes one tunable for the INI file and the overlay.
struct Field {
    const char* key;
    const char* label;
    const char* category;
    enum Kind { Float, Bool, Color, Int } kind;
    size_t offset;
    float min;
    float max;
};
const std::vector<Field>& fields();

// Name of the state after the user changed a value by hand.
constexpr const char* kCustomPreset = "Custom";

class Config {
public:
    static Config& get();

    void load();
    void save();
    // Every change is written to settings.ini a moment later (call tick() once per frame).
    void markDirty();
    void tick();

    Settings settings;
    std::string preset = "Vibrant"; // active preset name, kCustomPreset after manual changes
    bool showOverlay = false;
    bool advancedMenu = false; // F8 menu: simple (everyday) or advanced (every setting)
    int menuSections = 0;      // F8 menu: one bit per open section (all collapsed at first)

    // Presets: the built-in ones, then the user's own (Documents\TrackMania\TMVS\presets\*.ini).
    std::vector<std::string> presetNames() const;
    const std::vector<std::string>& userPresets() const { return m_userPresets; }
    bool isUserPreset(const std::string& name) const;
    bool selectPreset(const std::string& name); // from the menu: also becomes the current mood's preset
    bool saveUserPreset(const std::string& name);
    void deleteUserPreset(const std::string& name);

    // Automatic preset per map mood (day / sunrise + sunset / night). "" = keep current.
    bool autoMood = true;
    std::string moodPreset[static_cast<int>(Mood::Count)] = {"Vibrant", "Golden Hour", "Horizon"};
    Mood mood = Mood::Unknown;
    void onMoodDetected(Mood m);

private:
    Config() = default;
    bool applyNamed(const std::string& name);
    void scanUserPresets();
    std::wstring presetFile(const std::string& name) const;

    std::wstring m_path;
    std::wstring m_presetDir;
    std::vector<std::string> m_userPresets;
    bool m_dirty = false;
    unsigned long m_dirtySince = 0;
};

} // namespace tmshaders
