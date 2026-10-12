// TM Vibrant Shaders for ManiaPlanet: the scene effects on the engine's HDR image (shader model 5).
// Ported from the TMNF mod (shaders/tmvs.hlsl), but on what ManiaPlanet's deferred renderer knows:
// its normals, its albedo, its light mask (sun shadow map, ambient) and its sun. Where TMNF had to
// guess light out of a finished 8-bit picture, the engine's own lighting is taken apart here: the
// sun part, the ambient part and what is left (light maps, lamps, neon, specular).
//
// Per frame (effects.cpp):
//   PS_DownsampleND (half, 2 targets) -> PS_OcclusionShadow (half) -> PS_BilateralBlur x2
//   -> PS_GI (quarter) -> PS_GITemporal -> PS_SpillDown (quarter) -> PS_SpillBlur x2
//   -> [sky, clouds: sky.hlsl] -> PS_Lighting (full, the result) -> [weather, cinema]
// entries: PS_DownsampleND:ps_5_0 PS_OcclusionShadow:ps_5_0 PS_BilateralBlur:ps_5_0 PS_GI:ps_5_0 PS_GITemporal:ps_5_0 PS_SpillDown:ps_5_0 PS_SpillBlur:ps_5_0 PS_Lighting:ps_5_0
#include "common.hlsli"
#include "sky.hlsli"
#include "weather.hlsli"

// ---------------------------------------------------------------------------------
// Half-resolution normal/depth and colour.
//   out0 = view normal (xyz), view z (w, SKY_Z for the sky): the closest of each 2x2 block
//   out1 = the engine's image (exposed, bright lights limited): what GI and neon light read
// ---------------------------------------------------------------------------------
float3 normalFromDepth(float2 uv, float zc)
{
    float2 px = u_Screen.zw;
    float zl = engineZ(uv - float2(px.x, 0)), zr = engineZ(uv + float2(px.x, 0));
    float zu = engineZ(uv - float2(0, px.y)), zd = engineZ(uv + float2(0, px.y));
    float zl2 = engineZ(uv - float2(2 * px.x, 0)), zr2 = engineZ(uv + float2(2 * px.x, 0));
    float zu2 = engineZ(uv - float2(0, 2 * px.y)), zd2 = engineZ(uv + float2(0, 2 * px.y));
    float3 pc = viewPosition(uv, zc);
    bool useLeft = abs(2 * zl - zl2 - zc) < abs(2 * zr - zr2 - zc);
    bool useUp = abs(2 * zu - zu2 - zc) < abs(2 * zd - zd2 - zc);
    float3 dx = useLeft ? pc - viewPosition(uv - float2(px.x, 0), zl) : viewPosition(uv + float2(px.x, 0), zr) - pc;
    float3 dy = useUp ? pc - viewPosition(uv - float2(0, px.y), zu) : viewPosition(uv + float2(0, px.y), zd) - pc;
    float3 n = normalize(cross(dx, dy));
    if (dot(n, pc) > 0) n = -n;
    return n;
}

// The surface normal (view space) to light with: the engine's, facing the camera.
float3 surfaceNormal(float2 uv, float z)
{
    float3 n;
    if (u_Engine.x < 0.5)
        n = normalFromDepth(uv, z);
    else
    {
        n = engineNormal(uv);
        if (dot(n, viewPosition(uv, z)) > 0.0) n = -n;
    }
    return n;
}

struct NDOut
{
    float4 nd : SV_Target0;
    float4 colour : SV_Target1;
};

NDOut PS_DownsampleND(VSOut i)
{
    float2 o = u_Screen.zw * 0.5;
    float4 best = float4(0, 0, 1, SKY_Z);
    float2 bestUV = i.uv;
    float3 colour = 0;
    [unroll] for (int k = 0; k < 4; k++)
    {
        float2 suv = i.uv + float2(k & 1 ? o.x : -o.x, k & 2 ? o.y : -o.y);
        float z = engineZ(suv);
        if (z < best.w)
        {
            best.w = z;
            bestUV = suv;
        }
        // Limited: the sun's glare must not light up whole walls by bounce.
        colour += min(engineColour(suv), 6.0);
    }
    NDOut r;
    r.nd = best.w < SKY_Z ? float4(surfaceNormal(bestUV, best.w), best.w) : float4(0, 0, 1, SKY_Z);
    r.colour = float4(colour * 0.25, 1);
    return r;
}

