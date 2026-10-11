#pragma once
#include <d3d9.h>
#include <string>

namespace tmshaders {
namespace thumbnails {

// The picture of a preset for the menu: the built-in ones are embedded in the DLL, yours are
// taken in the game when you save them (presets\<name>.jpg). nullptr = no picture.
// Managed textures: they survive a device reset.
IDirect3DTexture9* get(IDirect3DDevice9* device, const std::string& preset);

// Saves the middle of the finished frame (before the HUD) as a preset's picture.
bool savePicture(IDirect3DDevice9* device, IDirect3DSurface9* frame, const std::wstring& path);

} // namespace thumbnails
} // namespace tmshaders
