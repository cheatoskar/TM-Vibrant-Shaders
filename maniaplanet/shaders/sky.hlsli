// The sky in the lighting composite (scene.hlsl PS_Lighting): what a sky pixel shows, and the light
// a sky puts on the scene. The sky stage's passes are in sky.hlsl; its full-res sky texture already
// holds the finished sky (custom sky or the game's enhanced, clouds, lightning), so a sky pixel just
// reads it. Include after common.hlsli.
//   t3 = the sky stage's sky (full res), t4 = sky average (1x1)
//   u_Pass0: x = sky texture present, y = sky average present
//
// TMNF's sky colours are in its units (the game's 8-bit image made linear). Here everything is the
// engine's HDR image times its exposure: u_Misc.z converts (the TMVS curve shows TMNF's value v as
// TMNF showed it when it gets v * u_Misc.z; the other curves match mid grey).

float tmnfScale() { return u_Misc.z > 0.0 ? u_Misc.z : 1.0; }

float3 blackHoleDirection() { return u_BlackHole.xyz; }

// Direction (xyz) and light strength (w) of the black hole of the current sky, if any.
float4 blackHoleLight()
{
    int mode = (int)(u_Sky.x + 0.5);
    if (u_BlackHole.w <= 0.0) return 0;
    if (mode == 3) return float4(blackHoleDirection(), saturate(u_BlackHole.w * 0.45));
    if (mode == 5) return float4(blackHoleDirection(), saturate(u_BlackHole.w * 0.25));
    return 0;
}

// A sky pixel. c = the engine's sky (display linear). Without the stage's texture (nothing to
// change this frame) the engine's sky stays.
float3 skyPixel(float2 uv, float3 rd, float3 c, float4 skyAvg, float daylight, float night)
{
    if (u_Pass0.x > 0.5) c = t3_tex(uv).rgb;
    return c;
}

// Light the sky puts on a surface (the black hole's warm light), added before the haze.
//   unlit = the surface's light before the night darkening, nWorld = its normal, rd = view ray.
float3 skyLightOnScene(float3 unlit, float3 nWorld, float3 rd, float fogAmount)
{
    // A black hole in the sky is the brightest light around: warm light on everything that
    // faces it, and the haze glows towards it.
    float4 bh = blackHoleLight();
    if (bh.w <= 0.0) return 0;
    float3 bhColor = float3(1.0, 0.8, 0.6) * bh.w;
    float3 c = unlit * bhColor * saturate(dot(nWorld, bh.xyz) * 0.8 + 0.2) * 0.35;
    c += bhColor * pow(saturate(dot(rd, bh.xyz)), 6.0) * saturate(fogAmount * 2.0 + 0.05) * 0.4 * tmnfScale();
    return c;
}
