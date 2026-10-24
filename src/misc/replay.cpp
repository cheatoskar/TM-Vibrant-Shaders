#include "misc/replay.h"
#include "image.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace tmshaders {
namespace misc {
namespace {

constexpr uint32_t kReplayClass = 0x03093000;     // CGameCtnReplayRecord
constexpr uint32_t kReplayClassOld = 0x2407E000;  // the same class under its old id (older replays, e.g. TMX records of Nadeo maps); its chunks use the new ids
constexpr uint32_t kHeaderReplay = 0x03093000;    // version, map ident (UID, environment, author), time, nickname
constexpr uint32_t kHeaderXml = 0x03093001;       // the replay's XML (<challenge uid=...>)
constexpr uint32_t kGhostRaceTime = 0x03092005;   // u32 race time (ms)
constexpr uint32_t kGhostCheckpoints = 0x0309200B; // u32 count, count x (u32 time, u32 stunt score)
constexpr uint32_t kGhostSamples = 0x0303F005;    // u32 size, u32 compressed size, zlib data
constexpr uint32_t kSkip = 0x534B4950;            // "PIKS": a skippable chunk (its size follows)
constexpr size_t kMaxBody = 64u << 20;            // a replay with an embedded map is a few MB at most

// A bounded little-endian reader.
struct Reader {
    const unsigned char* p;
    size_t size, at = 0;
    bool ok = true;
    Reader(const unsigned char* data, size_t n) : p(data), size(n) {}
    bool has(size_t n) const { return ok && at <= size && size - at >= n; }
    uint32_t u32() {
        if (!has(4)) {
            ok = false;
            return 0;
        }
        uint32_t v;
        memcpy(&v, p + at, 4);
        at += 4;
        return v;
    }
    uint8_t u8() {
        if (!has(1)) {
            ok = false;
            return 0;
        }
        return p[at++];
    }
    std::string string() {
        const uint32_t n = u32();
        if (!has(n) || n > (1u << 20)) {
            ok = false;
            return std::string();
        }
        std::string s(reinterpret_cast<const char*>(p + at), n);
        at += n;
        return s;
    }
    void skip(size_t n) {
        if (!has(n)) ok = false;
        else at += n;
    }
};

// Lookback strings (CMwId in a file): the first one of a chunk is preceded by the version (3);
// each is 0xFFFFFFFF (empty), a new string (bit 30 or 31 set, no index), an index into the strings
// read so far (1-based, with those bits), or a plain number (a predefined collection).
struct Lookback {
    bool versionRead = false;
    std::vector<std::string> seen;
    bool read(Reader& r, std::string& out) {
        if (!versionRead) {
            versionRead = true;
            const uint32_t version = r.u32();
            if (version < 2 || version > 3) return false;
        }
        const uint32_t v = r.u32();
        if (!r.ok) return false;
        out.clear();
        if (v == 0xFFFFFFFFu) return true;
        if ((v & 0xC0000000u) && !(v & 0x3FFFFFFFu)) {
            out = r.string();
            seen.push_back(out);
            return r.ok;
        }
        if (!(v & 0xC0000000u)) {
            char number[16];
            snprintf(number, sizeof(number), "#%u", v);
            out = number;
            return true;
        }
        const uint32_t index = v & 0x3FFFFFFFu;
        if (index == 0 || index > seen.size()) return false;
        out = seen[index - 1];
        return true;
    }
};

// <header ...><challenge uid="..."/>: the map's UID, should the binary header lack it.
std::string xmlAttribute(const std::string& xml, const char* element, const char* name) {
    const size_t e = xml.find(std::string("<") + element);
    if (e == std::string::npos) return std::string();
    const std::string key = std::string(" ") + name + "=\"";
    const size_t a = xml.find(key, e);
    const size_t close = xml.find('>', e);
    if (a == std::string::npos || (close != std::string::npos && a > close)) return std::string();
    const size_t start = a + key.size(), end = xml.find('"', start);
    return end == std::string::npos ? std::string() : xml.substr(start, end - start);
}

bool parseHeaderChunks(const unsigned char* data, size_t size, Replay& out, std::string& error) {
    Reader r(data, size);
    const uint32_t count = r.u32();
    if (!r.ok || count > 64) {
        error = "bad header chunk table";
        return false;
    }
    struct Entry {
        uint32_t id, size;
    };
    std::vector<Entry> entries;
    for (uint32_t i = 0; i < count; i++) {
        Entry e;
        e.id = r.u32();
        e.size = r.u32() & 0x7FFFFFFFu; // bit 31: "heavy" (skipped by quick readers)
        entries.push_back(e);
    }
    if (!r.ok) {
        error = "bad header chunk table";
        return false;
    }
    for (const Entry& e : entries) {
        if (!r.has(e.size)) {
            error = "header chunk past its end";
            return false;
        }
        Reader c(data + r.at, e.size);
        if (e.id == kHeaderReplay) {
            const uint32_t version = c.u32();
            if (version >= 2) {
                Lookback ids;
                std::string uid, collection, author;
                if (ids.read(c, uid) && ids.read(c, collection) && ids.read(c, author)) out.mapUid = uid;
                const uint32_t time = c.u32();
                if (c.ok && out.raceTime < 0 && time != 0xFFFFFFFFu) out.raceTime = static_cast<int>(time);
                std::string nickname = c.string();
                if (c.ok) out.driver = nickname;
            }
        } else if (e.id == kHeaderXml) {
            const std::string xml = c.string();
            if (c.ok && out.mapUid.empty()) out.mapUid = xmlAttribute(xml, "challenge", "uid");
        }
        r.at += e.size;
    }
    if (out.mapUid.empty()) {
        error = "no map UID in the header";
        return false;
    }
    return true;
}

uint32_t at32(const std::vector<unsigned char>& b, size_t i) {
    uint32_t v;
    memcpy(&v, b.data() + i, 4);
    return v;
}

// The first skippable chunk `id` in the body: its data (after "PIKS" and the size).
bool findSkippable(const std::vector<unsigned char>& body, uint32_t id, size_t& start, size_t& size) {
    for (size_t i = 0; i + 12 <= body.size(); i++) {
        if (at32(body, i) != id || at32(body, i + 4) != kSkip) continue;
        const uint32_t n = at32(body, i + 8);
        if (n > body.size() - (i + 12)) continue;
        start = i + 12;
        size = n;
        return true;
    }
    return false;
}

// The ghost's record (CGameGhost's samples, inflated): u32 class (0x0A02B000 the car), u32 skip
// list 2, u32, u32 period (ms), u32, then the samples' bytes (u32 size + data), u32 count, the first
// sample's offset and either one size for all or -1 and each size. Every sample starts with the
// car's position (three floats). Only the speed between neighbours is kept.
bool readSpeeds(const std::vector<unsigned char>& data, Replay& out) {
    Reader r(data.data(), data.size());
    const uint32_t classId = r.u32();
    if (!r.ok || classId == 0xFFFFFFFFu) return false;
    r.u32();
    r.u32();
    const uint32_t period = r.u32();
    r.u32();
    const uint32_t stateSize = r.u32();
    if (!r.has(stateSize) || period == 0 || period > 1000) return false;
    const size_t stateAt = r.at;
    r.skip(stateSize);
    const uint32_t count = r.u32();
    uint32_t first = 0;
    int32_t each = 0;
    std::vector<uint32_t> sizes;
    if (count > 0) first = r.u32();
    if (count > 1) {
        each = static_cast<int32_t>(r.u32());
        if (each == -1) {
            for (uint32_t i = 0; i + 1 < count && r.ok; i++) sizes.push_back(r.u32());
        }
    }
    if (!r.ok || count < 2 || count > 200000 || each == 0) return false;
    std::vector<float> position; // x, y, z of each sample, dropped at the end
    position.reserve(static_cast<size_t>(count) * 3);
    size_t offset = first;
    for (uint32_t i = 0; i < count && offset + 12 <= stateSize; i++) {
        float p[3];
        memcpy(p, data.data() + stateAt + offset, 12);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]) || fabsf(p[0]) > 1e5f || fabsf(p[1]) > 1e5f || fabsf(p[2]) > 1e5f) break;
        position.insert(position.end(), p, p + 3);
        offset += each == -1 ? (i < sizes.size() ? sizes[i] : 0) : static_cast<size_t>(each);
    }
    const size_t n = position.size() / 3;
    if (n < 2) return false;
    const float dt = static_cast<float>(period) / 1000.0f;
    out.samplePeriod = static_cast<int>(period);
    // Each step's speed; a respawn jumps the car (thousands of km/h for one step): such steps don't
    // count, a sample takes the steps on both sides that do (none: the speed before it).
    std::vector<float> step(n - 1);
    for (size_t i = 0; i + 1 < n; i++) {
        const float* pa = &position[i * 3];
        const float* pb = &position[(i + 1) * 3];
        step[i] = sqrtf((pb[0] - pa[0]) * (pb[0] - pa[0]) + (pb[1] - pa[1]) * (pb[1] - pa[1]) + (pb[2] - pa[2]) * (pb[2] - pa[2])) / dt;
    }
    constexpr float kTooFast = 280.0f; // m/s, 1000 km/h
    out.speeds.assign(n, 0.0f);
    for (size_t i = 0; i < n; i++) {
        float sum = 0.0f;
        int valid = 0;
        if (i > 0 && step[i - 1] < kTooFast) sum += step[i - 1], valid++;
        if (i + 1 < n && step[i] < kTooFast) sum += step[i], valid++;
        out.speeds[i] = valid ? sum / valid : (i > 0 ? out.speeds[i - 1] : 0.0f);
    }
    return true;
}

