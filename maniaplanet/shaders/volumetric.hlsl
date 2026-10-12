// TM Vibrant Shaders for ManiaPlanet: volumetric light (shader model 5). The TMNF mod marched the
// view rays through a height map it built from the depth buffer; here they go through the engine's
// own sun shadow maps, with the constants of its shadow pass (Tech3/DeferredShadowPssm_p): sunlit
// haze with the shadows of the stands, bridges and loops in it, also with the sun behind you.
//   t0 = that pass's linear depth, t1/t2 = its two shadow maps, b4 = a copy of its constants,
//   s4 = its comparison sampler. u_Pass0.x = steps, u_Pass0.y = frame; u_Pass1.xy = SceneP
//   RasterPos_SxyTwz.xy (pixel position -> its raster coordinates).
//   out.r = lit fraction of the haze along the ray, out.g = lit path length / range
// entries: PS_Volumetric:ps_5_0
#include "common.hlsli"

cbuffer EngineShadow : register(b4)
{
    float4 g_sh[12];
};
SamplerComparisonState g_shadowCmp : register(s4);

float engineShadowAt(float3 p)
{
    float4 q = float4(p, 1.0);
    float3 a = float3(dot(q, g_sh[5]), dot(q, g_sh[6]), dot(q, g_sh[7]));
    float3 b = float3(dot(q, g_sh[8]), dot(q, g_sh[9]), dot(q, g_sh[10]));
    float litA = any(a.xy < 0.0) || any(a.xy > 1.0) ? 1.0 : g_in1.SampleCmpLevelZero(g_shadowCmp, a.xy, a.z);
    float litB = any(b.xy < 0.0) || any(b.xy > 1.0) ? 1.0 : g_in2.SampleCmpLevelZero(g_shadowCmp, b.xy, b.z);
    return lerp(litA, litB, g_sh[3].x);
}

float4 PS_Volumetric(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    // The full-res pixel this half-res one stands for (the shadow pass's SV_Position).
    float2 px = floor(uv * u_Screen.xy) + 0.5;
    float2 rp = px * u_Pass1.xy;
    float3 rayBase = g_sh[0].xyz + rp.x * g_sh[1].xyz + rp.y * g_sh[2].xyz;
    float zOurs = engineZ(uv);
    float3 rdView = normalize(viewPosition(uv, 1.0));
    float dist = zOurs >= SKY_Z ? 1e6 : zOurs / max(rdView.z, 1e-3);
    float range = min(dist, u_Volume.y);
    // Its depth parameter is linear along the ray like ours; the scale between them from this
    // pixel's depth (sky: none to compare with, the same units assumed).
    float scale = 1.0;
    if (zOurs < SKY_Z)
    {
        float zTheirs = g_sh[0].w * g_in0.Load(int3(px, 0)).r + g_sh[1].w;
        scale = zTheirs / max(zOurs, 1e-3);
    }
    float3 rd = viewToWorldDir(rdView);
    float3 cam = cameraWorld();
    float steps = u_Pass0.x;
    float noise = ign(i.pos.xy);
    float falloff = max(u_Atmo.y * 0.012, 1e-4);
    float lit = 0.0, total = 0.0, prev = 0.0;
    [loop] for (int k = 0; k < (int)steps; k++)
    {
        // More samples close to the camera, where shafts are sharp and large.
        float f = (k + noise) / steps;
        float t = f * f * range;
        float dt = t - prev;
        prev = t;
        float l = engineShadowAt(rayBase * (t * rdView.z * scale));
        float w = dt * exp(-max(rd.y * t, 0.0) * falloff); // thinner haze higher up
        lit += l * w;
        total += w;
    }
    // The rest of the ray up to `range` counts as lit.
    float rest = max(range - prev, 0.0);
    lit += rest;
    total += rest;
    return float4(total > 0.0 ? lit / total : 1.0, lit / u_Volume.y, 0, 1);
}
