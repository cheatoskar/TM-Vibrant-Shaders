#include "horizon.h"
#include "log.h"
#include <windows.h>

namespace tmshaders {
namespace horizon {
namespace {

typedef uint32_t(__cdecl* GetUiStateFn)(void);
typedef void(__cdecl* SetCaptureActiveFn)(int active);
typedef uint32_t(__cdecl* VersionFn)(void);

GetUiStateFn g_getUiState = nullptr;
SetCaptureActiveFn g_setCapture = nullptr;
uint32_t g_version = 0;
bool g_found = false;
bool g_searched = false;
uint32_t g_lastSearch = 0;
int g_searches = 0;
bool g_capturing = false;
bool g_oldHostLogged = false;

// The legacy loader's host, then the ModLoader product's (TrackMania Forever).
const wchar_t* const kModules[] = {L"host.dll", L"HorizonHost.dll"};

void search() {
    g_searches++;
    for (const wchar_t* name : kModules) {
        HMODULE module = GetModuleHandleW(name);
        if (!module) continue;
        const auto ui = reinterpret_cast<GetUiStateFn>(GetProcAddress(module, "HorizonHost_GetUiState"));
        const auto capture = reinterpret_cast<SetCaptureActiveFn>(GetProcAddress(module, "HorizonHost_SetCaptureActive"));
        const auto version = reinterpret_cast<VersionFn>(GetProcAddress(module, "HorizonHost_Version"));
        if (!ui || !capture || !version) {
            // A host from before these exports (0.1.0): it keeps drawing as it does; looked for again
            // in case another module of that name is the real one later.
            if (!g_oldHostLogged) {
                g_oldHostLogged = true;
                TMVS_LOG("horizon: %ls is loaded but has no HorizonHost_* exports (an older Horizon Host): no menu hiding, no capture handshake", name);
            }
            continue;
        }
        g_getUiState = ui;
        g_setCapture = capture;
        g_version = version();
        g_found = true;
        TMVS_LOG("horizon: Horizon Host %u.%u.%u found (%ls): our menu gives way to its menu, captures hide its interface where needed",
                 (g_version >> 16) & 0xff, (g_version >> 8) & 0xff, g_version & 0xff, name);
        return;
    }
}

} // namespace

void tick(uint32_t now) {
    if (g_found) return;
    if (g_searched && now - g_lastSearch < kSearchEveryMs) return;
    g_searched = true;
    g_lastSearch = now;
    search();
}

bool found() {
    return g_found;
}

uint32_t version() {
    return g_version;
}

uint32_t uiState() {
    return g_found ? g_getUiState() : 0u;
}

bool menuOpen() {
    return (uiState() & kMenuOpen) != 0;
}

void capture(bool active) {
    if (!g_found) return;
    // Every frame while on (the host ends a capture 10 s after the last call); off once.
    if (active) {
        if (!g_capturing) TMVS_LOG("horizon: capture on (its interface hidden)");
        g_setCapture(1);
    } else if (g_capturing) {
        g_setCapture(0);
        TMVS_LOG("horizon: capture off");
    }
    g_capturing = active;
}

bool capturing() {
    return g_capturing;
}

void release() {
    capture(false);
}

int searches() {
    return g_searches;
}

} // namespace horizon
} // namespace tmshaders
