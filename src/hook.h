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
    void (*setRenderTarget)(DWORD index, IDirect3DSurface9* surface) = nullptr;
    void (*draw)(IDirect3DDevice9* device) = nullptr;
    void (*setTransform)(D3DTRANSFORMSTATETYPE type, const D3DMATRIX* matrix) = nullptr;
    void (*setLight)(DWORD index, const D3DLIGHT9* light) = nullptr;
};

bool install(const DeviceCallbacks& callbacks);
void remove();

// Calls made by the plugin itself between these are not traced or intercepted.
void beginInternal();
void endInternal();

struct InternalScope {
    InternalScope() { beginInternal(); }
    ~InternalScope() { endInternal(); }
};

} // namespace hook
} // namespace tmshaders
