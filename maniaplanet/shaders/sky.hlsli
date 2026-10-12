// The sky in the lighting composite (scene.hlsl PS_Lighting): what a sky pixel shows, and the light
// a sky puts on the scene. The sky stage's passes are in sky.hlsl. Include after common.hlsli.
//   t3 = custom sky (full res), t4 = sky average (1x1), t5 = clouds (half res)
//   u_Pass0: x = sky texture present, y = sky average present, z = clouds present

// A sky pixel. c = the engine's sky (display linear), rd = view ray (world), night = u_Sky.y.
float3 skyPixel(float2 uv, float3 rd, float3 c, float4 skyAvg, float daylight, float night)
{
    if (u_Sky.x > 0.5 && u_Pass0.x > 0.5)
    {
        c = t3_tex(uv).rgb;
    }
    else
    {
        // The game's sky: a richer gradient, glow around the sun.
        float horizon = 1.0 - saturate(rd.y * 3.0);
        float3 graded = lerp(c, c * c / max(luma(c), 1e-3), 0.35);
        graded *= 1.0 + horizon * 0.25;
        c = lerp(c, graded, u_Atmo.w);
        if (u_SunView.w > 0.5) c += sunScatter(rd, u_Rays.z * 0.5) * daylight * 0.5;
        // Day-for-night on the game's own sky.
        c = lerp(c, luma(c) * float3(0.85, 0.9, 1.0), saturate(night * 1.6)) * lerp(1.0, 0.3, night);
    }
    if (u_Clouds.x > 0.0 && u_Pass0.z > 0.5)
    {
        float4 cloud = t5_texL(uv);
        c = c * lerp(1.0, cloud.a, u_Clouds.x) + cloud.rgb * lerp(1.0, 0.45, night) * u_Clouds.x;
    }
    return c;
}

// Light the sky puts on a surface (the black hole's warm light), added before the haze.
//   unlit = the surface's light before the night darkening, nWorld = its normal, rd = view ray.
float3 skyLightOnScene(float3 unlit, float3 nWorld, float3 rd, float fogAmount)
{
    return 0;
}