// CGameGhost 0x0303F005 (skippable or not): u32 size, u32 compressed size, zlib (0x78 ..).
void findSpeeds(const std::vector<unsigned char>& body, Replay& out) {
    for (size_t i = 0; i + 16 <= body.size(); i++) {
        if (at32(body, i) != kGhostSamples) continue;
        size_t at = i + 4;
        if (at32(body, at) == kSkip) at += 8;
        if (at + 10 > body.size()) continue;
        const uint32_t size = at32(body, at), packed = at32(body, at + 4);
        at += 8;
        if (size == 0 || size > (32u << 20) || packed < 3 || packed > body.size() - at || body[at] != 0x78) continue;
        std::vector<unsigned char> data;
        if (image::inflateRaw(body.data() + at + 2, packed - 2, size, data) && readSpeeds(data, out)) return;
    }
}

} // namespace

float speedAt(const Replay& replay, int ms) {
    if (replay.samplePeriod <= 0 || replay.speeds.empty() || ms < 0) return -1.0f;
    const float f = static_cast<float>(ms) / static_cast<float>(replay.samplePeriod);
    const size_t i = static_cast<size_t>(f);
    if (i + 1 >= replay.speeds.size()) return i < replay.speeds.size() + 2 ? replay.speeds.back() : -1.0f;
    const float t = f - static_cast<float>(i);
    return replay.speeds[i] * (1.0f - t) + replay.speeds[i + 1] * t;
}

