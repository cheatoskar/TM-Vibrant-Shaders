#include "clips/ring.h"
#include <algorithm>
#include <cstring>

namespace tmshaders {
namespace clips {

void PictureRing::setLimits(double seconds, size_t maxBytes) {
    m_keep = static_cast<int64_t>(seconds * 1e7);
    m_maxBytes = maxBytes;
    trim();
}

void PictureRing::push(Picture p) {
    m_bytes += p.data ? p.data->size() : 0;
    m_pictures.push_back(std::move(p));
    trim();
}

void PictureRing::clear() {
    m_pictures.clear();
    m_pictures.shrink_to_fit();
    m_bytes = 0;
}

void PictureRing::trim() {
    // Whole groups go: the oldest kept picture is always a key frame (once one came).
    auto dropFront = [this](size_t n) {
        for (size_t i = 0; i < n && !m_pictures.empty(); i++) {
            m_bytes -= m_pictures.front().data ? m_pictures.front().data->size() : 0;
            m_pictures.pop_front();
        }
    };
    for (;;) {
        if (m_pictures.empty()) return;
        const bool tooBig = m_bytes > m_maxBytes;
        size_t next = 1;
        while (next < m_pictures.size() && !m_pictures[next].key) next++;
        if (next >= m_pictures.size()) {
            // One group, or no key frame at all (an encoder that marks none): only the byte limit
            // takes from it, picture by picture, so the buffer never grows past it.
            if (!tooBig) return;
            dropFront(1);
            continue;
        }
        const bool tooOld = m_pictures[next].time <= m_pictures.back().time - m_keep;
        if (!tooOld && !tooBig && m_pictures.front().key) return;
        dropFront(next);
    }
}

std::vector<Picture> PictureRing::cut(int64_t from, int64_t to) const {
    std::vector<Picture> out;
    if (m_pictures.empty() || to < from) return out;
    // The last key frame at or before `from`, else the first key frame after it.
    size_t first = m_pictures.size();
    for (size_t i = 0; i < m_pictures.size(); i++) {
        if (!m_pictures[i].key) continue;
        if (m_pictures[i].time <= from || first == m_pictures.size()) first = i;
        if (m_pictures[i].time > from) break;
    }
    if (first == m_pictures.size()) return out;
    for (size_t i = first; i < m_pictures.size() && m_pictures[i].time <= to; i++) out.push_back(m_pictures[i]);
    return out;
}

std::vector<int64_t> PictureRing::durations(const std::vector<Picture>& pictures, int64_t lastDuration) {
    std::vector<int64_t> out(pictures.size());
    for (size_t i = 0; i < pictures.size(); i++) {
        out[i] = i + 1 < pictures.size() ? std::max<int64_t>(pictures[i + 1].time - pictures[i].time, 1) : lastDuration;
    }
    return out;
}

size_t SoundRing::bytesFor(int rate, int channels, double seconds) {
    const double want = std::max(0.0, seconds * rate);
    const size_t chunks = static_cast<size_t>((static_cast<uint64_t>(want + 0.5) + kChunkFrames - 1) / kChunkFrames);
    return chunks * kChunkFrames * static_cast<size_t>(channels) * sizeof(int16_t);
}

void SoundRing::reset(int rate, int channels, double seconds, int64_t start) {
    clear();
    m_rate = rate;
    m_channels = channels;
    m_start = start;
    const double want = std::max(0.0, seconds * rate);
    const size_t count = static_cast<size_t>((static_cast<uint64_t>(want + 0.5) + kChunkFrames - 1) / kChunkFrames);
    std::vector<std::shared_ptr<std::vector<int16_t>>> chunks;
    chunks.reserve(count);
    for (size_t i = 0; i < count; i++) chunks.push_back(std::make_shared<std::vector<int16_t>>(kChunkFrames * static_cast<size_t>(channels), int16_t(0)));
    m_chunks.swap(chunks);
    m_capacity = static_cast<uint64_t>(count) * kChunkFrames;
}

void SoundRing::clear() {
    m_chunks.clear();
    m_chunks.shrink_to_fit();
    m_capacity = 0;
    m_total = 0;
}

void SoundRing::push(const int16_t* samples, size_t frames) {
    if (!m_capacity) return;
    const size_t ch = static_cast<size_t>(m_channels);
    while (frames) {
        // The circle is whole chunks: a chunk's end is where it wraps too.
        const uint64_t slot = m_total % m_capacity;
        const size_t chunk = static_cast<size_t>(slot / kChunkFrames), at = static_cast<size_t>(slot % kChunkFrames);
        const size_t n = std::min(frames, kChunkFrames - at);
        memcpy(m_chunks[chunk]->data() + at * ch, samples, n * ch * sizeof(int16_t));
        samples += n * ch;
        frames -= n;
        m_total += n;
    }
}

int64_t SoundRing::timeOf(uint64_t frame) const {
    return m_start + static_cast<int64_t>(frame * 10000000ull / static_cast<uint64_t>(m_rate));
}

SoundRing::Span SoundRing::span(int64_t from, int64_t to, uint64_t marginFrames) const {
    Span s;
    s.channels = m_channels;
    s.frames = to > from ? static_cast<size_t>((to - from) * m_rate / 10000000) : 0;
    if (!m_capacity || !s.frames) return s;
    // The stream's frame at `from` (may be before the stream: silence there).
    s.first = from >= m_start ? static_cast<int64_t>((from - m_start) * m_rate / 10000000) : -static_cast<int64_t>((m_start - from) * m_rate / 10000000);
    const uint64_t kept = std::min<uint64_t>(m_total, m_capacity);
    s.validFrom = m_total - kept;
    // The capture writes over the oldest frames next: those within the margin count as gone.
    if (m_total + marginFrames > m_capacity) s.validFrom = std::max<uint64_t>(s.validFrom, m_total + marginFrames - m_capacity);
    s.validTo = m_total;
    s.capacity = m_capacity;
    s.chunks = m_chunks;
    return s;
}

std::vector<int16_t> SoundRing::copy(const Span& s, int* copies) {
    std::vector<int16_t> out(s.frames * static_cast<size_t>(s.channels), 0);
    if (copies) *copies = 0;
    if (!s.capacity || !s.frames || s.validTo <= s.validFrom) return out;
    const size_t ch = static_cast<size_t>(s.channels);
    const int64_t lo = std::max<int64_t>(s.first, static_cast<int64_t>(s.validFrom));
    const int64_t hi = std::min<int64_t>(s.first + static_cast<int64_t>(s.frames), static_cast<int64_t>(s.validTo));
    for (int64_t n = lo; n < hi;) {
        const uint64_t slot = static_cast<uint64_t>(n) % s.capacity;
        const size_t chunk = static_cast<size_t>(slot / kChunkFrames), at = static_cast<size_t>(slot % kChunkFrames);
        const size_t len = static_cast<size_t>(std::min<int64_t>(hi - n, static_cast<int64_t>(kChunkFrames - at)));
        memcpy(out.data() + static_cast<size_t>(n - s.first) * ch, s.chunks[chunk]->data() + at * ch, len * ch * sizeof(int16_t));
        n += static_cast<int64_t>(len);
        if (copies) (*copies)++;
    }
    return out;
}

std::vector<int16_t> SoundRing::cut(int64_t from, int64_t to, size_t& frames) const {
    const Span s = span(from, to);
    frames = s.frames;
    return copy(s);
}

} // namespace clips
} // namespace tmshaders
