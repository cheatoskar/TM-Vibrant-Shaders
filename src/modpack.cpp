#include "modpack.h"
#include "image.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace tmshaders {
namespace modpack {

namespace {

std::string toUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], n, nullptr, nullptr);
    return out;
}

bool endsWithNoCase(const std::wstring& text, const wchar_t* tail) {
    const size_t n = wcslen(tail);
    return text.size() >= n && _wcsicmp(text.c_str() + text.size() - n, tail) == 0;
}

uint32_t le32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t le16(const unsigned char* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

// --- Zip (stored and deflated entries; no zip64, no encryption: mods are plain zips) ----------

struct ZipEntry {
    std::string name;
    uint16_t method = 0;
    uint32_t packed = 0, size = 0, local = 0;
};

struct File {
    FILE* f = nullptr;
    explicit File(const std::wstring& path) : f(_wfopen(path.c_str(), L"rb")) {}
    ~File() {
        if (f) fclose(f);
    }
    bool read(uint64_t at, void* out, size_t n) const {
        return f && _fseeki64(f, static_cast<long long>(at), SEEK_SET) == 0 && fread(out, 1, n, f) == n;
    }
    uint64_t size() const {
        if (!f || _fseeki64(f, 0, SEEK_END) != 0) return 0;
        const long long n = _ftelli64(f);
        return n > 0 ? static_cast<uint64_t>(n) : 0;
    }
};

bool zipDirectory(const File& file, std::vector<ZipEntry>& entries) {
    const uint64_t size = file.size();
    if (size < 22) return false;
    // The end record is in the last 64 KB + 22 bytes (the archive comment comes after it).
    const size_t tail = static_cast<size_t>(std::min<uint64_t>(size, 65557));
    std::vector<unsigned char> end(tail);
    if (!file.read(size - tail, end.data(), tail)) return false;
    size_t at = tail - 22 + 1;
    bool found = false;
    while (at-- > 0) {
        if (le32(&end[at]) == 0x06054b50) {
            found = true;
            break;
        }
    }
    if (!found) return false;
    const uint32_t count = le16(&end[at + 10]), dirSize = le32(&end[at + 12]), dirAt = le32(&end[at + 16]);
    if (dirSize > 16u * 1024 * 1024 || static_cast<uint64_t>(dirAt) + dirSize > size) return false;
    std::vector<unsigned char> dir(dirSize);
    if (dirSize && !file.read(dirAt, dir.data(), dirSize)) return false;
    size_t p = 0;
    for (uint32_t i = 0; i < count && p + 46 <= dir.size(); i++) {
        if (le32(&dir[p]) != 0x02014b50) return false;
        ZipEntry e;
        e.method = le16(&dir[p + 10]);
        e.packed = le32(&dir[p + 20]);
        e.size = le32(&dir[p + 24]);
        const size_t nameLength = le16(&dir[p + 28]), extra = le16(&dir[p + 30]), comment = le16(&dir[p + 32]);
        e.local = le32(&dir[p + 42]);
        if (p + 46 + nameLength > dir.size()) return false;
        e.name.assign(reinterpret_cast<const char*>(&dir[p + 46]), nameLength);
        std::replace(e.name.begin(), e.name.end(), '\\', '/');
        entries.push_back(e);
        p += 46 + nameLength + extra + comment;
    }
    return true;
}

bool readEntry(const File& file, const ZipEntry& e, std::vector<unsigned char>& data) {
    if (e.size > 64u * 1024 * 1024 || e.packed > 64u * 1024 * 1024) return false; // a texture, not a disc image
    unsigned char local[30];
    if (!file.read(e.local, local, sizeof(local)) || le32(local) != 0x04034b50) return false;
    const uint64_t start = static_cast<uint64_t>(e.local) + 30 + le16(&local[26]) + le16(&local[28]);
    std::vector<unsigned char> packed(e.packed);
    if (e.packed && !file.read(start, packed.data(), e.packed)) return false;
    if (e.method == 0) {
        if (e.packed != e.size) return false;
        data.swap(packed);
        return true;
    }
    return e.method == 8 && image::inflateRaw(packed.data(), packed.size(), e.size, data);
}

// --- DDS -------------------------------------------------------------------------------------

uint32_t rgb565(uint16_t c) {
    const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((r * 527 + 23) >> 6) << 16 | ((g * 259 + 33) >> 6) << 8 | ((b * 527 + 23) >> 6);
}

// One 4x4 block's colours (the colour half of DXT1/3/5).
void colourBlock(const unsigned char* b, bool dxt1, uint32_t out[16]) {
    const uint16_t c0 = le16(b), c1 = le16(b + 2);
    uint32_t c[4];
    c[0] = rgb565(c0);
    c[1] = rgb565(c1);
    auto mix = [](uint32_t a, uint32_t b2, int wa, int wb, int d) {
        uint32_t r = 0;
        for (int s = 0; s < 24; s += 8) r |= ((((a >> s) & 255) * wa + ((b2 >> s) & 255) * wb) / d) << s;
        return r;
    };
    if (c0 > c1 || !dxt1) {
        c[2] = mix(c[0], c[1], 2, 1, 3);
        c[3] = mix(c[0], c[1], 1, 2, 3);
    } else {
        c[2] = mix(c[0], c[1], 1, 1, 2);
        c[3] = 0; // transparent black: shown black in a preview
    }
    const uint32_t bits = le32(b + 4);
    for (int i = 0; i < 16; i++) out[i] = c[(bits >> (i * 2)) & 3];
}

struct DdsFormat {
    enum Kind { Dxt1, Dxt3, Dxt5, Rgb32, Rgb24, Lum8 } kind = Dxt1;
    uint32_t masks[3] = {}; // Rgb32 / Rgb24: red, green, blue
};

size_t levelBytes(const DdsFormat& f, int w, int h) {
    const size_t bw = static_cast<size_t>((w + 3) / 4), bh = static_cast<size_t>((h + 3) / 4);
    switch (f.kind) {
        case DdsFormat::Dxt1: return bw * bh * 8;
        case DdsFormat::Dxt3:
        case DdsFormat::Dxt5: return bw * bh * 16;
        case DdsFormat::Rgb32: return static_cast<size_t>(w) * h * 4;
        case DdsFormat::Rgb24: return static_cast<size_t>(w) * h * 3;
        default: return static_cast<size_t>(w) * h;
    }
}

int maskShift(uint32_t mask) {
    int s = 0;
    while (mask && !(mask & 1)) {
        mask >>= 1;
        s++;
    }
    return s;
}

uint32_t maskedChannel(uint32_t v, uint32_t mask) {
    if (!mask) return 0;
    const int s = maskShift(mask);
    const uint32_t m = mask >> s, x = (v & mask) >> s;
    return m ? x * 255 / m : 0;
}

void decodeLevel(const DdsFormat& f, const unsigned char* d, int w, int h, std::vector<uint32_t>& out) {
    out.assign(static_cast<size_t>(w) * h, 0xFF000000u);
    if (f.kind == DdsFormat::Rgb32 || f.kind == DdsFormat::Rgb24 || f.kind == DdsFormat::Lum8) {
        const int bpp = f.kind == DdsFormat::Rgb32 ? 4 : (f.kind == DdsFormat::Rgb24 ? 3 : 1);
        for (size_t i = 0; i < out.size(); i++) {
            const unsigned char* p = d + i * bpp;
            if (bpp == 1) {
                out[i] = 0xFF000000u | p[0] * 0x010101u;
                continue;
            }
            const uint32_t v = bpp == 4 ? le32(p) : (p[0] | (p[1] << 8) | (p[2] << 16));
            out[i] = 0xFF000000u | maskedChannel(v, f.masks[0]) << 16 | maskedChannel(v, f.masks[1]) << 8 | maskedChannel(v, f.masks[2]);
        }
        return;
    }
    const size_t blockSize = f.kind == DdsFormat::Dxt1 ? 8 : 16;
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    uint32_t c[16];
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            const unsigned char* b = d + (static_cast<size_t>(by) * bw + bx) * blockSize;
            colourBlock(f.kind == DdsFormat::Dxt1 ? b : b + 8, f.kind == DdsFormat::Dxt1, c);
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    const int px = bx * 4 + x, py = by * 4 + y;
                    if (px < w && py < h) out[static_cast<size_t>(py) * w + px] = 0xFF000000u | c[y * 4 + x]; // opaque: a preview
                }
            }
        }
    }
}

