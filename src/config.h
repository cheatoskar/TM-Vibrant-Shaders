#pragma once
#include "settings.h"
#include <string>
#include <vector>

namespace tmshaders {

enum class Preset : int {
    Vibrant = 0,     // Sildur's Vibrant style: saturated, warm sun, strong shafts
    Realistic,       // the game's own colours, only the light is new (was Cinematic until 1.1)
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
    Snowstorm,       // blizzard: driving snow, snow fields, white-out haze
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
// Name of the state while the look comes from the map's comments.
constexpr const char* kMapLookPreset = "Author's Shader";

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
    // F8 menu: the page (0 shaders, 1 customize, 2 studio, 3 maps, 4 performance).
    int menuPage = 0;
    int menuSections = 0;      // F8 menu: one bit per open section (all collapsed at first)
    bool hoverPreview = true;  // F8 menu: pointing at a preset shows it in the game
    bool menuFull = false;     // F8 menu over the whole screen (else the game stays in view on the right)
    bool checkUpdates = true;  // ask GitHub for a newer release when the game starts
    // Picking a preset also makes it the one for this kind of map (day / sunset / night).
    bool rememberMoodPick = true;
    // The preset the menu points at: shown in the game until the pointer leaves (not saved).
    std::string previewPreset;
    // The pointed-at preset's values into `s` (its machine settings stay); false = none.
    bool previewSettings(Settings& s) const { return !previewPreset.empty() && namedSettings(previewPreset, s); }
    // Any preset's values (for the menu's cost estimate); false = unknown name.
    bool presetSettings(const std::string& name, Settings& s) const { return namedSettings(name, s); }

    // Presets: the built-in ones, then the user's own (Documents\TrackMania\TMVS\presets\*.ini).
    std::vector<std::string> presetNames() const;
    const std::vector<std::string>& userPresets() const { return m_userPresets; }
    bool isUserPreset(const std::string& name) const;
    bool selectPreset(const std::string& name); // from the menu: also becomes the current mood's preset
    // Saves the current look under your name; `description` = a line about it for the menu
    // ("" keeps the one the preset had).
    bool saveUserPreset(const std::string& name, const std::string& description = std::string());
    void deleteUserPreset(const std::string& name);
    // Your preset's line about itself, stored in its file ([Info] Description=).
    std::string presetDescription(const std::string& name) const;
    void setPresetDescription(const std::string& name, const std::string& description);
    // Documents\TrackMania\TMVS\presets: presets dropped in there show up without a restart.
    const std::wstring& presetDir() const { return m_presetDir; }
    void rescanUserPresets() { scanUserPresets(); }
    // A picture of your preset: the next frame (a moment later, once the look has settled)
    // is saved as presets\<name>.jpg for the menu.
    std::wstring presetPicture(const std::string& name) const;
    void requestPicture(const std::string& name);
    bool takePictureRequest(std::string& name); // true once the picture is due
    int pictureVersion() const { return m_pictureVersion; } // changes with every new picture
    void pictureTaken() { m_pictureVersion++; }
    // The preset your look started from (also after your own changes) and its values: the
    // menu offers to reset each setting to it.
    const Settings& baseline() const { return m_baseline; }
    const std::string& basePreset() const { return m_basePreset; }

    // Automatic preset per map mood (day / sunrise + sunset / night). "" = keep current.
    bool autoMood = true;
    std::string moodPreset[static_cast<int>(Mood::Count)] = {"Vibrant", "Golden Hour", "Horizon"};
    Mood mood = Mood::Unknown;
    void onMoodDetected(Mood m);

    // Map looks: the map's author writes "[TMVS] Preset=Snowstorm Snow=1.5 ..." into the map's
    // comments (map editor). With the mod you get exactly that look on that map; on the next
    // map without one your own look is back. settings.ini always keeps your own look.
    bool useMapLooks = true;
    void onMapComments(const std::string& comments); // every map load ("" = no comments)
    void setUseMapLooks(bool use);
    bool mapLookActive() const { return m_mapLook; }
    std::string mapTag() const; // the current look, packed for a map's comments
    // The comments with the tag in place of an older one (the author's text stays).
    static std::string withMapTag(const std::string& comments, const std::string& tag);

private:
    Config() = default;
    bool applyNamed(const std::string& name);
    bool namedSettings(const std::string& name, Settings& out) const;
    Settings ownSettings() const;
    void leaveMapLook();
    void scanUserPresets();
    std::wstring presetFile(const std::string& name) const;

    std::wstring m_path;
    std::wstring m_presetDir;
    std::vector<std::string> m_userPresets;
    bool m_mapLook = false;
    Settings m_ownSettings;       // your look while a map look is shown
    std::string m_ownPreset;
    std::string m_mapComments;    // of the current map, to apply its look when switched on
    Settings m_baseline;
    std::string m_basePreset = "Vibrant";
    Settings m_ownBaseline;       // yours while a map look is shown
    std::string m_ownBasePreset;
    bool m_dirty = false;
    unsigned long m_dirtySince = 0;
    std::string m_pictureRequest;
    unsigned long m_pictureRequestTick = 0;
    int m_pictureVersion = 0;
};

} // namespace tmshaders
