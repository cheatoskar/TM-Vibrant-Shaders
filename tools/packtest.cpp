// Resource pack test (src/modpack.cpp): the DDS decoder, the zip reader, the mood of a decoration
// and the choice per environment and mood, on made-up data; then every pack in your
// Documents\TrackMania\Skins\<Environment>\Mod: its picture is read as the menu reads it.
//   tmvs_packtest [--png <folder>]   (--png: writes each picture there, to look at)
#include "config.h"
#include "image.h"
#include "modpack.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace tmshaders;

namespace {

int g_failed = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            std::printf("FAILED line %d: %s\n", __LINE__, #cond); \
            g_failed++;                                            \
        }                                                          \
    } while (0)

void put32(std::vector<unsigned char>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; i++) v[at + i] = static_cast<unsigned char>(x >> (i * 8));
}

std::vector<unsigned char> ddsHeader(int w, int h, int mips, uint32_t pfFlags, uint32_t fourCC, uint32_t bits) {
    std::vector<unsigned char> d(128, 0);
    memcpy(d.data(), "DDS ", 4);
    put32(d, 4, 124);
    put32(d, 12, static_cast<uint32_t>(h));
    put32(d, 16, static_cast<uint32_t>(w));
    put32(d, 28, static_cast<uint32_t>(mips));
    put32(d, 76, 32);
    put32(d, 80, pfFlags);
    put32(d, 84, fourCC);
    put32(d, 88, bits);
    put32(d, 92, 0x00ff0000);
    put32(d, 96, 0x0000ff00);
    put32(d, 100, 0x000000ff);
    return d;
}

// An 8x8 32-bit DDS with its mips, each level one colour (8x8 red, 4x4 green, 2x2 blue, 1x1 white).
std::vector<unsigned char> mippedDds() {
    std::vector<unsigned char> d = ddsHeader(8, 8, 4, 0x40, 0, 32);
    const uint32_t colours[] = {0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu};
    int size = 8;
    for (uint32_t c : colours) {
        for (int i = 0; i < size * size; i++) {
            const size_t at = d.size();
            d.resize(at + 4);
            put32(d, at, c);
        }
        size /= 2;
    }
    return d;
}

