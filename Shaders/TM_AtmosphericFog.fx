/**
 * TrackMania Vibrant Shaders - TM_AtmosphericFog.fx
 * Depth-based Volumetric Distance Fog & Horizon Atmosphere
 * Inspired by BSL Shaders & Sildur's Atmosphere
 * Custom shader by Antigravity
 */

#include "ReShade.fxh"

uniform float fFogDensity <
    ui_type = "slider";
    ui_min = 0.0; ui_max = 2.0;
    ui_label = "Fog Density";
    ui_category = "Atmospheric Fog";
> = 0.45;

uniform float fFogStart <
    ui_type = "slider";
    ui_min = 0.0; ui_max = 0.8;
    ui_label = "Fog Start Distance";
    ui_tooltip = "Distance from camera before fog begins (0 = immediate, 0.5 = far)";
    ui_category = "Atmospheric Fog";
> = 0.08;

uniform float fFogCurve <
    ui_type = "slider";
    ui_min = 0.5; ui_max = 4.0;
    ui_label = "Fog Falloff Curve";
    ui_tooltip = "Exponential falloff power for realistic volumetric haze";
    ui_category = "Atmospheric Fog";
> = 1.8;

uniform float3 f3HorizonColor <
    ui_type = "color";
    ui_label = "Horizon Fog Tint";
    ui_tooltip = "Warm/soft atmospheric color blending into sky";
    ui_category = "Atmosphere Color";
> = float3(0.85, 0.90, 0.98);

uniform float3 f3SunFogColor <
    ui_type = "color";
    ui_label = "Sunlight Scattering Tint";
    ui_tooltip = "Golden scatter around sun direction";
    ui_category = "Atmosphere Color";
> = float3(1.0, 0.92, 0.78);

uniform float2 f2SunPos <
    ui_type = "slider";
    ui_min = 0.0; ui_max = 1.0;
    ui_label = "Sun Position (X, Y)";
    ui_category = "Atmosphere Color";
> = float2(0.5, 0.15);

uniform float fSunScatterPower <
    ui_type = "slider";
    ui_min = 0.0; ui_max = 1.0;
    ui_label = "Sunlight In-Scattering";
    ui_category = "Atmosphere Color";
> = 0.40;

float4 PS_AtmosphericFog(float4 pos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float3 color = tex2D(ReShade::BackBuffer, texcoord).rgb;
    float depth = ReShade::GetLinearizedDepth(texcoord);

    // If depth is at maximum (skybox), soften fog transition to avoid hiding clouds
    float skyMask = smoothstep(0.97, 1.0, depth);

    // Calculate distance factor with smooth falloff
    float fogFactor = saturate((depth - fFogStart) / (1.0 - fFogStart));
    fogFactor = pow(fogFactor, fFogCurve) * fFogDensity;

    // Reduce fog on skybox so sky textures remain visible
    fogFactor *= (1.0 - skyMask * 0.4);

    // Sun directional in-scattering (Mie scattering approximation)
    float sunDist = length(texcoord - f2SunPos);
    float sunGlow = saturate(1.0 - sunDist * 1.5);
    sunGlow = pow(sunGlow, 3.0) * fSunScatterPower;

    float3 currentFogColor = lerp(f3HorizonColor, f3SunFogColor, sunGlow);

    // Alpha blend atmospheric haze
    float3 finalColor = lerp(color, currentFogColor, saturate(fogFactor));

    return float4(finalColor, 1.0);
}

technique TM_AtmosphericFog
<
    ui_label = "TM Atmospheric Distance Fog (BSL / Sildurs)";
    ui_tooltip = "Volumetric depth-based atmospheric haze with sunlight in-scattering.";
>
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_AtmosphericFog;
    }
}
