// TM Vibrant Shaders for ManiaPlanet: the pass on the finished image, right before the interface
// (shader model 4). After the HDR stage (hdr.hlsl) it adds chromatic aberration and sharpening;
// when the HDR stage didn't run this frame, it applies the whole look here. It also draws the
// check views of what the mod reads from the engine.
// entries: VS_Full:vs_4_0 PS_Grade:ps_4_0

cbuffer GradeParams : register(b0)
{
    float4 g_p3; // texel size of the source (xy), source is an sRGB view (z), view (w)
    float4 g_p4; // split position (0 = off), has depth, the HDR stage ran (z), 0
    float4 g_invViewProj[4]; // the engine's clip -> world (rows)
    float4 g_camera; // eye in world (xyz), has camera (w)
    float4 g_has;    // engine textures present: normals, motion, albedo
};

Texture2D g_src : register(t0);
Texture2D<float> g_depth : register(t2);
Texture2D g_normal : register(t4); // TMapDeferredPixelNormalInC
Texture2D g_motion : register(t5); // TMapMotion
Texture2D g_albedo : register(t6); // TMapDeferredMDiffuse (material colour)
SamplerState g_linear : register(s0);
SamplerState g_point : register(s1);

#include "look.hlsli"

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// One triangle over the whole target, no vertex buffer.
VSOut VS_Full(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 readSource(float2 uv)
{
    float3 c = g_src.SampleLevel(g_point, uv, 0).rgb;
    return g_p3.z > 0.5 ? lookToGamma(c) : c;
}

// Contrast-adaptive sharpening (after AMD's CAS): stronger where the neighbourhood is flat,
// none where it already has hard contrast, so edges don't ring.
float3 sharpen(float2 uv, float3 c, float amount)
{
    float2 t = g_p3.xy;
    float3 n = readSource(uv + float2(0, -t.y));
    float3 s = readSource(uv + float2(0, t.y));
    float3 e = readSource(uv + float2(t.x, 0));
    float3 w = readSource(uv + float2(-t.x, 0));
    float3 mn = min(c, min(min(n, s), min(e, w)));
    float3 mx = max(c, max(max(n, s), max(e, w)));
    float3 amp = sqrt(saturate(min(mn, 1 - mx) / max(mx, 1e-4)));
    float3 wgt = -amp / lerp(8.0, 5.0, saturate(amount));
    return saturate((c + (n + s + e + w) * wgt) / (1 + 4 * wgt));
}

float3 checkView(float2 uv, float3 original, float view)
{
    // Magenta: not found.
    float3 v = float3(0.5, 0, 0.5);
    float d = g_p4.y > 0.5 ? g_depth.SampleLevel(g_point, uv, 0) : 0;
    if (view < 2.5)
    {
        // Depth (reversed Z: near = 1, far = 0), near bright.
        if (g_p4.y > 0.5) v = pow(saturate(d), 0.25).xxx;
    }
    else if (view < 3.5)
    {
        if (g_has.x > 0.5) v = g_normal.SampleLevel(g_point, uv, 0).rgb;
    }
    else if (view < 4.5)
    {
        if (g_has.y > 0.5) v = saturate(g_motion.SampleLevel(g_point, uv, 0).rgb * float3(20, 20, 0) + float3(0.5, 0.5, 0.5));
    }
    else if (view < 5.5)
    {
        if (g_has.z > 0.5) v = lookToGamma(g_albedo.SampleLevel(g_point, uv, 0).rgb);
    }
    else if (g_camera.w > 0.5 && g_p4.y > 0.5)
    {
        // World grid: lines every 8 m (bright every 32 m, a block), from depth and the engine's
        // camera. Correct when the lines stay on the ground while the camera moves.
        v = original * 0.6;
        if (d > 0)
        {
            float4 clip = float4(uv.x * 2 - 1, 1 - uv.y * 2, d, 1);
            float4 w = float4(dot(g_invViewProj[0], clip), dot(g_invViewProj[1], clip), dot(g_invViewProj[2], clip),
                              dot(g_invViewProj[3], clip));
            float3 p = w.xyz / w.w;
            float3 f = fwidth(p) * 1.5 + 1e-4;
            float3 minor = 1 - smoothstep(0, f, abs(frac(p / 8 + 0.5) - 0.5) * 8);
            float3 major = 1 - smoothstep(0, f * 1.5, abs(frac(p / 32 + 0.5) - 0.5) * 32);
            v = lerp(v, float3(0.2, 0.9, 1.0), max(minor.x, minor.z) * 0.6);
            v = lerp(v, float3(1.0, 0.85, 0.2), max(major.x, major.z));
            v = lerp(v, float3(1.0, 0.3, 0.3), minor.y * 0.5); // height every 8 m
        }
    }
    return v;
}

float4 PS_Grade(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float4 raw = g_src.SampleLevel(g_point, uv, 0);
    float3 original = g_p3.z > 0.5 ? lookToGamma(raw.rgb) : raw.rgb;
    float view = g_p3.w;
    float3 c;
    if (view > 1.5)
    {
        c = checkView(uv, original, view);
    }
    else if (lookGameSide(uv.x))
    {
        // Compare (split): the game's image (the HDR stage drew its left side the game's way).
        c = original;
    }
    else
    {
        c = original;
        // Chromatic aberration toward the screen edges.
        if (g_l3.y > 0.0)
        {
            float2 d = uv - 0.5;
            float2 ca = d * dot(d, d) * g_l3.y * 0.03;
            c.r = readSource(uv - ca).r;
            c.b = readSource(uv + ca).b;
        }
        if (g_l3.z > 0) c = sharpen(uv, c, g_l3.z);
        if (g_p4.z < 0.5)
        {
            // No HDR stage this frame: the whole look on the finished image.
            float3 lin = lookToLinear(c) * exp2(g_l0.x);
            c = lookToGamma(lookFinish(lookGrade(lin), uv, i.pos.xy));
        }
    }
    if (g_p4.x != 0 && abs(uv.x - abs(g_p4.x)) < g_p3.x) c = float3(1, 1, 1);
    return float4(g_p3.z > 0.5 ? lookToLinear(c) : c, raw.a);
}
