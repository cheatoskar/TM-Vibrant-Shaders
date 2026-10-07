#include "gbx.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace tmshaders {
namespace gbx {
namespace {

constexpr uint32_t kChallengeClass = 0x03043000;
constexpr uint32_t kThumbnailChunk = 0x03043007; // thumbnail and comments
constexpr size_t kHeaderLimit = 1 << 20;          // the header (thumbnail included) is far smaller

struct Reader {
    const std::vector<unsigned char>& d;
    size_t o;
    size_t end;
    bool u32(uint32_t& v) {
        if (o + 4 > end) return false;
        memcpy(&v, &d[o], 4);
        o += 4;
        return true;
    }
    bool skip(size_t n) {
        if (o + n > end) return false;
        o += n;
        return true;
    }
    bool expect(const char* text) {
        const size_t n = strlen(text);
        if (o + n > end || memcmp(&d[o], text, n) != 0) return false;
        o += n;
        return true;
    }
};

// Chunk 0x03043007: version, thumbnail size, "<Thumbnail.jpg>", the JPEG, "</Thumbnail.jpg>",
// "<Comments>", the comments (u32 length + UTF-8), "</Comments>".
bool commentsFromChunk(const std::vector<unsigned char>& d, size_t start, size_t size, std::string& comments) {
    Reader r{d, start, start + size};
    uint32_t version = 0, thumbnail = 0, length = 0;
    if (!r.u32(version) || version == 0) return false;
    if (!r.u32(thumbnail) || !r.expect("<Thumbnail.jpg>") || !r.skip(thumbnail) || !r.expect("</Thumbnail.jpg>") ||
        !r.expect("<Comments>") || !r.u32(length) || length > r.end - r.o) {
        return false;
    }
    comments.assign(reinterpret_cast<const char*>(&d[r.o]), length);
    return true;
}

} // namespace

bool readMapComments(const std::wstring& path, std::string& comments) {
    comments.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::vector<unsigned char> d(kHeaderLimit);
    DWORD read = 0;
    const BOOL ok = ReadFile(file, d.data(), static_cast<DWORD>(d.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok) return false;
    d.resize(read);

    // "GBX", version (6 in Forever), format (4 bytes from version 4), class, header size, chunks.
    if (d.size() < 21 || memcmp(d.data(), "GBX", 3) != 0) return false;
    uint16_t version = 0;
    memcpy(&version, &d[3], 2);
    if (version < 6) return false;
    Reader r{d, 9, d.size()};
    uint32_t classId = 0, headerSize = 0, count = 0;
    if (!r.u32(classId) || !r.u32(headerSize) || !r.u32(count)) return false;
    if (classId != kChallengeClass || count == 0 || count > 64) return false;
    std::vector<uint32_t> ids(count), sizes(count);
    for (uint32_t i = 0; i < count; i++) {
        if (!r.u32(ids[i]) || !r.u32(sizes[i])) return false;
        sizes[i] &= 0x7fffffff; // the top bit marks a "heavy" chunk
    }
    size_t at = r.o;
    for (uint32_t i = 0; i < count; i++) {
        if (at + sizes[i] > d.size()) return false;
        if (ids[i] == kThumbnailChunk) return commentsFromChunk(d, at, sizes[i], comments);
        at += sizes[i];
    }
    return false;
}

} // namespace gbx
} // namespace tmshaders
