// The TMNF mod's look (src/config.cpp settings, shaders/tmvs.hlsl PS_Final) for ManiaPlanet:
// exposure, white balance, contrast, lift/gamma/gain, vibrance, saturation, split toning,
// vignette, .cube look and film grain. Shared by the HDR stage (hdr.hlsl, on the engine's HDR
// image before the curve) and the grading pass before the interface (mp.hlsl, on the finished
// image when the HDR stage didn't run). Include after g_linear (s0) is declared.

cbuffer LookParams : register(b3)
{
    float4 g_l0;   // exposure (EV), contrast, saturation, vibrance
    float4 g_l1;   // temperature, tint, split toning, tone knee
    float4 g_l2;   // lift, gamma, gain, vignette
    float4 g_l3;   // film grain, chromatic aberration, sharpen, LUT amount (0 = no LUT)
    float4 g_lSun; // sun colour (rgb, the engine's light), tone curve (0 game, 1 TMVS, 2 ACES)
    float4 g_lSky; // sky colour (rgb), bloom
    float4 g_l6;   // frame (grain seed), LUT size, split position (0 = off), bloom radius
};

Texture3D g_lookLut : register(t7);

static const float3 kLookLuma = float3(0.2126, 0.7152, 0.0722);

// Compare split: the game's own image on this side of the line (g_l6.z: position, 0 = off,
// negative = the game on the right).
bool lookGameSide(float u)
{
    return g_l6.z > 0.0 ? u < g_l6.z : (g_l6.z < 0.0 ? u > -g_l6.z : false);
}

float lookLuma(float3 c)
{
    return dot(c, kLookLuma);
}

float3 lookToGamma(float3 c)
{
    c = max(c, 0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float3 lookToLinear(float3 c)
{
    c = max(c, 0);
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float lookHash(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

// TMNF's grade(): on linear light, before the tone curve.
float3 lookGrade(float3 c)
{
    // White balance (temperature / tint gains).
    c *= float3(1.0 + g_l1.x * 0.18, 1.0 - g_l1.y * 0.1, 1.0 - g_l1.x * 0.18);

    // Contrast around mid grey in log space.
    float3 lc = log2(max(c, 1e-5));
    lc = (lc - log2(0.18)) * g_l0.y + log2(0.18);
    c = exp2(lc);

    // Lift / gamma / gain.
    c = c * g_l2.z + g_l2.x * (1.0 - c);
    c = pow(max(c, 0.0), 1.0 / max(g_l2.y, 0.05));

    // Vibrance (boosts dull colours more) and saturation.
    float l = lookLuma(c);
    float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
    float sat = (mx - mn) / max(mx, 1e-4);
    c = lerp(l.xxx, c, 1.0 + g_l0.w * (1.0 - sat));
    c = lerp(l.xxx, c, g_l0.z);

    // Split toning: shadows toward the sky's colour, highlights toward the sun's.
    float3 coolTint = g_lSky.rgb / max(lookLuma(g_lSky.rgb), 1e-3);
    float3 warmTint = g_lSun.rgb / max(lookLuma(g_lSun.rgb), 1e-3);
    float w = smoothstep(0.0, 0.6, l);
    c *= lerp(float3(1, 1, 1), lerp(coolTint, warmTint, w), g_l1.z * 0.25);
    return max(c, 0.0);
}

// After the curve: vignette (on linear light), then the .cube look and grain on display values.
// Returns linear light again.
float3 lookFinish(float3 c, float2 uv, float2 pixel)
{
    float2 d = uv - 0.5;
    c *= saturate(1.0 - dot(d, d) * g_l2.w * 1.6);
    float3 srgb = lookToGamma(saturate(c));
    if (g_l3.w > 0.0)
    {
        float n = g_l6.y;
        float3 look = g_lookLut.SampleLevel(g_linear, saturate(srgb) * ((n - 1) / n) + 0.5 / n, 0).rgb;
        srgb = lerp(srgb, look, g_l3.w);
    }
    float grain = lookHash(pixel + frac(g_l6.x) * 1000.0) - 0.5;
    float l = lookLuma(srgb);
    srgb += grain * (g_l3.x * smoothstep(0.0, 0.45, l) * (1.2 - l) + 1.0 / 255.0);
    return lookToLinear(saturate(srgb));
}
