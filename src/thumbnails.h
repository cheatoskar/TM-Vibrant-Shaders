#pragma once
#include <d3d9.h>
#include <string>

namespace tmshaders {
namespace thumbnails {

// The picture of a preset for the menu: the built-in ones are embedded in the DLL, yours are
// taken in the game when you save them (presets\<name>.jpg). nullptr = no picture.
// Managed textures: they survive a device reset.
IDirect3DTexture9* get(IDirect3DDevice9* device, const std::string& preset);

// The mod's icon for the menu's header, size x size pixels (drawn texel for pixel). Managed, kept
// until the size or the device changes. nullptr = none.
IDirect3DTexture9* icon(IDirect3DDevice9* device, int size);

// A resource pack's picture (its load screen, at most kPackPreviewWidth wide): made once per
// zip and write time, on a thread of its own, one at a time. nullptr = none, or not made yet
// (asked again next frame). width / height: the picture's size.
constexpr int kPackPreviewWidth = 256;
IDirect3DTexture9* packPreview(IDirect3DDevice9* device, const std::wstring& zip, unsigned long long written, int& width, int& height);

// Saves the middle of the finished frame (before the HUD) as a preset's picture.
bool savePicture(IDirect3DDevice9* device, IDirect3DSurface9* frame, const std::wstring& path);

} // namespace thumbnails
} // namespace tmshaders
