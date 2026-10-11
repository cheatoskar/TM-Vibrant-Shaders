// TM Vibrant Shaders for ManiaPlanet: the HDR stage, in place of the engine's tone map (shader model 4).
// Runs at the engine's ToneMapGlobalFilmCurve draw with the same inputs:
//   t0 = the engine's HDR image (scene + its bloom), t1 = its 2x2 exposure texture,
//   b2 = its tone-map constants (bound by the engine for its own draw: we read them on the GPU).
// Per frame: PS_BloomDown x N (first level exposes and thresholds) -> PS_BloomUp x N-1 -> PS_ToneMap.

cbuffer ToneParams : register(b0)
{
    float4 g_tone0; // curve (0 engine, 1 TMVS, 2 ACES), knee, hue keep, exposure (stops)
    float4 g_tone1; // bloom strength, has bloom, 0, 0
};

cbuffer PassParams : register(b1)
{
    float4 g_texel; // texel of the pass input (xy), first bloom level (z), up-sample radius (w)
};

// The engine's own constants of its tone map (ToneMapGlobalFilmCurve, cbuffer ShaderP).
cbuffer EngineTone : register(b2)
{
    float4 g_eA;  // A, B, B*C, output scale (1 / curve(white))
    float4 g_eD;  // D*E, D*F, E/F, grey scale (desaturation)
    float4 g_eUV; // where in the 2x2 exposure texture this frame's exposure is (xy)
};

Texture2D g_src : register(t0);
Texture2D g_exposure : register(t1);
Texture2D g_bloom : register(t2);
SamplerState g_linear : register(s0);
SamplerState g_point : register(s1);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

float engineExposure()
{
    return g_exposure.SampleLevel(g_point, g_eUV.xy, 0).z;
}

// ---- bloom (ported from the TMNF mod: Karis average against fireflies, soft threshold) ----