// ---------------------------------------------------------------------------------
// Ambient occlusion and contact shadows (half res). The engine has a sun shadow map: these add
// what it is too coarse for, the occlusion in corners and the short shadows of small things.
//   t0 = half normal/depth. out.r = AO visibility, out.g = sun visibility
// ---------------------------------------------------------------------------------
float4 PS_OcclusionShadow(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float4 nd = t0_tex(uv);
    if (nd.w >= SKY_Z) return float4(1, 1, 1, 1);
    float3 p = viewPosition(uv, nd.w);
    float3 n = nd.xyz;
    float noise = ign(i.pos.xy);
    float noise2 = u_Temporal.x > 0.5 ? frac(hash12(i.pos.xy + 17.31) + u_Temporal.z * 0.618034) : ign(i.pos.xy + float2(47.0, 17.0));

    // --- Ambient occlusion (horizon-based, normal-aware) ---
    float radius = u_Light.y;
    float projScale = abs(u_Proj.y) * 0.5 * u_Screen.y * 0.5; // half-res pixels per metre at z = 1
    float radiusPx = clamp(radius * projScale / p.z, 3.0, u_Screen.y * 0.1);
    float occlusion = 0.0;
    float samples = u_Quality.x;
    float angle = noise * 2.0 * PI;
    [loop] for (int k = 0; k < (int)samples; k++)
    {
        float t = (k + noise2) / samples;
        float a = angle + t * 9.0 * 2.0 * PI;
        float2 suv = uv + float2(cos(a), sin(a)) * (radiusPx * t + 1.0) * u_Screen.zw * 2.0;
        float3 v = viewPosition(suv, t0_tex(suv).w) - p;
        float vv = dot(v, v);
        float cosine = dot(v, n) * rsqrt(vv + 1e-6);
        float minDist = 0.004 * p.z;
        occlusion += saturate(cosine - 0.12) * saturate(1.0 - vv / (radius * radius)) * saturate(vv / (minDist * minDist) - 1.0);
    }
    float ao = saturate(1.0 - occlusion * 2.2 / samples);
    ao = ao * ao;
    ao = lerp(1.0, ao, saturate(1.5 - p.z / (250.0 * u_Water.w)));

    // --- Contact shadows: short screen-space rays towards the engine's sun ---
    float sunVis = 1.0;
    float3 l = worldToViewDir(u_SunOut.yzw);
    float ndl = dot(n, l);
    if (u_Engine2.x > 0.5 && ndl > 0.0 && u_Light.z > 0.0)
    {
        float steps = u_Quality.y;
        float maxLen = u_Light.w;
        float3 origin = p + n * (0.02 + p.z * 0.002);
        float hit = 0.0;
        [loop] for (int s = 0; s < (int)steps; s++)
        {
            float t = (s + noise) / steps;
            t = t * t * maxLen + 0.05;
            float3 q = origin + l * t;
            if (q.z <= 0.06) break;
            float2 quv = projectToUV(q);
            if (any(quv < 0.0) || any(quv > 1.0)) break;
            float sceneZ = engineZ(quv);
            float delta = q.z - sceneZ;
            if (delta > 0.015 + q.z * 0.002 && delta < 0.3 + t * 0.12)
            {
                float edge = min(min(quv.x, 1.0 - quv.x), min(quv.y, 1.0 - quv.y));
                hit = saturate(edge * 20.0);
                break;
            }
        }
        hit *= saturate(ndl * 6.0);
        sunVis = 1.0 - hit * saturate(1.5 - p.z / (300.0 * u_Water.w));
    }
    return float4(ao, sunVis, 1, 1);
}

