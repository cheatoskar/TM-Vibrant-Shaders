#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// The sound this process plays, captured with WASAPI loopback and handed out as 16-bit PCM at a
// rate and channel count of the caller's choice (the video export's sound pass: the game asks its
// OpenAL capture device for 44.1 kHz stereo and would otherwise record the default recording
// device, a microphone on many PCs).
//
// Sources, in this order: process loopback of this process and its children (Windows 10 2004 and
// newer: only the game's own sound, the mod's weather sound included), else the loopback of the
// default playback device (everything the PC plays). The data runs on its own thread; the reader
// gets what came in, and where the loopback delivered nothing for a while (a playback device that
// is silent sends no packets) silence is put in by the clock, so the sound keeps its length.
namespace tmshaders {
namespace loopback {

enum class Source { None, Process, System };
enum class Mode { Auto, ProcessOnly, SystemOnly, SystemMixFormat }; // the last: the playback device in its own format (tests the converter)
const char* sourceName(Source source);

struct Stats {
    uint64_t captured = 0;  // frames that came from the loopback (converted)
    uint64_t delivered = 0; // frames read
    uint64_t filled = 0;    // frames of silence put in by the clock (the loopback sent nothing)
    uint64_t underrun = 0;  // frames of silence a read asked for beyond what was there
    uint64_t dropped = 0;   // frames dropped: more came than the clock allows
    unsigned glitches = 0;  // discontinuities the audio engine reported
};

class Capture {
public:
    Capture();
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    // Starts preparing on the capture thread (activating the source can take a moment).
    void prepare(int rate, int channels, Mode mode = Mode::Auto);
    // Waits until prepared; true when a source is ready.
    bool waitReady(unsigned timeoutMs);
    // From here the clock runs and the sound collects.
    bool start();
    // Stops and takes in what the source still had (returns when done, at most 300 ms).
    void stop();
    size_t available();
    // Copies `frames` frames; silence where there were fewer (counted as underrun).
    void read(int16_t* out, size_t frames);

    Source source() const;
    std::string description() const; // the source and the format it delivers
    std::string error() const;
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace loopback
} // namespace tmshaders
