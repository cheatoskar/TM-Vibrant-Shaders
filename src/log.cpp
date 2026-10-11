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

} // namespace

const std::wstring& dataDir() {
    static std::wstring dir = [] {
        WCHAR docs[MAX_PATH] = {};
        std::wstring path;
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) {
            path = std::wstring(docs) + L"\\TrackMania";
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

std::wstring g_fileName = L"tmvs.log";

void setFileName(const wchar_t* name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_fileName = name;
}

void write(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) {
        // The last run's log stays as *.prev.log: a rare problem is often noticed only after
        // the game was started again.
        const std::wstring path = dataDir() + L"\\" + g_fileName;
        if (path.size() > 4 && !_wcsicmp(path.c_str() + path.size() - 4, L".log")) {
            MoveFileExW(path.c_str(), (path.substr(0, path.size() - 4) + L".prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        g_file = _wfopen(path.c_str(), L"w");
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