// Depth-aware separable blur (half res). t0 = AO/shadow, t1 = half normal/depth,
// u_Pass0.xy = direction * texel.
float4 PS_BilateralBlur(VSOut i) : SV_Target
{
    float4 centreND = t1_tex(i.uv);
    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int k = -4; k <= 4; k++)
    {
        float2 suv = i.uv + u_Pass0.xy * k;
        float4 nd = t1_tex(suv);
        float w = exp(-k * k / 10.0);
        w *= exp(-abs(nd.w - centreND.w) / (0.02 * centreND.w + 0.05));
        w *= pow(saturate(dot(nd.xyz, centreND.xyz)), 4.0) + 0.001;
        sum += t0_tex(suv).rgb * w;
        wsum += w;
    }
    return float4(sum / max(wsum, 1e-4), 1);
}

// ---------------------------------------------------------------------------------
// Bounce light (quarter res): every visible surface facing this one lights it with its colour.
//   t0 = half normal/depth, t1 = half colour. u_Pass0.x = samples, u_Pass0.y = radius (m),
//   u_Pass0.z = frame. out.rgb = bounced light (display linear)
// ---------------------------------------------------------------------------------
float4 PS_GI(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float4 nd = t0_tex(uv);
    if (nd.w >= SKY_Z) return 0;
    float3 p = viewPosition(uv, nd.w);
    float3 n = nd.xyz;
    float2 seed = i.pos.xy + u_Pass0.z * 5.588238;
    float noise = frac(52.9829189 * frac(dot(seed, float2(0.06711056, 0.00583715))));
    float noise2 = frac(hash12(i.pos.xy + 41.7) + u_Pass0.z * 0.618034);
    float radius = u_Pass0.y;
    float projScale = abs(u_Proj.y) * 0.5 * u_Screen.y * 0.25;
    float radiusPx = clamp(radius * projScale / p.z, 2.0, u_Screen.y * 0.08);
    float samples = u_Pass0.x;
    float3 sum = 0;
    float angle = noise * 2.0 * PI;
    [loop] for (int k = 0; k < (int)samples; k++)
    {
        float t = (k + noise2) / samples;
        float a = angle + t * 7.0 * 2.0 * PI;
        float2 suv = uv + float2(cos(a), sin(a)) * (radiusPx * sqrt(t) + 1.0) * u_Screen.zw * 4.0;
        if (any(suv < 0.0) || any(suv > 1.0)) continue;
        float4 snd = t0_tex(suv);
        if (snd.w >= SKY_Z) continue;
        float3 d = viewPosition(suv, snd.w) - p;
        float dd = dot(d, d) + 1e-4;
        float3 dir = d * rsqrt(dd);
        float w = saturate(dot(n, dir)) * saturate(dot(snd.xyz, -dir)) * radius * radius / max(dd, 0.1 * radius * radius);
        sum += t1_texL(suv).rgb * w;
    }
    float fade = saturate(2.0 - p.z / (120.0 * u_Water.w));
    return float4(min(sum / samples * fade, 4.0), 1);
}

// GI history: t0 = this frame's GI, t1 = history (rgb, view z), t2 = half normal/depth,
// u_Pass0.x = history valid.
float4 PS_GITemporal(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float3 current = t0_tex(uv).rgb;
    float z = t2_tex(uv).w;
    if (u_Pass0.x < 0.5 || z >= SKY_Z) return float4(current, z);
    float prevZ;
    float2 prevUV = reproject(worldPosition(viewPosition(uv, z)), prevZ);
    if (any(prevUV < 0.0) || any(prevUV > 1.0)) return float4(current, z);
    float4 history = t1_texL(prevUV);
    float error = abs(history.a - prevZ) / max(prevZ, 0.1);
    float weight = lerp(0.1, 1.0, smoothstep(0.03, 0.1, error));
    float2 t = u_Screen.zw * 4.0;
    float3 lo = current, hi = current;
    [unroll] for (int k = 0; k < 4; k++)
    {
        float2 o = k == 0 ? float2(t.x, 0) : (k == 1 ? float2(-t.x, 0) : (k == 2 ? float2(0, t.y) : float2(0, -t.y)));
        float3 n = t0_tex(uv + o * 1.5).rgb;
        lo = min(lo, n);
        hi = max(hi, n);
    }
    float3 margin = (hi - lo) * 0.25 + 0.01;
    history.rgb = clamp(history.rgb, lo - margin, hi + margin);
    return float4(lerp(history.rgb, current, weight), z);
}

