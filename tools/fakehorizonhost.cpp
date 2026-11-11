// A stand-in for Horizon Host's host.dll in tools/horizontest.cpp: the three exports mods use
// (HorizonHost_GetUiState, _SetCaptureActive, _Version) with counters the test reads through
// FakeHost_* exports. Built twice: host.dll, and with FAKE_HORIZON_OLD as HorizonHost.dll without the
// exports (a host from before them).
#include <windows.h>
#include <cstdint>

#define FAKE_EXPORT extern "C" __declspec(dllexport)

namespace {
uint32_t g_ui = 0;
int g_captureOn = 0, g_captureOff = 0, g_active = 0;
int g_uiCalls = 0, g_versionCalls = 0;
} // namespace

FAKE_EXPORT int __cdecl FakeHost_Loaded() {
    return 1;
}

#ifndef FAKE_HORIZON_OLD
FAKE_EXPORT uint32_t __cdecl HorizonHost_GetUiState() {
    g_uiCalls++;
    return g_active ? 0u : g_ui; // nothing on screen while a capture is on (as the real host)
}

FAKE_EXPORT void __cdecl HorizonHost_SetCaptureActive(int active) {
    if (active) g_captureOn++;
    else g_captureOff++;
    g_active = active;
}

FAKE_EXPORT uint32_t __cdecl HorizonHost_Version() {
    g_versionCalls++;
    return 0x000200u;
}

FAKE_EXPORT void __cdecl FakeHost_SetUiState(uint32_t state) {
    g_ui = state;
}

// which: 0 calls with 1, 1 calls with 0, 2 the capture is on now, 3 GetUiState calls, 4 Version calls.
FAKE_EXPORT int __cdecl FakeHost_Count(int which) {
    switch (which) {
    case 0: return g_captureOn;
    case 1: return g_captureOff;
    case 2: return g_active;
    case 3: return g_uiCalls;
    case 4: return g_versionCalls;
    }
    return -1;
}
#endif

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
