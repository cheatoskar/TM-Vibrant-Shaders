#pragma once
#include <cstddef>
#include <string>

// The clips' settings, in one file both builds use: Documents\TrackMania\TMClips\clips.ini. TM Vibrant
// Shaders (its Video page) and the standalone TM Clips read and write the same file, so a choice made
// in one carries over to the other.
namespace tmshaders {
namespace clips {

struct Settings {
    // Instant replay: record into the buffer. Off for new players (1.6): no frame is copied or read
    // back until it is switched on; a clips.ini that has Enabled keeps it (load()).
    bool enabled = false;
    int onFinish = 1;         // save on finish: 0 off, 1 personal best only, 2 every finish
    int bufferSeconds = 60;   // what the buffer keeps (Adaptive buffer off)
    bool adaptiveBuffer = true; // the buffer from the map's author time (adaptiveBufferSeconds)
    int keySeconds = 30;      // the save key saves this much (the last seconds): 30, 60 or 90
    float before = 3.0f;      // a run's clip starts this long before its start
    float after = 3.0f;       // and ends this long after the finish
    int height = 1080;        // at most this many rows (720, 1080, 1440), 0 = the game's own size
    int fps = 60;
    int bitrate = 12;         // Mbit/s on average
    int encoder = 0;          // 0 auto (NVIDIA, then Media Foundation), 1 NVIDIA only, 2 Media Foundation only
    bool sound = true;
    int soundSource = 0;      // 0 the game, 1 the game and the microphone, 2 everything the PC plays
    bool noHud = false;       // TM Vibrant Shaders: the picture before the HUD
    bool toast = true;        // "Clip saved" in the game
    bool checkpointClips = false; // a clip of each sector when a checkpoint is passed (TM Vibrant Shaders: the race's checkpoint times)
    std::string saveKey = "F3";
    std::string lastRunKey = "Ctrl+F3";       // the last finished run, also after a restart
    std::string runSoFarKey = "Ctrl+Shift+F3"; // the run that runs now, from its start
    std::string menuKey = "Shift+F3"; // TM Clips' own window (the standalone)

    bool sameRecording(const Settings& o) const {
        return enabled == o.enabled && bufferSeconds == o.bufferSeconds && adaptiveBuffer == o.adaptiveBuffer && height == o.height &&
               fps == o.fps && bitrate == o.bitrate && encoder == o.encoder && sound == o.sound && soundSource == o.soundSource;
    }
};

// Tests (TMVS 2.0's fake-host runs): this folder instead of Documents\TrackMania. Before dataDir()/clipsDir().
void setDocumentsRoot(const std::wstring& root);
// Documents\TrackMania\TMClips (settings, bests, the standalone's log) and Documents\TrackMania\Clips.
const std::wstring& dataDir();
const std::wstring& clipsDir();

// True: the buffer was shortened to fit the memory limit (clampBuffer).
bool load(Settings& s);
bool save(const Settings& s);
// The file changed since load() or save() (the other build wrote it).
bool changedOnDisk();

// The game is 32-bit (2 GB of address space, most of it the game's): the buffer's pictures and its
// sound take at most this much together.
constexpr size_t kMemoryLimit = 350u << 20;
constexpr int kSoundRate = 48000;
constexpr int kSoundChannels = 2;
// The most the pictures may take (a quarter more than the bitrate asks, plus 16 MB), and the sound's
// circle (the buffer plus 10 s), bytes.
size_t videoLimit(double bitsPerSecond, double seconds);
size_t soundBytes(double seconds);
// The longest buffer that fits kMemoryLimit at this bitrate (Mbit/s).
int maxBufferSeconds(int bitrate, bool sound);
// Shortens the buffer (and the save key's seconds) to what fits. True: shortened.
bool clampBuffer(Settings& s);

// Memory the buffer takes at these settings (video at the bitrate plus the sound), bytes; `seconds`:
// another length than the setting's (the adaptive buffer's).
size_t memoryEstimate(const Settings& s);
size_t memoryEstimate(const Settings& s, int seconds);

// The adaptive buffer for a map: max(60 s, the save key's seconds, author time x 1.5 + 10 s), at
// most `most` (the memory limit's). authorMs <= 0 (not known): 60 s (or the key's seconds).
int adaptiveBufferSeconds(int authorMs, int keySeconds, int most);

// UTF-8 and UTF-16, any length.
std::string utf8(const std::wstring& text);
std::wstring utf16(const std::string& text);

// Personal bests per map (bests.ini): the best time (ms) saved for `map`, -1 none.
int bestTime(const std::string& map);
void setBestTime(const std::string& map, int ms);

} // namespace clips
} // namespace tmshaders