// ---------------------------------------------------------------------------------
// What the engine's light doesn't explain: lamps, neon, screens. Light maps can add ambient too:
// up to `room` times the engine's ambient counts as lighting. The sun's glint (on chrome, glass,
// the rails) is no light source: white light in the sun's mirror direction stays lighting, only
// clearly coloured light (neon) counts there.
//   rest = what is left after the engine's sun and ambient, nWorld/rd for the mirror direction.
// ---------------------------------------------------------------------------------
float3 emissiveOf(float3 rest, float3 albedo, float e, float3 nWorld, float3 rd)
{
    float3 room = 2.0 * e * (albedo * u_Ambient.rgb * 1.6 + 0.02 * luma(u_Ambient.rgb));
    float3 light = max(rest - room, 0.0);
    float peak = max(light.r, max(light.g, light.b));
    float sat = (peak - min(light.r, min(light.g, light.b))) / max(peak, 1e-4);
    float mirror = u_Engine2.x > 0.5 ? smoothstep(0.55, 0.9, dot(reflect(rd, nWorld), u_SunOut.yzw)) : 0.0;
    light *= 1.0 - mirror * (1.0 - smoothstep(0.55, 0.85, sat));
    // Reflections of the sky (car paint, glass) are as bright as neon but pale: neon is pure colour
    // (capture: neon rest saturation 0.8-1.0, the car's sky reflection 0.4). White lamps count in part.
    light *= lerp(0.3, 1.0, smoothstep(0.45, 0.8, sat));
    // On coloured paint the sky's reflection leaves the opposite colour behind (red paint: its red
    // goes to the lit part, cyan stays): light against the material's own colour is a reflection.
    float3 chromaA = albedo - luma(albedo);
    float3 chromaL = light - luma(light);
    float albedoSat = saturate(length(chromaA) / max(luma(albedo), 0.02) * 0.8);
    float against = saturate(-dot(chromaA, chromaL) * rsqrt(max(dot(chromaA, chromaA) * dot(chromaL, chromaL), 1e-10)));
    light *= 1.0 - against * albedoSat;
    // Light of the sun's own colour (the sunset's orange on rails and glass) is its reflection.
    float3 sunC = u_EngineSun.rgb - luma(u_EngineSun.rgb);
    float sunSat = saturate(length(sunC) / max(luma(u_EngineSun.rgb), 1e-3));
    float likeSun = dot(sunC, chromaL) * rsqrt(max(dot(sunC, sunC) * dot(chromaL, chromaL), 1e-10));
    if (u_Engine2.x > 0.5) light *= 1.0 - smoothstep(0.75, 0.95, likeSun) * sunSat * 0.9;
    return light;
}

// ---------------------------------------------------------------------------------
// Neon light spill (quarter res): coloured lights light up the surfaces around them.
//   out.rgb = light, out.a = view z
// ---------------------------------------------------------------------------------
float4 PS_SpillDown(VSOut i) : SV_Target
{
    float3 sum = 0, strongest = 0;
    float zmin = SKY_Z;
    float e = u_Misc.x > 0.5 ? 1.0 : engineExposure();
    [unroll] for (int k = 0; k < 4; k++)
    {
        float2 suv = i.uv + (float2(k & 1, k >> 1) * 2.0 - 1.0) * u_Screen.zw;
        float z = engineZ(suv);
        if (z >= SKY_Z) continue;
        float3 c = engineColour(suv);
        float3 albedo = u_Engine.y > 0.5 ? engineAlbedo(suv) : c;
        float3 n = surfaceNormal(suv, z);
        float3 sunPart, ambPart;
        float shadow;
        engineLight(suv, n, albedo, sunPart, ambPart, shadow);
        float3 rd = viewToWorldDir(normalize(viewPosition(suv, 1.0)));
        float3 light = emissiveOf(max(c - sunPart - ambPart, 0.0), albedo, e, viewToWorldDir(n), rd);
        // Coloured light lights its surroundings most; white lamps a little (white is also
        // the sun's glint on chrome and glass).
        float peak = max(light.r, max(light.g, light.b));
        float sat = (peak - min(light.r, min(light.g, light.b))) / max(peak, 1e-4);
        light *= lerp(0.35, 1.0, smoothstep(0.3, 0.7, sat));
        light = min(light, 8.0);
        sum += light;
        strongest = max(strongest, light);
        zmin = min(zmin, z);
    }
    return float4(lerp(sum * 0.25, strongest, 0.5), zmin);
}

