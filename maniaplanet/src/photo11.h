#pragma once
// Photo mode for ManiaPlanet, after the TMNF mod's (src/photo.cpp): F6 on/off, the interface
// hidden, full quality, depth of field on the point clicked (middle click), aperture on
// Ctrl+wheel, F12 saves photo_NNN.png (the frame without the interface) in Documents\ManiaPlanet\TMVS.
// Not here: hi-res tiled photos and zoom (they need the engine's projection), and coming on by
// itself with the free camera (ManiaPlanet's camera isn't known to the mod).
#include <d3d11.h>
#include <windows.h>

namespace tmshaders {
struct Settings;
}

namespace tmvs_mp {
namespace photo {

bool active();
void toggle();
// The interface (HUD, the game's menus) is skipped while photo mode is on (Hide the HUD).
bool hideInterface();

// From the game window's procedure: middle click = focus there, Ctrl + middle click = no focus,
// Ctrl + wheel = aperture, Esc = leave photo mode. True = used (the game doesn't get it).
bool handleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

// F12 in photo mode: a photo of the next frame, without the interface.
void request();

// The look of this frame: full quality, depth of field on the focus point, no motion blur.
void adjust(tmshaders::Settings& s);

// The shaders off (F5) in photo mode: only the camera's effects (depth of field, sharpening) on
// the game's own image; every other effect and the grade off, the engine's tone curve. After adjust.
void vanillaLook(tmshaders::Settings& s);

// Compare split (Shift+F6, as in TMNF): one side of a line shows the game's own image. Only in
// photo mode (switching it on turns photo mode on). Ctrl+Shift+wheel moves the line.
bool& splitOn();
float& splitPosition();
bool& splitSwapped();
void toggleSplit();
bool splitShown();
// For the look: the line's position (0 = no split), negative = the game's image on the right.
float splitForLook();

// The focus point (uv), -1 = none.
void focusPoint(float out[2]);

// The depth of field amount (0..1) as a camera aperture: 1 = f/1.4 (most blur), 0 = f/16.
float fNumber(float dof);

// After the mod drew its effects onto the finished image (before the interface): takes the photo.
void afterRender(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Resource* frame);

// A line for the screen (top left), "" = nothing to show; notify shows one for a few seconds.
const char* status();
void notify(const char* text, DWORD seconds);

} // namespace photo
} // namespace tmvs_mp
