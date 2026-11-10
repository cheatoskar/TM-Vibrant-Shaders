#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

// The clips' buffer: encoded pictures and the sound, the last N seconds in memory. Pure data, no
// Windows calls (tools/clipstest.cpp tests the cuts). Times are 100 ns units of one clock (the
// performance counter, see clips::now100ns).
namespace tmshaders {
namespace clips {

struct Picture {
    int64_t time = 0;  // when the frame was shown
    bool key = false;  // a key frame (IDR): a clip can start here
    std::shared_ptr<const std::vector<uint8_t>> data;
};

class PictureRing {
public:
    // Keeps at least `seconds` (whole groups of pictures from a key frame on) and at most `maxBytes`.
    // The byte limit always holds: without a second key frame the oldest pictures go one by one.
    void setLimits(double seconds, size_t maxBytes);
    void push(Picture p);
    void clear();
    // The pictures from the last key frame at or before `from` (or the first one kept, if the buffer
    // starts later) up to the last one shown at or before `to`. Empty: nothing in that time.
    std::vector<Picture> cut(int64_t from, int64_t to) const;
    // A picture's length: up to the next one's time (the last one: `lastDuration`).
    static std::vector<int64_t> durations(const std::vector<Picture>& pictures, int64_t lastDuration);

    bool empty() const { return m_pictures.empty(); }
    size_t count() const { return m_pictures.size(); }
    size_t bytes() const { return m_bytes; }
    size_t maxBytes() const { return m_maxBytes; }
    int64_t oldest() const { return m_pictures.empty() ? 0 : m_pictures.front().time; }
    int64_t newest() const { return m_pictures.empty() ? 0 : m_pictures.back().time; }

private:
    void trim();
    std::deque<Picture> m_pictures;
    size_t m_bytes = 0;
    int64_t m_keep = 600000000; // 60 s
    size_t m_maxBytes = 512u << 20;
};

// 16-bit PCM in a circle of a fixed length; frame n of the stream was captured at
// start + n / rate seconds. The circle is made of chunks (no single block of tens of MB in the
// game's 32-bit address space), each held by a shared pointer: a save takes the chunks it needs
// under the recorder's lock (span(), quick) and copies the samples after it (copy()).
class SoundRing {
public:
    static constexpr size_t kChunkFrames = 32768; // 128 KB of stereo

    // The length is rounded up to whole chunks. Throws std::bad_alloc when the memory isn't there.
    void reset(int rate, int channels, double seconds, int64_t start);
    void clear(); // the memory goes (once no save holds a chunk)
    void push(const int16_t* samples, size_t frames);

    struct Span {
        int channels = 2;
        size_t frames = 0;        // asked for
        int64_t first = 0;        // the stream's frame at the span's start (may be before the stream)
        uint64_t validFrom = 0;   // the frames kept: [validFrom, validTo)
        uint64_t validTo = 0;
        uint64_t capacity = 0;    // frames
        std::vector<std::shared_ptr<std::vector<int16_t>>> chunks;
    };
    // The span from `from` to `to`. Frames that will be written over within `marginFrames` count as
    // gone (a save copies after the lock is let go, the capture writes on meanwhile).
    Span span(int64_t from, int64_t to, uint64_t marginFrames = 0) const;
    // The span's samples (silence where nothing was kept): one memcpy per chunk it touches.
    // `copies`: how many memcpy were made (tests).
    static std::vector<int16_t> copy(const Span& s, int* copies = nullptr);
    // span() and copy() at once.
    std::vector<int16_t> cut(int64_t from, int64_t to, size_t& frames) const;

    int rate() const { return m_rate; }
    int channels() const { return m_channels; }
    uint64_t total() const { return m_total; }
    uint64_t capacity() const { return m_capacity; }
    size_t bytes() const { return m_chunks.size() * kChunkFrames * static_cast<size_t>(m_channels) * sizeof(int16_t); }
    int64_t timeOf(uint64_t frame) const;
    // The memory reset() takes for `seconds`.
    static size_t bytesFor(int rate, int channels, double seconds);

private:
    int m_rate = 48000, m_channels = 2;
    int64_t m_start = 0;
    uint64_t m_total = 0; // frames pushed since reset
    std::vector<std::shared_ptr<std::vector<int16_t>>> m_chunks;
    uint64_t m_capacity = 0; // frames, whole chunks
};

} // namespace clips
} // namespace tmshaders