// t0 = light buffer, u_Pass0.xy = direction * texel. Radius scales with 1/z (about 3 m).
float4 PS_SpillBlur(VSOut i) : SV_Target
{
    float4 centre = t0_tex(i.uv);
    float z = min(centre.a, 2000.0);
    float pxPerMetre = abs(u_Proj.y) * u_Screen.y * 0.25 * 0.5 / z;
    float stepPx = clamp(3.0 * pxPerMetre / 6.0, 1.6, 12.0);
    float3 sum = 0;
    [unroll] for (int k = -6; k <= 6; k++)
    {
        float4 s = t0_texL(i.uv + u_Pass0.xy * k * stepPx);
        float w = exp(-k * k / 18.0) * exp(-abs(s.a - centre.a) / (0.25 * z + 0.5));
        sum += s.rgb * w;
    }
    return float4(sum / 6.4, centre.a);
}

// ---------------------------------------------------------------------------------
// Lighting composite (full res): the result, display-linear HDR.
//   t0 = AO/contact shadow (half, blurred), t1 = half normal/depth, t2 = GI (quarter),
//   t3 = custom sky (full, from sky.hlsl), t4 = sky average (1x1), t5 = clouds (half),
//   t6 = neon light (quarter), t7 = reflections (half)
//   u_Pass0: x = sky texture present, y = sky average present, z = clouds present, w = reflections present
// ---------------------------------------------------------------------------------
float3 upsampleOcclusion(float2 uv, float z, float3 n)
{
    float2 halfTexel = u_Screen.zw * 2.0;
    float2 base = (floor(uv / halfTexel - 0.5) + 0.5) * halfTexel;
    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int j = 0; j < 2; j++)
    {
        [unroll] for (int k = 0; k < 2; k++)
        {
            float2 suv = base + float2(k, j) * halfTexel;
            float4 nd = t1_tex(suv);
            float2 f = 1.0 - abs(uv - suv) / halfTexel;
            float w = max(f.x * f.y, 0.001);
            w *= exp(-abs(nd.w - z) / (0.03 * z + 0.05));
            w *= pow(saturate(dot(nd.xyz, n)), 8.0) + 0.002;
            sum += t0_tex(suv).rgb * w;
            wsum += w;
        }
    }
    return wsum > 1e-4 ? sum / wsum : 1.0;
}

float3 upsampleGI(float2 uv, float z, float3 n)
{
    float2 texel = u_Screen.zw * 4.0;
    float2 base = (floor(uv / texel - 0.5) + 0.5) * texel;
    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int j = 0; j < 2; j++)
    {
        [unroll] for (int k = 0; k < 2; k++)
        {
            float2 suv = base + float2(k, j) * texel;
            float4 nd = t1_tex(suv);
            float2 f = 1.0 - abs(uv - suv) / texel;
            float w = max(f.x * f.y, 0.001);
            w *= exp(-abs(nd.w - z) / (0.03 * z + 0.05));
            w *= pow(saturate(dot(nd.xyz, n)), 8.0) + 0.002;
            sum += t2_tex(suv).rgb * w;
            wsum += w;
        }
    }
    return wsum > 1e-4 ? sum / wsum : 0.0;
}

float3 upsampleSpill(float2 uv, float z)
{
    float2 t = u_Screen.zw * 4.0;
    float3 sum = 0;
    float wsum = 0.0;
    [unroll] for (int k = 0; k < 4; k++)
    {
        float2 o = float2(k & 1 ? 0.5 : -0.5, k & 2 ? 0.5 : -0.5) * t;
        float4 s = t6_texL(uv + o);
        float w = exp(-abs(s.a - z) / (0.06 * z + 0.15));
        sum += s.rgb * w;
        wsum += w;
    }
    return sum / max(wsum, 0.5);
}

