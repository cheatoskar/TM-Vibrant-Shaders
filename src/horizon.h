#pragma once
#include <cstdint>

// Horizon Host (TM Horizon's in-game mod menu: horizon\host.dll from its dinput8 loader, or the
// ModLoader product's HorizonHost.dll) draws its menu and bar in its own hook of the device's Present.
// Its hook sits in front of ours, so whatever reads the back buffer at our Present finds Horizon's
// interface on it, and our menu is drawn over Horizon's. The host exports three plain C functions
// for mods that are not its plugins (horizon-host docs/plugin-abi.md, "For mods that are not
// plugins"):
//   uint32_t HorizonHost_GetUiState(void)     1 menu open, 2 bar visible, 4 a detached panel (0 while
//                                             a capture is active)
//   void HorizonHost_SetCaptureActive(int)    non-zero: the host draws nothing and takes no input; it
//                                             ends by itself 10 s after the last call
//   uint32_t HorizonHost_Version(void)        (major << 16) | (minor << 8) | patch
// An older host without them, or no host: nothing here does anything.
//
// The game's thread only (the Present).
namespace tmshaders {
namespace horizon {

constexpr uint32_t kMenuOpen = 1u, kBarVisible = 2u, kPanelVisible = 4u;

// Once a frame: looks for the host every few seconds until it is found (it may load after us);
// logged once with its version. now: GetTickCount().
void tick(uint32_t now);
bool found();
uint32_t version();
// What of Horizon is on screen this frame (0: nothing, or no host).
uint32_t uiState();
bool menuOpen();

// Once a frame (the end of our Present): whether one of our captures reads the whole back buffer at a
// Present to come, where Horizon's interface would be on it. While true the host is told every frame
// to draw nothing; once false it is told once to draw again.
void capture(bool active);
bool capturing();
// Shutdown: a capture still on ends.
void release();

// Tests: how often the host was searched for, and the clock between searches.
int searches();
constexpr uint32_t kSearchEveryMs = 3000;

} // namespace horizon
} // namespace tmshaders
