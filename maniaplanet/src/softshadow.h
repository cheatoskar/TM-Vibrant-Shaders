#pragma once
#include <d3d11.h>
#include <cstddef>

namespace tmvs_mp {
namespace softshadow {

// Soft sun shadows (PCSS, shaders/softshadow.hlsli): at the engine's sun shadow pass
// (Tech3/DeferredShadowPssm_p) the mod's pixel shader draws in place of the engine's, with
// everything else the engine bound left as it is, and the engine's shader is bound again right
// after. Off unless SoftShadows (maniaplanet.ini) is above 0. Nothing in the game's memory changes.
//
// Only the engine's variants that pick their split by its rectangle are replaced (1 to 5 splits,
// with or without the opacity and sprite maps; the shader model 5 and 4 builds): they are known
// by their DXBC checksum. Any other variant, or a binding that doesn't look like that variant's
// (constant buffer size, textures, samplers), keeps the engine's shader. Each case is logged once.

// What the engine's shader is: its split count and extra maps (bit 0 opacity, bit 1 sprite).
struct Variant {
    bool known = false;
    int splits = 0;
    int extras = 0;
    int normalSlot() const { return 4 + ((extras & 1) ? 1 : 0) + ((extras & 2) ? 2 : 0); }
};

// The replacement's settings. lightSize: the light's angular diameter in degrees (0: the engine's
// shadows); reach: the farthest blocker that still widens the penumbra (m). mode 1 (the test
// tool): the engine's own result from the replacement (an exact port), 2: debug channels.
struct Params {
    float lightSize = 0.0f;
    float reach = 40.0f;
    int mode = 0;
};

bool init(ID3D11Device* device); // the replacement shaders, its sampler and constants
void release();

// The engine created a pixel shader named Tech3/DeferredShadowPssm_p: remembers its variant.
// (The game's loading threads call this.)
void noteEngineShader(ID3D11PixelShader* shader, const void* code, size_t size);
Variant variantOf(ID3D11PixelShader* shader);
// The variant for a DXBC blob (by its checksum).
Variant identify(const void* code, size_t size);

// Before the engine's shadow draw: binds the replacement when everything matches. True: call
// end() right after the draw (it binds the engine's shader again and what was at b5 and s5).
bool begin(ID3D11DeviceContext* ctx, const Params& params);
void end(ID3D11DeviceContext* ctx);

// What the last shadow pass got, for the menu.
const char* status();

} // namespace softshadow
} // namespace tmvs_mp
