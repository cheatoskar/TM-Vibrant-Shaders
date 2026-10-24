#pragma once
#include "misc/misc.h"
#include "misc/replay.h"
#include "imgui.h"
#include <string>
#include <vector>

// Shared by the Misc page's parts (misc.cpp: settings, page, window placing, dispatch; mapview.cpp;
// splits.cpp; tmx.cpp; confetti.cpp).
namespace tmshaders {
namespace misc {

// What the split times compare with.
enum Compare : int { kCompareRecord = 0, kCompareGhost = 1, kCompareBest = 2, kCompareReplay = 3 };

// [Misc] in settings.ini. Machine settings, not part of a look (no preset or share code has them).
// Sizes are pixels on a 1440 pixel high screen (scaled with the screen's height).
struct Settings {
    bool minimap = false;
    float mapX = -1.0f, mapY = -1.0f; // window position (pixels; < 0: the default place, top right)
    float mapSize = 260.0f;
    float mapOpacity = 0.85f;
    bool mapAutoZoom = true;          // follows the car, closer the slower; off: the whole map
    float mapZoom = 1.0f;             // the auto zoom's scale: more is closer (0.5 - 2)
    bool mapRotate = false;           // the car's heading up instead of a fixed north
    bool mapTextured = true;          // "Real look": the blocks' meshes from above; off: squares
    bool splits = false;
    bool splitAttached = true;        // under the minimap (or top right); off: at splitX, splitY
    float splitX = -1.0f, splitY = -1.0f;
    float splitWidth = 260.0f;
    int splitCompare = kCompareBest;
    bool splitBar = true;             // the sector clock
    bool session = true;              // the Session panel (with the split times)
    bool sessionAttached = true;      // under the split times; off: at sessionX, sessionY
    float sessionX = -1.0f, sessionY = -1.0f;
    int finishEffect = 0;             // 0 off, 1 subtle, 2 big
};
Settings& settings();
void settingsChanged(); // saved a moment later with settings.ini

// Sizes: pixels on this screen for a size given at 1440 pixels of height.
float screenScale();
// The panels' text and spacing against the menu's (its font is made once for the screen at start and
// never below 1080p): 1 on most screens, less in a small window, so everything keeps its proportions.
float textScale();
// Pushes the spacing scaled by textScale() for one window (around Begin and End).
struct PanelStyle {
    PanelStyle();
    ~PanelStyle();
};

// The overlay windows' place and size: from the settings with the menu closed, when it opens and
// when the page's sliders change them; moved and resized by hand while the menu is open, which
// goes back into the settings when the mouse is let go. One per window.
struct WindowPlace {
    ImVec2 applied{-1.0f, -1.0f}, appliedSize{-1.0f, -1.0f};
    bool wasOpen = false;
    bool autoHeight = false;
    bool lockHeight = false;    // the height is the caller's alone (only the width can be dragged)
    float contentHeight = 0.0f; // the content's height last frame (autoHeight)
    // Before Begin. size.y <= 0: only the width is the window's, its height follows the content.
    void before(const Frame& frame, ImVec2 pos, ImVec2 size);
    // After Begin: true when the player moved or resized it; pos and size are then the new ones.
    bool after(const Frame& frame, ImVec2& pos, ImVec2& size);
    // Before End: notes the content's height (autoHeight).
    void end();
};
// The window flags for an overlay window (movable and resizable only with the menu open).
int windowFlags(const Frame& frame, bool resizable);
// With the menu open: a strip at the top with a move symbol and the window's name, and a grip in
// the lower right corner (resizable): the window can be dragged and resized. Returns the height the
// strip takes at the top of the content (0 with the menu closed).
float windowChrome(const Frame& frame, const char* title, bool resizable);

// The minimap's picture from above (topview.cpp).
namespace topview {
// Once a frame: the picture is wanted (minimap on, Real look on, a Stadium map). Off: let go.
void setWanted(bool wanted);
// The map's area (world metres, x and z) and its size in grid cells; a new area starts over.
void setRegion(const void* map, float x0, float z0, float x1, float z1, int cellsX, int cellsZ);
void clearRegion();
// misc::sceneDraw: one draw of the main camera.
void onDraw(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start, UINT count,
            const float view[16], const float projection[16]);
// Draws what is queued into the picture (at Present, outside ImGui's drawing), a share a frame.
void render(IDirect3DDevice9* device);
// The picture over the map's area (row 0 at z0), null while nothing is in it.
IDirect3DTexture9* picture();
int meshes();
void releaseDevice();
void precompile(); // its shaders, on a thread at startup
} // namespace topview

// Minimap (mapview.cpp).
namespace mapview {
void draw(const Frame& frame);
bool wanted();
void drawPageInfo(); // a line about the map read
// The minimap's window this frame (for the panels placed under it); false: not shown.
bool rect(ImVec2& pos, ImVec2& size);
} // namespace mapview

// Split times and the session (splits.cpp).
namespace splits {
// Once a frame (also with the windows hidden): follows the race, notes checkpoints and the finish.
void update();
void draw(const Frame& frame);
bool wanted();
void drawPage(); // what to compare with (record, ghost, best run, replays) on the Misc page
// The run just finished (once): its time and whether it beat your best (for the finish effect).
bool takeFinish(int& time, bool& best);
} // namespace splits

// TMX (tmx.cpp): the map's world record replay. One request at a time, at least 20 s apart, never
// twice for a map (the replay is kept; a map that isn't on TMX is noted too).
namespace tmx {
enum class State { Idle, Busy, Done, Failed };
struct Status {
    State state = State::Idle;
    std::string message; // for the page
    std::string uid;     // the map it is about
    std::wstring file;   // the replay on disk (Done)
};
// The replay downloaded before for this map (Documents\TrackMania\TMVS\Replays\<uid>_*.Replay.Gbx), "" none.
std::wstring cached(const std::string& uid);
// The map was looked up before and isn't on TMX (or has no record replay).
bool knownMissing(const std::string& uid);
// Starts the lookup and download now (the page's button). False: not now (one runs, or too soon).
bool request(const std::string& uid);
// For the automatic choice: asked once a frame; requests when allowed (once per map and session,
// never for a map cached or known missing, 20 s after the last request).
void want(const std::string& uid);
Status status();
} // namespace tmx

// Finish effects (confetti.cpp).
namespace confetti {
void start(bool big, bool personalBest);
void draw(const Frame& frame);
bool active();
} // namespace confetti

std::wstring dataFolder(const wchar_t* sub); // Documents\TrackMania\TMVS\<sub> (made if missing)
std::string utf8(const std::wstring& text);
std::wstring wide(const std::string& text);

} // namespace misc
} // namespace tmshaders
