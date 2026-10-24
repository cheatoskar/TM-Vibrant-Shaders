#pragma once
#include "misc/misc.h"
#include "misc/replay.h"
#include <string>
#include <vector>

// Shared by the Misc page's parts (misc.cpp: settings, page, dispatch; mapview.cpp; splits.cpp;
// tmx.cpp; confetti.cpp).
namespace tmshaders {
namespace misc {

// [Misc] in settings.ini. Machine settings, not part of a look (no preset or share code has them).
struct Settings {
    bool minimap = false;
    float mapX = -1.0f, mapY = -1.0f; // window position (pixels; < 0: top right)
    float mapSize = 260.0f;           // window side (pixels at menu scale 1)
    float mapOpacity = 0.85f;
    float mapZoom = 1.0f;             // 1: the whole map; more: closer around the car
    bool mapRotate = false;           // the car's heading up instead of a fixed north
    bool splits = false;
    float splitX = -1.0f, splitY = -1.0f; // < 0: top centre
    int splitReference = 0;           // 0 your best run, 1 the loaded replay
    bool splitBar = true;             // the sector clock under the delta
    int finishEffect = 0;             // 0 off, 1 subtle, 2 big
};
Settings& settings();
void settingsChanged(); // saved a moment later with settings.ini

// Minimap (mapview.cpp).
namespace mapview {
void draw(const Frame& frame);
bool wanted();
void drawPageInfo(); // a line about the map read (cells, size)
} // namespace mapview

// Split times (splits.cpp).
namespace splits {
// Once a frame (also with the windows hidden): follows the race, notes checkpoints and the finish.
void update();
void draw(const Frame& frame);
bool wanted();
void drawPage(); // the reference (best run, replays, TMX) on the Misc page
// The run just finished (once): its time and whether it beat your best (for the finish effect).
bool takeFinish(int& time, bool& best);
} // namespace splits

// TMX (tmx.cpp): the map's world record replay, on a button press only, one request at a time.
namespace tmx {
enum class State { Idle, Busy, Done, Failed };
struct Status {
    State state = State::Idle;
    std::string message;   // for the page
    std::wstring file;     // the replay on disk (Done)
};
// The replay downloaded before for this map (Documents\TrackMania\TMVS\Replays\<uid>*.Replay.Gbx), "" none.
std::wstring cached(const std::string& uid);
void request(const std::string& uid); // starts the lookup and download (ignored while one runs)
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