bool parseReplay(const std::vector<unsigned char>& file, bool body, Replay& out, std::string& error) {
    out = Replay();
    Reader r(file.data(), file.size());
    if (file.size() < 9 || memcmp(file.data(), "GBX", 3) != 0) {
        error = "not a GBX file";
        return false;
    }
    r.at = 3;
    const unsigned version = r.u8() | (r.u8() << 8);
    if (version < 6) {
        error = "GBX version " + std::to_string(version) + " (only 6)";
        return false;
    }
    const uint8_t format = r.u8(), refs = r.u8(), compression = r.u8(), extra = r.u8();
    (void)refs;
    (void)extra;
    if (format != 'B') {
        error = "not a binary GBX";
        return false;
    }
    const uint32_t classId = r.u32();
    if (classId != kReplayClass && classId != kReplayClassOld) {
        char text[64];
        snprintf(text, sizeof(text), "not a replay (class %08X)", classId);
        error = text;
        return false;
    }
    const uint32_t userSize = r.u32();
    if (!r.has(userSize) || userSize == 0) {
        error = "no header";
        return false;
    }
    if (!parseHeaderChunks(file.data() + r.at, userSize, out, error)) return false;
    r.at += userSize;
    if (!body) return true;
    r.u32(); // nodes
    const uint32_t external = r.u32();
    if (!r.ok || external != 0) {
        error = external ? "the replay refers to other files" : "truncated";
        return false;
    }
    std::vector<unsigned char> data;
    if (compression == 'C') {
        const uint32_t size = r.u32(), packed = r.u32();
        if (!r.has(packed) || size == 0 || size > kMaxBody) {
            error = "body sizes not as expected";
            return false;
        }
        data.resize(size);
        if (!lzoDecompress(file.data() + r.at, packed, data.data(), size)) {
            error = "body doesn't decompress";
            return false;
        }
    } else {
        data.assign(file.begin() + r.at, file.end());
    }
    size_t at = 0, n = 0;
    if (findSkippable(data, kGhostRaceTime, at, n) && n >= 4) out.raceTime = static_cast<int>(at32(data, at));
    if (findSkippable(data, kGhostCheckpoints, at, n) && n >= 4) {
        const uint32_t count = at32(data, at);
        if (count <= 1024 && 4 + static_cast<size_t>(count) * 8 <= n) {
            for (uint32_t i = 0; i < count; i++) out.checkpoints.push_back(static_cast<int>(at32(data, at + 4 + i * 8)));
        }
    }
    if (out.checkpoints.empty()) {
        error = "no checkpoint times in the replay";
        return false;
    }
    findSpeeds(data, out); // optional: without it no speed comparison
    return true;
}

