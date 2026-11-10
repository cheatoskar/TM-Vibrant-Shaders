#pragma once
#include "planeread.h"
#include "gfx.h"
#include <d3d9.h>
#include <cstdint>
#include <deque>
#include <functional>

// The clips' frames on the graphics card: the picture (the back buffer, or with "Without HUD" the
// scene TM Vibrant Shaders shaded before the HUD) scaled to the video's size with StretchRect, then
// to NV12 by two small passes (shaders/clips.hlsl) and read back a frame or two later without
// waiting (PlaneReadback, the video export's). Everything on the device's thread; the device's state
// is put back as it was (a state block, the render target and depth buffer).
namespace tmshaders {
namespace clips {

class Grabber {
public:
    // Calls bracketing the grabber's own device calls (TM Vibrant Shaders: hook::beginInternal /
    // endInternal, so its device hooks leave them alone). Null: none.
    static void setInternalCalls(void (*begin)(), void (*end)());

    // The scene before the HUD into the scaled copy now (StretchRect only); the next grab() uses it.
    bool copy(IDirect3DDevice9* device, IDirect3DSurface9* source, int width, int height);
    // A frame: from `source` (null: the copy made since the last grab) to NV12, queued for read-back.
    bool grab(IDirect3DDevice9* device, IDirect3DSurface9* source, int width, int height, int64_t time);
    bool fresh() const { return m_fresh; }
    // Frames whose read-back finished, oldest first (`all`: also those still on their way, waited for).
    void collect(const std::function<void(const PlaneReadback::Frame&, int64_t)>& use, bool all = false);
    void releaseDevice(); // before a reset or when recording stops
    void releaseQueries();

    double lastGrabMs = 0.0; // the game's thread in the last grab()
    std::string lastError;

private:
    bool ensure(IDirect3DDevice9* device, int width, int height, D3DFORMAT format);
    bool scale(IDirect3DDevice9* device, IDirect3DSurface9* source);

    gfx::Target m_scaled, m_luma, m_chroma;
    IDirect3DSurface9* m_resolve = nullptr; // a multisampled back buffer at its own size first
    UINT m_resolveW = 0, m_resolveH = 0;
    D3DFORMAT m_resolveFormat = D3DFMT_UNKNOWN;
    IDirect3DPixelShader9* m_psLuma = nullptr;
    IDirect3DPixelShader9* m_psChroma = nullptr;
    gfx::Quad m_quad;
    PlaneReadback m_readback;
    std::deque<int64_t> m_times; // of the frames queued for read-back, in order
    bool m_fresh = false;
    bool m_failedOnce = false;
};

} // namespace clips
} // namespace tmshaders
