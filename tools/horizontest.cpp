// Tests the Horizon Host handshake (src/horizon.cpp) against the fake host next to this exe
// (tools/fakehorizonhost.cpp, built as host.dll and as an old HorizonHost.dll without the exports):
// the host found late and only every few seconds, the old host ignored, the UI state passed on, and
// the capture calls (every frame while on, once off, nothing without a host).
//   tmvs_horizontest.exe                  the fake host
//   tmvs_horizontest.exe --real <host.dll>  a real host.dll (horizon-host's build): found, its version
// Exit code 0 = all passed. Its log goes to this exe's folder, never the game's.
#include "horizon.h"
#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <string>

using namespace tmshaders;

static int g_failed = 0;
#define CHECK(...) \
    do { \
        if (!(__VA_ARGS__)) { \
            printf("FAIL line %d: %s\n", __LINE__, #__VA_ARGS__); \
            g_failed++; \
        } \
    } while (0)

static std::wstring exeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    return dir.substr(0, dir.find_last_of(L"\\/"));
}

typedef void(__cdecl* SetUiFn)(uint32_t);
typedef int(__cdecl* CountFn)(int);

static int fakeTest() {
    const std::wstring dir = exeDir();
    const uint32_t t = 100000;

    // No host: nothing happens, nothing is called.
    horizon::tick(t);
    CHECK(horizon::searches() == 1);
    CHECK(!horizon::found());
    CHECK(horizon::uiState() == 0);
    CHECK(!horizon::menuOpen());
    horizon::capture(true);
    CHECK(!horizon::capturing());
    horizon::capture(false);
    horizon::tick(t + 1000);
    CHECK(horizon::searches() == 1); // not again before kSearchEveryMs

    // A host without the exports (0.1.0): not taken.
    HMODULE old = LoadLibraryW((dir + L"\\HorizonHost.dll").c_str());
    CHECK(old != nullptr);
    horizon::tick(t + horizon::kSearchEveryMs);
    CHECK(horizon::searches() == 2);
    CHECK(!horizon::found());

    // The host loads after us: found at the next search, not before.
    HMODULE host = LoadLibraryW((dir + L"\\host.dll").c_str());
    CHECK(host != nullptr);
    if (!host) return 1;
    const auto setUi = reinterpret_cast<SetUiFn>(GetProcAddress(host, "FakeHost_SetUiState"));
    const auto count = reinterpret_cast<CountFn>(GetProcAddress(host, "FakeHost_Count"));
    CHECK(setUi && count);
    if (!setUi || !count) return 1;
    horizon::tick(t + horizon::kSearchEveryMs + 1000);
    CHECK(horizon::searches() == 2);
    CHECK(!horizon::found());
    horizon::tick(t + 2 * horizon::kSearchEveryMs);
    CHECK(horizon::searches() == 3);
    CHECK(horizon::found());
    CHECK(horizon::version() == 0x000200u);
    CHECK(count(4) == 1); // the version asked once
    horizon::tick(t + 60000);
    CHECK(horizon::searches() == 3); // found: never searched again

    // The UI state as the host gives it.
    setUi(horizon::kBarVisible);
    CHECK(horizon::uiState() == horizon::kBarVisible);
    CHECK(!horizon::menuOpen());
    setUi(horizon::kMenuOpen | horizon::kBarVisible);
    CHECK(horizon::menuOpen());

    // Captures: nothing sent while none runs; every frame while on; off once.
    horizon::capture(false);
    CHECK(count(0) == 0 && count(1) == 0);
    for (int frame = 0; frame < 3; frame++) horizon::capture(true);
    CHECK(count(0) == 3);
    CHECK(count(2) == 1);
    CHECK(horizon::capturing());
    CHECK(horizon::uiState() == 0); // the host shows nothing meanwhile
    CHECK(!horizon::menuOpen());
    horizon::capture(false);
    horizon::capture(false);
    CHECK(count(1) == 1);
    CHECK(count(2) == 0);
    CHECK(!horizon::capturing());
    CHECK(horizon::menuOpen()); // its menu again

    // Shutdown ends a capture still on; without one it sends nothing.
    horizon::capture(true);
    horizon::release();
    CHECK(count(1) == 2 && count(2) == 0);
    horizon::release();
    CHECK(count(1) == 2);

    FreeLibrary(host);
    FreeLibrary(old);
    return 0;
}

static int realTest(const wchar_t* path) {
    HMODULE host = LoadLibraryW(path);
    CHECK(host != nullptr);
    if (!host) {
        printf("could not load %ls (error %lu)\n", path, GetLastError());
        return 1;
    }
    // Its file name decides whether we find it (host.dll or HorizonHost.dll).
    horizon::tick(GetTickCount());
    CHECK(horizon::found());
    if (!horizon::found()) return 1;
    const uint32_t v = horizon::version();
    printf("real host %u.%u.%u, ui state %u\n", (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff, horizon::uiState());
    CHECK(v >= 0x000200u);
    CHECK(!horizon::menuOpen()); // no game, no device: nothing on screen
    horizon::capture(true);
    CHECK(horizon::capturing());
    CHECK(horizon::uiState() == 0);
    horizon::capture(false);
    CHECK(!horizon::capturing());
    // Not freed: a host may run threads of its own until the process ends.
    return 0;
}

int main(int argc, char** argv) {
    log::setDataDir(exeDir() + L"\\horizontest");
    log::setFileName(L"tmvs_horizontest.log");
    if (argc == 3 && !strcmp(argv[1], "--real")) {
        wchar_t path[MAX_PATH] = {};
        MultiByteToWideChar(CP_ACP, 0, argv[2], -1, path, MAX_PATH);
        realTest(path);
    } else {
        fakeTest();
    }
    if (g_failed) printf("%d FAILED\n", g_failed);
    else printf("all passed\n");
    return g_failed ? 1 : 0;
}