// The colour of the haze: the sky near the horizon (sky average), else the engine's fog colour.
float4 hazeColour()
{
    float e = u_Misc.x > 0.5 ? 1.0 : engineExposure();
    float3 fog = u_Fog.rgb * e;
    float4 h = float4(luma(fog) > 1e-4 ? fog : u_SkyColor.rgb * 0.6, 0.5);
    if (u_Pass0.y > 0.5) h = t4_tex(float2(0.5, 0.5));
    return h;
}

// In-scattered sun light from the volumetric pass (volumetric.hlsl): dark gaps where shadows cross
// the haze, bright shafts in between. Returns (extra light, lit fraction for the regular haze).
float4 volumetricLight(float2 uv, float3 rd, float daylight)
{
    float4 r = float4(0, 0, 0, 1);
    if (u_Volume.x > 0.0 && u_Pass1.x > 0.5)
    {
        float2 v = g_volume.SampleLevel(g_linear, uv, 0).rg;
        float mu = dot(rd, u_SunWorld.xyz);
        float phase = henyeyGreenstein(mu, 0.6) * 0.6 + 0.05;
        float haze = 1.0 - exp(-v.y * u_Volume.y * (u_Atmo.x * 0.0009 + 0.0015) * 3.0);
        r = float4(u_SunColor.rgb * tmnfScale() * daylight * phase * haze * u_Volume.x, lerp(1.0, v.x, saturate(u_Volume.x)));
    }
    return r;
}

