// TM Vibrant Shaders for ManiaPlanet: passes on the engine's finished, tone-mapped image (shader model 4).
// Order per frame: PS_Down4 (luma, quarter size) -> PS_BlurH -> PS_BlurV -> PS_Grade (into the game's target).

cbuffer GradeParams : register(b0)
{
    float4 g_p0; // exposure (stops), contrast, saturation, vibrance
    float4 g_p1; // warmth, tint, clarity, sharpen
    float4 g_p2; // vignette, shadows, highlights, LUT mix
    float4 g_p3; // texel size of the source (xy), source is an sRGB view (z), view: 0 normal, 1 split, 2 depth (w)
    float4 g_p4; // split position, has depth, has LUT, LUT size
    float4 g_invViewProj[4]; // the engine's clip -> world (rows)
    float4 g_camera; // eye in world (xyz), has camera (w)
    float4 g_has;    // engine textures present: normals, motion, light buffer
};

cbuffer PassParams : register(b1)
{
    float4 g_texel; // texel size of the pass input (xy)
};

Texture2D g_src : register(t0);
Texture2D g_blur : register(t1);
Texture2D<float> g_depth : register(t2);
Texture3D g_lut : register(t3);
Texture2D g_normal : register(t4); // TMapDeferredPixelNormalInC
Texture2D g_motion : register(t5); // TMapMotion
Texture2D g_light : register(t6);  // TMapDeferredMDiffuse (diffuse light)
SamplerState g_linear : register(s0);
SamplerState g_point : register(s1);

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

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

float3 toGamma(float3 c)
{
    c = max(c, 0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float3 toLinear(float3 c)
{
    c = max(c, 0);
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 readSource(float2 uv)
{
    float3 c = g_src.SampleLevel(g_point, uv, 0).rgb;
    return g_p3.z > 0.5 ? toGamma(c) : c;
}

// Luma of the image at a quarter of its size: 4 bilinear taps cover the 4x4 block.
float4 PS_Down4(VSOut i) : SV_Target
{
    float2 t = g_texel.xy;
    float3 c = g_src.SampleLevel(g_linear, i.uv + t * float2(-1, -1), 0).rgb
             + g_src.SampleLevel(g_linear, i.uv + t * float2( 1, -1), 0).rgb
             + g_src.SampleLevel(g_linear, i.uv + t * float2(-1,  1), 0).rgb
             + g_src.SampleLevel(g_linear, i.uv + t * float2( 1,  1), 0).rgb;
    c *= 0.25;
    if (g_p3.z > 0.5) c = toGamma(c);
    return dot(c, kLuma).xxxx;
}

// Wide Gaussian (17 texels in 9 linear taps) for the local contrast.
float blur(float2 uv, float2 dir)
{
    const float w[5] = { 0.1964825501511404, 0.2969069646728344, 0.09447039785044732, 0.010381362401148057, 0.0007 };
    const float o[5] = { 0.0, 1.411764705882353, 3.2941176470588234, 5.176470588235294, 7.05 };
    float sum = g_src.SampleLevel(g_linear, uv, 0).r * w[0];
    [unroll] for (int k = 1; k < 5; k++)
    {
        float2 d = dir * g_texel.xy * o[k] * 2.0;
        sum += (g_src.SampleLevel(g_linear, uv + d, 0).r + g_src.SampleLevel(g_linear, uv - d, 0).r) * w[k];
    }
    return sum / (w[0] + 2 * (w[1] + w[2] + w[3] + w[4]));
}

float4 PS_BlurH(VSOut i) : SV_Target
{
    return blur(i.uv, float2(1, 0)).xxxx;
}

float4 PS_BlurV(VSOut i) : SV_Target
{
    return blur(i.uv, float2(0, 1)).xxxx;
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

float4 PS_Grade(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float4 raw = g_src.SampleLevel(g_point, uv, 0);
    float3 original = g_p3.z > 0.5 ? toGamma(raw.rgb) : raw.rgb;
    float view = g_p3.w;

    if (view > 1.5)
    {
        // Checks of what the mod reads from the engine. Magenta: not found.
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
            if (g_has.z > 0.5) v = toGamma(g_light.SampleLevel(g_point, uv, 0).rgb);
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
                float line8 = max(minor.x, minor.z), line32 = max(major.x, major.z);
                v = lerp(v, float3(0.2, 0.9, 1.0), line8 * 0.6);
                v = lerp(v, float3(1.0, 0.85, 0.2), line32);
                v = lerp(v, float3(1.0, 0.3, 0.3), minor.y * 0.5); // height every 8 m
            }
        }
        return float4(g_p3.z > 0.5 ? toLinear(v) : v, raw.a);
    }

    float3 c = original;
    if (g_p1.w > 0) c = sharpen(uv, c, g_p1.w);

    // Local contrast against the blurred luma, faded out towards black and white.
    float L = dot(c, kLuma);
    float detail = L - g_blur.SampleLevel(g_linear, uv, 0).r;
    float protect = 1 - pow(abs(L * 2 - 1), 2);
    c += detail * g_p1.z * 1.5 * protect;

    // Exposure and white balance in linear light; white balance keeps the brightness.
    float3 lin = toLinear(saturate(c)) * exp2(g_p0.x);
    float before = dot(lin, kLuma);
    lin *= float3(1 + 0.12 * g_p1.x, 1 - 0.08 * g_p1.y, 1 - 0.12 * g_p1.x);
    lin *= before / max(dot(lin, kLuma), 1e-5);
    c = toGamma(lin);

    // Shadows: lift or deepen the darks without greying black. Highlights: pull the bright mids
    // down (detail back in bright areas) while white stays white.
    c = saturate(c);
    c *= 1 + g_p2.y * 0.6 * pow(1 - c, 4);
    c -= g_p2.z * 0.6 * c * c * (1 - c);

    // S-curve contrast around mid grey (0 and 1 stay put).
    c = saturate(c);
    c = lerp(c, c * c * (3 - 2 * c), g_p0.y);

    // Saturation and vibrance (vibrance leaves already saturated colours alone).
    L = dot(c, kLuma);
    float sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    c = lerp(L.xxx, c, g_p0.z * (1 + g_p0.w * (1 - sat)));
    c = saturate(c);

    if (g_p4.z > 0.5)
    {
        float n = g_p4.w;
        float3 look = g_lut.SampleLevel(g_linear, c * ((n - 1) / n) + 0.5 / n, 0).rgb;
        c = lerp(c, look, g_p2.w);
    }

    // Vignette, round on any aspect ratio.
    float2 v = (uv - 0.5) * float2(g_p3.y / g_p3.x, 1);
    c *= 1 - g_p2.x * smoothstep(0.35, 1.0, length(v));

    if (view > 0.5)
    {
        if (uv.x < g_p4.x) c = original;
        if (abs(uv.x - g_p4.x) < g_p3.x) c = float3(1, 1, 1);
    }
    return float4(g_p3.z > 0.5 ? toLinear(c) : c, raw.a);
}
