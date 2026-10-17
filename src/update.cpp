#include "update.h"
#include "log.h"
#include "tm_shaders_version.h"
#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace tmshaders {
namespace update {

namespace {
std::mutex g_mutex;
Info g_info;

// The value of a top-level string field in GitHub's JSON ("tag_name": "v1.3.1"), with the
// escapes undone. Enough for the few fields we read; the first match is the release's own.
std::string jsonString(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t at = json.find(needle);
    if (at == std::string::npos) return {};
    at = json.find(':', at + needle.size());
    if (at == std::string::npos) return {};
    at = json.find_first_not_of(" \t\r\n", at + 1);
    if (at == std::string::npos || json[at] != '"') return {};
    std::string out;
    for (size_t i = at + 1; i < json.size(); i++) {
        const char ch = json[i];
        if (ch == '"') break;
        if (ch != '\\' || i + 1 >= json.size()) {
            out += ch;
            continue;
        }
        const char e = json[++i];
        switch (e) {
            case 'n': out += '\n'; break;
            case 'r': break;
            case 't': out += "    "; break;
            case 'u': {
                // A code point as UTF-8 (surrogate pairs, e.g. emoji, become '?').
                const unsigned cp = i + 4 < json.size() ? static_cast<unsigned>(strtoul(json.substr(i + 1, 4).c_str(), nullptr, 16)) : '?';
                i += 4;
                if (cp < 0x80) {
                    out += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0xD800 || cp > 0xDFFF) {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += '?';
                }
                break;
            }
            default: out += e; break;
        }
    }
    return out;
}

bool download(const wchar_t* path, std::string& body) {
    HINTERNET session = WinHttpOpen(L"TMVibrantShaders/" TM_SHADERS_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 8000);
    HINTERNET connection = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                        WINHTTP_FLAG_SECURE)
                                   : nullptr;
    bool ok = request && WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    if (ok) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        ok = status == 200;
    }
    while (ok) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
        std::vector<char> chunk(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) {
            ok = false;
            break;
        }
        body.append(chunk.data(), read);
        if (body.size() > (1u << 20)) break; // a release is a few kB
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok && !body.empty();
}

void run() {
    std::wstring path = L"/repos/";
    for (const char* c = kRepository; *c; c++) path += static_cast<wchar_t>(*c);
    path += L"/releases/latest";
    std::string body;
    Info info;
    if (download(path.c_str(), body)) {
        std::string tag = jsonString(body, "tag_name");
        info.version = !tag.empty() && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
        info.url = std::string("https://github.com/") + kRepository + "/releases/tag/" + tag;
        info.notes = jsonString(body, "body");
        info.state = info.version.empty() ? State::Failed : (newer(info.version, TM_SHADERS_VERSION_A) ? State::Available : State::UpToDate);
    } else {
        info.state = State::Failed;
    }
    const Install how = install();
    TMVS_LOG("update: %s (latest release %s, this is %s%s)",
             info.state == State::Available ? "a new version is out" : (info.state == State::UpToDate ? "up to date" : "check failed"),
             info.version.empty() ? "unknown" : info.version.c_str(), TM_SHADERS_VERSION_A,
             how == Install::ModLoaderServer ? ", ModLoader with the update server"
                                             : (how == Install::ModLoader ? ", ModLoader without the update server" : ""));
    std::lock_guard<std::mutex> lock(g_mutex);
    g_info = info;
}
} // namespace

void check() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_info.state == State::Checking) return;
        g_info.state = State::Checking;
    }
    std::thread(run).detach();
}

Info latest() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_info;
}

namespace {
std::wstring lowerAscii(std::wstring s) {
    for (wchar_t& c : s)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
    return s;
}

bool fileContains(const std::wstring& path, const std::string& needle) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::string text(1 << 16, '\0');
    text.resize(fread(&text[0], 1, text.size(), f));
    fclose(f);
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return text.find(needle) != std::string::npos;
}

Install detectInstall() {
    HMODULE self = nullptr;
    wchar_t buffer[1024] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&detectInstall), &self) ||
        !GetModuleFileNameW(self, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]))))
        return Install::Other;
    // <TMLoader>\database\TmForever\products\TM Vibrant Shaders\<version>\TMVibrantShaders.dll
    std::wstring path = buffer;
    if (lowerAscii(path).find(L"\\products\\tm vibrant shaders\\") == std::wstring::npos) return Install::Other;
    // config.yaml: in %LOCALAPPDATA%\TMLoader, or next to the database (portable ModLoader).
    std::string needle = kModLoaderServer;
    for (char& c : needle)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    needle = needle.substr(needle.find("://") + 3); // with or without https://, trailing slash optional
    if (!needle.empty() && needle.back() == '/') needle.pop_back();
    std::wstring root = path;
    for (int up = 0; up < 6 && root.find_last_of(L"\\/") != std::wstring::npos; up++) root.resize(root.find_last_of(L"\\/"));
    if (fileContains(root + L"\\config.yaml", needle)) return Install::ModLoaderServer;
    wchar_t local[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n > 0 && n < MAX_PATH && fileContains(std::wstring(local) + L"\\TMLoader\\config.yaml", needle)) return Install::ModLoaderServer;
    return Install::ModLoader;
}
} // namespace

Install install() {
    static const Install cached = detectInstall();
    return cached;
}

bool newer(const std::string& version, const std::string& than) {
    const char* a = version.c_str();
    const char* b = than.c_str();
    for (int part = 0; part < 4; part++) {
        char* endA = nullptr;
        char* endB = nullptr;
        const unsigned long x = strtoul(a, &endA, 10), y = strtoul(b, &endB, 10);
        if (x != y) return x > y;
        a = *endA == '.' ? endA + 1 : endA;
        b = *endB == '.' ? endB + 1 : endB;
        if (*a < '0' || *a > '9') a = "0";
        if (*b < '0' || *b > '9') b = "0";
    }
    // Same numbers: a pre-release (1.4.0-preview) is older than the release (1.4.0).
    const bool preA = strchr(version.c_str(), '-') != nullptr, preB = strchr(than.c_str(), '-') != nullptr;
    return !preA && preB;
}

} // namespace update
} // namespace tmshaders
