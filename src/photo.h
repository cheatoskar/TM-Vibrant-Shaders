#pragma once
#include <d3d9.h>
#include <windows.h>
#include <string>

namespace tmshaders {
struct Settings;

// Photo mode: no HUD, full quality, effects that reach far, focus where you click, photos
// without the HUD (also tiled, at several times the screen size). It comes on by itself
// with the free camera (cam 7, unless switched off in the menu); F6 turns it on or off.
namespace photo {

bool active();
void toggle();

// The compact panel (exposure, aperture, zoom, vignette, grain, focus, hide car / HUD): drawn by
// the overlay while photo mode is on; its key (PhotoPanel, F10) hides it for a clean view.
bool panelShown();
void togglePanel();
float& zoom();     // Shift+wheel's zoom, 1..8
void clearFocus(); // back to the automatic focus

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

// A photo or a tile is waiting for the next frame (needed with the shaders off, where
// the pipeline doesn't run and processScene only hands the game's image over).
bool pending();

// The depth of field amount (0..1) as a camera aperture: 1 = f/1.4 (most blur), 0 = f/16.
float fNumber(float dof);

// The focus point (uv), -1 = none.
void focusPoint(float out[2]);

// After the pipeline shaded the main camera (before the HUD): takes the photo or the tile.
void afterRender(IDirect3DDevice9* device, IDirect3DSurface9* frame);

// Compare split (Shift+F6, also turns photo mode on): one side of the line shows the game's
// image as it came in, without the mod's effects (only photo mode's depth of field, the same
// lens as your side); in photos too, hi-res ones included (each tile gets its part of the line),
// with both sides' names burnt in at the bottom (SplitPhotoLabels). Ctrl+Shift+wheel moves the line.
void toggleSplit();
bool splitShown();      // switched on, photo mode on (also while a hi-res photo is taken)
bool& splitOn();        // the switch (for the menu)
float& splitPosition(); // 0..1 from the left
bool& splitSwapped();   // the game's image on the right instead of the left
// What the other side shows: "" the game's image, else a preset's look (rendered by a pipeline
// of its own, see plugin.cpp). Your look stays on the mod's side.
std::string& splitPreset();
// Copies the other side's image (`original`: the unshaded one, the game's through photo mode's
// lens, or the preset's; same size) onto its side of `shaded` and draws the line (in a hi-res
// photo's tile: where the line falls in the whole photo). Call it before afterRender, so the photo has it.
void applySplit(IDirect3DDevice9* device, IDirect3DSurface9* original, IDirect3DSurface9* shaded);
// The game draws one frame, lit for your look: real night, its moon, its sky and its sun's halo
// follow your look on both sides. "" when the preset on the other side would get the same light,
// else a short line saying what it misses (its sun effects can't show on a frame lit otherwise).
std::string splitLightNote();

// A line for the screen (photo mode on, progress, saved as ...), "" = nothing to show.
const char* status();
// Shows a line there for a few seconds (also outside photo mode).
void notify(const char* text, DWORD seconds);

} // namespace photo
} // namespace tmshaders
