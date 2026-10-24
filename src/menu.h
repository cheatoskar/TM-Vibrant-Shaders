#pragma once
#include "imgui.h"
#include <windows.h>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace tmshaders {
struct Field;
struct Settings;

// The F8 menu, shared by the TMNF and the ManiaPlanet builds. The game side fills a Host.
namespace menu {

// GPU time per pass, for the Performance page.
struct PassTimings {
    virtual ~PassTimings() = default;
    virtual int count() const = 0;
    virtual const char* name(int pass) const = 0;
    virtual float time(int pass) const = 0; // ms
    virtual float total() const = 0;        // ms
};

// The Resource packs page's packs (TrackMania Nations/United: the environment mods in
// Skins\<Environment>\Mod). The choices themselves are in Config (packChoices).
struct ResourcePacks {
    struct Pack {
        std::string environment; // "Stadium"
        std::string name;        // shown
        std::string file;        // the choice saved for it (UTF-8)
        std::string detail;      // a line about it ("4.0 MB")
        bool usable = true;      // false: the game doesn't know it yet (added after it started)
    };
    virtual ~ResourcePacks() = default;
    virtual const std::vector<Pack>& packs() = 0;
    virtual std::vector<std::string> environments() = 0; // Stadium first
    virtual void refresh() = 0;                           // the folders read again
    // The pack's picture (0: none, or not loaded yet) and its size in pixels.
    virtual ImTextureID preview(size_t index, int& width, int& height) = 0;
    virtual void openFolder(const std::string& environment) = 0;
    virtual std::string status() = 0; // what the last map loaded with ("" before the first)
    virtual bool hooked() = 0;        // false: this game build can't
    // The loaded map: whether its pack can be switched now, and whether the choice differs.
    struct Live {
        bool possible = false, differs = false;
        std::string environment, shown, wanted;
    };
    virtual Live live() = 0;
    virtual std::string applyNow() = 0; // switches the loaded map's textures; a line saying how it went
};

struct Host {
    // The Resource packs page (null: no such page, TrackMania 2).
    ResourcePacks* resourcePacks = nullptr;
    bool depthAvailable = true;
    bool engineHooks = true;
    bool sunKnown = false;
    float sunDirection[3] = {};
    const char* shaderError = nullptr;
    const PassTimings* timings = nullptr;
    // The picture of a preset (0 = none).
    std::function<ImTextureID(const std::string&)> picture;
    // The mod's icon for the header, as a square texture of that many pixels (0 = none).
    std::function<ImTextureID(int)> icon;
    // What auto quality turned off right now (null: nothing, or no auto quality in this game).
    std::function<const char*()> autoQualityReduced;
    // The comments of the map open in the editor (Author's Shader).
    std::function<bool(std::string&)> mapComments;
    std::function<bool(const std::string&)> setMapComments;
    // Photo mode: the f-number of an aperture setting.
    std::function<float(float)> fNumber;
    // Photo mode's compare split (null: none): on/off, line position, game's image on the right,
    // and turning photo mode on when the split is switched on.
    bool* splitOn = nullptr;
    float* splitPosition = nullptr;
    bool* splitSwapped = nullptr;
    // What the other side shows: "" the game's image, else a preset (null: only the game's image).
    std::string* splitPreset = nullptr;
    // The compare split switch's second line: what photos and the game's side get (null: TMNF's).
    const char* splitNote = nullptr;
    std::function<void()> photoOn;
    // TrackMania Nations/United: photo mode can pause a finished race (the *Pause the game* switch).
    bool photoFreeze = false;
    // The settings this game can do (null: all); the others are not shown.
    std::function<bool(const Field&)> supports;
    // Whether shaders can be reloaded (TMNF only). The hotkeys themselves are in keys.h.
    bool reloadShaders = true;
    // The debug views (value of Settings::debugView, name); empty: TMNF's list.
    std::vector<std::pair<int, const char*>> debugViews;
    // This game's own tooltips (key, text), ahead of the shared ones: settings that work
    // differently there (ManiaPlanet's real night, its moon, replays it can't tell from driving).
    std::vector<std::pair<const char*, const char*>> tips;
    // This game's own widget for Game moonlight (EngineMoon), with the menu's label; returns
    // whether it changed (null: TMNF's three choices). TrackMania 2 can't move its moon.
    std::function<bool(Settings&, const char*)> engineMoon;
    // The game's own options, a panel first on the Settings page (null: none).
    const char* gamePanelTitle = nullptr;
    std::function<void()> gamePanel;
};

// Colours, sizes and fonts (after ImGui::CreateContext, before the renderer backend).
void setupStyle(HWND window);
float scale();
void draw(const Host& host);
// A setting's explanation for its tooltip (null: none), for the host's own panel too.
const char* tooltip(const char* key);
// Set when the user asks for a shader reload from the menu; cleared by the call.
bool consumeReloadRequest();
// Photo mode's compare split, at its line near the bottom of the screen: `mine` (your look's
// name) on its side, the other side (`other`: "" = the game's image, else a preset) as a list to
// switch it without the menu. `note`: a line above the list (empty: none); `bottom`: the bar's
// distance from the bottom of the screen (pixels at scale 1). Between NewFrame and Render.
void drawSplitBar(const std::string& mine, std::string& other, float position, bool mineLeft, const std::string& note,
                  float bottom = 24.0f);

} // namespace menu
} // namespace tmshaders
