// Soft sun shadows in place of Tech3/DeferredShadowPssm_p: the variant without extra maps (face
// normal at t4). See softshadow.hlsli.
// entries: PS_SoftShadow:ps_5_0
#define SOFT_SPRITE 0
#define SOFT_OPACITY 0
#include "softshadow.hlsli"

float4 PS_SoftShadow(float4 pos : SV_Position) : SV_Target
{
    return softShadow(pos);
}
