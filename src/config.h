#pragma once
#include "settings.h"
#include <string>
#include <utility>
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
// The mood of a map's time of day (0..1, the engine's decoration mood).
Mood moodOfDayTime(float dayTime);

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

// Share codes: a look as one line of text ("TMVS1:..."), for Discord and the like. All of the
// look's settings (not the machine's) that differ from the Settings defaults, so a code reads the
// same in TrackMania Nations/United and TrackMania 2.
struct ShareStats {
    int applied = 0; // settings taken from the code
    int unknown = 0; // keys this version doesn't have (or machine settings): ignored
    int invalid = 0; // known keys with a value that can't be read: ignored
    int clamped = 0; // values outside the setting's range, moved into it
};
std::string shareCodeFor(const Settings& settings);
// What a TMVS1 code does not mention: the Settings defaults as they were when the format came out
// (Sharpen was 0.35 then, 0.8 since 1.4), so old codes keep their look and old versions read new ones.
Settings shareCodeBase();
// `look` comes in with your machine settings and leaves with the code's look on success; on
// failure it is untouched and `error` says why in a sentence for the menu.
bool decodeShareCode(const std::string& text, Settings& look, ShareStats& stats, std::string& error);
std::string shareStatusLine(const ShareStats& stats); // "Loaded 37 settings, 2 unknown ignored"
// The Windows clipboard as Unicode text. Reading refuses text of more than 1 MB.
bool copyTextToClipboard(const std::string& ascii);
bool textFromClipboard(std::string& text, std::string& error);

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
    // Photo mode (not in presets): on by itself with the free camera (cam 7), tiles per side
    // of a hi-res photo (2..4, ~0.84x the screen each), depth of field on the clicked point.
    bool photoAuto = true;
    bool photoHideHud = true;
    bool photoHideCar = false; // photo mode leaves the cars out (TMNF)
    int photoTiles = 2;
    float photoDof = 0.5f;
    bool splitPhotoLabels = true; // the compare split's names burnt into photos (TMNF)
    // Colour look-up table on top of every preset: a .cube file in TMVS\luts ("" = none)
    // and how much of it (0..1).
    std::string lutFile;
    float lutAmount = 1.0f;
    // Resource packs (TrackMania Nations/United: the environment mods in
    // Skins\<Environment>\Mod), one choice per environment, used at every map load: "" the
    // map's own mod (the game's behaviour), "None", or a zip's file name. perMood: one choice
    // per map mood (day, sunset, night) instead of `all`. Machine settings ([ResourcePacks]).
    struct PackChoice {
        std::string all;
        std::string mood[static_cast<int>(Mood::Count)];
        bool perMood = false;
    };
    std::vector<std::pair<std::string, PackChoice>> packChoices; // by environment ("Stadium")
    bool packReplaceMapMod = true; // a chosen pack also replaces a map's own mod
    PackChoice& packChoice(const std::string& environment);
    const PackChoice* findPackChoice(const std::string& environment) const;
    // Picking a preset also makes it the one for this kind of map (day / sunset / night).
    bool rememberMoodPick = false;
    // The preset the menu points at: shown in the game until the pointer leaves (not saved).
    std::string previewPreset;
    // The pointed-at preset's values into `s` (its machine settings stay); false = none.
    bool previewSettings(Settings& s) const { return !previewPreset.empty() && namedSettings(previewPreset, s); }
    // Any preset's values (for the menu's cost estimate); false = unknown name.
    bool presetSettings(const std::string& name, Settings& s) const { return namedSettings(name, s); }
    // When your preset's file was last written (zero for a built-in one or no file): a cached
    // copy of its values is stale when this changes.
    unsigned long long presetWritten(const std::string& name) const;

    // The current look as a share code, and a code applied as your unsaved look ("Custom" on top of
    // the built-in preset it is closest to; an exact copy of a built-in preset selects it). Nothing
    // saved is touched. false = `message` is the reason, else the status line.
    std::string shareCode() const;
    bool applyShareCode(const std::string& text, std::string& message);

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
    // You picked a preset in the menu (with "Picked preset applies to all maps of this type"
    // off): it holds on every map, also on ones of another type, until you choose a preset for
    // a map type again (Maps page) or resumeMoodPresets(). Before, the next map of another type
    // switched to its preset: Horizon picked, the replay's video export loaded Golden Hour.
    bool manualPick = false;
    void resumeMoodPresets();
    void onMoodDetected(Mood m);
    // The look a map of this mood gets once loaded (its mood preset, or the current look).
    Settings lookForMood(Mood m) const;

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
