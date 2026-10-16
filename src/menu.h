#pragma once
#include "imgui.h"
#include <windows.h>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace tmshaders {
struct Field;

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

struct Host {
    bool depthAvailable = true;
    bool engineHooks = true;
    bool sunKnown = false;
    float sunDirection[3] = {};
    const char* shaderError = nullptr;
    const PassTimings* timings = nullptr;
    // The picture of a preset (0 = none).
    std::function<ImTextureID(const std::string&)> picture;
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
    std::function<void()> photoOn;
    // The settings this game can do (null: all); the others are not shown.
    std::function<bool(const Field&)> supports;
    // Whether shaders can be reloaded (TMNF only). The hotkeys themselves are in keys.h.
    bool reloadShaders = true;
    // The debug views (value of Settings::debugView, name); empty: TMNF's list.
    std::vector<std::pair<int, const char*>> debugViews;
    // The game's own options, a panel first on the Settings page (null: none).
    const char* gamePanelTitle = nullptr;
    std::function<void()> gamePanel;
};

// Colours, sizes and fonts (after ImGui::CreateContext, before the renderer backend).
void setupStyle(HWND window);
float scale();
void draw(const Host& host);
// Set when the user asks for a shader reload from the menu; cleared by the call.
bool consumeReloadRequest();

} // namespace menu
} // namespace tmshaders
