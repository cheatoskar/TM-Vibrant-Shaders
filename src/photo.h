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

// The depth of field amount (0..1) as a camera aperture: 1 = f/1.4 (most blur), 0 = f/16.
float fNumber(float dof);

// The focus point (uv), -1 = none.
void focusPoint(float out[2]);

// After the pipeline shaded the main camera (before the HUD): takes the photo or the tile.
void afterRender(IDirect3DDevice9* device, IDirect3DSurface9* frame);

// Compare split (Shift+F6, also turns photo mode on): one side of the line shows the game's
// image as it came in, without the mod's effects; in photos too, not in hi-res ones (the line
// would sit in every tile). Ctrl+Shift+wheel moves the line.
void toggleSplit();
bool splitShown();      // switched on, photo mode on, no hi-res photo being taken
bool& splitOn();        // the switch (for the menu)
float& splitPosition(); // 0..1 from the left
bool& splitSwapped();   // the game's image on the right instead of the left
// Copies the unshaded image (`original`, same size) onto its side of `shaded` and draws the
// line. Call it before afterRender, so the photo has it.
void applySplit(IDirect3DDevice9* device, IDirect3DSurface9* original, IDirect3DSurface9* shaded);

// A line for the screen (photo mode on, progress, saved as ...), "" = nothing to show.
const char* status();
// Shows a line there for a few seconds (also outside photo mode).
void notify(const char* text, DWORD seconds);

} // namespace photo
} // namespace tmshaders
