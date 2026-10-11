#include "lut.h"
#include "log.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

namespace tmshaders {
namespace lut {
namespace {

uint8_t toByte(float v) {
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

std::string narrow(const std::wstring& text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &out[0], n, nullptr, nullptr);
    return out;
}

// The built-in looks, as grades on display (sRGB) values. Kept simple: a contrast curve,
// saturation, a split tone (shadows / highlights) and levels.
struct Rgb {
    float r, g, b;
};

float lumaOf(const Rgb& c) {
    return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

Rgb saturation(Rgb c, float s) {
    const float l = lumaOf(c);
    return {l + (c.r - l) * s, l + (c.g - l) * s, l + (c.b - l) * s};
}

float curve1(float x, float contrast) { // S-curve around 0.5, 1 = identity
    x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    return x < 0.5f ? 0.5f * powf(x / 0.5f, contrast) : 1.0f - 0.5f * powf((1.0f - x) / 0.5f, contrast);
}

Rgb curve(Rgb c, float contrast) {
    return {curve1(c.r, contrast), curve1(c.g, contrast), curve1(c.b, contrast)};
}

Rgb splitTone(Rgb c, Rgb shadow, Rgb highlight, float amount) {
    float w = (lumaOf(c) - 0.15f) / 0.7f;
    w = w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
    w = w * w * (3.0f - 2.0f * w);
    const Rgb t = {shadow.r + (highlight.r - shadow.r) * w, shadow.g + (highlight.g - shadow.g) * w, shadow.b + (highlight.b - shadow.b) * w};
    return {c.r * (1.0f + (t.r - 1.0f) * amount), c.g * (1.0f + (t.g - 1.0f) * amount), c.b * (1.0f + (t.b - 1.0f) * amount)};
}

Rgb levels(Rgb c, float black, float white) {
    return {black + c.r * (white - black), black + c.g * (white - black), black + c.b * (white - black)};
}

struct Look {
    const char* name;
    Rgb (*grade)(Rgb);
};

const Look kLooks[] = {
    {"Warm Film", [](Rgb c) { return levels(saturation(splitTone(curve(c, 1.15f), {0.97f, 1.0f, 1.05f}, {1.09f, 1.02f, 0.88f}, 0.9f), 1.05f), 0.015f, 0.99f); }},
    {"Teal & Orange", [](Rgb c) { return saturation(splitTone(curve(c, 1.1f), {0.86f, 1.02f, 1.12f}, {1.1f, 1.0f, 0.84f}, 0.75f), 1.08f); }},
    {"Bleach Bypass", [](Rgb c) { return splitTone(curve(saturation(c, 0.6f), 1.25f), {0.98f, 1.0f, 1.03f}, {1.0f, 1.0f, 1.0f}, 1.0f); }},
    {"Faded Vintage", [](Rgb c) { return levels(saturation(splitTone(curve(c, 0.92f), {1.0f, 1.02f, 0.98f}, {1.06f, 1.02f, 0.86f}, 0.9f), 0.82f), 0.07f, 0.93f); }},
    {"Cold Night", [](Rgb c) { return saturation(splitTone(curve(c, 1.08f), {0.86f, 0.95f, 1.16f}, {0.96f, 1.0f, 1.06f}, 0.9f), 0.9f); }},
    {"Vivid", [](Rgb c) { return curve(saturation(c, 1.28f), 1.06f); }},
    {"Noir", [](Rgb c) { return curve(saturation(c, 0.0f), 1.3f); }},
};

bool writeLook(const std::wstring& file, const Look& look) {
    const int n = 33;
    FILE* f = _wfopen(file.c_str(), L"wb");
    if (!f) return false;
    fprintf(f, "TITLE \"%s\"\nLUT_3D_SIZE %d\n", look.name, n);
    for (int b = 0; b < n; b++) {
        for (int g = 0; g < n; g++) {
            for (int r = 0; r < n; r++) {
                Rgb c = look.grade({r / float(n - 1), g / float(n - 1), b / float(n - 1)});
                const auto clamp = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
                fprintf(f, "%.5f %.5f %.5f\n", clamp(c.r), clamp(c.g), clamp(c.b));
            }
        }
    }
    return fclose(f) == 0;
}

} // namespace

void writeBuiltInLooks() {
    const std::wstring dir = folder();
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring marker = dir + L"\\.built-in-looks-1";
    if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    int written = 0;
    for (const Look& look : kLooks) {
        const std::wstring file = path(look.name) + L".cube";
        if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES && writeLook(file, look)) written++;
    }
    if (FILE* f = _wfopen(marker.c_str(), L"wb")) fclose(f);
    TMVS_LOG("lut: %d built-in looks written to the LUT folder", written);
}

bool loadCube(const std::wstring& path, Table& out, std::string& error) {
    out = Table();
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        error = "cannot open the file";
        return false;
    }
    int size = 0;
    float domainMin[3] = {0.0f, 0.0f, 0.0f}, domainMax[3] = {1.0f, 1.0f, 1.0f};
    std::vector<float> values;
    char line[512];
    bool ok = true;
    while (ok && fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0') continue;
        if (!strncmp(p, "LUT_3D_SIZE", 11)) {
            size = atoi(p + 11);
            if (size < 2 || size > 64) {
                error = "LUT_3D_SIZE must be 2..64";
                ok = false;
            } else {
                values.reserve(static_cast<size_t>(size) * size * size * 3);
            }
        } else if (!strncmp(p, "LUT_1D_SIZE", 11)) {
            error = "1D LUTs are not supported (only 3D)";
            ok = false;
        } else if (!strncmp(p, "DOMAIN_MIN", 10)) {
            sscanf(p + 10, "%f %f %f", &domainMin[0], &domainMin[1], &domainMin[2]);
        } else if (!strncmp(p, "DOMAIN_MAX", 10)) {
            sscanf(p + 10, "%f %f %f", &domainMax[0], &domainMax[1], &domainMax[2]);
        } else if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.') {
            float r = 0.0f, g = 0.0f, b = 0.0f;
            if (sscanf(p, "%f %f %f", &r, &g, &b) != 3) {
                error = "bad data line";
                ok = false;
            } else {
                values.push_back(r);
                values.push_back(g);
                values.push_back(b);
            }
        }
        // TITLE and unknown keywords are skipped.
    }
    fclose(f);
    if (!ok) return false;
    for (int k = 0; k < 3; k++) {
        if (domainMin[k] != 0.0f || domainMax[k] != 1.0f) {
            error = "only the input range 0..1 is supported (DOMAIN_MIN / DOMAIN_MAX)";
            return false;
        }
    }
    const size_t count = static_cast<size_t>(size) * size * size;
    if (size == 0 || values.size() != count * 3) {
        error = size == 0 ? "no LUT_3D_SIZE" : "wrong number of entries";
        return false;
    }
    // Red changes fastest in the file, as in the texture (x = red, y = green, z = blue).
    out.size = size;
    out.texels.resize(count);
    for (size_t i = 0; i < count; i++) {
        out.texels[i] = 0xff000000u | (static_cast<uint32_t>(toByte(values[i * 3])) << 16) | (static_cast<uint32_t>(toByte(values[i * 3 + 1])) << 8) |
                        toByte(values[i * 3 + 2]);
    }
    return true;
}

std::wstring folder() {
    return log::dataDir() + L"\\luts";
}

std::wstring path(const std::string& name) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    std::wstring wide(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, &wide[0], n);
    return folder() + L"\\" + wide;
}

std::vector<std::string> list() {
    std::vector<std::string> names;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((folder() + L"\\*.cube").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return names;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(narrow(data.cFileName));
    } while (FindNextFileW(find, &data));
    FindClose(find);
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    return names;
}

} // namespace lut
} // namespace tmshaders
