// Weather in the lighting composite (scene.hlsl PS_Lighting): wet surfaces, puddles, snow cover,
// reflections, rain on surfaces, and what the air adds (rain streaks, snow veil, lightning). The
// weather stage's passes and particles are in weather.hlsl. Include after common.hlsli.
//   t7 = screen-space reflections (half res: rgb, a = hit), u_Pass0.w = reflections present

// One surface pixel, as PS_Lighting knows it.
struct Surface
{
    float2 uv;
    float2 pixel;   // SV_Position.xy
    float3 p;       // view position
    float3 n;       // view normal
    float3 nWorld;
    float3 world;
    float3 rd;      // view ray (world)
    float dist;
    float z;        // view z
    float3 albedo;  // the engine's material colour (linear)
    float sunShadow; // the engine's shadow map: 1 lit, 0 shadowed
    float ao;
};

// Before the light is put together: water in the pores darkens the lit parts, snow covers them.
//   sun, ambient: the surface's sun and ambient light (albedo already in), glow: its light sources.
void weatherSurface(Surface s, inout float3 sun, inout float3 ambient, inout float3 glow)
{
}

// After the light, before the haze: reflections on wet and polished surfaces, puddles, water,
// rain on surfaces. lit = the surface's light.
float3 weatherReflect(Surface s, float3 lit, float4 skyAvg)
{
    return lit;
}

// The air in front of a surface or the sky (after the haze): rain streaks, snow veil, lightning.
//   dist = distance to what is seen (SKY_Z for the sky), fogAmount = the haze there.
float3 weatherAir(float2 uv, float3 rd, float dist, float3 c, float4 skyAvg, float fogAmount, float3 nWorld)
{
    return c;
}