float4 PS_Lighting(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float3 c = engineColour(uv);
    float z = engineZ(uv);
    float3 rdView = normalize(viewPosition(uv, 1.0));
    float3 rd = viewToWorldDir(rdView);
    float daylight = u_SunWorld.w;
    float night = u_Sky.y;
    float4 skyAvg = hazeColour();
    int debug = (int)(u_Post.w + 0.5);

    if (z >= SKY_Z)
    {
        if (debug >= 1 && debug <= 4) return float4(0, 0, 0, 1);
        c = skyPixel(uv, rd, c, skyAvg, daylight, night);
        if (u_SunView.w > 0.5) c += volumetricLight(uv, rd, daylight).rgb;
        return float4(weatherAir(uv, rd, SKY_Z, c, skyAvg, 0.0, float3(0, 1, 0)), 1);
    }

    float e = u_Misc.x > 0.5 ? 1.0 : engineExposure();
    float3 n = surfaceNormal(uv, z);
    float3 nWorld = viewToWorldDir(n);
    float3 p = viewPosition(uv, z);
    float dist = length(p);
    float3 albedo = u_Engine.y > 0.5 ? engineAlbedo(uv) : c;

    // The engine's light, taken apart. With every effect at zero the parts add up to the
    // engine's image exactly.
    float3 sunPart, ambPart;
    float engineShadow;
    engineLight(uv, n, albedo, sunPart, ambPart, engineShadow);
    if (u_Engine.z < 0.5) // no light mask: the lit image as it is
    {
        sunPart = 0;
        ambPart = 0;
        engineShadow = 1;
    }
    sunPart = min(sunPart, c);
    ambPart = min(ambPart, c - sunPart);
    float3 rest = c - sunPart - ambPart;
    float3 emissive = emissiveOf(rest, albedo, e, nWorld, rd);
    float3 restLight = rest - emissive; // light maps, local lights, reflections

    float3 occ = upsampleOcclusion(uv, z, n);
    float ao = lerp(1.0, occ.r, u_Light.x);
    float sunVis = lerp(1.0, occ.g, saturate(u_Light.z));

    Surface surf;
    surf.uv = uv;
    surf.pixel = i.pos.xy;
    surf.p = p;
    surf.n = n;
    surf.nWorld = nWorld;
    surf.world = worldPosition(p);
    surf.rd = rd;
    surf.dist = dist;
    surf.z = z;
    surf.albedo = albedo;
    surf.sunShadow = engineShadow * sunVis;
    surf.ao = ao;

    // Sun: contact shadows, the preset's warmth; it goes under a night sky (the moon stays where
    // the sun was, cold and dim).
    float3 sunTint = u_SunColor.rgb / max(luma(u_SunColor.rgb), 1e-3);
    float3 skyTint = u_SkyColor.rgb / max(luma(u_SkyColor.rgb), 1e-3);
    float3 sun = sunPart * sunVis * lerp(float3(1, 1, 1), sunTint, u_SunColor.w * 0.35);
    float3 moon = sunPart * sunVis * float3(0.35, 0.5, 0.85) * 0.05;
    sun = lerp(sun, moon, saturate(u_SunOut.x));
    // Ambient, light maps and reflections: occlusion, the sky's tint (more in shadow).
    float shade = 1.0 - saturate(engineShadow);
    float3 ambient = (ambPart + restLight) * ao * lerp(float3(1, 1, 1), skyTint, u_SkyColor.w * (0.15 + 0.25 * shade));
    // Bounce light and neon light: albedo times the light that arrives.
    if (u_Volume.z > 0.0) ambient += albedo * upsampleGI(uv, z, n) * u_Volume.z * 1.6 * lerp(0.5, 1.0, occ.r);
    if (u_Light2.x > 0.0)
        ambient += albedo * upsampleSpill(uv, z) * u_Light2.x * 3.0 * lerp(1.0, 2.0, saturate(dist / 120.0)) * ao;
    // Light sources: as bright as the engine drew them, the preset's highlight boost on top.
    float3 glow = emissive * lerp(1.0, u_Rays.w / 3.5, 0.5);

    weatherSurface(surf, sun, ambient, glow);
    float3 lit = weatherReflect(surf, sun + ambient, skyAvg);

    // Day-for-night: darker, cooler, less saturated - but lights keep glowing.
    float3 unlit = lit;
    lit = lerp(lit, luma(lit) * float3(0.55, 0.72, 1.0), night * 0.55) * lerp(1.0, 0.16, night);

    // Aerial perspective: exponential height fog lit by the sky and the sun.
    float density = (u_Atmo.x * 0.0009 + u_Snow.x * 0.005) * u_Sky3.w;
    float falloff = max(u_Atmo.y * 0.012, 1e-4);
    float camY = u_UpView.w;
    float rdy = rd.y;
    float fogAmount = abs(rdy) > 1e-4 ? density * exp(-camY * falloff) * (1.0 - exp(-dist * rdy * falloff)) / (rdy * falloff)
                                      : density * exp(-camY * falloff) * dist;
    fogAmount = saturate(1.0 - exp(-max(fogAmount, 0.0)));
    lit += skyLightOnScene(unlit, nWorld, rd, fogAmount);
    // The preset's colours are in TMNF's units, the sky average already in ours (tmnfScale, sky.hlsli).
    float3 fogColour = lerp(u_SkyColor.rgb * tmnfScale() * 0.7 * (0.3 + 0.7 * daylight), skyAvg.rgb, skyAvg.a * 0.75);
    float4 volume = volumetricLight(uv, rd, daylight);
    if (u_SunView.w > 0.5) fogColour += sunScatter(rd, u_Atmo.z * 0.6) * tmnfScale() * daylight * volume.a;
    fogColour *= lerp(1.0, 0.2, night);
    float3 result = lerp(lit, fogColour, fogAmount) + glow * (1.0 - 0.6 * fogAmount);
    if (u_SunView.w > 0.5) result += volume.rgb;
    result = weatherAir(uv, rd, dist, result, skyAvg, fogAmount, nWorld);

    if (debug == 1) return float4(pow(saturate(1.0 - dist / 500.0), 2.0).xxx, 1);
    if (debug == 2) return float4(nWorld * 0.5 + 0.5, 1);
    if (debug == 3) return float4(occ.rrr, 1);
    if (debug == 4) return float4(occ.ggg * engineShadow, 1);
    if (debug == 9) return float4(sunPart, 1);
    if (debug == 10) return float4(ambPart, 1);
    if (debug == 11) return float4(rest, 1);
    if (debug == 12) return float4(emissive, 1);
    return float4(result, 1);
}