// Area averaging down to at most maxWidth wide (no shimmering edges); smaller images stay.
void shrink(std::vector<uint32_t>& pixels, int& width, int& height, int maxWidth) {
    if (width <= maxWidth) return;
    const int w = maxWidth, h = std::max(1, static_cast<int>(static_cast<long long>(height) * maxWidth / width));
    std::vector<uint32_t> out(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; y++) {
        const int y0 = y * height / h, y1 = std::max(y0 + 1, (y + 1) * height / h);
        for (int x = 0; x < w; x++) {
            const int x0 = x * width / w, x1 = std::max(x0 + 1, (x + 1) * width / w);
            uint32_t sum[3] = {};
            for (int sy = y0; sy < y1; sy++) {
                for (int sx = x0; sx < x1; sx++) {
                    const uint32_t p = pixels[static_cast<size_t>(sy) * width + sx];
                    sum[0] += (p >> 16) & 255;
                    sum[1] += (p >> 8) & 255;
                    sum[2] += p & 255;
                }
            }
            const uint32_t n = static_cast<uint32_t>((y1 - y0) * (x1 - x0));
            out[static_cast<size_t>(y) * w + x] = 0xFF000000u | (sum[0] / n) << 16 | (sum[1] / n) << 8 | (sum[2] / n);
        }
    }
    pixels.swap(out);
    width = w;
    height = h;
}

