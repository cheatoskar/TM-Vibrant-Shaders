#pragma once
#include "shaderdb.h"
#include <d3d11.h>

namespace tmvs_mp {
namespace lights {

// EngineLightDraws (maniaplanet.ini): the light sources are what the engine draws as light, as in
// the TMNF mod's EngineLights, not guessed from the colours of its image.
//
// ManiaPlanet has no lamp passes of its own at night (captures 019-026): its lamps' light on the
// surroundings is baked (light maps). What glows is drawn by the materials themselves, and those
// say so by the textures they read:
//   - self-illuminated materials (TMapSelfIllum: lamp heads, the blocks' light strips, the car's
//     lights in CarDetails_DefRead; TMapAdvert: the screens; TMapIllum),
//   - additive draws (Tech3_Block_TAdd: the halos and flares, blended one + one).
// Each of those draws is drawn a second time into a float target of the same size as the HDR image
// (same shader, same geometry, the same depth test but one that lets the draw's own depth through
// and writes no depth or stencil), so the target holds what they added:
//   - a self-illuminated draw twice: once as it is, then subtracted again with its light texture
//     black: what is left is its own light, in its own blending (opaque, alpha, additive);
//   - an additive draw once, added;
//   - every later draw that covers the image (opaque with a depth write, or blended over it) takes
//     its share away again: as its source were black.
// So the target holds the part of each pixel of the HDR image that the engine's light sources
// drew, in the same units (before the exposure).
enum class Kind : uint8_t { None, Light, Added, Cover };

struct Draw {
    Kind kind = Kind::None;
    bool blend = false; // the engine's draw blends into render target 0
    D3D11_BLEND src = D3D11_BLEND_ONE, dest = D3D11_BLEND_ZERO;
    BOOL alphaToCoverage = FALSE;
    uint8_t lightSlot = kNoSlot;
};

// What a scene draw on the main context means for the light buffer, before the engine draws it.
// The first draw of a frame that reads the light mask into a float target of the screen's size is
// the lighting pass: its target is the HDR image, the buffer is cleared there.
Draw classify(ID3D11Device* device, ID3D11DeviceContext* ctx, const ShaderInfo& ps, ID3D11RenderTargetView* rtv,
              ID3D11Resource* rt, bool screenSized);

// The engine's state around the redraws.
struct Saved {
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    UINT rtvCount = 0;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT factor[4] = {};
    UINT sampleMask = 0xffffffff;
    ID3D11DepthStencilState* depth = nullptr; // the engine's, put back after the redraws
    UINT stencilRef = 0;
    ID3D11ShaderResourceView* light = nullptr; // the material's light texture, while it is black
    UINT lightSlot = 0;
    bool lightSwapped = false;
};
void save(ID3D11DeviceContext* ctx, Saved& s);
// Sets up redraw number `pass` of the engine's draw that was just made; false: no more.
bool setup(ID3D11DeviceContext* ctx, const Draw& d, int pass, Saved& s);
void restore(ID3D11DeviceContext* ctx, Saved& s);

// This frame's buffer if it took the HDR image the tone map reads, else null.
ID3D11Resource* frameTexture(ID3D11Resource* hdr);
const char* kindName(Kind k);
void endFrame();
void release();

} // namespace lights
} // namespace tmvs_mp
