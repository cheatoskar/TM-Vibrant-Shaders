#pragma once
#include <d3d9.h>

// Makes the scene depth buffer readable by shaders.
//
// D3D9 cannot sample a regular depth-stencil surface, so the device's automatic
// depth-stencil is shadowed by an INTZ depth texture: whenever the game binds the
// automatic surface, the INTZ surface is bound instead. INTZ cannot be multisampled,
// which is why the plugin turns the game's MSAA off and anti-aliases in post.
namespace tmshaders {
namespace depth {

constexpr D3DFORMAT kINTZ = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));

void onDeviceReady(IDirect3DDevice9* device);  // after CreateDevice / Reset
void onPreReset();

// Substitution hook for SetDepthStencilSurface.
IDirect3DSurface9* substitute(IDirect3DSurface9* surface);

// Replacement for CreateDepthStencilSurface: render-sized depth buffers the game creates
// itself (camera effects, replay/video rendering at export resolution) become INTZ
// textures too. Returns false to let the game create a normal surface.
bool createReadable(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms,
                    IDirect3DSurface9** out, HRESULT* result);

// The readable INTZ texture behind a bound depth-stencil surface, or nullptr. Not AddRef'd.
IDirect3DTexture9* textureFor(IDirect3DSurface9* depthStencil);

// The readable depth texture (raw [0,1] depth in .r), or nullptr if unavailable.
IDirect3DTexture9* texture();
IDirect3DSurface9* surface();
bool supported();

} // namespace depth
} // namespace tmshaders
