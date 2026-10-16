// Soft sun shadows in place of Tech3/DeferredShadowPssm_p: the variant with StaticSprite (t4, t5,
// s2; face normal at t6). See softshadow.hlsli.
// entries: PS_SoftShadowSprite:ps_5_0
#define SOFT_SPRITE 1
#define SOFT_OPACITY 0
#include "softshadow.hlsli"

float4 PS_SoftShadowSprite(float4 pos : SV_Position) : SV_Target
{
    return softShadow(pos);
}
