#pragma once
// Photo mode for ManiaPlanet, after the TMNF mod's (src/photo.cpp): F6 on/off, the interface
// hidden, full quality, depth of field on the point clicked (middle click), aperture on
// Ctrl+wheel, F12 saves photo_NNN.png (the frame without the interface) in Documents\ManiaPlanet\TMVS.
// Zoom (Shift+wheel) and hi-res photos (Shift+F12, tiles put together) through the game's camera
// (gamecamera.cpp, the *Game camera* option). Not here: coming on by itself with the free camera.
#include "gamecamera.h"
#include <d3d11.h>
#include <windows.h>
#include <string>

namespace tmshaders {
struct Settings;
}

namespace tmvs_mp {
namespace photo {

bool active();
void toggle();
// The interface (HUD, the game's menus) is skipped while photo mode is on (Hide the HUD).
bool hideInterface();
// Hide car (photo mode): the cars' draws are skipped (hooks.cpp).
bool hideCars();

// Once a frame at Present: what photo mode wants from the game's camera this frame (zoom, the
// hi-res photo's tile). `gameCamera`: the Game camera option is on (else zoom and tiles can't be
// done: a request says so).
gamecamera::Want cameraWant(bool gameCamera);
// A hi-res photo is being taken (no TAA jitter, the exposure frozen, effects of the whole frame off).
bool tiling();

// From the game window's procedure: middle click = focus there, Ctrl + middle click = no focus,
// Ctrl + wheel = aperture, Esc = leave photo mode. True = used (the game doesn't get it).
bool handleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

// F12 in photo mode: a photo of the next frame, without the interface. hiRes (Shift+F12): tiles
// of the view, each rendered by the game's camera zoomed in on it (photoTiles x photoTiles).
void request(bool hiRes = false);
// Shift+wheel zoom (1 = none).
float zoom();

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
// What the other side shows: "" the game's image, else a preset's look (drawn by a second
// Renderer, hooks.cpp). Your look stays on the mod's side.
std::string& splitPreset();
// The name of what the left or right side shows (your preset, "Game" or the other preset), for
// the labels at the line and in photos (SplitPhotoLabels).
std::string splitName(bool right);

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
