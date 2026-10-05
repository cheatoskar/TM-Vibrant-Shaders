#include "framecap.h"
#include <cstdio>
#include <cstring>

namespace tmshaders {
namespace {

constexpr char kMagicV1[4] = {'T', 'M', 'C', '1'};
constexpr char kMagic[4] = {'T', 'M', 'C', '2'}; // v2 adds the sun colour

struct Header {
    char magic[4];
    uint32_t width;
    uint32_t height;
    float view[16];
    float projection[16];
    float sunDirection[4];
    float time;
    float sunColor[4]; // v2 only
};

} // namespace

bool FrameCapture::save(const std::wstring& path) const {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    Header h{};
    memcpy(h.magic, kMagic, 4);
    h.width = width;
    h.height = height;
    memcpy(h.view, view, sizeof(view));
    memcpy(h.projection, projection, sizeof(projection));
    memcpy(h.sunDirection, sunDirection, sizeof(sunDirection));
    h.time = time;
    memcpy(h.sunColor, sunColor, sizeof(sunColor));
    fwrite(&h, sizeof(h), 1, f);
    fwrite(color.data(), sizeof(uint32_t), color.size(), f);
    fwrite(depth.data(), sizeof(float), depth.size(), f);
    fclose(f);
    return true;
}

bool FrameCapture::load(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    Header h{};
    bool ok = fread(&h, sizeof(Header) - sizeof(h.sunColor), 1, f) == 1;
    const bool v2 = ok && memcmp(h.magic, kMagic, 4) == 0;
    ok = ok && (v2 || memcmp(h.magic, kMagicV1, 4) == 0);
    if (ok && v2) ok = fread(h.sunColor, sizeof(h.sunColor), 1, f) == 1;
    if (ok) {
        memcpy(sunColor, h.sunColor, sizeof(sunColor));
        width = h.width;
        height = h.height;
        memcpy(view, h.view, sizeof(view));
        memcpy(projection, h.projection, sizeof(projection));
        memcpy(sunDirection, h.sunDirection, sizeof(sunDirection));
        time = h.time;
        const size_t count = static_cast<size_t>(width) * height;
        color.resize(count);
        depth.resize(count);
        ok = fread(color.data(), sizeof(uint32_t), count, f) == count && fread(depth.data(), sizeof(float), count, f) == count;
    }
    fclose(f);
    return ok;
}

} // namespace tmshaders
