#include "clips/ring.h"
#include <algorithm>

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
    m_bytes = 0;
}

void PictureRing::trim() {
    // Whole groups go: the oldest kept picture is always a key frame (once one came).
    for (;;) {
        if (m_pictures.size() < 2) return;
        size_t next = 1;
        while (next < m_pictures.size() && !m_pictures[next].key) next++;
        if (next >= m_pictures.size()) return; // one group only
        const bool tooOld = m_pictures[next].time <= m_pictures.back().time - m_keep;
        const bool tooBig = m_bytes > m_maxBytes;
        if (!tooOld && !tooBig && m_pictures.front().key) return;
        for (size_t i = 0; i < next; i++) {
            m_bytes -= m_pictures.front().data ? m_pictures.front().data->size() : 0;
            m_pictures.pop_front();
        }
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

void SoundRing::reset(int rate, int channels, double seconds, int64_t start) {
    m_rate = rate;
    m_channels = channels;
    m_start = start;
    m_total = 0;
    m_capacity = static_cast<size_t>(seconds * rate);
    m_data.assign(m_capacity * channels, 0);
}

void SoundRing::push(const int16_t* samples, size_t frames) {
    if (!m_capacity) return;
    for (size_t i = 0; i < frames; i++) {
        const size_t at = static_cast<size_t>((m_total + i) % m_capacity) * m_channels;
        for (int c = 0; c < m_channels; c++) m_data[at + c] = samples[i * m_channels + c];
    }
    m_total += frames;
}

int64_t SoundRing::timeOf(uint64_t frame) const {
    return m_start + static_cast<int64_t>(frame * 10000000ull / static_cast<uint64_t>(m_rate));
}

std::vector<int16_t> SoundRing::cut(int64_t from, int64_t to, size_t& frames) const {
    frames = to > from ? static_cast<size_t>((to - from) * m_rate / 10000000) : 0;
    std::vector<int16_t> out(frames * m_channels, 0);
    if (!m_capacity || !frames) return out;
    const uint64_t kept = std::min<uint64_t>(m_total, m_capacity);
    const uint64_t firstKept = m_total - kept;
    // The stream's frame at `from` (may be before the stream: silence there).
    const int64_t first = from >= m_start ? static_cast<int64_t>((from - m_start) * m_rate / 10000000) : -static_cast<int64_t>((m_start - from) * m_rate / 10000000);
    for (size_t i = 0; i < frames; i++) {
        const int64_t n = first + static_cast<int64_t>(i);
        if (n < static_cast<int64_t>(firstKept) || n >= static_cast<int64_t>(m_total)) continue;
        const size_t at = static_cast<size_t>(static_cast<uint64_t>(n) % m_capacity) * m_channels;
        for (int c = 0; c < m_channels; c++) out[i * m_channels + c] = m_data[at + c];
    }
    return out;
}

} // namespace clips
} // namespace tmshaders