float3 karis(float3 a, float3 b, float3 c, float3 d)
{
    float wa = 1.0 / (1.0 + dot(a, kLuma)), wb = 1.0 / (1.0 + dot(b, kLuma));
    float wc = 1.0 / (1.0 + dot(c, kLuma)), wd = 1.0 / (1.0 + dot(d, kLuma));
    return (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
}

float3 fetch(float2 uv)
{
    return min(g_src.SampleLevel(g_linear, uv, 0).rgb, 50000.0);
}

float4 PS_BloomDown(VSOut i) : SV_Target
{
    float2 uv = i.uv, t = g_texel.xy;
    float3 a = fetch(uv + t * float2(-2, -2)), b = fetch(uv + t * float2(0, -2)), c = fetch(uv + t * float2(2, -2));
    float3 d = fetch(uv + t * float2(-1, -1)), e = fetch(uv + t * float2(1, -1));
    float3 f = fetch(uv + t * float2(-2, 0)), g = fetch(uv), h = fetch(uv + t * float2(2, 0));
    float3 ii = fetch(uv + t * float2(-1, 1)), j = fetch(uv + t * float2(1, 1));
    float3 k = fetch(uv + t * float2(-2, 2)), l = fetch(uv + t * float2(0, 2)), m = fetch(uv + t * float2(2, 2));
    float3 result;
    if (g_texel.z > 0.5)
    {
        float exposure = engineExposure() * exp2(g_tone0.w);
        result = (karis(d, e, ii, j) * 0.5 + karis(a, b, f, g) * 0.125 + karis(b, c, g, h) * 0.125 +
                  karis(f, g, k, l) * 0.125 + karis(g, h, l, m) * 0.125) * exposure;
        // Soft threshold: mostly the bright part, a little of everything.
        float br = max(result.r, max(result.g, result.b));
        float soft = clamp(br - 0.6, 0.0, 0.8);
        soft = soft * soft / (4.0 * 0.4 + 1e-4);
        float contrib = max(soft, br - 1.0) / max(br, 1e-4);
        result *= lerp(0.08, 1.0, saturate(contrib));
    }
    else
    {
        result = (d + e + ii + j) * 0.125 + (a + c + k + m) * 0.03125 + (b + f + h + l) * 0.0625 + g * 0.125;
    }
    return float4(result, 1);
}

// t0 = the smaller level, t2 = this level.
float4 PS_BloomUp(VSOut i) : SV_Target
{
    float2 uv = i.uv, t = g_texel.xy;
    float3 s = g_src.SampleLevel(g_linear, uv + t * float2(-1, -1), 0).rgb + g_src.SampleLevel(g_linear, uv + t * float2(1, -1), 0).rgb +
               g_src.SampleLevel(g_linear, uv + t * float2(-1, 1), 0).rgb + g_src.SampleLevel(g_linear, uv + t * float2(1, 1), 0).rgb;
    s += (g_src.SampleLevel(g_linear, uv + t * float2(0, -1), 0).rgb + g_src.SampleLevel(g_linear, uv + t * float2(-1, 0), 0).rgb +
          g_src.SampleLevel(g_linear, uv + t * float2(1, 0), 0).rgb + g_src.SampleLevel(g_linear, uv + t * float2(0, 1), 0).rgb) * 2.0;
    s += g_src.SampleLevel(g_linear, uv, 0).rgb * 4.0;
    s /= 16.0;
    return float4(g_bloom.SampleLevel(g_linear, uv, 0).rgb + s * g_texel.w, 1);
}

// ---- curves ----

// The engine's: Hable's filmic curve per channel, then its grey mix (exactly its shader).
float3 engineCurve(float3 x)
{
    float3 f = (x * (g_eA.x * x + g_eA.z) + g_eD.x) / (x * (g_eA.x * x + g_eA.y) + g_eD.y) - g_eD.z;
    return f * g_eA.w + dot(f, float3(1, 1, 1)) * g_eD.w;
}

// TMVS (from the TMNF mod): linear below the knee, a long exponential shoulder above; half per
// channel, half on the brightest channel so lit colours keep their hue instead of bleaching.
float3 shoulder(float3 x, float knee)
{
    float3 over = max(x - knee, 0.0);
    float3 s = knee + (1.0 - knee) * (1.0 - exp(-over / ((1.0 - knee) * 1.7)));
    return x < knee ? x : s;
}

float3 tmvsCurve(float3 x)
{
    float knee = clamp(g_tone0.y, 0.2, 0.95);
    float peak = max(x.r, max(x.g, x.b));
    float3 hue = x * (shoulder(peak.xxx, knee).x / max(peak, 1e-5));
    return lerp(shoulder(x, knee), hue, saturate(g_tone0.z));
}

// ACES (Narkowicz's fit of the RRT+ODT).
float3 acesCurve(float3 x)
{
    return saturate(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14));
}

float3 curve(float3 x, int mode)
{
    if (mode == 1) return tmvsCurve(x);
    if (mode == 2) return acesCurve(x);
    return engineCurve(x);
}

// Our curves are fed so mid grey comes out as bright as with the engine's curve: the game's
// overall brightness stays, only highlights and colour change.
float matchScale(int mode)
{
    const float m = 0.18;
    float target = dot(engineCurve(m.xxx), kLuma);
    float lo = 0.01, hi = 100.0;
    [loop] for (int k = 0; k < 24; k++)
    {
        float mid = sqrt(lo * hi);
        if (dot(curve((m * mid).xxx, mode), kLuma) < target) lo = mid; else hi = mid;
    }
    return sqrt(lo * hi);
}

float4 PS_ToneMap(VSOut i) : SV_Target
{
    float4 hdr = g_src.SampleLevel(g_point, i.uv, 0);
    float3 x = min(hdr.rgb, 50000.0) * engineExposure() * exp2(g_tone0.w);
    if (g_tone1.y > 0.5) x = lerp(x, g_bloom.SampleLevel(g_linear, i.uv, 0).rgb, g_tone1.x);
    int mode = (int)(g_tone0.x + 0.5);
    if (mode != 0) x *= matchScale(mode);
    return float4(curve(x, mode), hdr.a);
}
