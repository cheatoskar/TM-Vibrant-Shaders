/**
 * TrackMania Vibrant Shaders - TM_DepthShading.fx
 * Screen-Space Depth Contact Shading & Ambient Contrast
 * Inspired by BSL Ambient Occlusion & Micro-Detail
 * Custom shader by Antigravity
 */

#include "ReShade.fxh"

uniform float fAOIntensity <
    ui_type = "slider";
    ui_min = 0.0; ui_max = 2.0;
    ui_label = "Contact Shading Intensity";
    ui_category = "Ambient Occlusion";
> = 0.55;

uniform float fAORadius <
    ui_type = "slider";
    ui_min = 1.0; ui_max = 6.0;
    ui_label = "Sample Radius (Pixels)";
    ui_category = "Ambient Occlusion";
> = 2.5;

uniform float fDepthThreshold <
    ui_type = "slider";
    ui_min = 0.001; ui_max = 0.05;
    ui_label = "Depth Sensitivity";
    ui_tooltip = "Controls sensitivity to depth edges and crevices";
    ui_category = "Ambient Occlusion";
> = 0.008;

float4 PS_DepthShading(float4 pos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float3 color = tex2D(ReShade::BackBuffer, texcoord).rgb;
    float centerDepth = ReShade::GetLinearizedDepth(texcoord);

    // Skip skybox
    if (centerDepth >= 0.99)
        return float4(color, 1.0);

    float occlusion = 0.0;
    const float2 offsets[8] = {
        float2( 1.0,  0.0),
        float2(-1.0,  0.0),
        float2( 0.0,  1.0),
        float2( 0.0, -1.0),
        float2( 0.7,  0.7),
        float2(-0.7, -0.7),
        float2( 0.7, -0.7),
        float2(-0.7,  0.7)
    };

    float2 pixelStep = BUFFER_PIXEL_SIZE * fAORadius;

    [unroll]
    for (int i = 0; i < 8; i++)
    {
        float2 sampleUV = texcoord + offsets[i] * pixelStep;
        float sampleDepth = ReShade::GetLinearizedDepth(sampleUV);
        float diff = sampleDepth - centerDepth;

        // If neighbor is closer to camera or inside crevice
        if (diff > 0.0001 && diff < fDepthThreshold)
        {
            occlusion += 1.0 - (diff / fDepthThreshold);
        }
    }

    occlusion = saturate((occlusion / 8.0) * fAOIntensity);

    // Darken shadowed crevices
    color *= (1.0 - occlusion);

    return float4(color, 1.0);
}

technique TM_DepthShading
<
    ui_label = "TM Depth Contact Shading (BSL Shading)";
    ui_tooltip = "Enhances depth contrast and contact shadows on track blocks and car.";
>
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_DepthShading;
    }
}
