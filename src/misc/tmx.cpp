#include "misc/internal.h"
#include "log.h"
#include "tm_shaders_version.h"
#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

// The map's world record replay from Trackmania Exchange, for the split times (only its checkpoint
// times are read). Only when the button is pressed, one request at a time, at least 20 s apart, and
// never twice for a map: the replay is kept in Documents\TrackMania\TMVS\Replays\<UID>_<replay>.Replay.Gbx
// and found there next time. The routes (checked by hand on 2026-10-24):
//   GET https://tmnf.exchange/api/tracks?uid=<UID>&fields=TrackId,TrackName,WRReplay.ReplayId,WRReplay.ReplayTime
//       {"More":false,"Results":[{"TrackId":..,"TrackName":"..","WRReplay":{"ReplayId":..,"ReplayTime":..}}]}
//   GET https://tmnf.exchange/recordgbx/<ReplayId>   the .Replay.Gbx
// A map not on tmnf.exchange is asked for on tmuf.exchange (United Forever's site, the same API).
namespace tmshaders {
namespace misc {
namespace tmx {
namespace {

std::mutex g_lock;
Status g_status;
DWORD g_lastRequest = 0;
constexpr DWORD kMinGapMs = 20000;

void set(State state, const std::string& message, const std::wstring& file = std::wstring()) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_status.state = state;
    g_status.message = message;
    g_status.file = file;
}

bool get(const wchar_t* host, const std::wstring& path, std::string& body, size_t limit, DWORD& status) {
    status = 0;
    HINTERNET session = WinHttpOpen(L"TMVibrantShaders/" TM_SHADERS_VERSION L" (split times; github.com/cheatoskar/TM-Vibrant-Shaders)",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 5000, 5000, 8000, 15000);
    HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                        WINHTTP_FLAG_SECURE)
                                   : nullptr;
    bool ok = request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    if (ok) {
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        ok = status == 200;
    }
    while (ok) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, &chunk[0], available, &read)) {
            ok = false;
            break;
        }
        body.append(chunk.data(), read);
        if (body.size() > limit) {
            ok = false;
            break;
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok && !body.empty();
}

// The number after "key": in the JSON (first one), -1 none.
long long jsonNumber(const std::string& json, const char* key) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t at = json.find(k);
    if (at == std::string::npos) return -1;
    const char* p = json.c_str() + at + k.size();
    while (*p == ' ') p++;
    if (*p < '0' || *p > '9') return -1;
    return _strtoi64(p, nullptr, 10);
}

std::wstring safeName(const std::string& uid) {
    std::wstring name;
    for (char c : uid) name += (isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? static_cast<wchar_t>(c) : L'_';
    return name;
}

void run(std::string uid) {
    const wchar_t* const hosts[] = {L"tmnf.exchange", L"tmuf.exchange"};
    for (const wchar_t* host : hosts) {
        set(State::Busy, "asking " + utf8(host) + "...");
        std::string body;
        DWORD status = 0;
        const std::wstring path = L"/api/tracks?uid=" + wide(uid) + L"&fields=TrackId,TrackName,WRReplay.ReplayId,WRReplay.ReplayTime";
        if (!get(host, path, body, 1u << 20, status)) {
            TMVS_LOG("misc: TMX: %ls answered %lu to the map's lookup", host, status);
            set(State::Failed, "no answer from " + utf8(host) + (status ? " (HTTP " + std::to_string(status) + ")" : ""));
            return;
        }
        const long long track = jsonNumber(body, "TrackId");
        if (track < 0) continue; // not on this site
        const long long replay = jsonNumber(body, "ReplayId");
        if (replay < 0) {
            set(State::Failed, "the map is on " + utf8(host) + " but has no record replay");
            return;
        }
        set(State::Busy, "downloading the record...");
        std::string file;
        Sleep(1000); // two requests, not at once
        if (!get(host, L"/recordgbx/" + std::to_wstring(replay), file, 32u << 20, status) || file.compare(0, 3, "GBX") != 0) {
            TMVS_LOG("misc: TMX: %ls answered %lu to the replay %lld", host, status, replay);
            set(State::Failed, "the download failed");
            return;
        }
        const std::wstring target = dataFolder(L"Replays") + L"\\" + safeName(uid) + L"_" + std::to_wstring(replay) + L".Replay.Gbx";
        FILE* f = _wfopen(target.c_str(), L"wb");
        const bool written = f && fwrite(file.data(), 1, file.size(), f) == file.size();
        if (f) fclose(f);
        if (!written) {
            DeleteFileW(target.c_str());
            set(State::Failed, "can't save the replay");
            return;
        }
        TMVS_LOG("misc: TMX: map %s is track %lld on %ls, its record replay %lld saved (%zu bytes)", uid.c_str(), track, host, replay, file.size());
        set(State::Done, "record replay downloaded", target);
        return;
    }
    set(State::Failed, "the map is not on TMX");
}

} // namespace

std::wstring cached(const std::string& uid) {
    if (uid.empty()) return std::wstring();
    WIN32_FIND_DATAW fd;
    const std::wstring folder = dataFolder(L"Replays");
    HANDLE h = FindFirstFileW((folder + L"\\" + safeName(uid) + L"_*.Replay.Gbx").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return std::wstring();
    FindClose(h);
    return folder + L"\\" + fd.cFileName;
}

void request(const std::string& uid) {
    if (uid.empty()) return;
    const std::wstring have = cached(uid);
    if (!have.empty()) {
        set(State::Done, "already downloaded", have);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_status.state == State::Busy) return;
        const DWORD now = GetTickCount();
        if (g_lastRequest && now - g_lastRequest < kMinGapMs) {
            g_status.state = State::Failed;
            g_status.message = "wait a moment before asking again";
            return;
        }
        g_lastRequest = now;
        g_status.state = State::Busy;
        g_status.message = "asking TMX...";
    }
    std::thread(run, uid).detach();
}

Status status() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_status;
}

} // namespace tmx
} // namespace misc
} // namespace tmshaders
