#pragma once
#include <d3d9.h>
#include <string>
#include <vector>

namespace tmshaders {
namespace capture {

// Saves a render-target surface as a 24-bit BMP (8-bit and FP16 RGBA formats).
bool saveSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface, const std::wstring& path);

// The same in two steps, so the slow half can run on another thread: readSurface copies the
// surface's pixels (device calls, the render thread), writeBmp converts and writes them (no
// device, any thread).
struct Pixels {
    D3DFORMAT format = D3DFMT_UNKNOWN;
    UINT width = 0, height = 0;
    size_t pitch = 0; // bytes per row in `bytes` (0: a format the BMP can't show, written black)
    std::vector<unsigned char> bytes;
};
bool readSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface, Pixels& out);
bool writeBmp(const Pixels& image, const std::wstring& path);

// Saves a single-channel float surface (R32F/R16F) as raw float32 with a small header:
// uint32 width, uint32 height, then width*height floats.
bool saveFloatSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface, const std::wstring& path);

} // namespace capture
} // namespace tmshaders
