#pragma once
#include <d3d9.h>
#include <windows.h>

namespace tmshaders {
struct Settings;

// Photo mode: no HUD, full quality, effects that reach far, focus where you click, photos
// without the HUD (also tiled, at several times the screen size). It comes on by itself
// with the free camera (cam 7, unless switched off in the menu); F6 turns it on or off.
namespace photo {

bool active();
void toggle();

// Once per frame before the game renders (RenderFrameBegin): follows the free camera, hides
// the HUD, and sets the projection of the hi-res tile being taken.
void beginFrame();

// From the window procedure (after the menu had its turn): middle click = focus there,
// Ctrl + middle click = no focus, Ctrl + wheel = more or less depth of field. True = used.
bool handleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

// F12 in photo mode: a photo at screen size, or (hiRes) a tiled one, larger than the screen.
void request(bool hiRes);

// The look of this frame: full quality, depth of field on the focus point, and while tiles
// are taken no effects that belong to the whole frame (vignette, grain, lens effects).
void adjust(Settings& s);

// A hi-res photo is being taken: TAA jitter off; exposure, focus and haze colour frozen.
bool tiling();

// The focus point (uv), -1 = none.
void focusPoint(float out[2]);

// After the pipeline shaded the main camera (before the HUD): takes the photo or the tile.
void afterRender(IDirect3DDevice9* device, IDirect3DSurface9* frame);

// A line for the screen (photo mode on, progress, saved as ...), "" = nothing to show.
const char* status();

} // namespace photo
} // namespace tmshaders
