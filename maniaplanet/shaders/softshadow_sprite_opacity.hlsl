// Soft sun shadows in place of Tech3/DeferredShadowPssm_p: the variant with StaticSprite and
// PSplitOpacity (t4-t6, s2, s3; face normal at t7). See softshadow.hlsli.
// entries: PS_SoftShadowSpriteOpacity:ps_5_0
#define SOFT_SPRITE 1
#define SOFT_OPACITY 1
#include "softshadow.hlsli"

float4 PS_SoftShadowSpriteOpacity(float4 pos : SV_Position) : SV_Target
{
    return softShadow(pos);
}
