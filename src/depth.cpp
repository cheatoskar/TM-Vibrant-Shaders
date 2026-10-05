#include "depth.h"
#include "gfx.h"
#include "hook.h"
#include "log.h"

namespace tmshaders {
namespace depth {
namespace {

IDirect3DTexture9* g_texture = nullptr;
IDirect3DSurface9* g_surface = nullptr;
IDirect3DSurface9* g_auto = nullptr; // not ref-counted: identity only (Reset requires no extra refs)
bool g_supported = false;

bool checkSupport(IDirect3DDevice9* device) {
    IDirect3D9* d3d = nullptr;
    if (FAILED(device->GetDirect3D(&d3d)) || !d3d) return false;
    D3DDEVICE_CREATION_PARAMETERS creation{};
    device->GetCreationParameters(&creation);
    D3DDISPLAYMODE mode{};
    d3d->GetAdapterDisplayMode(creation.AdapterOrdinal, &mode);
    HRESULT hr = d3d->CheckDeviceFormat(creation.AdapterOrdinal, creation.DeviceType, mode.Format, D3DUSAGE_DEPTHSTENCIL,
                                        D3DRTYPE_TEXTURE, kINTZ);
    d3d->Release();
    return SUCCEEDED(hr);
}

} // namespace

void onDeviceReady(IDirect3DDevice9* device) {
    onPreReset();
    g_supported = checkSupport(device);
    if (!g_supported) {
        TMVS_LOG("depth: INTZ depth textures are not supported by this driver, depth effects disabled");
        return;
    }

    IDirect3DSurface9* current = nullptr;
    if (FAILED(device->GetDepthStencilSurface(&current)) || !current) {
        TMVS_LOG("depth: device has no automatic depth-stencil surface");
        return;
    }
    D3DSURFACE_DESC desc{};
    current->GetDesc(&desc);
    g_auto = current;
    current->Release();

    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        TMVS_LOG("depth: depth buffer is multisampled (%d), cannot shadow it", static_cast<int>(desc.MultiSampleType));
        g_auto = nullptr;
        return;
    }

    hook::InternalScope internal;
    if (FAILED(device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_DEPTHSTENCIL, kINTZ, D3DPOOL_DEFAULT, &g_texture, nullptr)) ||
        !g_texture) {
        TMVS_LOG("depth: failed to create %ux%u INTZ texture", desc.Width, desc.Height);
        g_texture = nullptr;
        g_auto = nullptr;
        return;
    }
    g_texture->GetSurfaceLevel(0, &g_surface);
    device->SetDepthStencilSurface(g_surface);
    TMVS_LOG("depth: INTZ %ux%u shadows automatic depth-stencil %p", desc.Width, desc.Height, static_cast<void*>(g_auto));
}

void onPreReset() {
    gfx::release(g_surface);
    gfx::release(g_texture);
    g_auto = nullptr;
}

IDirect3DSurface9* substitute(IDirect3DSurface9* surface) {
    if (surface && surface == g_auto && g_surface) return g_surface;
    return surface;
}

bool createReadable(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms,
                    IDirect3DSurface9** out, HRESULT* result) {
    if (!g_supported || !out || ms != D3DMULTISAMPLE_NONE) return false;
    if (format != D3DFMT_D24S8 && format != D3DFMT_D24X8 && format != D3DFMT_D16 && format != D3DFMT_D32 && format != kINTZ)
        return false;
    if (width < 320 || height < 200) return false; // shadow maps, reflections, probes
    hook::InternalScope internal;
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(device->CreateTexture(width, height, 1, D3DUSAGE_DEPTHSTENCIL, kINTZ, D3DPOOL_DEFAULT, &texture, nullptr)) || !texture)
        return false;
    // The surface reference keeps its texture alive; the game owns and releases it.
    *result = texture->GetSurfaceLevel(0, out);
    texture->Release();
    TMVS_LOG("depth: game depth buffer %ux%u created as INTZ %p", width, height, static_cast<void*>(*out));
    return SUCCEEDED(*result);
}

IDirect3DTexture9* textureFor(IDirect3DSurface9* depthStencil) {
    if (!depthStencil) return nullptr;
    if (depthStencil == g_surface) return g_texture;
    D3DSURFACE_DESC desc{};
    depthStencil->GetDesc(&desc);
    if (desc.Format != kINTZ) return nullptr;
    IDirect3DTexture9* container = nullptr;
    if (FAILED(depthStencil->GetContainer(IID_IDirect3DTexture9, reinterpret_cast<void**>(&container))) || !container)
        return nullptr;
    container->Release(); // the bound surface keeps it alive for the frame
    return container;
}

IDirect3DTexture9* texture() {
    return g_texture;
}

IDirect3DSurface9* surface() {
    return g_surface;
}

bool supported() {
    return g_supported;
}

} // namespace depth
} // namespace tmshaders
