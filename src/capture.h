#pragma once
#include <d3d9.h>
#include <string>

namespace tmshaders {
namespace capture {

// Saves a render-target surface as a 24-bit BMP (8-bit and FP16 RGBA formats).
bool saveSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface, const std::wstring& path);

// Saves a single-channel float surface (R32F/R16F) as raw float32 with a small header:
// uint32 width, uint32 height, then width*height floats.
bool saveFloatSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface, const std::wstring& path);

} // namespace capture
} // namespace tmshaders
