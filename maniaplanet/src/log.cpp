// The ManiaPlanet build's log and data folder: <Documents>\ManiaPlanet\TMVS\. Same interface as the
// TMNF plugin's log (src/log.h), so shared code such as the LUT reader works unchanged.
#include "log.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace tmshaders {
namespace log {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
std::wstring g_fileName = L"tmvs-mp.log";

} // namespace

const std::wstring& dataDir() {
    static std::wstring dir = [] {
        WCHAR docs[MAX_PATH] = {};
        // Tests (tools/harness.cpp) point the mod at their own folder.
        if (GetEnvironmentVariableW(L"TMVS_MP_DATA", docs, MAX_PATH)) {
            CreateDirectoryW(docs, nullptr);
            return std::wstring(docs);
        }
        std::wstring path;
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) {
            path = std::wstring(docs) + L"\\ManiaPlanet";
            CreateDirectoryW(path.c_str(), nullptr);
            path += L"\\TMVS";
        } else {
            path = L"TMVS";
        }
        CreateDirectoryW(path.c_str(), nullptr);
        return path;
    }();
    return dir;
}

void setFileName(const wchar_t* name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_fileName = name;
}

void write(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) {
        g_file = _wfopen((dataDir() + L"\\" + g_fileName).c_str(), L"w");
        if (!g_file) return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_file, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_file, fmt, args);
    va_end(args);
    fputc('\n', g_file);
    fflush(g_file);
}

} // namespace log
} // namespace tmshaders
