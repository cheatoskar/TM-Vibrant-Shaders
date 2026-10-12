// Weather in the lighting composite (scene.hlsl PS_Lighting): wet surfaces, puddles, snow cover,
// reflections, rain on surfaces, and what the air adds (rain streaks, snow veil, lightning). The
// weather stage's passes and particles are in weather.hlsl. Include after common.hlsli.
//   t3 = custom sky (when u_Pass0.x), t7 = screen-space reflections (half res: rgb, a = hit),
//   u_Pass0.w = reflections present
//
// Ported from the TMNF mod (shaders/tmvs.hlsl, "Surfaces"). TMNF had to tell grass, dirt and
// lights from the colour of a lit 8-bit picture; here the engine's own albedo and normals say what
// a surface is, and the engine's light (engineLight) lights the snow. The helpers carry a wx
// prefix: sky.hlsli is included into the same shader.

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

// What weatherSurface found, for weatherReflect (the same pixel, one shader invocation).
static float g_wxWet = 0.0;
static float g_wxPuddle = 0.0;
static float g_wxSnow = 0.0;
static float g_wxGrass = 0.0;
static float g_wxDry = 0.0;
static float g_wxFlat = 0.0;

float wxNoise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash12(i), b = hash12(i + float2(1, 0)), c = hash12(i + float2(0, 1)), d = hash12(i + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float wxSaturation(float3 c)
{
    float peak = max(c.r, max(c.g, c.b));
    return (peak - min(c.r, min(c.g, c.b))) / max(peak, 1e-4);
}

// Grass, by its material colour (linear albedo): clearly green, facing up.
float wxGrassMask(float3 albedo, float3 nWorld)
{
    float greenness = (albedo.g - max(albedo.r, albedo.b)) / max(albedo.g, 1e-3);
    return smoothstep(0.22, 0.42, greenness) * smoothstep(0.55, 0.85, nWorld.y) * step(albedo.g, 0.6);
}

float wxPuddleMask(float3 world, float flatness, float grass)
{
    if (u_Weather.z <= 0.0) return 0.0;
    float n = wxNoise(world.xz * 0.16) * 0.65 + wxNoise(world.xz * 0.55 + 13.0) * 0.35;
    float threshold = 0.8 - u_Weather.z * 0.32;
    return smoothstep(threshold, threshold + 0.18, n) * flatness * (1.0 - grass) * saturate(u_Weather.x * 2.0);
}

// Expanding rings where raindrops hit standing water. Returns the surface slope.
float2 wxRippleSlope(float2 xz, float t)
{
    float2 g = 0;
    [unroll] for (int k = 0; k < 2; k++)
    {
        float2 q = xz * 2.3 + k * 3.71;
        float2 cell = floor(q);
        float2 f = frac(q) - 0.5;
        float h = hash12(cell + k * 17.0);
        float2 centre = (float2(h, hash12(cell + 5.3)) - 0.5) * 0.4;
        float phase = frac(t * 1.2 + h * 7.0);
        float2 d = f - centre;
        float dl = length(d);
        float x = dl - phase * 0.5;
        float wave = cos(x * 45.0) * exp(-x * x * 300.0) * (1.0 - phase);
        g += d / max(dl, 1e-3) * wave;
    }
    return g * 0.3;
}

// ---- Snow (TMNF snowCover) ----

// How deep the wind piled the snow here (0..1): long drifts, heaps and smaller mounds.
float wxSnowPile(float2 xz)
{
    return wxNoise(xz * 0.06 + 3.0) * 0.5 + wxNoise(xz * 0.21 + 7.0) * 0.32 + wxNoise(xz * 0.8 + 1.0) * 0.18;
}

// Height of the snow's surface (m): the piles, wind ripples and, close up, small lumps.
float wxSnowHeight(float2 xz, float near)
{
    float h = wxSnowPile(xz) * 0.8;
    h += sin(dot(xz, float2(0.8, 0.6)) * 3.0 + wxNoise(xz * 0.5) * 6.0) * 0.008 * near;
    h += (wxNoise(xz * 2.5 + 5.0) - 0.5) * 0.03 * near;
    return h;
}

// Snow on everything that faces up: fields on the grass and open ground, a thin even layer on the
// track and everything built (thicker in drifts), less on painted surfaces, none on steep walls;
// streaky crusts on walls facing the wind. Returns the coverage; `relief` = how the snow's own
// surface is lit (piles, ripples, lumps), 1 = flat.
float wxSnowCover(float3 albedo, float3 world, float3 nWorld, float dist, float grass, out float relief)
{
    relief = 1.0;
    float cover = u_Snow.y;
    if (cover <= 0.0) return 0.0;
    float up = smoothstep(0.35, 0.8, nWorld.y);
    float painted = smoothstep(0.3, 0.55, wxSaturation(albedo)) * (1.0 - grass);
    // Walls and kerbs facing the wind catch flakes: streaky crusts, more of them low down.
    float facing = saturate(-dot(nWorld.xz, u_Snow.zw) * 1.4) * (1.0 - up) * smoothstep(-0.5, 0.0, nWorld.y);
    if (up <= 0.0)
    {
        if (facing <= 0.0) return 0.0;
        float along = dot(world.xz, float2(u_Snow.w, -u_Snow.z));
        float streak = wxNoise(float2(along * 0.13, world.y * 0.4) + 3.7) * 0.5 + wxNoise(float2(along * 0.5, world.y * 1.3)) * 0.3 +
                       wxNoise(float2(along * 1.9, world.y * 2.9) + 7.0) * 0.2;
        return smoothstep(0.45, 0.9, streak) * facing * cover * 0.3 * (1.0 - 0.6 * painted);
    }
    // Piles: below full cover the wind heaps the snow into drifts and mounds with bare ground
    // between them; at 1 it closes into a blanket. Far away the piles blend into their average.
    float pile = lerp(wxSnowPile(world.xz), 0.5, smoothstep(80.0, 200.0, dist));
    float t = 0.85 - cover * 0.8 + 0.03 * (1.0 - grass);
    float depth = saturate((pile - t) / 0.2);
    float mask = saturate(smoothstep(0.0, 0.45, depth) * lerp(1.0, 0.6, painted) * up);
    if (mask <= 0.0) return 0.0;
    // Relief: lit from the light's side, the small lumps only close up (no shimmer far away).
    float near = 1.0 - smoothstep(12.0, 45.0, dist);
    float2 e = float2(0.06, 0.0);
    float h0 = wxSnowHeight(world.xz, near);
    float2 grad = float2(wxSnowHeight(world.xz + e.xy, near) - h0, wxSnowHeight(world.xz + e.yx, near) - h0) / e.x;
    float2 light = u_SunView.w > 0.5 && length(u_SunWorld.xz) > 0.05 ? normalize(u_SunWorld.xz) : float2(0.6, 0.8);
    float slope = clamp(dot(grad, light) * lerp(1.2, 3.0, near), -0.35, 0.3);
    // Thickness: where a pile ends the snow has an edge, its lit rim and its shaded flank.
    float edge = mask * (1.0 - mask) * 4.0;
    relief = 1.0 + slope - edge * 0.18;
    return saturate(mask + edge * 0.15);
}

// Snow glinting in the sun: single crystals catch it, a few at a time, changing with the view.
float3 wxSnowSparkle(float3 world, float3 rd, float dist, float lit)
{
    if (lit <= 0.0 || dist > 30.0) return 0;
    float2 cell = floor(world.xz * 30.0);
    float h = hash12(cell + floor(rd.xz * 40.0) * 0.37);
    return u_SunColor.rgb * step(0.996, h) * lit * 3.0 * (1.0 - dist / 30.0);
}

// The colour of driving snow: the overcast light, whitened.
float3 wxSnowHaze(float3 sky)
{
    return luma(sky) * float3(0.95, 0.97, 1.0) * 1.25 + 0.02;
}

// Before the light is put together: water in the pores darkens the lit parts, snow covers them.
//   sun, ambient: the surface's sun and ambient light (albedo already in), glow: its light sources.
void weatherSurface(Surface s, inout float3 sun, inout float3 ambient, inout float3 glow)
{
    g_wxWet = 0.0;
    g_wxPuddle = 0.0;
    g_wxSnow = 0.0;
    g_wxDry = 0.0;
    g_wxFlat = smoothstep(0.8, 0.97, s.nWorld.y);
    g_wxGrass = wxGrassMask(s.albedo, s.nWorld);
    if (u_Weather.x <= 0.0 && u_Snow.y <= 0.0 && u_Nature.w <= 0.0) return;

    float flat = g_wxFlat;
    float grass = g_wxGrass;
    float wet = u_Weather.x * (1.0 - grass * 0.5);
    float puddle = wxPuddleMask(s.world, flat, grass);

    // Snow lies on top: lit by the light that reaches the surface (the engine's sun and ambient
    // for a white surface, and what light maps and lamps add on this surface).
    float relief;
    float snow = wxSnowCover(s.albedo, s.world, s.nWorld, s.dist, grass, relief);
    if (snow > 0.0)
    {
        float3 sunUnit, ambUnit;
        float engineShadow;
        engineLight(s.uv, s.n, float3(1, 1, 1), sunUnit, ambUnit, engineShadow);
        // The contact shadows PS_Lighting found (sunShadow = engine shadow * contact shadow).
        float contact = saturate(s.sunShadow / max(engineShadow, 1e-3));
        float3 direct = (sunUnit * contact + ambUnit * s.ao);
        // Light the surface gets beyond the engine's sun and ambient (light maps, lamps): from
        // the lit colour over the albedo, capped (dark asphalt has reflections in it too).
        float3 seen = (sun + ambient) / max(luma(s.albedo), 0.05);
        float3 light = max(direct, min(seen, direct * 3.0 + 0.02));
        float3 snowColour = float3(0.86, 0.9, 0.96) * relief;
        // Under a night sky the moon replaces the sun (PS_Lighting does the same to `sun`).
        float3 snowSun = snowColour * sunUnit * contact * relief;
        float3 snowRest = snowColour * max(light - sunUnit * contact, 0.0);
        snowSun = lerp(snowSun, snowSun * float3(0.35, 0.5, 0.85) * 0.05, saturate(u_SunOut.x));
        float3 sparkle = wxSnowSparkle(s.world, s.rd, s.dist, luma(sunUnit * contact) * u_SunColor.w * (1.0 - saturate(u_SunOut.x)));
        sun = lerp(sun, snowSun + sparkle, snow);
        ambient = lerp(ambient, snowRest, snow);
        // Lights under the snow (a lit kerb) shine through less.
        glow *= 1.0 - 0.7 * snow;
        wet *= 1.0 - snow;
        puddle *= 1.0 - snow;
    }

    // Water fills the pores: wet asphalt and puddles are darker.
    float darken = lerp(1.0, 0.62, wet * lerp(0.4, 1.0, flat)) * lerp(1.0, 0.6, puddle);
    sun *= darken;
    ambient *= darken;

    // The dry track can be polished (Reflections): not the grass, not the snow, little on dirt
    // (warm brown, rough), nothing on lights.
    float sat = wxSaturation(s.albedo);
    float dirt = smoothstep(0.3, 0.45, sat) * (1.0 - smoothstep(0.75, 0.9, sat)) *
                 saturate((s.albedo.r - s.albedo.g) / max(s.albedo.r, 1e-3) * 8.0) *
                 saturate((s.albedo.g - s.albedo.b) / max(s.albedo.r, 1e-3) * 8.0);
    g_wxDry = flat * (1.0 - grass) * (1.0 - snow) * (1.0 - 0.85 * dirt);
    g_wxWet = wet;
    g_wxPuddle = puddle;
    g_wxSnow = snow;
}

float3 wxSkyReflection(float3 r, float4 skyAvg, float daylight)
{
    float3 zenith = u_SkyColor.rgb * (0.2 + 0.8 * daylight) * 0.8;
    return lerp(skyAvg.rgb, zenith, saturate(r.y * 1.5));
}

// Rain on every surface, not only the road: thin streams running down steep wet surfaces (walls,
// barriers, the car's flanks) and small splash rings on everything that faces the sky.
float3 wxRainOnSurfaces(float3 c, float3 world, float3 nWorld, float dist, float3 skyLight, float puddle)
{
    float rain = saturate(u_Weather.y) * saturate(u_Weather.x * 1.5);
    if (rain <= 0.0 || dist > 40.0) return c;
    float t = u_Proj2.z;
    float fade = 1.0 - smoothstep(20.0, 40.0, dist);
    float steep = 1.0 - smoothstep(0.35, 0.7, abs(nWorld.y));
    if (steep > 0.0)
    {
        float3 tangent = normalize(cross(float3(0, 1, 0), nWorld) + 1e-5);
        float u = dot(world, tangent) * 6.0;
        float col = floor(u);
        float h = hash12(float2(col, 3.7));
        float x = frac(u) - 0.5 - (h - 0.5) * 0.4 + sin(world.y * 7.0 + h * 6.0) * 0.08;
        float seg = frac(world.y * 1.5 + t * (0.8 + h) + h * 10.0);
        float stream = smoothstep(0.09, 0.0, abs(x)) * smoothstep(0.0, 0.1, seg) * smoothstep(0.75, 0.3, seg) * step(0.55, h);
        c = lerp(c, c * 0.6 + skyLight * 0.35, stream * steep * fade * rain);
    }
    float up = smoothstep(0.5, 0.8, nWorld.y) * (1.0 - puddle);
    if (up > 0.0)
    {
        float2 q = world.xz * 5.0;
        float2 cell = floor(q);
        float h = hash12(cell);
        float cycle = t * 2.5 * (0.5 + h) + h * 13.0;
        float phase = frac(cycle);
        float hit = step(1.0 - rain * 0.6, hash12(cell + floor(cycle) * 1.37));
        float2 centre = float2(hash12(cell + 1.3), hash12(cell + 2.9)) * 0.6 + 0.2;
        float d = length(frac(q) - centre);
        float ring = smoothstep(0.045, 0.0, abs(d - phase * 0.3)) * (1.0 - phase) * (1.0 - phase) * hit;
        c += skyLight * ring * up * fade * 0.35;
    }
    return c;
}

// After the light, before the haze: reflections on wet and polished surfaces, puddles, rain on
// surfaces. lit = the surface's light.
float3 weatherReflect(Surface s, float3 lit, float4 skyAvg)
{
    float wet = g_wxWet, puddle = g_wxPuddle;
    float flat = g_wxFlat;
    float daylight = u_SunWorld.w;
    float3 skyLight = skyAvg.rgb * 0.8 + 0.04 * lerp(1.0, 0.2, u_Sky.y);
    // Dry reflections: 0..1 a glossy sheen, 1..2 polished like a showroom floor.
    float polish = saturate(u_Nature.w - 1.0);
    float dry = g_wxDry;
    float reflectivity = max(wet * lerp(0.12, 0.55, flat), puddle);
    reflectivity = max(reflectivity, min(u_Nature.w, 1.0) * dry * lerp(0.35, 1.0, polish));
    polish *= dry;
    if (reflectivity > 0.0)
    {
        float t = u_Proj2.z;
        float2 slope = 0;
        if (u_Weather.y > 0.0) slope = wxRippleSlope(s.world.xz, t) * u_Weather.y * (puddle + wet * 0.25);
        // Rough wet asphalt breaks the reflection up.
        if (wet > 0.0)
        {
            float2 q = s.world.xz * 6.0;
            slope += (float2(wxNoise(q), wxNoise(q + 9.7)) - 0.5) * 0.12 * wet * (1.0 - puddle);
        }
        float3 nRefl = normalize(s.nWorld + float3(-slope.x, 0.0, -slope.y));
        float3 r = reflect(s.rd, nRefl);
        float NdotV = saturate(dot(nRefl, -s.rd));
        float smoothness = max(max(puddle, wet * 0.35), polish);
        float2 distort = worldToViewDir(float3(slope.x, 0.0, slope.y)).xy * float2(1, -1) * 0.04;
        float4 ssr = u_Pass0.w > 0.5 ? t7_texL(s.uv + distort) : float4(0, 0, 0, 0);
        // Rough wet asphalt and the polished dry track blur the reflection; puddles stay sharp.
        // Reflection blur (u_Sky3.z): the dry track becomes a glossy sheen instead of a mirror.
        float blurDry = u_Sky3.z * dry;
        float soften = max(max(polish, wet * (1.0 - puddle)), blurDry);
        if (soften > 0.0 && u_Pass0.w > 0.5)
        {
            float radius = 3.0 + 22.0 * blurDry * blurDry;
            float turn = ign(s.pixel) * 6.2831853;
            float4 soft = ssr * 2.0;
            [unroll] for (int k = 0; k < 8; k++)
            {
                float a = turn + k * 2.3999632;
                float rr = radius * sqrt((k + 0.5) / 8.0);
                soft += t7_texL(s.uv + float2(cos(a), sin(a)) * rr * u_Screen.zw);
            }
            ssr = lerp(ssr, soft / 10.0, soften);
        }
        // Standing water reads as a mirror even from above: strongly where it mirrors objects
        // and lights, gently where it only mirrors the sky.
        float F = max(0.02 + 0.98 * pow(1.0 - NdotV, 5.0), puddle * lerp(0.1, 0.4, ssr.a));
        F = max(F, polish * lerp(0.05, 0.55, ssr.a));
        // Where the reflection traced nothing: the sky in the mirrored direction (the custom sky
        // where that is on screen), else its average.
        float3 skyR = wxSkyReflection(r, skyAvg, daylight) * lerp(1.0, 0.2, u_Sky.y);
        if (u_Sky.x > 0.5 && u_Pass0.x > 0.5)
        {
            float3 rv = worldToViewDir(r);
            if (rv.z > 0.01)
            {
                float2 suv = projectToUV(rv * 100.0) + distort * 2.0;
                float edge = saturate(min(min(suv.x, 1.0 - suv.x), min(suv.y, 1.0 - suv.y)) * 8.0);
                if (edge > 0.0)
                {
                    float3 sky = 0;
                    [unroll] for (int k = 0; k < 3; k++) sky += t3_texL(suv + float2(0.0, (k - 1.0) * 0.011)).rgb;
                    skyR = lerp(skyR, sky / 3.0, edge * 0.7);
                }
            }
        }
        float3 refl = lerp(skyR, ssr.rgb, ssr.a);
        lit = lerp(lit, refl, saturate(F * reflectivity * lerp(0.6, 1.0, smoothness)));
        // Sun glint (GGX): sharp on puddles, broad on wet asphalt.
        if (u_SunView.w > 0.5)
        {
            float rough = lerp(0.35, 0.05, smoothness);
            float a2 = rough * rough * rough * rough;
            float3 h = normalize(u_SunWorld.xyz - s.rd);
            float nh = saturate(dot(nRefl, h));
            float dd = nh * nh * (a2 - 1.0) + 1.0;
            float spec = min(a2 / (PI * dd * dd) * F * 0.25, 30.0) * saturate(dot(nRefl, u_SunWorld.xyz));
            lit += u_SunColor.rgb * spec * reflectivity * s.sunShadow * daylight * 0.5;
        }
    }
    return wxRainOnSurfaces(lit, s.world, s.nWorld, s.dist, skyLight, puddle);
}

// ---- The air ----

// Distant rain: streaks on three cylinders around the camera (13 - 32 m away), beyond the rain
// particles, hidden behind closer geometry. Returns the streak coverage.
float wxRainStreaks(float3 rd, float sceneDist)
{
    float az = atan2(rd.x, rd.z);
    float horiz = max(sqrt(saturate(1.0 - rd.y * rd.y)), 0.05);
    float elevation = rd.y / horiz;
    float mppScale = 2.0 / (abs(u_Proj.y) * u_Screen.y); // metres per pixel at 1 m
    float acc = 0.0;
    [unroll] for (int l = 0; l < 3; l++)
    {
        float d = l == 0 ? 13.0 : (l == 1 ? 20.0 : 32.0);
        if (sceneDist > d)
        {
            float2 q = float2(az * d, elevation * d);
            q.x += q.y * 0.12 * u_Nature.z;     // wind slant
            q.y += u_Proj2.z * (7.5 + l * 0.6); // falling
            float2 cellSize = float2(0.16, 1.25);
            float2 cell = floor(q / cellSize);
            float2 f = frac(q / cellSize);
            float h = hash12(cell + l * 31.0);
            if (h < u_Weather.y * 0.85)
            {
                float h2 = frac(h * 97.31), h3 = frac(h * 57.17), h4 = frac(h * 23.93);
                float width = max(0.0012, d * mppScale * 0.75);
                float dx = abs(f.x - (0.2 + 0.6 * h2)) * cellSize.x;
                float len = 0.3 + 0.4 * h3;
                float y0 = h4 * (1.0 - len);
                float inY = smoothstep(y0, y0 + 0.08, f.y) * smoothstep(y0 + len, y0 + len - 0.12, f.y);
                acc += saturate(1.0 - dx / width) * (0.0012 / width) * inY * (0.8 - l * 0.2);
            }
        }
    }
    return acc;
}

// Distant snow: soft flakes drifting on three cylinders around the camera (12 - 30 m), beyond
// the snow particles. Returns the coverage.
float wxSnowVeil(float3 rd, float sceneDist)
{
    float az = atan2(rd.x, rd.z);
    float horiz = max(sqrt(saturate(1.0 - rd.y * rd.y)), 0.05);
    float elevation = rd.y / horiz;
    float mppScale = 2.0 / (abs(u_Proj.y) * u_Screen.y);
    float t = u_Proj2.z;
    float acc = 0.0;
    [unroll] for (int l = 0; l < 3; l++)
    {
        float d = l == 0 ? 12.0 : (l == 1 ? 19.0 : 30.0);
        if (sceneDist > d)
        {
            float2 q = float2(az * d, elevation * d);
            q.x += t * u_Nature.z * (2.0 + l) + sin(q.y * 0.7 + t * 0.9 + l) * 0.3; // drifting
            q.y += t * (1.1 + l * 0.15);                                              // falling
            float2 cellSize = float2(0.55, 0.55);
            float2 cell = floor(q / cellSize);
            float2 f = frac(q / cellSize) - 0.5;
            float h = hash12(cell + l * 17.0);
            if (h < saturate(u_Snow.x * 0.5) * 0.9)
            {
                float2 centre = float2(frac(h * 91.7), frac(h * 47.3)) * 0.6 - 0.3;
                float size = max(0.012, d * mppScale * 0.8) / cellSize.x;
                float r = length(f - centre) / size;
                acc += saturate(1.2 - r) * (0.75 - l * 0.18);
            }
        }
    }
    return saturate(acc);
}

// The air in front of a surface or the sky (after the haze): rain streaks, snow veil, lightning.
//   dist = distance to what is seen (SKY_Z for the sky), fogAmount = the haze there.
float3 weatherAir(float2 uv, float3 rd, float dist, float3 c, float4 skyAvg, float fogAmount, float3 nWorld)
{
    float night = u_Sky.y;
    bool sky = dist >= SKY_Z;
    // Lightning: a cold flash over everything, surfaces facing up and the haze most (the sky's
    // own glow and the bolt are the sky's).
    if (u_Light2.z > 0.0 && !sky)
    {
        float3 fogColour = skyAvg.rgb * lerp(1.0, 0.2, night);
        c += (c * 2.5 + fogColour * 0.4 * fogAmount + 0.01) * float3(0.8, 0.85, 1.0) * u_Light2.z * (0.6 + 0.4 * saturate(nWorld.y));
    }
    if (u_Weather.y > 0.0)
    {
        float streaks = wxRainStreaks(rd, dist);
        float3 light = sky ? skyAvg.rgb * 0.8 + 0.03 : skyAvg.rgb * 0.8 + u_SunColor.rgb * u_SunWorld.w * 0.15 + 0.03;
        c += light * lerp(1.0, 0.25, night) * streaks;
    }
    if (u_Snow.x > 0.0)
    {
        float3 haze = wxSnowHaze(skyAvg.rgb) * lerp(1.0, 0.3, night);
        if (sky) c = lerp(c, haze, saturate(u_Snow.x * 0.35) * (1.0 - saturate(rd.y * 1.5)));
        c = lerp(c, haze, wxSnowVeil(rd, dist) * 0.7);
    }
    return c;
}