uint32_t crc32(const std::vector<unsigned char>& data) {
    uint32_t crc = 0xffffffffu;
    for (unsigned char b : data) {
        crc ^= b;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

// A zip with one stored file.
std::vector<unsigned char> storedZip(const char* name, const std::vector<unsigned char>& data) {
    std::vector<unsigned char> z;
    auto u16 = [&z](uint32_t x) {
        z.push_back(static_cast<unsigned char>(x));
        z.push_back(static_cast<unsigned char>(x >> 8));
    };
    auto u32 = [&](uint32_t x) {
        u16(x & 0xffff);
        u16(x >> 16);
    };
    const uint32_t crc = crc32(data), n = static_cast<uint32_t>(strlen(name)), size = static_cast<uint32_t>(data.size());
    u32(0x04034b50); u16(10); u16(0); u16(0); u16(0); u16(0); u32(crc); u32(size); u32(size); u16(n); u16(0);
    z.insert(z.end(), name, name + n);
    z.insert(z.end(), data.begin(), data.end());
    const uint32_t dir = static_cast<uint32_t>(z.size());
    u32(0x02014b50); u16(20); u16(10); u16(0); u16(0); u16(0); u16(0); u32(crc); u32(size); u32(size); u16(n); u16(0); u16(0); u16(0); u16(0);
    u32(0); u32(0);
    z.insert(z.end(), name, name + n);
    const uint32_t dirSize = static_cast<uint32_t>(z.size()) - dir;
    u32(0x06054b50); u16(0); u16(0); u16(1); u16(1); u32(dirSize); u32(dir); u16(0);
    return z;
}

void testDds() {
    // DXT1: colour 0 red, colour 1 blue, every texel index 0 except the last (index 1).
    std::vector<unsigned char> d = ddsHeader(4, 4, 1, 0x4, 0x31545844, 0);
    const unsigned char block[8] = {0x00, 0xf8, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x40};
    d.insert(d.end(), block, block + 8);
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    std::string error;
    CHECK(modpack::decodeDds(d.data(), d.size(), 256, px, w, h, error));
    CHECK(w == 4 && h == 4 && px.size() == 16);
    if (px.size() == 16) {
        CHECK(px[0] == 0xffff0000u);
        CHECK(px[15] == 0xff0000ffu);
    }
    // Mips: the largest level at most 4 wide is the 4x4 one (green).
    std::vector<unsigned char> m = mippedDds();
    CHECK(modpack::decodeDds(m.data(), m.size(), 4, px, w, h, error));
    CHECK(w == 4 && h == 4 && !px.empty() && px[0] == 0xff00ff00u);
    CHECK(modpack::decodeDds(m.data(), m.size(), 256, px, w, h, error));
    CHECK(w == 8 && h == 8 && !px.empty() && px[0] == 0xffff0000u);
    // No mips: scaled down to the width.
    std::vector<unsigned char> one = ddsHeader(8, 4, 1, 0x40, 0, 32);
    for (int i = 0; i < 32; i++) {
        const size_t at = one.size();
        one.resize(at + 4);
        put32(one, at, (i % 8) < 4 ? 0xff000000u : 0xffffffffu); // left half black, right white
    }
    CHECK(modpack::decodeDds(one.data(), one.size(), 2, px, w, h, error));
    CHECK(w == 2 && h == 1 && px.size() == 2 && px[0] == 0xff000000u && px[1] == 0xffffffffu);
    // Cut short and wrong formats fail with a reason.
    CHECK(!modpack::decodeDds(m.data(), 140, 256, px, w, h, error) && !error.empty());
    std::vector<unsigned char> bad = ddsHeader(4, 4, 1, 0x4, 0x31495441, 0); // "ATI1"
    bad.resize(bad.size() + 16);
    CHECK(!modpack::decodeDds(bad.data(), bad.size(), 256, px, w, h, error) && error.find("ATI1") != std::string::npos);
}

void testZip() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring path = std::wstring(temp) + L"tmvs_packtest.zip";
    const std::vector<unsigned char> dds = mippedDds();
    const std::vector<unsigned char> zip = storedZip("Image/LoadScreen.dds", dds);
    FILE* f = _wfopen(path.c_str(), L"wb");
    CHECK(f != nullptr);
    if (!f) return;
    fwrite(zip.data(), 1, zip.size(), f);
    fclose(f);
    std::vector<std::string> names;
    CHECK(modpack::listZip(path, names) && names.size() == 1 && names[0] == "Image/LoadScreen.dds");
    std::vector<unsigned char> back;
    CHECK(modpack::readZipEntry(path, "image\\loadscreen.DDS", back) && back == dds);
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    std::string error;
    CHECK(modpack::loadPreview(path, 4, px, w, h, error) && w == 4 && px[0] == 0xff00ff00u);
    DeleteFileW(path.c_str());
    CHECK(!modpack::loadPreview(path, 4, px, w, h, error) && !error.empty());
}

void testChoice() {
    CHECK(modpack::moodOfDecoration("Day") == Mood::Day);
    CHECK(modpack::moodOfDecoration("Sunrise") == Mood::Dusk);
    CHECK(modpack::moodOfDecoration("Sunset") == Mood::Dusk);
    CHECK(modpack::moodOfDecoration("Night") == Mood::Night);
    CHECK(modpack::moodOfDecoration("NoStadium48x48") == Mood::Unknown);
    CHECK(modpack::moodOfDecoration("") == Mood::Unknown);
    Config& config = Config::get();
    config.packChoices.clear();
    std::string note;
    CHECK(modpack::choiceFor(config, "Stadium", "Day", note).empty()); // nothing chosen: the map's own
    Config::PackChoice& c = config.packChoice("Stadium");
    c.all = "DarkBlue.zip";
    CHECK(modpack::choiceFor(config, "stadium", "Night", note) == "DarkBlue.zip" && note.empty());
    CHECK(modpack::choiceFor(config, "Speed", "Night", note).empty());
    c.perMood = true;
    c.mood[static_cast<int>(Mood::Night)] = "NEONglowSTICKS.zip";
    c.mood[static_cast<int>(Mood::Day)] = modpack::kNone;
    CHECK(modpack::choiceFor(config, "Stadium", "Night", note) == "NEONglowSTICKS.zip" && note == "the night choice");
    CHECK(modpack::choiceFor(config, "Stadium", "Day", note) == modpack::kNone);
    CHECK(modpack::choiceFor(config, "Stadium", "Sunset", note).empty()); // the sunset choice: the map's own
    CHECK(modpack::choiceFor(config, "Stadium", "Weird", note) == "DarkBlue.zip" && note.find("not known") != std::string::npos);
    config.packChoices.clear();
}

void yourPacks(const std::wstring& pngFolder) {
    const std::vector<modpack::Pack> packs = modpack::scan();
    std::printf("%u packs in %ls\n", static_cast<unsigned>(packs.size()), modpack::skinsFolder().c_str());
    for (const modpack::Pack& p : packs) {
        std::vector<uint32_t> px;
        int w = 0, h = 0;
        std::string error;
        const DWORD start = GetTickCount();
        const bool ok = modpack::loadPreview(p.path, 256, px, w, h, error);
        const DWORD ms = GetTickCount() - start;
        std::printf("  %-8s %-24s %6.1f MB%s  %s", p.environment.c_str(), p.name.c_str(), p.size / 1048576.0, p.linked ? " .loc" : "     ",
                    ok ? "" : error.c_str());
        if (ok) std::printf("picture %dx%d (%lu ms)", w, h, static_cast<unsigned long>(ms));
        std::printf("\n");
        if (ok && !pngFolder.empty()) {
            std::vector<unsigned char> rgb(static_cast<size_t>(w) * h * 3);
            for (size_t i = 0; i < px.size(); i++) {
                rgb[i * 3] = static_cast<unsigned char>(px[i] >> 16);
                rgb[i * 3 + 1] = static_cast<unsigned char>(px[i] >> 8);
                rgb[i * 3 + 2] = static_cast<unsigned char>(px[i]);
            }
            const std::wstring out = pngFolder + L"\\" + std::wstring(p.file.begin(), p.file.end() - 4) + L".png";
            image::writePng(out, rgb.data(), w, h);
        }
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring png;
    for (int i = 1; i + 1 < argc; i++) {
        if (!wcscmp(argv[i], L"--png")) png = argv[i + 1];
    }
    testDds();
    testZip();
    testChoice();
    std::printf("%s (%d failed)\n", g_failed ? "FAILED" : "all passed", g_failed);
    yourPacks(png);
    return g_failed ? 1 : 0;
}
