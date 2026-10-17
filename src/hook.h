#pragma once
#include <d3d9.h>

namespace tmshaders {
namespace hook {

struct DeviceCallbacks {
    void (*adjustPresentParams)(D3DPRESENT_PARAMETERS* params) = nullptr;
    void (*deviceCreated)(IDirect3DDevice9* device) = nullptr;
    void (*present)(IDirect3DDevice9* device) = nullptr;
    void (*preReset)() = nullptr;
    void (*postReset)(IDirect3DDevice9* device) = nullptr;
    // Return true to replace the game's depth-stencil creation (result in *hr).
    bool (*createDepthStencil)(IDirect3DDevice9* device, UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms,
                               IDirect3DSurface9** out, HRESULT* hr) = nullptr;
    // May substitute the surface the game binds as depth-stencil.
    IDirect3DSurface9* (*setDepthStencil)(IDirect3DSurface9* surface) = nullptr;
    // May substitute the surface the game binds as render target (GameHdr: the float scene).
    IDirect3DSurface9* (*setRenderTarget)(DWORD index, IDirect3DSurface9* surface) = nullptr;
    void (*draw)(IDirect3DDevice9* device) = nullptr;
    // After the game's draw (GameHdr: an additive draw's own target is unbound again).
    void (*drawDone)(IDirect3DDevice9* device) = nullptr;
    // An indexed draw, after `draw`, with its parameters (CasterShadows keeps static meshes).
    void (*drawIndexed)(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start,
                        UINT count) = nullptr;
    void (*setTransform)(D3DTRANSFORMSTATETYPE type, const D3DMATRIX* matrix) = nullptr;
    void (*setLight)(DWORD index, const D3DLIGHT9* light) = nullptr;
    // Before the game's Clear (not the plugin's own).
    void (*clear)(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color) = nullptr;
    // Every pixel shader created, the plugin's own too (GameGBuffer: which ones write the normal).
    void (*createPixelShader)(IDirect3DPixelShader9* shader, const DWORD* code) = nullptr;
    // RenderScale: the game's viewport and scissor rect, in its own (full-size) numbers. Set: returns
    // what to set instead (scaled into a smaller target) or the given one. Get: after the device
    // filled in its values, may give the game back its own numbers.
    const D3DVIEWPORT9* (*setViewport)(IDirect3DDevice9* device, const D3DVIEWPORT9* viewport) = nullptr;
    void (*getViewport)(IDirect3DDevice9* device, D3DVIEWPORT9* viewport) = nullptr;
    const RECT* (*setScissorRect)(IDirect3DDevice9* device, const RECT* rect) = nullptr;
    void (*getScissorRect)(IDirect3DDevice9* device, RECT* rect) = nullptr;
    // RenderScale: the game's Clear rects and StretchRect rects (null = the whole surface); returns the
    // rects to use instead, or the given ones. The Clear's flags and values: a depth clear of the
    // scaled target is repeated at the full size.
    const D3DRECT* (*clearRects)(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects, DWORD flags, float z, DWORD stencil) = nullptr;
    void (*stretchRects)(IDirect3DSurface9* src, const RECT** srcRect, IDirect3DSurface9* dst, const RECT** dstRect) = nullptr;
};

bool install(const DeviceCallbacks& callbacks);

// Replaces an import of the game's exe (by DLL and function name). Returns false if the exe
// doesn't import it.
bool patchGameImport(const char* dll, const char* function, void* replacement, void** originalOut);
void remove();

// Free address space and video memory, to the log (failed resource creations are
// logged by themselves).
void logMemory(const char* when);

// Once per engine frame (the engine's frame hook, independent of Direct3D): logs ("hook diag:")
// when the game's device left our vtable or our Reset/Present slots, and when 120 frames pass
// without a Present through our detour. Logging only.
void diagFrame();

// Calls made by the plugin itself between these are not traced or intercepted.
void beginInternal();
void endInternal();

struct InternalScope {
    InternalScope() { beginInternal(); }
    ~InternalScope() { endInternal(); }
};

} // namespace hook
} // namespace tmshaders
