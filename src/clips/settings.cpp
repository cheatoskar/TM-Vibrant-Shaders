#include "clips/settings.h"
#include "inifile.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace tmshaders {
namespace clips {
namespace {

std::wstring documentsTrackMania() {
    WCHAR docs[MAX_PATH] = {};
    std::wstring path;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) {
        path = std::wstring(docs) + L"\\TrackMania";
    } else {
        path = L"TrackMania";
    }
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}

std::wstring iniPath() {
    return dataDir() + L"\\clips.ini";
}

std::wstring bestsPath() {
    return dataDir() + L"\\bests.ini";
}

FILETIME g_written{}; // the file's time after our last load or save

FILETIME fileTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return FILETIME{};
    return data.ftLastWriteTime;
}

int readInt(const wchar_t* key, int def, int lo, int hi) {
    const int v = static_cast<int>(GetPrivateProfileIntW(L"Clips", key, def, iniPath().c_str()));
    return std::clamp(v, lo, hi);
}

float readFloat(const wchar_t* key, float def, float lo, float hi) {
    wchar_t text[32] = {};
    GetPrivateProfileStringW(L"Clips", key, L"", text, 32, iniPath().c_str());
    if (!text[0]) return def;
    return std::clamp(static_cast<float>(_wtof(text)), lo, hi);
}

std::string readText(const wchar_t* key, const char* def) {
    wchar_t text[64] = {};
    GetPrivateProfileStringW(L"Clips", key, L"", text, 64, iniPath().c_str());
    if (!text[0]) return def;
    char narrow[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow, 64, nullptr, nullptr);
    return narrow;
}

std::wstring wide(const std::string& s) {
    wchar_t text[256] = {};
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, text, 256);
    return text;
}

} // namespace

const std::wstring& dataDir() {
    static const std::wstring dir = [] {
        std::wstring path = documentsTrackMania() + L"\\TMClips";
        CreateDirectoryW(path.c_str(), nullptr);
        return path;
    }();
    return dir;
}

const std::wstring& clipsDir() {
    static const std::wstring dir = [] {
        std::wstring path = documentsTrackMania() + L"\\Clips";
        CreateDirectoryW(path.c_str(), nullptr);
        return path;
    }();
    return dir;
}

void load(Settings& s) {
    const Settings d;
    s.enabled = readInt(L"Enabled", d.enabled, 0, 1) != 0;
    s.onFinish = readInt(L"SaveOnFinish", d.onFinish, 0, 2);
    s.bufferSeconds = readInt(L"BufferSeconds", d.bufferSeconds, 10, 600);
    s.keySeconds = readInt(L"KeySeconds", d.keySeconds, 5, 600);
    s.before = readFloat(L"Before", d.before, 0.0f, 30.0f);
    s.after = readFloat(L"After", d.after, 0.0f, 30.0f);
    s.height = readInt(L"Height", d.height, 0, 2160);
    s.fps = readInt(L"Fps", d.fps, 24, 120);
    s.bitrate = readInt(L"Bitrate", d.bitrate, 2, 80);
    s.encoder = readInt(L"Encoder", d.encoder, 0, 2);
    s.sound = readInt(L"Sound", d.sound, 0, 1) != 0;
    s.noHud = readInt(L"WithoutHud", d.noHud, 0, 1) != 0;
    s.toast = readInt(L"SavedMessage", d.toast, 0, 1) != 0;
    s.saveKey = readText(L"SaveKey", d.saveKey.c_str());
    s.menuKey = readText(L"MenuKey", d.menuKey.c_str());
    g_written = fileTime(iniPath());
}

bool save(const Settings& s) {
    IniFile ini;
    if (!ini.load(iniPath())) return false;
    auto setInt = [&ini](const wchar_t* key, int v) {
        wchar_t text[32];
        swprintf(text, 32, L"%d", v);
        ini.set(L"Clips", key, text);
    };
    auto setFloat = [&ini](const wchar_t* key, float v) {
        wchar_t text[32];
        swprintf(text, 32, L"%.2f", v);
        ini.set(L"Clips", key, text);
    };
    setInt(L"Enabled", s.enabled);
    setInt(L"SaveOnFinish", s.onFinish);
    setInt(L"BufferSeconds", s.bufferSeconds);
    setInt(L"KeySeconds", s.keySeconds);
    setFloat(L"Before", s.before);
    setFloat(L"After", s.after);
    setInt(L"Height", s.height);
    setInt(L"Fps", s.fps);
    setInt(L"Bitrate", s.bitrate);
    setInt(L"Encoder", s.encoder);
    setInt(L"Sound", s.sound);
    setInt(L"WithoutHud", s.noHud);
    setInt(L"SavedMessage", s.toast);
    ini.set(L"Clips", L"SaveKey", wide(s.saveKey).c_str());
    ini.set(L"Clips", L"MenuKey", wide(s.menuKey).c_str());
    const bool ok = ini.save(iniPath());
    g_written = fileTime(iniPath());
    return ok;
}

bool changedOnDisk() {
    const FILETIME now = fileTime(iniPath());
    return CompareFileTime(&now, &g_written) != 0;
}

size_t memoryEstimate(const Settings& s) {
    const double video = s.bitrate * 1e6 / 8.0 * s.bufferSeconds;
    const double sound = s.sound ? 48000.0 * 2 * 2 * (s.bufferSeconds + 5) : 0.0;
    return static_cast<size_t>(video + sound);
}

int bestTime(const std::string& map) {
    if (map.empty()) return -1;
    return static_cast<int>(GetPrivateProfileIntW(L"Bests", wide(map).c_str(), -1, bestsPath().c_str()));
}

void setBestTime(const std::string& map, int ms) {
    if (map.empty()) return;
    wchar_t text[32];
    swprintf(text, 32, L"%d", ms);
    WritePrivateProfileStringW(L"Bests", wide(map).c_str(), text, bestsPath().c_str());
}

} // namespace clips
} // namespace tmshaders
