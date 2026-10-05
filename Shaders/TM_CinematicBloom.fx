/**
 * TrackMania Vibrant Shaders - TM_CinematicBloom.fx
 * Soft Multi-Tap Bloom & IterationT-style Anamorphic Lens Flare Streaks
 * Custom shader by Antigravity
 */

#include "ReShade.fxh"

uniform float fBloomThreshold <
    ui_type = "slider";
    ui_min = 0.5; ui_max = 1.0;
    ui_label = "Bloom Threshold";
    ui_tooltip = "Luminance cutoff for highlights (stadium lights, taillights, sun reflections)";
    ui_category = "Bloom Settings";
> = 0.75;

uniform float fBloomIntensity <
    ui_type = "slider";
    ui_min = 0.0; ui_max = 2.5;
    ui_label = "Bloom Intensity";
    ui_category = "Bloom Settings";
> = 0.65;

uniform float fBloomRadius <
    ui_type = "slider";
    ui_min = 1.0; ui_max = 5.0;
    ui_label = "Bloom Spread / Radius";
    ui_category = "Bloom Settings";
> = 2.2;

uniform float fAnamorphicIntensity <
    ui_type = "slider";
    ui_min = 0.0; ui_max = 3.0;
    ui_label = "Anamorphic Flare Intensity";
    ui_tooltip = "Horizontal cinematic lens flare streaks (Iconic IterationT feature)";
    ui_category = "IterationT Lens Flare";
> = 0.85;

uniform float3 f3FlareTint <
    ui_type = "color";
    ui_label = "Anamorphic Flare Tint";
    ui_tooltip = "Cyan/blue or golden streak color";
    ui_category = "IterationT Lens Flare";
> = float3(0.7, 0.85, 1.0);

texture BloomTex1 { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA8; };
sampler BloomSampler1 { Texture = BloomTex1; };

texture BloomTex2 { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA8; };
sampler BloomSampler2 { Texture = BloomTex2; };

texture FlareTex { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA8; };
sampler FlareSampler { Texture = FlareTex; };

// Pass 1: Extract bright highlights
float4 PS_Threshold(float4 pos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float3 color = tex2D(ReShade::BackBuffer, texcoord).rgb;
    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    float bright = max(0.0, luma - fBloomThreshold);
    bright = bright / (bright + 0.5); // Soft knee curve
    return float4(color * bright, 1.0);
}

// Pass 2: Horizontal blur + Anamorphic streak
float4 PS_HorizontalBlur(float4 pos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float3 color = 0.0;
    float2 pixelOffset = float2(BUFFER_RCP_WIDTH * 4.0 * fBloomRadius, 0.0);

    const float weights[5] = { 0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216 };
    color += tex2D(BloomSampler1, texcoord).rgb * weights[0];

    [unroll]
    for (int i = 1; i < 5; i++)
    {
        color += tex2D(BloomSampler1, texcoord + pixelOffset * float(i)).rgb * weights[i];
        color += tex2D(BloomSampler1, texcoord - pixelOffset * float(i)).rgb * weights[i];
    }

    return float4(color, 1.0);
}

// Pass 3: Vertical blur for 2D bloom
float4 PS_VerticalBlur(float4 pos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float3 color = 0.0;
    float2 pixelOffset = float2(0.0, BUFFER_RCP_HEIGHT * 4.0 * fBloomRadius);

    const float weights[5] = { 0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216 };
    color += tex2D(BloomSampler2, texcoord).rgb * weights[0];

    [unroll]
    for (int i = 1; i < 5; i++)
    {
        color += tex2D(BloomSampler2, texcoord + pixelOffset * float(i)).rgb * weights[i];
        color += tex2D(BloomSampler2, texcoord - pixelOffset * float(i)).rgb * weights[i];
    }

    return float4(color, 1.0);
}

// Pass 4: Wide horizontal streak for anamorphic lens flare
float4 PS_AnamorphicStreak(float4 pos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    if (fAnamorphicIntensity <= 0.001) return float4(0, 0, 0, 1);

    float3 streak = 0.0;
    const int SAMPLES = 16;
    float stepX = BUFFER_RCP_WIDTH * 16.0;

    for (int i = -SAMPLES; i <= SAMPLES; i++)
    {
        float weight = 1.0 - abs(float(i)) / float(SAMPLES + 1);
        streak += tex2Dlod(BloomSampler1, float4(texcoord + float2(float(i) * stepX, 0.0), 0, 0)).rgb * weight;
    }

    streak = (streak / float(SAMPLES)) * fAnamorphicIntensity * f3FlareTint;
    return float4(streak, 1.0);
}

// Pass 5: Combine bloom + flare with main backbuffer
float4 PS_Combine(float4 pos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float3 base = tex2D(ReShade::BackBuffer, texcoord).rgb;
    float3 bloom = tex2D(BloomSampler1, texcoord).rgb * fBloomIntensity;
    float3 flare = tex2D(FlareSampler, texcoord).rgb;

    // Screen blend to preserve brights without harsh overexposure
    float3 result = 1.0 - (1.0 - base) * (1.0 - saturate(bloom + flare));

    return float4(result, 1.0);
}

technique TM_CinematicBloom
<
    ui_label = "TM Cinematic Bloom & Anamorphic Flares (IterationT)";
    ui_tooltip = "Soft dreamy glow and IterationT-style horizontal anamorphic flares.";
>
{
    pass Threshold
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_Threshold;
        RenderTarget = BloomTex1;
    }
    pass HBlur
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_HorizontalBlur;
        RenderTarget = BloomTex2;
    }
    pass VBlur
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_VerticalBlur;
        RenderTarget = BloomTex1;
    }
    pass Streak
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_AnamorphicStreak;
        RenderTarget = FlareTex;
    }
    pass Combine
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_Combine;
    }
}
