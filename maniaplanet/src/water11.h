#pragma once
// ManiaPlanet's water (Lagoon, Valley, Canyon, the lake around the Stadium), seen from the D3D11
// side only. The engine draws it in two steps:
//   1. Tech3/DeferredWaterFog_p: one draw per water volume, premultiplied over the HDR image (blend
//      one, inverse source alpha), fogs what lies under the surface by the light's path through the
//      water (its ObjectP holds the surface's plane: WorldToWaterDepth). That fog is the murk.
//   2. It copies the image for its refraction and draws the surface (Tech3/Sea_DefWrite_p:
//      refraction, reflection, Fresnel; it writes the depth).
// With WaterClarity or WaterSplash on, the mod draws the fog draws into a buffer of its own (the
// engine's draws with its own shader, only the render target and the blend state are the mod's)
// and puts it into the HDR image itself before the next draw (shaders/water.hlsl): less murk,
// caustics and foam with WaterClarity, exactly the engine's fog without. Nothing in the game's
// memory changes, no engine shader is replaced. The fog buffer and the planes stay for the effects
// of the frame: WaterSplash finds the surface near the car with them (effects_water.cpp).
#include "shaderdb.h"
#include <d3d11.h>

namespace tmvs_mp {
namespace water {

constexpr int kMaxPlanes = 8;

struct Params {
    float clarity = 0.0f; // WaterClarity (0: the engine's own fog)
    bool splash = false;  // WaterSplash wants the fog buffer
    double time = 0.0;    // the effects' clock (caustics, foam)
};

// The engine's water fog shaders (by name, from the GpuCache table).
bool isFogShader(const char* name);

// Before one of the engine's water fog draws into a screen-sized target. True: the draw goes into
// the mod's fog buffer, call endFog() right after it.
bool beginFog(ID3D11Device* device, ID3D11DeviceContext* ctx, const ShaderInfo& ps, const Params& params);
void endFog(ID3D11DeviceContext* ctx);

// The fog buffer is waiting to go into the HDR image (before the next other draw of the engine:
// its copy for the refraction, or the tone map at the latest).
bool pending();
// Puts it in. The G-buffer textures of this frame (any may be null: no caustics, a plainer foam).
void compose(ID3D11DeviceContext* ctx, ID3D11Resource* albedo, ID3D11Resource* normal, ID3D11Resource* lightMask);

// After the tone map (the effects have used the buffer): the next render starts over. At Present:
// this frame's references go.
void endRender();
void endFrame();
void release();

// This frame's water for the effects at the tone map (null / 0: no water seen this frame).
ID3D11ShaderResourceView* fogView();
// The planes' constant buffers (kMaxPlanes, the first planeCount() filled).
ID3D11Buffer* const* planeBuffers();
int planeCount();

// What the last frame's water did, for the menu's status line.
const char* status();

} // namespace water
} // namespace tmvs_mp
