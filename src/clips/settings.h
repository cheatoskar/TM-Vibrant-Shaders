#pragma once
#include <cstddef>
#include <string>

// The clips' settings, in one file both builds use: Documents\TrackMania\TMClips\clips.ini. TM Vibrant
// Shaders (its Video page) and the standalone TM Clips read and write the same file, so a choice made
// in one carries over to the other.
namespace tmshaders {
namespace clips {

struct Settings {
    bool enabled = true;      // record into the buffer
    int onFinish = 1;         // save on finish: 0 off, 1 personal best only, 2 every finish
    int bufferSeconds = 60;   // what the buffer keeps
    int keySeconds = 30;      // the save key saves this much (the last seconds)
    float before = 3.0f;      // a run's clip starts this long before its start
    float after = 3.0f;       // and ends this long after the finish
    int height = 1080;        // at most this many rows (720, 1080, 1440), 0 = the game's own size
    int fps = 60;
    int bitrate = 12;         // Mbit/s on average
    int encoder = 0;          // 0 auto (NVIDIA, then Media Foundation), 1 NVIDIA only, 2 Media Foundation only
    bool sound = true;
    bool noHud = false;       // TM Vibrant Shaders: the picture before the HUD
    bool toast = true;        // "Clip saved" in the game
    std::string saveKey = "F3";
    std::string menuKey = "Shift+F3"; // TM Clips' own window (the standalone)

    bool sameRecording(const Settings& o) const {
        return enabled == o.enabled && bufferSeconds == o.bufferSeconds && height == o.height && fps == o.fps && bitrate == o.bitrate &&
               encoder == o.encoder && sound == o.sound;
    }
};

// Documents\TrackMania\TMClips (settings, bests, the standalone's log) and Documents\TrackMania\Clips.
const std::wstring& dataDir();
const std::wstring& clipsDir();

void load(Settings& s);
bool save(const Settings& s);
// The file changed since load() or save() (the other build wrote it).
bool changedOnDisk();

// Memory the buffer takes at these settings (video at the bitrate plus the sound), bytes.
size_t memoryEstimate(const Settings& s);

// Personal bests per map (bests.ini): the best time (ms) saved for `map`, -1 none.
int bestTime(const std::string& map);
void setBestTime(const std::string& map, int ms);

} // namespace clips
} // namespace tmshaders