bool isPreviewName(const std::string& name, const char* extension) {
    const size_t slash = name.find_last_of('/');
    const std::string file = slash == std::string::npos ? name : name.substr(slash + 1);
    return _stricmp(file.c_str(), (std::string("preview") + extension).c_str()) == 0;
}

} // namespace

std::wstring skinsFolder() {
    WCHAR docs[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) return L"";
    return std::wstring(docs) + L"\\TrackMania\\Skins";
}

std::vector<Pack> scan() {
    std::vector<Pack> packs;
    const std::wstring skins = skinsFolder();
    if (skins.empty()) return packs;
    WIN32_FIND_DATAW env{};
    HANDLE envs = FindFirstFileW((skins + L"\\*").c_str(), &env);
    if (envs == INVALID_HANDLE_VALUE) return packs;
    do {
        if (!(env.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || env.cFileName[0] == L'.') continue;
        const std::wstring folder = skins + L"\\" + env.cFileName + L"\\Mod";
        WIN32_FIND_DATAW zip{};
        HANDLE zips = FindFirstFileW((folder + L"\\*.zip").c_str(), &zip);
        if (zips == INVALID_HANDLE_VALUE) continue;
        do {
            if (zip.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring file = zip.cFileName;
            if (!endsWithNoCase(file, L".zip")) continue; // *.zip also matches *.zipx
            Pack p;
            p.environment = toUtf8(env.cFileName);
            p.file = file;
            p.name = toUtf8(file.substr(0, file.size() - 4));
            p.path = folder + L"\\" + file;
            p.size = (static_cast<uint64_t>(zip.nFileSizeHigh) << 32) | zip.nFileSizeLow;
            p.written = (static_cast<uint64_t>(zip.ftLastWriteTime.dwHighDateTime) << 32) | zip.ftLastWriteTime.dwLowDateTime;
            p.linked = GetFileAttributesW((p.path + L".loc").c_str()) != INVALID_FILE_ATTRIBUTES;
            packs.push_back(p);
        } while (FindNextFileW(zips, &zip));
        FindClose(zips);
    } while (FindNextFileW(envs, &env));
    FindClose(envs);
    std::sort(packs.begin(), packs.end(), [](const Pack& a, const Pack& b) {
        const bool sa = a.environment == "Stadium", sb = b.environment == "Stadium";
        if (sa != sb) return sa;
        const int e = _stricmp(a.environment.c_str(), b.environment.c_str());
        if (e != 0) return e < 0;
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return packs;
}

std::vector<std::string> environments(const std::vector<Pack>& packs) {
    std::vector<std::string> out = {"Stadium"};
    for (const Pack& p : packs) {
        if (std::find(out.begin(), out.end(), p.environment) == out.end()) out.push_back(p.environment);
    }
    return out;
}

bool listZip(const std::wstring& zipPath, std::vector<std::string>& names) {
    File file(zipPath);
    std::vector<ZipEntry> entries;
    if (!file.f || !zipDirectory(file, entries)) return false;
    for (const ZipEntry& e : entries) names.push_back(e.name);
    return true;
}

bool readZipEntry(const std::wstring& zipPath, const char* name, std::vector<unsigned char>& data) {
    File file(zipPath);
    std::vector<ZipEntry> entries;
    if (!file.f || !zipDirectory(file, entries)) return false;
    std::string wanted = name;
    std::replace(wanted.begin(), wanted.end(), '\\', '/');
    for (const ZipEntry& e : entries) {
        if (_stricmp(e.name.c_str(), wanted.c_str()) == 0) return readEntry(file, e, data);
    }
    return false;
}

bool decodeDds(const unsigned char* data, size_t size, int maxWidth, std::vector<uint32_t>& pixels, int& width, int& height,
               std::string& error) {
    if (size < 128 || memcmp(data, "DDS ", 4) != 0 || le32(data + 4) != 124) {
        error = "not a DDS file";
        return false;
    }
    const int h = static_cast<int>(le32(data + 12)), w = static_cast<int>(le32(data + 16));
    const uint32_t mips = std::max<uint32_t>(1, le32(data + 28)), flags = le32(data + 80), fourCC = le32(data + 84);
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        error = "bad DDS size";
        return false;
    }
    DdsFormat f;
    size_t offset = 128;
    if (flags & 0x4) { // DDPF_FOURCC
        if (fourCC == 0x31545844) f.kind = DdsFormat::Dxt1;      // "DXT1"
        else if (fourCC == 0x33545844 || fourCC == 0x32545844) f.kind = DdsFormat::Dxt3; // "DXT3", "DXT2"
        else if (fourCC == 0x35545844 || fourCC == 0x34545844) f.kind = DdsFormat::Dxt5; // "DXT5", "DXT4"
        else if (fourCC == 0x30315844 && size >= 148) {         // "DX10": DXGI format follows
            const uint32_t dxgi = le32(data + 128);
            offset = 148;
            if (dxgi == 71 || dxgi == 72) f.kind = DdsFormat::Dxt1;
            else if (dxgi == 74 || dxgi == 75) f.kind = DdsFormat::Dxt3;
            else if (dxgi == 77 || dxgi == 78) f.kind = DdsFormat::Dxt5;
            else if (dxgi == 28 || dxgi == 29) {
                f.kind = DdsFormat::Rgb32;
                f.masks[0] = 0x000000ff, f.masks[1] = 0x0000ff00, f.masks[2] = 0x00ff0000;
            } else if (dxgi == 87 || dxgi == 88 || dxgi == 91) {
                f.kind = DdsFormat::Rgb32;
                f.masks[0] = 0x00ff0000, f.masks[1] = 0x0000ff00, f.masks[2] = 0x000000ff;
            } else {
                error = "DDS format not supported (DXGI " + std::to_string(dxgi) + ")";
                return false;
            }
        } else {
            char cc[5] = {static_cast<char>(fourCC), static_cast<char>(fourCC >> 8), static_cast<char>(fourCC >> 16), static_cast<char>(fourCC >> 24), 0};
            error = std::string("DDS format not supported (") + cc + ")";
            return false;
        }
    } else {
        const uint32_t bits = le32(data + 88);
        f.masks[0] = le32(data + 92), f.masks[1] = le32(data + 96), f.masks[2] = le32(data + 100);
        if ((flags & 0x40) && bits == 32) f.kind = DdsFormat::Rgb32; // DDPF_RGB
        else if ((flags & 0x40) && bits == 24) f.kind = DdsFormat::Rgb24;
        else if ((flags & 0x20000) && bits == 8) f.kind = DdsFormat::Lum8; // DDPF_LUMINANCE
        else {
            error = "DDS format not supported (" + std::to_string(bits) + "-bit)";
            return false;
        }
    }
    // The largest mip level that is at most maxWidth wide; without mips the first, scaled down.
    int lw = w, lh = h;
    uint32_t level = 0;
    while (level + 1 < mips && lw > maxWidth) {
        offset += levelBytes(f, lw, lh);
        lw = std::max(1, lw / 2);
        lh = std::max(1, lh / 2);
        level++;
    }
    if (offset + levelBytes(f, lw, lh) > size) {
        error = "DDS file cut short";
        return false;
    }
    decodeLevel(f, data + offset, lw, lh, pixels);
    width = lw;
    height = lh;
    shrink(pixels, width, height, maxWidth);
    return true;
}

bool loadPreview(const std::wstring& zipPath, int maxWidth, std::vector<uint32_t>& pixels, int& width, int& height, std::string& error) {
    File file(zipPath);
    std::vector<ZipEntry> entries;
    if (!file.f) {
        error = "can't open the zip";
        return false;
    }
    if (!zipDirectory(file, entries)) {
        error = "not a zip this can read";
        return false;
    }
    // The load screen first (the mod's own picture), then a preview picture anywhere in it.
    const ZipEntry* pick = nullptr;
    for (const ZipEntry& e : entries) {
        if (_stricmp(e.name.c_str(), "Image/LoadScreen.dds") == 0) pick = &e;
    }
    for (const char* extension : {".png", ".jpg", ".jpeg", ".dds"}) {
        for (const ZipEntry& e : entries) {
            if (!pick && isPreviewName(e.name, extension)) pick = &e;
        }
    }
    if (!pick) {
        error = "no LoadScreen.dds or preview picture";
        return false;
    }
    std::vector<unsigned char> data;
    if (!readEntry(file, *pick, data)) {
        error = "can't read " + pick->name;
        return false;
    }
    const size_t dot = pick->name.find_last_of('.');
    const std::string extension = dot == std::string::npos ? "" : pick->name.substr(dot);
    if (_stricmp(extension.c_str(), ".dds") == 0) {
        if (!decodeDds(data.data(), data.size(), maxWidth, pixels, width, height, error)) {
            error = pick->name + ": " + error;
            return false;
        }
        return true;
    }
    if (!image::decodeJpeg(data.data(), data.size(), pixels, width, height)) {
        error = "can't decode " + pick->name;
        return false;
    }
    shrink(pixels, width, height, maxWidth);
    return true;
}

Mood moodOfDecoration(const char* decoration) {
    // The decorations of every environment: Sunrise, Day, Sunset, Night. Sunrise and sunset
    // fall on the same side of moodOfDayTime (a low, warm sun).
    if (!decoration || !decoration[0]) return Mood::Unknown;
    auto has = [decoration](const char* word) {
        const size_t n = strlen(word);
        for (const char* p = decoration; *p; p++) {
            if (_strnicmp(p, word, n) == 0) return true;
        }
        return false;
    };
    if (has("Night")) return Mood::Night;
    if (has("Sunset") || has("Sunrise")) return Mood::Dusk;
    if (has("Day")) return Mood::Day;
    return Mood::Unknown;
}

std::string choiceFor(const Config& config, const char* environment, const char* decoration, std::string& note) {
    note.clear();
    const Config::PackChoice* choice = config.findPackChoice(environment ? environment : "");
    if (!choice) return {};
    if (!choice->perMood) return choice->all;
    const Mood mood = moodOfDecoration(decoration);
    if (mood == Mood::Unknown) {
        note = std::string("map type not known (decoration \"") + (decoration ? decoration : "") + "\"): using the All moods choice";
        return choice->all;
    }
    static const char* const kMoods[] = {"day", "sunset", "night"};
    note = std::string("the ") + kMoods[static_cast<int>(mood)] + " choice";
    return choice->mood[static_cast<int>(mood)];
}

} // namespace modpack
} // namespace tmshaders
