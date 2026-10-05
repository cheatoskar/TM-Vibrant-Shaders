/**
 * TrackMania Vibrant Shaders - TM_VibrantColor.fx
 * Inspired by Minecraft Sildur's Vibrant & BSL Tone Mapping
 * Custom shader by Antigravity
 */

#include "ReShade.fxh"

uniform float fExposure <
    ui_type = "slider";
    ui_min = 0.5; ui_max = 2.5;
    ui_label = "Exposure";
    ui_category = "Tone & Exposure";
> = 1.08;

uniform float fContrast <
    ui_type = "slider";
    ui_min = 0.5; ui_max = 2.0;
    ui_label = "Contrast";
    ui_category = "Tone & Exposure";
> = 1.15;

uniform float fColorTemp <
    ui_type = "slider";
    ui_min = -0.5; ui_max = 0.5;
    ui_label = "Color Temperature (Warm/Cool)";
    ui_tooltip = "Warm golden tint like Sildur's Vibrant / BSL sunset";
    ui_category = "Color Grading";
> = 0.08;

uniform float fVibrance <
    ui_type = "slider";
    ui_min = -1.0; ui_max = 1.0;
    ui_label = "Vibrance (Smart Saturation)";
    ui_tooltip = "Boosts under-saturated colors (grass, track elements) without over-saturating skin/whites";
    ui_category = "Color Grading";
> = 0.35;

uniform float fSkyVibrance <
    ui_type = "slider";
    ui_min = 0.0; ui_max = 1.5;
    ui_label = "Sky Blue Boost";
    ui_tooltip = "Selectively saturates sky blues like Sildur's vibrant skies";
    ui_category = "Color Grading";
> = 0.25;

uniform float fFoliageBoost <
    ui_type = "slider";
    ui_min = 0.0; ui_max = 1.5;
    ui_label = "Foliage / Grass Boost";
    ui_tooltip = "Enhances trackside grass and green scenery";
    ui_category = "Color Grading";
> = 0.20;

uniform int iTonemapMode <
    ui_type = "combo";
    ui_items = "None\0ACES Filmic (Cinematic)\0Uncharted 2 Filmic\0Reinhard Extended\0";
    ui_label = "Tonemapping Operator";
    ui_category = "Filmic Curve";
> = 1;

uniform float3 f3SunTint <
    ui_type = "color";
    ui_label = "Sunlight Tint (Highlights)";
    ui_category = "Highlights & Shadows";
> = float3(1.03, 1.01, 0.94);

uniform float3 f3ShadowTint <
    ui_type = "color";
    ui_label = "Shadow Tint";
    ui_category = "Highlights & Shadows";
> = float3(0.95, 0.97, 1.03);

// ACES Filmic Tone Mapping Curve (Krzysztof Narkowicz fitting)
float3 ACESFilm(float3 x)
{
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Uncharted 2 Filmic Curve
float3 Uncharted2Tonemap(float3 x)
{
    float A = 0.15;
    float B = 0.50;
    float C = 0.10;
    float D = 0.20;
    float E = 0.02;
    float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

// RGB to HSV conversion
float3 RGBtoHSV(float3 c)
{
    float4 K = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    float4 p = lerp(float4(c.bg, K.wz), float4(c.gb, K.xy), step(c.b, c.g));
    float4 q = lerp(float4(p.xyw, c.r), float4(c.r, p.yzx), step(p.x, c.r));

    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return float3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

// HSV to RGB conversion
float3 HSVtoRGB(float3 c)
{
    float4 K = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * lerp(K.xxx, saturate(p - K.xxx), c.y);
}

float4 PS_TM_VibrantColor(float4 pos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float3 color = tex2D(ReShade::BackBuffer, texcoord).rgb;

    // 1. Exposure adjustment
    color *= fExposure;

    // 2. Color Temperature (Warm/Cool shift)
    float3 tempShift = float3(1.0 + fColorTemp * 0.4, 1.0 + fColorTemp * 0.1, 1.0 - fColorTemp * 0.4);
    color *= tempShift;

    // 3. Highlight / Shadow Split Toning
    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    float3 splitTone = lerp(f3ShadowTint, f3SunTint, smoothstep(0.2, 0.8, luma));
    color *= splitTone;

    // 4. Selective Foliage & Sky Vibrance (BSL & Sildur's look)
    float3 hsv = RGBtoHSV(color);
    
    // Sky blue boost (Hue ~ 0.55 - 0.70)
    float skyMask = smoothstep(0.50, 0.60, hsv.x) * (1.0 - smoothstep(0.68, 0.75, hsv.x));
    hsv.y += skyMask * fSkyVibrance;

    // Foliage boost (Hue ~ 0.20 - 0.45)
    float foliageMask = smoothstep(0.18, 0.26, hsv.x) * (1.0 - smoothstep(0.42, 0.50, hsv.x));
    hsv.y += foliageMask * fFoliageBoost;

    // Smart Vibrance (affects less saturated colors more)
    float maxCol = max(color.r, max(color.g, color.b));
    float minCol = min(color.r, min(color.g, color.b));
    float sat = maxCol - minCol;
    hsv.y += (1.0 - sat) * fVibrance * 0.5;

    color = HSVtoRGB(saturate(hsv));

    // 5. Tonemapping
    if (iTonemapMode == 1)
    {
        color = ACESFilm(color);
    }
    else if (iTonemapMode == 2)
    {
        float3 curr = Uncharted2Tonemap(color * 2.0);
        float3 whiteScale = 1.0 / Uncharted2Tonemap(float3(11.2, 11.2, 11.2));
        color = saturate(curr * whiteScale);
    }
    else if (iTonemapMode == 3)
    {
        color = color / (color + 1.0);
    }

    // 6. S-Curve Contrast
    color = saturate(color);
    color = pow(color, float3(fContrast, fContrast, fContrast));
    // S-curve smoothstep for crisp filmic pop
    color = color * color * (3.0 - 2.0 * color);

    return float4(color, 1.0);
}

technique TM_VibrantColor
<
    ui_label = "TM Vibrant Color (BSL / Sildurs Tone)";
    ui_tooltip = "Warm filmic tone mapping, smart vibrance, sky and grass boost for TrackMania.";
>
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_TM_VibrantColor;
    }
}
