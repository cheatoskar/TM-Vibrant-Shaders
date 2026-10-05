#pragma once

#if !defined(__RESHADE__) || __RESHADE__ < 30000
    #define BUFFER_WIDTH 1920
    #define BUFFER_HEIGHT 1080
    #define BUFFER_RCP_WIDTH (1.0 / 1920.0)
    #define BUFFER_RCP_HEIGHT (1.0 / 1080.0)
    #define BUFFER_SCREEN_SIZE float2(BUFFER_WIDTH, BUFFER_HEIGHT)
    #define BUFFER_PIXEL_SIZE float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT)
    #define BUFFER_ASPECT_RATIO (1920.0 / 1080.0)
#else
    #define BUFFER_SCREEN_SIZE float2(BUFFER_WIDTH, BUFFER_HEIGHT)
    #define BUFFER_PIXEL_SIZE float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT)
    #define BUFFER_ASPECT_RATIO (BUFFER_WIDTH * BUFFER_RCP_HEIGHT)
#endif

namespace ReShade
{
    texture BackBufferTex : COLOR;
    sampler BackBuffer
    {
        Texture = BackBufferTex;
        AddressU = CLAMP;
        AddressV = CLAMP;
        MinFilter = LINEAR;
        MagFilter = LINEAR;
    };

    texture DepthBufferTex : DEPTH;
    sampler DepthBuffer
    {
        Texture = DepthBufferTex;
        AddressU = CLAMP;
        AddressV = CLAMP;
        MinFilter = POINT;
        MagFilter = POINT;
    };

    float GetLinearizedDepth(float2 texcoord)
    {
        #if !defined(RESHADE_DEPTH_INPUT_IS_REVERSED)
            #define RESHADE_DEPTH_INPUT_IS_REVERSED 0
        #endif
        #if !defined(RESHADE_DEPTH_INPUT_IS_LOGARITHMIC)
            #define RESHADE_DEPTH_INPUT_IS_LOGARITHMIC 0
        #endif

        float depth = tex2Dlod(DepthBuffer, float4(texcoord, 0, 0)).r;

        #if RESHADE_DEPTH_INPUT_IS_REVERSED
            depth = 1.0 - depth;
        #endif

        const float C = 0.01;
        #if RESHADE_DEPTH_INPUT_IS_LOGARITHMIC
            depth = (exp(depth * log(C + 1.0)) - 1.0) / C;
        #endif

        #if !defined(RESHADE_DEPTH_LINEARIZATION_FAR_PLANE)
            #define RESHADE_DEPTH_LINEARIZATION_FAR_PLANE 1000.0
        #endif

        const float N = 1.0;
        const float F = RESHADE_DEPTH_LINEARIZATION_FAR_PLANE;
        depth = (2.0 * N) / (F + N - depth * (F - N));
        return saturate(depth);
    }
}

void PostProcessVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 texcoord : TEXCOORD)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    pos = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