bool readFile(const std::wstring& path, size_t limit, std::vector<unsigned char>& data, std::string& error) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        error = "can't open the file";
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    if (size.QuadPart <= 0 || size.QuadPart > static_cast<LONGLONG>(kMaxBody)) {
        CloseHandle(f);
        error = "file size not as expected";
        return false;
    }
    const size_t n = static_cast<size_t>(size.QuadPart) < limit ? static_cast<size_t>(size.QuadPart) : limit;
    data.resize(n);
    DWORD read = 0;
    const bool ok = ReadFile(f, data.data(), static_cast<DWORD>(n), &read, nullptr) && read == n;
    CloseHandle(f);
    if (!ok) error = "can't read the file";
    return ok;
}

bool readReplayHeader(const std::wstring& path, Replay& out, std::string& error) {
    std::vector<unsigned char> data;
    // The header is a few hundred bytes (the XML, no thumbnail in replays).
    if (!readFile(path, 16 * 1024, data, error)) return false;
    return parseReplay(data, false, out, error);
}

bool readReplay(const std::wstring& path, Replay& out, std::string& error) {
    std::vector<unsigned char> data;
    if (!readFile(path, kMaxBody, data, error)) return false;
    return parseReplay(data, true, out, error);
}

// LZO1X, the decompressor alone (written from the format: literal runs and matches, each
// match's last two bits the count of literals after it).
bool lzoDecompress(const unsigned char* in, size_t inSize, unsigned char* out, size_t size) {
    const unsigned char* ip = in;
    const unsigned char* const end = in + inSize;
    unsigned char* op = out;
    unsigned char* const outEnd = out + size;
    size_t t = 0;
    const unsigned char* m = nullptr;
#define NEED_IN(n) \
    if (static_cast<size_t>(end - ip) < static_cast<size_t>(n)) return false
#define NEED_OUT(n) \
    if (static_cast<size_t>(outEnd - op) < static_cast<size_t>(n)) return false
#define LONG_COUNT(base)          \
    do {                          \
        while (true) {            \
            NEED_IN(1);           \
            if (*ip != 0) break;  \
            t += 255;             \
            ip++;                 \
            if (t > size) return false; \
        }                         \
        t += (base) + *ip++;      \
    } while (0)
#define COPY_LITERALS(n)               \
    do {                               \
        NEED_IN(n);                    \
        NEED_OUT(n);                   \
        memcpy(op, ip, n);             \
        op += (n);                     \
        ip += (n);                     \
    } while (0)
#define COPY_MATCH(n)                                    \
    do {                                                 \
        if (m < out || m >= op) return false;            \
        NEED_OUT(n);                                     \
        for (size_t k = 0; k < (n); k++) *op++ = *m++;   \
    } while (0)

    NEED_IN(1);
    enum { kLoop, kFirstLiteralRun, kMatch, kMatchNext } state = kLoop;
    if (*ip > 17) {
        t = *ip++ - 17u;
        if (t < 4) {
            state = kMatchNext;
        } else {
            COPY_LITERALS(t);
            state = kFirstLiteralRun;
        }
    }
    while (true) {
        switch (state) {
        case kLoop:
            NEED_IN(1);
            t = *ip++;
            if (t >= 16) {
                state = kMatch;
                break;
            }
            if (t == 0) LONG_COUNT(15);
            COPY_LITERALS(t + 3);
            state = kFirstLiteralRun;
            break;
        case kFirstLiteralRun:
            NEED_IN(1);
            t = *ip++;
            if (t >= 16) {
                state = kMatch;
                break;
            }
            NEED_IN(1);
            m = op - (1 + 0x0800) - (t >> 2) - (static_cast<size_t>(*ip++) << 2);
            COPY_MATCH(3);
            t = ip[-2] & 3;
            if (t == 0) state = kLoop;
            else state = kMatchNext;
            break;
        case kMatch:
            if (t >= 64) {
                NEED_IN(1);
                m = op - 1 - ((t >> 2) & 7) - (static_cast<size_t>(*ip++) << 3);
                t = (t >> 5) - 1;
                COPY_MATCH(t + 2);
            } else if (t >= 32) {
                t &= 31;
                if (t == 0) LONG_COUNT(31);
                NEED_IN(2);
                m = op - 1 - ((ip[0] >> 2) + (static_cast<size_t>(ip[1]) << 6));
                ip += 2;
                COPY_MATCH(t + 2);
            } else if (t >= 16) {
                const size_t high = (t & 8) << 11;
                t &= 7;
                if (t == 0) LONG_COUNT(7);
                NEED_IN(2);
                const size_t distance = high + (ip[0] >> 2) + (static_cast<size_t>(ip[1]) << 6);
                ip += 2;
                if (distance == 0) return op == outEnd; // the end marker
                m = op - distance - 0x4000;
                COPY_MATCH(t + 2);
            } else {
                NEED_IN(1);
                m = op - 1 - (t >> 2) - (static_cast<size_t>(*ip++) << 2);
                COPY_MATCH(2);
            }
            t = ip[-2] & 3;
            state = t == 0 ? kLoop : kMatchNext;
            break;
        case kMatchNext:
            COPY_LITERALS(t);
            NEED_IN(1);
            t = *ip++;
            state = kMatch;
            break;
        }
    }
#undef NEED_IN
#undef NEED_OUT
#undef LONG_COUNT
#undef COPY_LITERALS
#undef COPY_MATCH
}

} // namespace misc
} // namespace tmshaders
