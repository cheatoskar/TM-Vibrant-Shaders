#include "lut.h"
#include "log.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

} // namespace

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
