#pragma once
#include "clips/ring.h"
#include <cstdint>
#include <memory>
#include <string>

// The clips' recorder: frames from the game's thread go to a worker that encodes them into the
// buffer (clips/ring.h), next to the game's sound (WASAPI loopback, src/loopback.cpp); a save cuts a
// time span out of the buffer and writes the MP4 on a thread of its own (venc::writeMp4). The game's
// thread only copies a frame into a pooled buffer; when the worker falls behind, frames are dropped
// (counted), the game never waits. The encoder is opened on the worker too (NVENC takes a moment):
// until it is ready, frames are dropped.
namespace tmshaders {
namespace clips {

// The performance counter in 100 ns units (the clips' clock), and a counter value in them.
int64_t now100ns();
int64_t counterTo100ns(int64_t counter);

struct RecorderSetup {
    int width = 0, height = 0;
    double fps = 60.0;
    double bitrate = 12e6;
    int encoder = 0;          // EncoderSetup::choice
    bool softwareOnly = false;
    bool sound = true;
    double seconds = 60.0;    // the buffer
};

struct SaveJob {
    std::wstring path;
    int64_t from = 0, to = 0; // the span (100 ns)
    std::string what;         // for the message ("0:42.37", "last 30 s")
};

struct SaveResult {
    bool ok = false;
    std::wstring path;
    std::string what;
    std::string line;         // for the log
    double seconds = 0.0;     // the clip's length
};

struct RecorderStats {
    int framesIn = 0, framesDropped = 0, framesEncoded = 0;
    double encodeMs = 0.0;    // in the encoder, summed
    double encodeMaxMs = 0.0;
    size_t bufferBytes = 0;
    double bufferSeconds = 0.0;
};

class Recorder {
public:
    enum class State { Off, Starting, Running, Failed };

    Recorder();
    ~Recorder(); // stops (waits for the worker; a save that runs finishes first)

    void start(const RecorderSetup& setup);
    void stop();
    State state() const;
    std::string failure() const;      // State::Failed: why
    std::string description() const;  // the encoder and the sound's source

    // A buffer for the next frame (NV12, width x height x 1.5), null when not running or when the
    // worker has every buffer (the frame is dropped). submit() hands it over with the time the frame
    // was shown.
    uint8_t* acquire();
    void submit(int64_t time);

    // Saving: the span is taken out of the buffer once the buffer reached its end (or 2 s later).
    void save(const SaveJob& job);
    bool takeResult(SaveResult& out);
    bool saving() const;

    RecorderStats stats() const;
    int64_t newest() const;

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

} // namespace clips
} // namespace tmshaders
