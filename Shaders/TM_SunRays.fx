/**
 * TrackMania Vibrant Shaders - TM_SunRays.fx
 * Screen-Space Crepuscular Rays / Volumetric God Rays
 * Inspired by Sildur's Vibrant Volumetric Lighting & IterationT Light Shafts
 * Custom shader by Antigravity
 */

#include "ReShade.fxh"

uniform float2 f2SunPos <
    ui_type = "slider";
    ui_min = -0.5; ui_max = 1.5;
    ui_label = "Sun Screen Position (X, Y)";
    ui_tooltip = "Normalized coordinates of the sun. (0.5, 0.15) is top-center.";
    ui_category = "Sun Source";
> = float2(0.5, 0.15);

uniform float fRayDensity <
    ui_type = "slider";
    ui_min = 0.1; ui_max = 2.0;
    ui_label = "Ray Density / Length";
    ui_category = "Ray Properties";
> = 0.90;

uniform float fRayDecay <
    ui_type = "slider";
    ui_min = 0.85; ui_max = 0.99;
    ui_label = "Ray Falloff Decay";
    ui_category = "Ray Properties";
> = 0.96;

uniform float fRayWeight <
    ui_type = "slider";
    ui_min = 0.05; ui_max = 1.0;
    ui_label = "Ray Intensity Weight";
    ui_category = "Ray Properties";
> = 0.35;

uniform float fRayExposure <
    ui_type = "slider";
    ui_min = 0.1; ui_max = 3.0;
    ui_label = "Overall Ray Exposure";
    ui_category = "Ray Properties";
> = 1.10;

uniform float fLumaThreshold <
    ui_type = "slider";
    ui_min = 0.4; ui_max = 0.98;
    ui_label = "Luminance Threshold";
    ui_tooltip = "Only light sources brighter than this cast sun rays";
    ui_category = "Ray Properties";
> = 0.72;

uniform float3 f3RayColor <
    ui_type = "color";
    ui_label = "Godray Sunlight Color";
    ui_tooltip = "Warm golden sun tint like Sildur's Vibrant Shaders";
    ui_category = "Color";
> = float3(1.0, 0.88, 0.65);

uniform bool bUseDepthOcclusion <
    ui_label = "Enable Depth Occlusion";
    ui_tooltip = "Uses depth buffer to prevent foreground objects from glowing";
    ui_category = "Depth Masking";
> = true;

texture RaySourceTex { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA8; };
sampler RaySourceSampler { Texture = RaySourceTex; };

// Pass 1: Extract bright sky/sunlight pixels
float4 PS_ExtractLight(float4 pos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float3 color = tex2D(ReShade::BackBuffer, texcoord).rgb;
    float depth = ReShade::GetLinearizedDepth(texcoord);

    float luma = dot(color, float3(0.299, 0.587, 0.114));
    
    // Depth masking: sky has depth ~ 1.0
    float depthFactor = 1.0;
    if (bUseDepthOcclusion)
    {
        depthFactor = smoothstep(0.4, 0.95, depth);
    }

    float bright = max(0.0, luma - fLumaThreshold) / (1.0 - fLumaThreshold + 0.001);
    bright = pow(bright, 2.0) * depthFactor;

    return float4(color * bright * f3RayColor, 1.0);
}

// Pass 2: Radial blur / crepuscular ray accumulation
float4 PS_RenderGodRays(float4 pos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float2 deltaTexCoord = (texcoord - f2SunPos);
    const int NUM_SAMPLES = 48;
    deltaTexCoord *= 1.0 / float(NUM_SAMPLES) * fRayDensity;

    float2 coord = texcoord;
    float3 illuminationDecay = 1.0;
    float3 rayColor = 0.0;

    // Jitter dithering to eliminate banding
    float dither = frac(sin(dot(texcoord, float2(12.9898, 78.233))) * 43758.5453) * 0.02;
    coord -= deltaTexCoord * dither;

    [unroll(48)]
    for (int i = 0; i < NUM_SAMPLES; i++)
    {
        coord -= deltaTexCoord;
        float3 sampleColor = tex2Dlod(RaySourceSampler, float4(coord, 0, 0)).rgb;
        sampleColor *= illuminationDecay * fRayWeight;
        rayColor += sampleColor;
        illuminationDecay *= fRayDecay;
    }

    rayColor *= (fRayExposure / float(NUM_SAMPLES) * 10.0);

    float3 baseColor = tex2D(ReShade::BackBuffer, texcoord).rgb;
    // Screen / Additive blend
    float3 finalColor = baseColor + rayColor;

    return float4(finalColor, 1.0);
}

technique TM_SunRays
<
    ui_label = "TM Volumetric Sun Rays (Sildurs / IterationT)";
    ui_tooltip = "Crepuscular god rays radiating from the sun, occluded by stadium structures and geometry.";
>
{
    pass Extract
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_ExtractLight;
        RenderTarget = RaySourceTex;
    }
    pass Blend
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_RenderGodRays;
    }
}
