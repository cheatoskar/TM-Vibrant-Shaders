// Soft sun shadows in place of Tech3/DeferredShadowPssm_p: the variant with PSplitOpacity (t4, s2;
// face normal at t5). See softshadow.hlsli.
// entries: PS_SoftShadowOpacity:ps_5_0
#define SOFT_SPRITE 0
#define SOFT_OPACITY 1
#include "softshadow.hlsli"

float4 PS_SoftShadowOpacity(float4 pos : SV_Position) : SV_Target
{
    return softShadow(pos);
}
