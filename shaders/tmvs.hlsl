// TM Vibrant Shaders - deferred-style post pipeline for TrackMania Forever (Direct3D 9, ps_3_0).
//
// The game renders a finished LDR image plus a depth buffer (made readable through INTZ).
// From depth and the engine's camera matrices this pipeline reconstructs view-space
// position and normals, then re-lights the frame: ambient occlusion, ray-marched sun
// shadows, directional sun light and sky-coloured ambient, aerial perspective,
// volumetric light shafts, physically based bloom, auto exposure, filmic grading and
// post anti-aliasing. Each entry point below is one full-screen pass (see pipeline.cpp).

// ---------------------------------------------------------------------------------
// Frame constants (c0-c19, uploaded once per frame)
// ---------------------------------------------------------------------------------
float4 u_Screen    : register(c0);  // w, h, 1/w, 1/h (full resolution)
float4 u_Proj      : register(c1);  // P00, P11, P20, P21
float4 u_Proj2     : register(c2);  // P22, P32, time (s), frame index
float4 u_ViewToW0  : register(c3);  // view->world rotation row 0, camera world x
float4 u_ViewToW1  : register(c4);  // row 1, camera world y
float4 u_ViewToW2  : register(c5);  // row 2, camera world z
float4 u_SunView   : register(c6);  // sun direction (view space), sun known
float4 u_SunWorld  : register(c7);  // sun direction (world space), daylight factor
float4 u_SunScreen : register(c8);  // sun uv, on-screen fade, in front of camera
float4 u_SunColor  : register(c9);  // rgb, sun light strength
float4 u_SkyColor  : register(c10); // rgb, ambient tint strength
float4 u_Light     : register(c11); // AO strength, AO radius (m), shadow strength, shadow length (m)
float4 u_Atmo      : register(c12); // fog density, height falloff, sun scatter, sky enhance
float4 u_Rays      : register(c13); // god rays, decay, sun glow, highlight boost
float4 u_Bloom     : register(c14); // bloom, radius, lens flare, chromatic aberration
float4 u_Grade0    : register(c15); // exposure mul, auto exposure, contrast, saturation
float4 u_Grade1    : register(c16); // vibrance, temperature, tint, split toning
float4 u_Grade2    : register(c17); // lift, gamma, gain, vignette
float4 u_Post      : register(c18); // film grain, sharpen, fxaa, debug view
float4 u_UpView    : register(c19); // world up in view space, camera height
float4 u_Sky       : register(c20); // sky mode, night amount, sky rotation (rad), sky brightness
float4 u_Sky2      : register(c21); // cloud amount, star amount, effect size, planet size
float4 u_Quality   : register(c22); // AO samples, shadow steps, cloud steps, long shadow steps
float4 u_Weather   : register(c23); // wetness, rain, puddles, water surfaces
float4 u_Cine      : register(c24); // motion blur (shutter), depth of field, focus distance (0 = auto), max blur (px at 1080p)
float4 u_Nature    : register(c25); // grass detail, mowing stripes, wind, dry reflections
float4 u_Clouds    : register(c26); // volumetric clouds, coverage, base height (m), thickness (m)
float4 u_Planet    : register(c27); // planet type, view (0 distant, 1 ring plane), azimuth, elevation (rad)
float4 u_PrevView0 : register(c28); // world -> previous frame's view space: column 0 (rotation, translation)
float4 u_PrevView1 : register(c29); // column 1
float4 u_PrevView2 : register(c30); // column 2
float4 u_PrevProj  : register(c31); // previous frame's P00, P11, P20, P21

// Per-pass constants
float4 u_Pass0 : register(c32);
float4 u_Pass1 : register(c33);

float4 u_Temporal  : register(c34); // TAA on, history valid, noise frame (0..63), long shadows
float4 u_HeightMap : register(c35); // world x/z of the height map corner, world size (m), long shadow range (m)
float4 u_Light2    : register(c36); // neon light spill, game sun direction known, lightning flash, lightning bolt
float4 u_Water     : register(c38); // water heights (world y): blocks, sea; z: 1 = known, 0 = no water, -1 = guess by colour
float4 u_Snow      : register(c39); // snowfall, snow cover, 0, 0
float4 u_Volume    : register(c37); // volumetric light strength, march range (m), global illumination, lens drops

sampler2D s0 : register(s0);
sampler2D s1 : register(s1);
sampler2D s2 : register(s2);
sampler2D s3 : register(s3);
sampler2D s4 : register(s4);
sampler2D s5 : register(s5);
sampler2D s6 : register(s6);
sampler3D s7 : register(s7); // tiling cloud noise (volume texture)
sampler2D s8 : register(s8);
sampler2D s9 : register(s9);
sampler2D s10 : register(s10); // volumetric light (lighting pass)
sampler2D s11 : register(s11); // global illumination (lighting pass)

static const float PI = 3.14159265;
static const float SKY_Z = 60000.0;
static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);

// ---------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------
float3 toLinear(float3 c) { return c * (c * (c * 0.305306011 + 0.682171111) + 0.012522878); }
float3 toSRGB(float3 c) {
    c = max(c, 0.0);
    float3 s1 = sqrt(c), s2 = sqrt(s1), s3 = sqrt(s2);
    return saturate(0.585122381 * s1 + 0.783140355 * s2 - 0.368262736 * s3);
}
float luma(float3 c) { return dot(c, LUMA); }

float linearDepth(float d) {
    // Hardware depth -> view z. TrackMania uses an infinite far plane (P22 = 1).
    return (d >= 0.999999) ? SKY_Z : u_Proj2.y / (d - u_Proj2.x);
}

float3 viewPosition(float2 uv, float z) {
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return float3((ndc.x - u_Proj.z) * z / u_Proj.x, (ndc.y - u_Proj.w) * z / u_Proj.y, z);
}

float2 projectToUV(float3 p) {
    float2 ndc = float2(p.x * u_Proj.x / p.z + u_Proj.z, p.y * u_Proj.y / p.z + u_Proj.w);
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

float3 viewToWorldDir(float3 v) { return v.x * u_ViewToW0.xyz + v.y * u_ViewToW1.xyz + v.z * u_ViewToW2.xyz; }
float3 worldToViewDir(float3 w) { return float3(dot(w, u_ViewToW0.xyz), dot(w, u_ViewToW1.xyz), dot(w, u_ViewToW2.xyz)); }
float3 cameraWorld() { return float3(u_ViewToW0.w, u_ViewToW1.w, u_ViewToW2.w); }
float3 worldPosition(float3 viewPos) { return cameraWorld() + viewToWorldDir(viewPos); }

// Where a (static) world point was on screen in the previous frame. prevZ = its depth then.
float2 reproject(float3 world, out float prevZ) {
    float4 w = float4(world, 1.0);
    float3 v = float3(dot(w, u_PrevView0), dot(w, u_PrevView1), dot(w, u_PrevView2));
    prevZ = v.z;
    float2 ndc = float2(v.x * u_PrevProj.x / v.z + u_PrevProj.z, v.y * u_PrevProj.y / v.z + u_PrevProj.w);
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

float ign(float2 pixel) {
    // Interleaved gradient noise. With TAA on it changes every frame, so the temporal
    // filter averages it out (more effective samples for AO, shadows, clouds).
    pixel += u_Temporal.z * 5.588238;
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float hash12(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float henyeyGreenstein(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(abs(1.0 + g2 - 2.0 * g * cosTheta), 1.5));
}

// ---------------------------------------------------------------------------------
// Pass 0: linear depth with a 5-tap median (full res, R32F)
//   s0 = hardware depth (INTZ). TrackMania has coplanar layers that z-fight a few cm
//   apart (ground decals); the median removes that pixel noise before normals/AO.
// ---------------------------------------------------------------------------------
float rawZ(float2 uv) { return linearDepth(tex2Dlod(s0, float4(uv, 0, 0)).r); }

float4 PS_LinearDepth(float2 uv : TEXCOORD0) : COLOR0 {
    float2 px = u_Screen.zw;
    float c = rawZ(uv);
    // Snap to the closest layer among neighbours that are within z-fighting distance,
    // so interleaved coplanar layers read as one surface. Real edges are left alone.
    float z = c;
    float tolerance = 0.006 * c;
    [unroll] for (int y = -1; y <= 1; y++) {
        [unroll] for (int x = -1; x <= 1; x++) {
            float n = rawZ(uv + float2(x, y) * px);
            if (abs(n - c) < tolerance) z = min(z, n);
        }
    }
    return float4(z, 0, 0, 1);
}

// ---------------------------------------------------------------------------------
// Pass 1: reconstructed view-space normals (full res)
//   s0 = linear depth (R32F)
//   out = normal.xyz (view space, facing camera), view z (SKY_Z for sky)
// ---------------------------------------------------------------------------------
float sampleZ(float2 uv) { return tex2Dlod(s0, float4(uv, 0, 0)).r; }

float4 PS_Prepare(float2 uv : TEXCOORD0) : COLOR0 {
    float2 px = u_Screen.zw;
    float zc = sampleZ(uv);
    if (zc >= SKY_Z) return float4(0, 0, 0, SKY_Z);

    float zl = sampleZ(uv - float2(px.x, 0)), zr = sampleZ(uv + float2(px.x, 0));
    float zu = sampleZ(uv - float2(0, px.y)), zd = sampleZ(uv + float2(0, px.y));
    float zl2 = sampleZ(uv - float2(2 * px.x, 0)), zr2 = sampleZ(uv + float2(2 * px.x, 0));
    float zu2 = sampleZ(uv - float2(0, 2 * px.y)), zd2 = sampleZ(uv + float2(0, 2 * px.y));

    // Pick the neighbour that best continues the surface (avoids normals bleeding over edges).
    float3 pc = viewPosition(uv, zc);
    bool useLeft = abs(2 * zl - zl2 - zc) < abs(2 * zr - zr2 - zc);
    bool useUp = abs(2 * zu - zu2 - zc) < abs(2 * zd - zd2 - zc);
    float3 dx = useLeft ? pc - viewPosition(uv - float2(px.x, 0), zl) : viewPosition(uv + float2(px.x, 0), zr) - pc;
    float3 dy = useUp ? pc - viewPosition(uv - float2(0, px.y), zu) : viewPosition(uv + float2(0, px.y), zd) - pc;

    float3 n = normalize(cross(dx, dy));
    if (dot(n, pc) > 0) n = -n; // face the camera regardless of the projection's handedness
    return float4(n, zc);
}

// Half-resolution copy of the normal/depth buffer (closest of each 2x2 block).
//   s0 = full normal/depth, u_Pass0.xy = full-res texel size
float4 PS_DownsampleND(float2 uv : TEXCOORD0) : COLOR0 {
    float2 o = u_Pass0.xy * 0.5;
    float4 a = tex2Dlod(s0, float4(uv + float2(-o.x, -o.y), 0, 0));
    float4 b = tex2Dlod(s0, float4(uv + float2(o.x, -o.y), 0, 0));
    float4 c = tex2Dlod(s0, float4(uv + float2(-o.x, o.y), 0, 0));
    float4 d = tex2Dlod(s0, float4(uv + float2(o.x, o.y), 0, 0));
    float4 r = a;
    if (b.w < r.w) r = b;
    if (c.w < r.w) r = c;
    if (d.w < r.w) r = d;
    return r;
}

// ---------------------------------------------------------------------------------
// Long-range shadows: a world-space height map (top view around the camera) is built
// from the depth of every frame (pipeline.cpp splats the depth buffer into it). The sun
// ray is traced through it, so shadows can be long and casters can be off screen.
// Stored heights are offset by +10000 m; 0 = never seen.
// ---------------------------------------------------------------------------------
// s0 = previous height map, s1 = this frame's splat, u_Pass0.xy = uv shift of the map
// origin since last frame, u_Pass0.z = 1 to drop the history, u_Pass0.w = decay (m)
float4 PS_HeightMerge(float2 uv : TEXCOORD0) : COLOR0 {
    float2 puv = uv + u_Pass0.xy;
    float previous = (u_Pass0.z > 0.5 || any(puv < 0.0) || any(puv > 1.0)) ? 0.0 : tex2Dlod(s0, float4(puv, 0, 0)).r;
    float seen = tex2Dlod(s1, float4(uv, 0, 0)).r;
    if (seen <= 0.0) return float4(previous, 0, 0, 1);
    // Seen again: the new height wins, but walls only sink slowly. Points that hit a wall
    // top in one frame may miss it in the next; moving cars must not leave a ridge behind.
    return float4(previous > 0.0 ? max(seen, previous - u_Pass0.w) : seen, 0, 0, 1);
}

float4 PS_HeightSplat(float2 data : TEXCOORD0) : COLOR0 {
    return float4(data.x, 0, 0, 1);
}

// s2 = height map. Returns 0 (lit) .. 1 (shadowed).
float longShadow(float3 pView, float3 nView, float noise) {
    float3 sun = u_SunWorld.xyz;
    if (sun.y < 0.02) return 0.0;
    float3 origin = worldPosition(pView) + viewToWorldDir(nView) * 0.2;
    float range = u_HeightMap.w;
    float steps = u_Quality.w;
    float occlusion = 0.0;
    [loop] for (int i = 0; i < (int)steps; i++) {
        float t = (i + noise) / steps;
        t = t * t * range + 0.7;
        float3 q = origin + sun * t;
        float2 m = (q.xz - u_HeightMap.xy) / u_HeightMap.z;
        if (any(m < 0.0) || any(m > 1.0)) break;
        float h = tex2Dlod(s2, float4(m, 0, 0)).r;
        if (h > 0.0) {
            // Soft edge that widens with distance (penumbra of the sun disc).
            float above = h - 10000.0 - q.y - (0.35 + t * 0.012);
            occlusion = max(occlusion, saturate(above / (0.3 + t * 0.04)));
            if (occlusion > 0.99) break;
        }
    }
    return occlusion;
}

// ---------------------------------------------------------------------------------
// Volumetric light: sun shafts with real shadows in the haze, also with the sun off screen.
// 1. Shadow height map (256^2 over the height map's area): for every column, the height
//    below which the sun is blocked. One walk towards the sun per column, so the march
//    along the view ray below needs a single lookup per step.
//    s0 = height map. out.r = shadow height + 10000 (0 = nothing known: lit)
// ---------------------------------------------------------------------------------
float4 PS_ShadowHeight(float2 uv : TEXCOORD0) : COLOR0 {
    float3 sun = u_SunWorld.xyz;
    float hl = length(sun.xz);
    if (sun.y < 0.02 || hl < 1e-3) return 0;
    float2 dir = sun.xz / hl / u_HeightMap.z; // uv per metre towards the sun
    float slope = sun.y / hl;                 // the sun ray climbs this much per metre
    float best = 0.0;
    [loop] for (int i = 0; i < 40; i++) {
        float d = pow((i + 0.5) / 40.0, 1.5) * 160.0;
        float2 m = uv + dir * d;
        if (any(m < 0.0) || any(m > 1.0)) break;
        float h = tex2Dlod(s0, float4(m, 0, 0)).r;
        if (h > 0.0) best = max(best, h - d * slope);
    }
    return float4(best, 0, 0, 1);
}

// 2. March the view ray through the haze (half res). s0 = half normal/depth,
//    s1 = shadow height map, u_Pass0.x = steps.
//    out.r = lit fraction of the haze along the ray, out.g = lit path length / range
float4 PS_Volumetric(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {
    float z = tex2Dlod(s0, float4(uv, 0, 0)).w;
    float3 rdView = normalize(viewPosition(uv, 1.0));
    float dist = z >= SKY_Z ? 1e6 : z / max(rdView.z, 1e-3);
    float range = min(dist, u_Volume.y);
    float3 rd = viewToWorldDir(rdView);
    float3 cam = cameraWorld();
    float steps = u_Pass0.x;
    float noise = ign(vpos);
    float falloff = max(u_Atmo.y * 0.012, 1e-4);
    float lit = 0.0, total = 0.0, prev = 0.0;
    [loop] for (int i = 0; i < (int)steps; i++) {
        // More samples close to the camera, where shafts are sharp and large.
        float f = (i + noise) / steps;
        float t = f * f * range;
        float dt = t - prev;
        prev = t;
        float3 p = cam + rd * t;
        float2 m = (p.xz - u_HeightMap.xy) / u_HeightMap.z;
        float sh = (any(m < 0.0) || any(m > 1.0)) ? 0.0 : tex2Dlod(s1, float4(m, 0, 0)).r;
        float l = sh > 0.0 ? saturate((p.y - (sh - 10000.0)) * 0.7 + 0.5) : 1.0;
        float w = dt * exp(-max(p.y - cam.y, 0.0) * falloff); // thinner haze higher up
        lit += l * w;
        total += w;
    }
    // The rest of the ray up to `range` (after the last sample) counts as lit.
    float rest = max(range - prev, 0.0);
    lit += rest;
    total += rest;
    return float4(total > 0.0 ? lit / total : 1.0, lit / u_Volume.y, 0, 1);
}

// In-scattered sun light from the volumetric pass: dark gaps where shadows cross the haze,
// bright shafts in between. Returns (extra light, lit fraction for the regular haze).
float4 volumetricLight(float2 uv, float3 rd, float daylight) {
    if (u_Volume.x <= 0.0) return float4(0, 0, 0, 1);
    float2 v = tex2Dlod(s10, float4(uv, 0, 0)).rg;
    float mu = dot(rd, u_SunWorld.xyz);
    float phase = henyeyGreenstein(mu, 0.6) * 0.6 + 0.05;
    float haze = 1.0 - exp(-v.y * u_Volume.y * (u_Atmo.x * 0.0009 + 0.0015) * 3.0);
    float3 shaft = u_SunColor.rgb * daylight * phase * haze * u_Volume.x;
    return float4(shaft, lerp(1.0, v.x, saturate(u_Volume.x)));
}

// ---------------------------------------------------------------------------------
// Pass 2: ambient occlusion + ray-marched sun shadows (half res)
//   s0 = half normal/depth, s1 = full normal/depth, s2 = height map (long shadows)
//   out.r = AO visibility, out.g = sun visibility, out.b = long-shadow visibility
// ---------------------------------------------------------------------------------
float4 PS_OcclusionShadow(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {
    float4 nd = tex2Dlod(s0, float4(uv, 0, 0));
    if (nd.w >= SKY_Z) return float4(1, 1, 1, 1);
    float3 p = viewPosition(uv, nd.w);
    float3 n = nd.xyz;
    float noise = ign(vpos);
    float noise2 = hash12(vpos + 17.31);

    // --- Ambient occlusion (horizon-based, normal-aware) ---
    float radius = u_Light.y;
    float projScale = abs(u_Proj.y) * 0.5 * u_Screen.y * 0.5; // half-res pixels per metre at z = 1
    float radiusPx = clamp(radius * projScale / p.z, 3.0, u_Screen.y * 0.1);
    float occlusion = 0.0;
    const float AO_SAMPLES = u_Quality.x;
    float angle = noise * 2.0 * PI;
    [loop] for (int i = 0; i < (int)AO_SAMPLES; i++) {
        float t = (i + noise2) / AO_SAMPLES;
        float a = angle + t * 9.0 * 2.0 * PI; // spiral
        float2 suv = uv + float2(cos(a), sin(a)) * (radiusPx * t + 1.0) * u_Screen.zw * 2.0;
        float3 v = viewPosition(suv, tex2Dlod(s0, float4(suv, 0, 0)).w) - p;
        float vv = dot(v, v);
        float cosine = dot(v, n) * rsqrt(vv + 1e-6);
        float minDist = 0.01 * p.z; // below depth precision / z-fighting
        occlusion += saturate(cosine - 0.12) * saturate(1.0 - vv / (radius * radius)) * saturate(vv / (minDist * minDist) - 1.0);
    }
    float ao = saturate(1.0 - occlusion * 2.2 / AO_SAMPLES);
    ao = ao * ao;
    ao = lerp(1.0, ao, saturate(1.5 - p.z / 250.0)); // fade far away

    // --- Screen-space sun shadows (traced against the full-resolution depth) ---
    float sunVis = 1.0;
    float3 l = u_SunView.xyz;
    float ndl = dot(n, l);
    if (u_SunView.w > 0.5 && ndl > 0.0 && u_Light.z > 0.0) {
        const float STEPS = u_Quality.y;
        float maxLen = u_Light.w;
        float3 origin = p + n * (0.02 + p.z * 0.003);
        float hit = 0.0;
        [loop] for (int s = 0; s < (int)STEPS; s++) {
            float t = (s + noise) / STEPS;
            t = t * t * maxLen + 0.08;
            float3 q = origin + l * t;
            if (q.z <= 0.21) break;
            float2 quv = projectToUV(q);
            if (any(quv < 0.0) || any(quv > 1.0)) break;
            float sceneZ = tex2Dlod(s1, float4(quv, 0, 0)).w;
            float delta = q.z - sceneZ;
            if (delta > 0.02 + q.z * 0.004 && delta < 0.3 + t * 0.12) {
                float edge = min(min(quv.x, 1.0 - quv.x), min(quv.y, 1.0 - quv.y));
                hit = saturate(edge * 20.0);
                break;
            }
        }
        // Grazing sun: rely less on the trace (depth precision) and more on N.L.
        hit *= saturate(ndl * 6.0);
        sunVis = 1.0 - hit * saturate(1.5 - p.z / 300.0);
    }
    float longVis = 1.0;
    if (u_Temporal.w > 0.0 && u_SunView.w > 0.5 && ndl > 0.0) {
        longVis = 1.0 - longShadow(p, n, noise2) * u_Temporal.w;
        sunVis = min(sunVis, longVis);
    }
    return float4(ao, sunVis, longVis, 1);
}

// Depth-aware separable blur of AO/shadow (half res).
//   s0 = AO/shadow, s1 = half normal/depth, u_Pass0.xy = direction * texel
float4 PS_BilateralBlur(float2 uv : TEXCOORD0) : COLOR0 {
    float4 centerND = tex2Dlod(s1, float4(uv, 0, 0));
    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int i = -4; i <= 4; i++) {
        float2 suv = uv + u_Pass0.xy * i;
        float4 nd = tex2Dlod(s1, float4(suv, 0, 0));
        float w = exp(-i * i / 10.0);
        w *= exp(-abs(nd.w - centerND.w) / (0.02 * centerND.w + 0.05));
        w *= pow(saturate(dot(nd.xyz, centerND.xyz)), 4.0) + 0.001;
        sum += tex2Dlod(s0, float4(suv, 0, 0)).rgb * w;
        wsum += w;
    }
    return float4(sum / max(wsum, 1e-4), 1);
}

// ---------------------------------------------------------------------------------
// Screen-space global illumination (quarter res): one diffuse bounce. Every visible surface
// that faces this pixel lights it with its own colour from the game's image: green from
// the grass on the walls, red from the car on the road, blue from the neon borders.
//   s0 = half normal/depth, s1 = scene colour (sRGB, bilinear), u_Pass0.x = samples,
//   u_Pass0.y = radius (m). out.rgb = bounced light (linear)
// ---------------------------------------------------------------------------------
float4 PS_GI(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {
    float4 nd = tex2Dlod(s0, float4(uv, 0, 0));
    if (nd.w >= SKY_Z) return 0;
    float3 p = viewPosition(uv, nd.w);
    float3 n = nd.xyz;
    // u_Pass0.z: frame (0..63), the noise changes every frame; PS_GITemporal averages it.
    float2 seed = vpos + u_Pass0.z * 5.588238;
    float noise = frac(52.9829189 * frac(dot(seed, float2(0.06711056, 0.00583715))));
    float noise2 = frac(hash12(vpos + 41.7) + u_Pass0.z * 0.618034);
    float radius = u_Pass0.y;
    float projScale = abs(u_Proj.y) * 0.5 * u_Screen.y * 0.25; // quarter-res pixels per metre at z = 1
    float radiusPx = clamp(radius * projScale / p.z, 2.0, u_Screen.y * 0.08);
    float samples = u_Pass0.x;
    float3 sum = 0;
    float angle = noise * 2.0 * PI;
    [loop] for (int i = 0; i < (int)samples; i++) {
        float t = (i + noise2) / samples;
        float a = angle + t * 7.0 * 2.0 * PI;
        float2 suv = uv + float2(cos(a), sin(a)) * (radiusPx * sqrt(t) + 1.0) * u_Screen.zw * 4.0;
        if (any(suv < 0.0) || any(suv > 1.0)) continue;
        float4 snd = tex2Dlod(s0, float4(suv, 0, 0));
        if (snd.w >= SKY_Z) continue;
        float3 d = viewPosition(suv, snd.w) - p;
        float dd = dot(d, d) + 1e-4;
        float3 dir = d * rsqrt(dd);
        // Lambert at the receiver and at the sender (it has to face us), soft distance falloff.
        // Form factor: each sample stands for 1/N of the disc (pi r^2), seen under
        // cos * cos / (pi d^2). Close facing surfaces (a wall next to the road) bounce most.
        float w = saturate(dot(n, dir)) * saturate(dot(snd.xyz, -dir)) * radius * radius / max(dd, 0.1 * radius * radius);
        sum += toLinear(tex2Dlod(s1, float4(suv, 0, 0)).rgb) * w;
    }
    // Fades out far away, where the radius is below a pixel.
    float fade = saturate(2.0 - p.z / 120.0);
    return float4(min(sum / samples * fade, 4.0), 1);
}

// GI history: s0 = this frame's GI (blurred), s1 = history (rgb, view z), s2 = half
// normal/depth, u_Pass0.x = history valid. Moving surfaces and disocclusions start over.
float4 PS_GITemporal(float2 uv : TEXCOORD0) : COLOR0 {
    float3 current = tex2Dlod(s0, float4(uv, 0, 0)).rgb;
    float z = tex2Dlod(s2, float4(uv, 0, 0)).w;
    if (u_Pass0.x < 0.5 || z >= SKY_Z) return float4(current, z);
    float prevZ;
    float2 prevUV = reproject(worldPosition(viewPosition(uv, z)), prevZ);
    if (any(prevUV < 0.0) || any(prevUV > 1.0)) return float4(current, z);
    float4 history = tex2Dlod(s1, float4(prevUV, 0, 0));
    float error = abs(history.a - prevZ) / max(prevZ, 0.1);
    float weight = lerp(0.1, 1.0, smoothstep(0.03, 0.1, error));
    return float4(lerp(history.rgb, current, weight), z);
}

// ---------------------------------------------------------------------------------
// Sky helpers
// ---------------------------------------------------------------------------------
float3 sunScatter(float3 rd, float strength) {
    float mu = dot(rd, u_SunWorld.xyz);
    float mie = henyeyGreenstein(mu, 0.7) * 0.3 + henyeyGreenstein(mu, 0.25) * 0.4;
    return u_SunColor.rgb * mie * strength;
}


// =================================================================================
// Custom skies (sky mode 1-4). Everything works on the world-space view direction.
// =================================================================================
float hash13(float3 p) {
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

float noise3(float3 p) {
    float3 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash13(i), n100 = hash13(i + float3(1, 0, 0));
    float n010 = hash13(i + float3(0, 1, 0)), n110 = hash13(i + float3(1, 1, 0));
    float n001 = hash13(i + float3(0, 0, 1)), n101 = hash13(i + float3(1, 0, 1));
    float n011 = hash13(i + float3(0, 1, 1)), n111 = hash13(i + float3(1, 1, 1));
    return lerp(lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y),
                lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y), f.z);
}

float fbm(float3 p) {
    float v = 0.0, a = 0.5;
    [loop] for (int i = 0; i < 5; i++) {
        v += a * noise3(p);
        p = p * 2.03 + 17.1;
        a *= 0.5;
    }
    return v;
}

float3 rotateY(float3 v, float a) {
    float c = cos(a), s = sin(a);
    return float3(c * v.x - s * v.z, v.y, s * v.x + c * v.z);
}

// Point stars on a 3D grid, anti-aliased, with gentle twinkle. Each star covers at least
// a pixel or two (smaller ones flicker away under TAA), the brightest get a glow.
float3 stars(float3 rd, float density) {
    float3 sum = 0;
    [loop] for (int layer = 0; layer < 3; layer++) {
        float scale = layer == 0 ? 70.0 : (layer == 1 ? 160.0 : 340.0);
        float chance = layer == 0 ? 0.035 : (layer == 1 ? 0.08 : 0.1);
        float3 p = rd * scale;
        float3 cell = floor(p);
        float h = hash13(cell + layer * 71.0);
        if (h > 1.0 - chance * density) {
            float3 center = cell + 0.5 + (float3(hash13(cell + 3.1), hash13(cell + 5.7), hash13(cell + 9.3)) - 0.5) * 0.6;
            float d = length(p - center);
            float size = layer == 0 ? 0.07 : (layer == 1 ? 0.16 : 0.15);
            float b = smoothstep(size, 0.0, d) * (layer == 0 ? 14.0 : (layer == 1 ? 5.5 : 2.6));
            if (layer == 0) b += exp(-d * 30.0) * 0.6; // glow of the bright ones (feeds the bloom)
            float twinkle = 0.75 + 0.25 * sin(u_Proj2.z * (2.0 + h * 5.0) + h * 40.0);
            float t = frac(h * 91.7);
            float3 tint = t < 0.3 ? float3(0.7, 0.8, 1.0) : (t < 0.8 ? float3(1, 1, 1) : float3(1.0, 0.8, 0.6));
            sum += tint * b * twinkle;
        }
    }
    return sum;
}

// Milky way: a dusty band of glowing gas along a great circle.
float3 galaxy(float3 rd) {
    float3 bandNormal = normalize(float3(0.35, 0.55, 0.76));
    float band = dot(rd, bandNormal);
    float core = exp(-band * band * 18.0);
    float wide = exp(-band * band * 4.0);
    float gas = fbm(rd * 5.0 + 3.0);
    float dust = smoothstep(0.45, 0.75, fbm(rd * 9.0 - 7.0));
    float3 axis = normalize(cross(bandNormal, float3(0, 1, 0)));
    float bulge = pow(saturate(dot(rd, axis) * 0.5 + 0.5), 6.0);
    float3 col = lerp(float3(0.45, 0.55, 0.85), float3(0.85, 0.75, 0.8), gas);
    col = lerp(col, float3(1.0, 0.82, 0.62), bulge);
    float3 g = col * (core * (0.006 + gas * 0.035) * (1.0 + bulge * 1.2) + wide * 0.0015);
    g *= 1.0 - dust * core * 0.6;
    // Faint nebula clouds elsewhere.
    float neb = pow(fbm(rd * 2.5 + 11.0), 3.0);
    g += float3(0.5, 0.15, 0.6) * neb * 0.04 + float3(0.1, 0.3, 0.6) * pow(fbm(rd * 3.0 - 5.0), 4.0) * 0.04;
    return g;
}

float3 spaceBackground(float3 rd) {
    return galaxy(rd) * 2.2 + stars(rd, u_Sky2.y) + float3(0.004, 0.006, 0.012);
}

// Shooting stars: every few seconds one streaks across the upper sky.
float3 meteors(float3 rd, float amount) {
    float3 sum = 0;
    float t = u_Proj2.z / 2.7;
    [unroll] for (int k = 0; k < 2; k++) {
        float slot = floor(t + k * 0.5) - k * 0.5;
        float age = (t - slot) * 2.7;                       // seconds since this slot began
        float h1 = hash12(float2(slot, 1.7 + k)), h2 = hash12(float2(slot, 9.3 + k)), h3 = hash12(float2(slot, 4.1 + k));
        if (h3 > 0.6 || age > 0.9) continue;
        float az = h1 * 6.2832, el = 0.35 + h2 * 0.6;
        float3 start = float3(sin(az) * cos(el), sin(el), cos(az) * cos(el));
        float3 dir = normalize(cross(start, float3(cos(az + 1.3), 0.4 - h2, sin(az + 1.3))));
        float travel = age * 0.45;                           // radians per second
        float3 head = normalize(start + dir * travel);
        // Distance to the trail segment behind the head (small angles: plane approximation).
        float3 v = rd - head;
        float along = -dot(v, dir);
        float across = length(v + dir * along);
        float trail = saturate(along / 0.12);
        float fade = smoothstep(0.0, 0.1, age) * smoothstep(0.9, 0.5, age);
        float glow = smoothstep(0.0015, 0.0, across) * (along > 0.0 ? 1.0 - trail : 0.0) * step(-0.002, along);
        sum += float3(0.85, 0.9, 1.0) * glow * fade * 4.0;
    }
    return sum * amount;
}

// Frame around a sky direction: x = right, y = up, z = forward.
float3x3 skyFrame(float3 f) {
    float3 r = normalize(cross(float3(0, 1, 0), f));
    return float3x3(r, cross(f, r), f);
}

float3 blackHoleDirection() {
    return normalize(rotateY(float3(0.0, 0.28, 1.0), u_Sky.z));
}

// Accretion disk emission where a light ray crosses the disk plane.
//   x: crossing point (units of the Schwarzschild radius), dir: ray direction there,
//   n/e1/e2: disk frame. Returns rgb emission and opacity.
float4 diskSample(float3 x, float3 dir, float3 n, float3 e1, float3 e2) {
    const float inner = 3.0, outer = 11.0;
    float r = length(x);
    if (r < inner * 0.9 || r > outer) return 0;
    float phi = atan2(dot(x, e2), dot(x, e1));
    // Keplerian shear: inner gas orbits faster, which turns the noise into spiral streaks.
    float spin = u_Proj2.z * 0.35 * pow(inner / r, 1.5);
    float a = phi + spin + log(r) * 1.8;
    float3 q = float3(cos(a) * 2.5, sin(a) * 2.5, r * 0.9);
    float gas = noise3(q) * 0.65 + noise3(q * 2.7 + 11.0) * 0.35;
    float lanes = 0.75 + 0.25 * noise3(float3(r * 2.5, cos(a) * 0.6, sin(a) * 0.6));
    // Smooth, bright gas with faint lanes: Gargantua is clean, not noisy.
    float density = saturate(gas * 0.7 + 0.35) * lerp(1.0, lanes, 0.5);
    density *= smoothstep(inner * 0.9, inner * 1.15, r) * smoothstep(outer, outer * 0.55, r);

    // Temperature falls off outward: white-hot inside, deep orange at the rim.
    float heat = pow(inner / r, 1.6);
    float3 col = lerp(float3(1.0, 0.45, 0.15), float3(1.0, 0.9, 0.78), saturate(heat * 1.6 + 0.15));
    col = lerp(col, float3(0.85, 0.9, 1.0), saturate(heat - 0.75) * 1.5);
    // Relativistic beaming: the side orbiting towards the camera is much brighter.
    float v = min(sqrt(0.5 / max(r - 1.0, 0.5)), 0.7);
    float3 orbit = normalize(cross(n, x));
    float g = sqrt(1.0 - v * v) / (1.0 - v * dot(orbit, -dir));
    float beaming = clamp(g * g * g, 0.12, 3.5);
    float redshift = sqrt(saturate(1.0 - 1.0 / r));
    // White-hot and far brighter than anything else in the sky: it feeds a big bloom.
    float3 emission = col * (0.6 + heat * 7.0) * beaming * redshift;
    return float4(emission * density, saturate(density * 1.4));
}

// Black hole after Interstellar's Gargantua: light rays are traced through Schwarzschild
// space-time, so the shadow, the photon ring and the disk lensed over and under the hole
// all come out of the bending itself.
//   bh = direction of the hole, dist = camera distance in Schwarzschild radii (size).
float3 blackHoleSky(float3 rd, float3 bh, float dist) {
    float3x3 frame = skyFrame(bh);
    float cosA = dot(rd, bh);
    float angle = acos(clamp(cosA, -1.0, 1.0));
    // Disk plane: almost edge-on, the camera slightly above it, a little rolled.
    float3 n = normalize(frame[1] * cos(0.09) - frame[2] * sin(0.09) + frame[0] * 0.06);
    float3 e1 = normalize(cross(n, frame[2]));
    float3 e2 = cross(n, e1);

    float3 col = 0;
    float alpha = 0.0;
    float closest = 1e4;
    float3 outDir;
    const float traceRadius = 26.0;
    // Close up (camera inside the trace sphere) every ray is traced from the camera.
    bool inside = dist < traceRadius;
    float cone = inside ? 4.0 : asin(min(traceRadius / dist, 0.99));
    if (angle < cone) {
        // Straight to the trace sphere, then integrate the bent ray inside it.
        float3 p = -bh * dist;
        if (!inside) {
            float b = dot(p, rd);
            float c = dot(p, p) - traceRadius * traceRadius;
            p += rd * (-b - sqrt(max(b * b - c, 0.0)));
        }
        float3 v = rd;
        float3 h = cross(p, v);
        float h2 = dot(h, h);
        bool captured = false;
        [loop] for (int i = 0; i < 220; i++) {
            float r = length(p);
            closest = min(closest, r);
            float dt = clamp(0.06 * r, 0.02, 1.5);
            float3 prev = p;
            v += -1.5 * h2 * p / pow(r, 5.0) * dt;
            p += v * dt;
            float s0 = dot(prev, n), s1 = dot(p, n);
            if (s0 * s1 < 0.0 && alpha < 0.99) {
                float3 x = lerp(prev, p, s0 / (s0 - s1));
                float4 d = diskSample(x, normalize(v), n, e1, e2);
                col += (1.0 - alpha) * d.rgb;
                alpha += (1.0 - alpha) * d.a;
            }
            if (r < 1.0 || alpha > 0.99) { captured = true; break; }
            if (r > traceRadius && dot(p, v) > 0.0) break;
            if (i == 219) captured = true;
        }
        outDir = normalize(v);
        // Bending accumulated outside the trace sphere (straight-line approximation).
        float impact = dist * sin(angle);
        float outside = 2.0 / max(impact, 1.0) * (1.0 - sqrt(max(1.0 - impact * impact / (traceRadius * traceRadius), 0.0)));
        outDir = normalize(outDir + normalize(bh - outDir * dot(outDir, bh) + 1e-5) * outside);
        if (!captured) col += (1.0 - alpha) * spaceBackground(outDir);
        // Photon ring: light that circled the hole, a thin sharp line around the shadow.
        if (!captured) col += float3(1.0, 0.92, 0.82) * exp(-abs(closest - 2.65) * 9.0) * 1.6 * (1.0 - alpha);
    } else {
        // Weak field: background bent toward the hole by 2 rs / b.
        float deflect = 2.0 / (dist * sin(angle));
        outDir = normalize(rd + normalize(bh - rd * cosA) * deflect);
        col = spaceBackground(outDir);
    }
    // Soft glow of the inner disk scattered around the hole (feeds the bloom).
    // Light of the inner disk scattered around the hole: a tight hot glow and a wide halo
    // that spills into the sky (it is a light source, not a sticker).
    float glow = exp(-angle * dist / 9.0) * smoothstep(2.3, 3.4, angle * dist);
    float halo = exp(-angle * dist / 45.0);
    return col + float3(1.0, 0.78, 0.55) * (glow * 0.5 + halo * 0.06);
}

// ---------------------------------------------------------------------------------
// Ring world (sky mode 5, after IterationT): a gas giant fills part of the sky, its rings
// sweep over the stadium, moons hang in the distance and a black hole sits far away.
// Units: planet radius = 1, camera at the origin. Lit by the game's sun.
// ---------------------------------------------------------------------------------
float3 dirFromAngles(float azimuth, float elevation) {
    return float3(sin(azimuth) * cos(elevation), sin(elevation), cos(azimuth) * cos(elevation));
}

// Cloud bands of the planet. lat = -1..1 along the spin axis, lon = longitude (rad),
// q = point on the unit sphere (for turbulence).
float3 planetAlbedo(int type, float lat, float lon, float3 q) {
    float t = u_Proj2.z * 0.004;
    // Turbulent band edges: the latitude is warped by noise.
    float warp = (noise3(q * 4.0 + float3(t, 0, 0)) - 0.5) * 0.09 + (noise3(q * 11.0 - float3(0, t, 0)) - 0.5) * 0.035;
    float l = lat + warp;
    float bands = noise3(float3(l * 9.0, 0.5, 1.5)) * 0.65 + noise3(float3(l * 27.0, 3.5, 0.5)) * 0.35;
    float fine = noise3(float3(l * 70.0, lon * 0.4, 2.0));
    float3 col;
    if (type == 1) {
        // Jupiter: cream zones, brown and rust belts, the Great Red Spot.
        col = lerp(float3(0.55, 0.36, 0.24), float3(0.95, 0.88, 0.76), smoothstep(0.35, 0.65, bands));
        col = lerp(col, float3(0.78, 0.5, 0.32), smoothstep(0.55, 0.8, fine) * 0.35);
        float2 spot = float2((lon - 1.2) * 0.55, (lat + 0.38) * 2.2);
        float swirl = noise3(float3(spot * 6.0, t * 3.0));
        float oval = smoothstep(0.32, 0.18, length(spot) + (swirl - 0.5) * 0.08);
        col = lerp(col, float3(0.8, 0.38, 0.24), oval * 0.85);
    } else if (type == 2) {
        // Ice giant: deep blue with faint bands, white methane streaks and a dark storm.
        col = lerp(float3(0.13, 0.27, 0.72), float3(0.32, 0.52, 0.92), smoothstep(0.3, 0.7, bands));
        float streak = smoothstep(0.72, 0.9, noise3(float3(l * 40.0, lon * 3.0, 7.0)));
        col = lerp(col, float3(0.9, 0.95, 1.0), streak * 0.6);
        float2 spot = float2((lon + 0.8) * 0.6, (lat + 0.25) * 2.4);
        col *= 1.0 - smoothstep(0.22, 0.12, length(spot)) * 0.55;
    } else if (type == 3) {
        // Exotic: violet and rose bands with teal storms.
        col = lerp(float3(0.42, 0.22, 0.62), float3(0.9, 0.6, 0.82), smoothstep(0.3, 0.7, bands));
        col = lerp(col, float3(0.25, 0.7, 0.75), smoothstep(0.6, 0.85, fine) * 0.4);
    } else {
        // Saturn: pale gold, low-contrast bands, a bluish pole.
        col = lerp(float3(0.78, 0.66, 0.45), float3(0.97, 0.9, 0.72), smoothstep(0.25, 0.75, bands));
        col = lerp(col, float3(0.88, 0.8, 0.62), fine * 0.25);
        col = lerp(col, float3(0.56, 0.64, 0.7), smoothstep(0.78, 0.95, abs(lat)));
    }
    return col;
}

float3 ringColor(int type) {
    if (type == 1) return float3(0.75, 0.62, 0.5);
    if (type == 2) return float3(0.62, 0.72, 0.85);
    if (type == 3) return float3(0.7, 0.85, 1.0);
    return float3(0.93, 0.84, 0.68);
}

// Ring particle density at radius rr (planet radii); x = position in the ring plane,
// near = 0..1 how close the camera is (close up the rings break into clumps).
float ringDensity(int type, float rr, float2 x, float near) {
    float inner = 1.3, outer = type == 2 ? 2.0 : 2.45;
    if (rr < inner || rr > outer) return 0.0;
    float bands = 0.5 + 0.5 * noise3(float3(rr * 42.0, 0.0, 0.0)) * noise3(float3(rr * 11.0, 3.0, 0.0));
    float d = bands * smoothstep(inner, inner + 0.06, rr) * smoothstep(outer, outer - 0.12, rr);
    d *= 1.0 - 0.92 * smoothstep(0.02, 0.0, abs(rr - (inner + outer) * 0.5 - 0.17)); // Cassini division
    d *= lerp(0.45, 1.0, smoothstep(1.55, 1.65, rr));                                 // faint inner ring
    // Close up: ringlets with gaps between them, and clumps of ice.
    float ringlets = noise3(float3(rr * 160.0, 0.3, 2.1));
    d *= lerp(1.0, smoothstep(0.2, 0.7, ringlets) * 0.85 + 0.15, near);
    d *= lerp(1.0, 0.55 + 0.6 * noise3(float3(rr * 520.0, 1.7, 4.2)), near);
    d *= lerp(1.0, 0.6 + 0.6 * noise3(float3(x * 600.0, rr * 300.0)), near * 0.3);
    if (type == 1) d *= 0.7;  // Jupiter: dusty rings
    if (type == 2) d *= 0.5;  // ice giant: thin rings
    return saturate(d);
}

// Direction (xyz) and light strength (w) of the black hole of the current sky, if any.
float4 blackHoleLight() {
    int mode = (int)(u_Sky.x + 0.5);
    if (mode == 3) return float4(blackHoleDirection(), saturate(u_Sky2.z * 0.45));
    if (mode == 5) return float4(normalize(rotateY(float3(0.55, 0.22, -0.8), u_Sky.z)), saturate(u_Sky2.z * 0.25));
    return 0;
}

// Ringed gas giant (sky detail for the space skies). Returns rgb and coverage.
float4 ringedPlanet(float3 rd, float3 light) {
    float size = u_Sky2.w;
    if (size <= 0.0) return 0;
    float3 dir = normalize(rotateY(float3(-0.62, 0.14, 0.8), u_Sky.z));
    float radius = sin(0.16 * size);
    float3 center = dir;                          // planet at distance 1
    float3 ringN = normalize(float3(0.18, 1.0, -0.12));
    float3 toC = center;

    // Sphere.
    float b = dot(rd, toC);
    float c = dot(toC, toC) - radius * radius;
    float disc = b * b - c;
    float tPlanet = disc > 0.0 ? b - sqrt(disc) : 1e9;
    // Ring plane through the centre.
    float denom = dot(rd, ringN);
    float tRing = abs(denom) > 1e-5 ? dot(center, ringN) / denom : -1.0;

    float4 result = 0;
    if (tRing > 0.0 && tRing < tPlanet) {
        float3 x = rd * tRing - center;
        float rr = length(x) / radius;
        if (rr > 1.22 && rr < 2.3) {
            float bands = 0.55 + 0.45 * noise3(float3(rr * 38.0, 0.0, 0.0)) * noise3(float3(rr * 9.0, 3.0, 0.0));
            float density = bands * smoothstep(1.22, 1.3, rr) * smoothstep(2.3, 2.15, rr);
            density *= 1.0 - 0.9 * smoothstep(0.015, 0.0, abs(rr - 1.95));       // Cassini division
            density *= lerp(0.55, 1.0, smoothstep(1.5, 1.6, rr));                // fainter C ring
            // Planet shadow across the rings.
            float3 wp = rd * tRing;
            float lb = dot(light, center - wp);
            float lc = dot(center - wp, center - wp) - radius * radius;
            float shadow = (lb > 0.0 && lb * lb - lc > 0.0) ? 0.08 : 1.0;
            int type = (int)(u_Planet.x + 0.5);
            density *= type == 1 ? 0.7 : (type == 2 ? 0.6 : 1.0);
            float3 ringCol = ringColor(type) * (0.35 + 0.65 * abs(dot(light, ringN))) * shadow;
            result = float4(ringCol * 0.7 * density, density * 0.85);
        }
    }
    if (tPlanet < 1e8 && (result.a < 0.999)) {
        float3 x = rd * tPlanet;
        float3 nrm = normalize(x - center);
        float lat = dot(nrm, ringN);
        float3 e1 = normalize(cross(ringN, float3(0.3, 0.1, 0.9)));
        float lon = atan2(dot(nrm, cross(ringN, e1)), dot(nrm, e1));
        float3 albedo = planetAlbedo((int)(u_Planet.x + 0.5), lat, lon, nrm);
        float ndl = dot(nrm, light);
        float lit = smoothstep(-0.08, 0.35, ndl);
        // Shadow of the rings on the planet.
        float denomL = dot(light, ringN);
        float tl = abs(denomL) > 1e-4 ? dot(center - x, ringN) / denomL : -1.0;
        float rs = tl > 0.0 ? length(x + light * tl - center) / radius : 0.0;
        float ringShadow = (rs > 1.25 && rs < 2.3) ? 0.35 : 1.0;
        float limb = pow(saturate(dot(nrm, -rd)), 0.35);
        float3 planet = albedo * (lit * ringShadow * 0.8 * limb + 0.01);
        result.rgb += (1.0 - result.a) * planet;
        result.a = 1.0;
    }
    return result;
}

// Small rocky moon (direction, angular radius): colour and coverage.
float4 moon(float3 rd, float3 dir, float radius, float3 light, float3 tint) {
    float b = dot(rd, dir);
    float disc = b * b - (1.0 - radius * radius);
    if (disc <= 0.0 || b <= 0.0) return 0;
    float3 x = rd * (b - sqrt(disc));
    float3 nrm = normalize(x - dir);
    float craters = noise3(nrm * 9.0) * 0.6 + noise3(nrm * 23.0) * 0.4;
    float3 albedo = tint * (0.55 + 0.45 * craters);
    return float4(albedo * (saturate(dot(nrm, light)) * 0.9 + 0.012), smoothstep(0.0, radius * radius * 0.08, disc));
}

float3 ringWorldSky(float3 rd) {
    int type = (int)(u_Planet.x + 0.5);
    float3 sun = u_SunWorld.xyz;
    if (u_SunView.w < 0.5 || sun.y < -0.3) sun = normalize(float3(0.5, 0.35, -0.8)); // no usable game sun

    // Background: stars, galaxy and a small, far black hole (with its lensing).
    float3 bhDir = normalize(rotateY(float3(0.55, 0.22, -0.8), u_Sky.z));
    float3 col = blackHoleSky(rd, bhDir, 70.0 / (u_Sky2.z * 0.55));
    // The sun as a star.
    float mu = dot(rd, sun);
    col += u_SunColor.rgb * (smoothstep(0.99955, 0.9998, mu) * 6.0 + pow(saturate(mu), 300.0) * 0.6 + pow(saturate(mu), 20.0) * 0.03);

    // Moons.
    float4 m1 = moon(rd, normalize(rotateY(float3(-0.75, 0.38, -0.3), u_Sky.z)), 0.022, sun, float3(0.8, 0.78, 0.74));
    col = lerp(col, m1.rgb, m1.a);
    float4 m2 = moon(rd, normalize(rotateY(float3(0.2, 0.55, -0.9), u_Sky.z)), 0.009, sun, float3(0.85, 0.7, 0.55));
    col = lerp(col, m2.rgb, m2.a);

    // Planet placement.
    //   View 0: the classic distant view of a ringed planet, from above the rings.
    //   View 1: just outside the rings, a little below them - the lit rings rise from the
    //           planet and sweep over the stadium.
    //   View 2: on the rings - floating just above them, the banded ring plane stretches
    //           to the horizon and rises over the stadium on one side.
    int view = (int)(u_Planet.y + 0.5);
    float3 dir = dirFromAngles(u_Planet.z, u_Planet.w);
    float size = u_Sky2.w > 0.0 ? max(u_Sky2.w, 0.3) : 1.0;
    float dist = view == 2 ? 2.2 : (view == 1 ? 2.6 : 5.5 / size);
    float3 center = dir * dist;
    float3 side = normalize(cross(float3(0, 1, 0), dir));
    float3 inPlane, ringN;
    if (view == 0) {
        // The ring plane contains the planet direction and leans over the sky.
        inPlane = normalize(side * cos(0.32) + float3(0, 1, 0) * sin(0.32));
        ringN = normalize(cross(inPlane, dir));
        if (ringN.y < 0.0) ringN = -ringN;
        center -= ringN * (dist * 0.22 + dot(center, ringN)); // camera well above the rings
    } else {
        // Ring plane through the planet direction, rolled sideways so it crosses the view
        // diagonally. Next to the rings the camera is below them; on the rings it floats
        // just above the ring particles.
        float roll = view == 2 ? 0.35 : 0.45;
        float3 up = normalize(cross(dir, side));
        ringN = normalize(up * cos(roll) + side * sin(roll));
        inPlane = normalize(cross(dir, ringN));
        float offset = view == 2 ? -0.035 : 0.15; // > 0: plane above the camera
        center -= ringN * (dot(center, ringN) - offset);
    }
    float3 axis = normalize(ringN + dir * 0.05);
    // The planet's own star lights it from behind the viewer (a big gibbous planet, like in
    // IterationT) instead of the game's sun, which is often behind it. Seen from below the
    // rings, the star is below them too, so the side we look at is lit.
    sun = normalize(-dir * 0.55 + side * 0.65 + (view == 0 ? float3(0, 0.5, 0) : (view == 1 ? -ringN * 0.3 : ringN * 0.45)));

    // Planet sphere.
    float b = dot(rd, center);
    float disc = b * b - (dot(center, center) - 1.0);
    float tPlanet = (disc > 0.0 && b > 0.0) ? b - sqrt(disc) : 1e9;
    // Ring plane.
    float denom = dot(rd, ringN);
    float tRing = abs(denom) > 1e-6 ? dot(center, ringN) / denom : -1.0;

    float3 planetCol = 0;
    float planetA = 0.0;
    if (tPlanet < 1e8) {
        float3 x = rd * tPlanet;
        float3 nrm = normalize(x - center);
        float lat = dot(nrm, axis);
        float3 e1 = normalize(cross(axis, float3(0.3, 0.1, 0.9)));
        float lon = atan2(dot(nrm, cross(axis, e1)), dot(nrm, e1)) + u_Proj2.z * 0.01;
        float cl = sqrt(saturate(1.0 - lat * lat));
        float3 local = float3(cos(lon) * cl, lat, sin(lon) * cl);
        float3 albedo = planetAlbedo(type, lat, lon, local);
        float ndl = dot(nrm, sun);
        float lit = smoothstep(-0.06, 0.4, ndl);
        // Shadow of the rings on the planet.
        float dl = dot(sun, ringN);
        float tl = abs(dl) > 1e-4 ? dot(center - x, ringN) / dl : -1.0;
        float3 sx = x + sun * tl - center;
        float ringShadow = tl > 0.0 ? 1.0 - ringDensity(type, length(sx), sx.xz, 0.0) * 0.75 : 1.0;
        float rim = pow(1.0 - saturate(dot(nrm, -rd)), 3.0);
        float3 atmo = type == 2 ? float3(0.4, 0.6, 1.0) : (type == 3 ? float3(0.8, 0.5, 1.0) : float3(1.0, 0.85, 0.65));
        planetCol = albedo * (lit * ringShadow * 0.7 + 0.012) + atmo * rim * saturate(ndl + 0.3) * 0.35;
        planetA = smoothstep(0.0, 0.002 * dist, disc); // soft limb
    }
    float3 ringCol = 0;
    float ringA = 0.0;
    if (tRing > 0.0) {
        float3 x = rd * tRing - center;
        float rr = length(x);
        float2 px = float2(dot(x, inPlane), dot(x, dir));
        float d = ringDensity(type, rr, px, saturate(1.0 - tRing / 1.2));
        if (d > 0.0) {
            // Shadow of the planet on the rings.
            float3 wp = rd * tRing;
            float lb = dot(sun, center - wp);
            float lc = dot(center - wp, center - wp) - 1.0;
            float shadow = (lb > 0.0 && lb * lb - lc > 0.0) ? 0.06 : 1.0;
            // Lit side, or back-lit with forward scattering through the ring.
            bool litSide = dot(sun, ringN) * -denom > 0.0;
            float front = 0.35 + 0.65 * saturate(abs(dot(sun, ringN)) * 2.0);
            float forward = pow(saturate(dot(rd, sun)), 6.0) * 1.5;
            float near = saturate(1.0 - tRing / 1.2);
            ringCol = ringColor(type) * (litSide ? front : front * 0.35 + forward) * shadow * lerp(0.65, 0.8, near);
            ringA = d * lerp(0.9, 0.8, near);
        }
    }
    // Composite back to front.
    if (tRing > 0.0 && tRing < tPlanet) {
        col = lerp(lerp(col, planetCol, planetA), ringCol, ringA);
    } else {
        col = lerp(lerp(col, ringCol, ringA), planetCol, planetA);
    }
    return col;
}

// Aurora: a folded curtain line (level set of a warped noise field) sampled on stacked
// altitude slices. Many jittered slices blend into smooth vertical veils.
float noise2(float2 p) {
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash12(i), b = hash12(i + float2(1, 0)), c = hash12(i + float2(0, 1)), d = hash12(i + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// footprint: size of a pixel in p units. Where the thin lines get smaller than a pixel they
// widen (keeping their average brightness): no moiré rings near the horizon.
float auroraCurtain(float2 p, float t, float footprint) {
    float2 w = float2(noise2(p * 0.45 + float2(t, 0.0)), noise2(p * 0.45 + float2(5.2, 1.3 - t)));
    p += (w - 0.5) * 3.2;
    float activity = smoothstep(0.4, 0.75, noise2(p * 0.12 + float2(3.7, t * 0.5)));  // patches, not a full sky
    float k = 26.0 / (1.0 + footprint * 12.0);
    // The fine octave is the first to alias: it fades out first.
    float fine = saturate(1.0 - footprint * 3.0);
    float n = noise2(p * 0.7) * lerp(1.0, 0.7, fine) + (noise2(p * 1.6 + 9.1) - 0.5) * 0.3 * fine + 0.15 * fine;
    return exp(-abs(n - 0.5) * k) * pow(k / 26.0, 0.6) * activity;
}

// The glowing curtains, marched through `slices` layers of height (40 = full quality).
// Fewer slices take bigger steps through the same height range. jitter: 0..1 start offset,
// changing every frame so TAA smooths the banding of the low slice counts.
float3 auroraCurtains(float3 rd, float slices, float jitter, float pixAngle) {
    if (rd.y <= 0.0) return 0;
    float ca = cos(u_Sky.z), sa = sin(u_Sky.z);
    float2 flat = float2(rd.x * ca - rd.z * sa, rd.x * sa + rd.z * ca);
    float t = u_Proj2.z * 0.02;
    float stride = 40.0 / slices;
    float3 acc = 0, avg = 0;
    [loop] for (int i = 0; i < (int)slices; i++) {
        float fi = (i + jitter) * stride;
        float height = 1.0 + pow(fi, 1.4) * 0.012;
        float den = rd.y * 1.6 + 0.12;
        float q = height / den * 1.6;
        float2 p = flat * q;
        // How far p moves per pixel: along the slice (q) and towards the horizon (dq/dy).
        float footprint = (q + 1.6 * q / den) * pixAngle;
        float curtain = auroraCurtain(p, t, footprint);
        float3 tint = lerp(float3(0.12, 1.0, 0.4), float3(0.15, 0.65, 0.95), saturate((fi - 8.0) / 24.0));
        tint = lerp(tint, float3(0.75, 0.25, 0.9), saturate((fi - 22.0) / 18.0));
        avg = lerp(avg, tint * curtain, 1.0 - pow(0.55, stride));
        acc += avg * exp2(-fi * 0.085) * smoothstep(0.0, 4.0, fi) * stride;
    }
    return acc * 0.2 * smoothstep(0.0, 0.12, rd.y);
}

// Procedural daytime atmosphere with drifting clouds (sky mode 1).
float3 clearSky(float3 rd) {
    float3 sun = u_SunWorld.xyz;
    float elev = sun.y;
    float y = max(rd.y, 0.0);
    float day = saturate(elev * 4.0 + 0.3);
    float3 zenith = lerp(float3(0.02, 0.03, 0.08), float3(0.12, 0.3, 0.75), day);
    float3 horizon = lerp(float3(0.9, 0.45, 0.25), float3(0.6, 0.78, 1.0), saturate(elev * 3.0));
    horizon *= lerp(0.15, 1.0, day);
    float3 col = lerp(horizon, zenith, pow(y, 0.45));
    float mu = dot(rd, sun);
    col += u_SunColor.rgb * (henyeyGreenstein(mu, 0.75) * 0.25 + pow(saturate(mu), 8.0) * 0.4) * day;
    col += u_SunColor.rgb * smoothstep(0.9994, 0.99975, mu) * 3.0 * day;
    // Clouds on a plane.
    if (rd.y > 0.01) {
        float2 cp = rd.xz / rd.y * 2.0 + float2(u_Proj2.z * 0.01, u_Proj2.z * 0.004);
        float d = fbm(float3(cp, 0.0));
        float cover = smoothstep(0.62 - u_Sky2.x * 0.3, 0.85, d);
        float light = saturate(0.6 + 0.6 * dot(normalize(float3(rd.x, 0.0, rd.z) + 1e-4), normalize(float3(sun.x, 0.0, sun.z) + 1e-4)));
        float3 cloudCol = lerp(float3(0.55, 0.6, 0.7) * lerp(0.2, 1.0, day), u_SunColor.rgb * 1.2, light * day);
        col = lerp(col, cloudCol, cover * smoothstep(0.01, 0.15, rd.y) * 0.9);
    }
    // Night: fade to a starry sky.
    col += spaceBackground(rd) * saturate(1.0 - day * 1.5) * 0.6;
    return col;
}

// FSR 1 EASU (AMD FidelityFX Super Resolution, edge-adaptive spatial upsampling), for the
// effects rendered at half resolution. 12 taps around the sample, an edge direction and
// length from the luma gradients, then a Lanczos-like kernel stretched along the edge,
// clamped to the 4 nearest texels so it never rings. size = input w, h, 1/w, 1/h.
void easuSet(inout float2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {
    float dc = lD - lC, cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = lenX > 0.0 ? 1.0 / lenX : 0.0;
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    len += lenX * lenX * w;
    float ec = lE - lC, ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = lenY > 0.0 ? 1.0 / lenY : 0.0;
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    len += lenY * lenY * w;
}

void easuTap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len2, float lob, float clp, float3 c) {
    float2 v = float2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len2;
    float d2 = min(dot(v, v), clp);
    float wB = 0.4 * d2 - 1.0, wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 1.5625 * wB - 0.5625;
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

float easuLuma(float3 c) { return c.g + 0.5 * (c.r + c.b); }

float3 easuSample(sampler2D tex, float2 uv, float4 size) {
    float2 pp = uv * size.xy - 0.5;
    float2 fp = floor(pp);
    float2 f = pp - fp;
    float2 base = (fp + 0.5) * size.zw;
    #define EASU_TAP(x, y) tex2Dlod(tex, float4(base + float2(x, y) * size.zw, 0, 0)).rgb
    float3 b = EASU_TAP(0, -1), c = EASU_TAP(1, -1);
    float3 e = EASU_TAP(-1, 0), F = EASU_TAP(0, 0), g = EASU_TAP(1, 0), h = EASU_TAP(2, 0);
    float3 i = EASU_TAP(-1, 1), j = EASU_TAP(0, 1), k = EASU_TAP(1, 1), l = EASU_TAP(2, 1);
    float3 n = EASU_TAP(0, 2), o = EASU_TAP(1, 2);
    #undef EASU_TAP
    float bL = easuLuma(b), cL = easuLuma(c), eL = easuLuma(e), fL = easuLuma(F), gL = easuLuma(g), hL = easuLuma(h);
    float iL = easuLuma(i), jL = easuLuma(j), kL = easuLuma(k), lL = easuLuma(l), nL = easuLuma(n), oL = easuLuma(o);
    float2 dir = 0;
    float len = 0;
    easuSet(dir, len, (1.0 - f.x) * (1.0 - f.y), bL, eL, fL, gL, jL);
    easuSet(dir, len, f.x * (1.0 - f.y), cL, fL, gL, hL, kL);
    easuSet(dir, len, (1.0 - f.x) * f.y, fL, iL, jL, kL, nL);
    easuSet(dir, len, f.x * f.y, gL, jL, kL, lL, oL);
    float dirR = dot(dir, dir);
    bool zero = dirR < 1.0 / 32768.0;
    dir = zero ? float2(1, 0) : dir * rsqrt(dirR);
    len = len * 0.5;
    len *= len;
    float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + (0.21 - 0.5) * len;
    float clp = 1.0 / lob;
    float3 aC = 0;
    float aW = 0;
    easuTap(aC, aW, float2(0, -1) - f, dir, len2, lob, clp, b);
    easuTap(aC, aW, float2(1, -1) - f, dir, len2, lob, clp, c);
    easuTap(aC, aW, float2(-1, 1) - f, dir, len2, lob, clp, i);
    easuTap(aC, aW, float2(0, 1) - f, dir, len2, lob, clp, j);
    easuTap(aC, aW, float2(0, 0) - f, dir, len2, lob, clp, F);
    easuTap(aC, aW, float2(-1, 0) - f, dir, len2, lob, clp, e);
    easuTap(aC, aW, float2(1, 1) - f, dir, len2, lob, clp, k);
    easuTap(aC, aW, float2(2, 1) - f, dir, len2, lob, clp, l);
    easuTap(aC, aW, float2(2, 0) - f, dir, len2, lob, clp, h);
    easuTap(aC, aW, float2(1, 0) - f, dir, len2, lob, clp, g);
    easuTap(aC, aW, float2(1, 2) - f, dir, len2, lob, clp, o);
    easuTap(aC, aW, float2(0, 2) - f, dir, len2, lob, clp, n);
    float3 lo = min(min(F, g), min(j, k)), hi = max(max(F, g), max(j, k));
    return clamp(aC / max(aW, 1e-5), lo, hi);
}

// One sky mode. `mode` is a literal in every entry point, so each one only carries its own
// sky (smaller shaders, faster to compile and to run). The aurora's curtains come from the
// half-res pass in s1 (u_Pass0 = its size).
float3 customSky(float3 rd, uniform int mode) {
    float3 c;
    if (mode == 1) {
        c = clearSky(rd);
    } else if (mode == 2) {
        c = spaceBackground(rd);
        float3 moon = normalize(rotateY(float3(0.4, 0.5, -0.75), u_Sky.z));
        float m = dot(rd, moon);
        c += float3(0.9, 0.95, 1.0) * (smoothstep(0.9993, 0.9996, m) * 3.0 + pow(saturate(m), 400.0) * 0.4);
        float4 planet = ringedPlanet(rd, moon);
        c = c * (1.0 - planet.a) + planet.rgb * 0.5;
        c += meteors(rd, saturate(u_Sky2.y));
    } else if (mode == 3) {
        c = blackHoleSky(rd, blackHoleDirection(), 70.0 / u_Sky2.z);
        float3 pdir = normalize(rotateY(float3(-0.62, 0.14, 0.8), u_Sky.z));
        float4 planet = ringedPlanet(rd, normalize(blackHoleDirection() * 0.45 - pdir * 0.75 + float3(0, 0.35, 0)));
        c = c * (1.0 - planet.a) + planet.rgb;
        c += meteors(rd, saturate(u_Sky2.y));
    } else if (mode == 5) {
        c = ringWorldSky(rd);
        c += meteors(rd, saturate(u_Sky2.y) * 0.6);
    } else {
        c = spaceBackground(rd) * 0.7;
        c += meteors(rd, saturate(u_Sky2.y));
        // Below the horizon: dark ground haze (the space skies go on: you're in space).
        c = lerp(c, c * 0.15 + float3(0.01, 0.012, 0.02), smoothstep(0.0, -0.08, rd.y));
    }
    return c * u_Sky.w;
}

// Pass: custom sky (full res, only where depth = sky). s0 = full normal/depth
float4 skyPass(float2 uv, uniform int mode) {
    if (tex2Dlod(s0, float4(uv, 0, 0)).w < SKY_Z) return 0;
    float3 rd = viewToWorldDir(normalize(viewPosition(uv, 1.0)));
    float3 c = customSky(rd, mode);
    if (mode == 4) c += easuSample(s1, uv, u_Pass0) * u_Sky.w;
    return float4(min(c, 12.0), 1);
}
float4 PS_SkyClear(float2 uv : TEXCOORD0) : COLOR0 { return skyPass(uv, 1); }
float4 PS_SkyStars(float2 uv : TEXCOORD0) : COLOR0 { return skyPass(uv, 2); }
float4 PS_SkyBlackHole(float2 uv : TEXCOORD0) : COLOR0 { return skyPass(uv, 3); }
float4 PS_SkyAurora(float2 uv : TEXCOORD0) : COLOR0 { return skyPass(uv, 4); }
float4 PS_SkyRing(float2 uv : TEXCOORD0) : COLOR0 { return skyPass(uv, 5); }

// Pass: the aurora's curtains at half resolution. Only where one of the four full-res
// pixels below is sky. s0 = full normal/depth, u_Pass0.x = slices, u_Pass0.zw = full texel
float4 PS_AuroraHalf(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {
    float2 o = u_Pass0.zw * 0.5;
    float z = max(max(tex2Dlod(s0, float4(uv + float2(-o.x, -o.y), 0, 0)).w, tex2Dlod(s0, float4(uv + float2(o.x, -o.y), 0, 0)).w),
                  max(tex2Dlod(s0, float4(uv + float2(-o.x, o.y), 0, 0)).w, tex2Dlod(s0, float4(uv + float2(o.x, o.y), 0, 0)).w));
    if (z < SKY_Z) return 0;
    float3 rd = viewToWorldDir(normalize(viewPosition(uv, 1.0)));
    float3 rdNext = viewToWorldDir(normalize(viewPosition(uv + float2(0.0, u_Pass0.w * 2.0), 1.0)));
    return float4(auroraCurtains(rd, u_Pass0.x, ign(vpos), length(rdNext - rd)), 1);
}

// Lightning bolt (u_Light2.w = azimuth + 7 * distance step 0..50, < 0 = none): a jagged
// channel from the cloud base down to the ground, with two branches. Drawn in angular
// space around the strike's direction; geometry hides its lower part like a real one.
float boltWalk(float x, float seed) { // piecewise-linear random walk: the zigzag of a channel
    float i = floor(x);
    float a = hash12(float2(i, seed)) * 2.0 - 1.0;
    float b = hash12(float2(i + 1.0, seed)) * 2.0 - 1.0;
    return lerp(a, b, frac(x));
}

float boltPath(float h, float seed) {
    return boltWalk(h * 6.0, seed) * 0.55 + boltWalk(h * 21.0, seed + 3.1) * 0.2 + boltWalk(h * 67.0, seed + 7.7) * 0.06;
}

float boltLine(float d, float width) {
    return exp(-d * d / (width * width)) + 0.015 * exp(-abs(d) / (width * 6.0));
}

float3 lightningBolt(float3 rd) {
    if (u_Light2.w < 0.0 || u_Light2.z <= 0.02) return 0;
    float stepD = floor(u_Light2.w / 7.0);
    float az = u_Light2.w - stepD * 7.0;
    float distance = stepD / 50.0;
    float range = lerp(900.0, 4500.0, distance);
    // Storm clouds tower high: the channel starts well above the visible cloud base.
    float base = max(u_Clouds.x > 0.0 ? u_Clouds.z : 650.0, 900.0) - u_UpView.w;
    float top = atan(max(base, 100.0) / range); // elevation where the channel leaves the cloud
    float da = atan2(rd.x, rd.z) - az;
    da -= 6.2831853 * floor(da / 6.2831853 + 0.5);
    float e = asin(clamp(rd.y, -1.0, 1.0));
    float h = e / top; // 0 = ground .. 1 = cloud base
    float2 q = float2(da * cos(e), e) / top;
    if (h < -0.1 || h > 1.1 || abs(q.x) > 1.2) return 0;
    // About 4 m of glowing channel, never thinner than a pixel.
    float px = 2.0 / (u_Proj.y * u_Screen.y);
    float width = max(4.0 / range, px * 0.8) / top;
    float seed = az * 13.7;
    float mainX = boltPath(h, seed) * 0.35;
    float b = boltLine(q.x - mainX, width);
    // Branches: split off the main channel and wander sideways, fading out.
    for (int k = 0; k < 2; k++) {
        float h0 = k == 0 ? 0.78 : 0.5;
        float len = k == 0 ? 0.35 : 0.25;
        float t = (h0 - h) / len; // 0 at the fork .. 1 at the tip
        if (t < 0.0 || t > 1.0) continue;
        float side = hash12(float2(seed, k)) > 0.5 ? 1.0 : -1.0;
        float x = boltPath(h0, seed) * 0.35 + side * t * len * 0.8 + boltWalk(t * 9.0, seed + k * 5.3) * 0.05 * t;
        b += boltLine(q.x - x, width * 0.7) * 0.5 * (1.0 - t);
    }
    // The channel fades into the cloud base and is dimmer and hazier far away.
    b *= smoothstep(1.08, 0.92, h) * (1.0 - 0.55 * distance);
    float strobe = saturate(u_Light2.z * 1.4 - 0.15);
    return float3(0.82, 0.86, 1.0) * b * strobe * 10.0;
}


// ---------------------------------------------------------------------------------
// Pass 3: average sky colour near the horizon (1x1), used as fog colour.
//   s0 = scene colour, s1 = full normal/depth, s2 = custom sky (when enabled)
// ---------------------------------------------------------------------------------
float4 PS_SkyAverage(float2 uv : TEXCOORD0) : COLOR0 {
    float3 sum = 0;
    float wsum = 0;
    [loop] for (int y = 0; y < 10; y++) {
        [loop] for (int x = 0; x < 16; x++) {
            float2 suv = float2((x + 0.5) / 16.0, (y + 0.5) / 10.0 * 0.75);
            float z = tex2Dlod(s1, float4(suv, 0, 0)).w;
            if (z >= SKY_Z) {
                float3 rd = viewToWorldDir(normalize(viewPosition(suv, 1.0)));
                float w = exp(-max(rd.y, 0.0) * 6.0); // favour the horizon
                float3 skyCol = u_Sky.x > 0.5 ? tex2Dlod(s2, float4(suv, 0, 0)).rgb : toLinear(tex2Dlod(s0, float4(suv, 0, 0)).rgb);
                sum += skyCol * w;
                wsum += w;
            }
        }
    }
    float3 fallback = u_SkyColor.rgb * 0.6;
    float3 avg = wsum > 0.01 ? sum / wsum : fallback;
    return float4(avg, saturate(wsum / 20.0));
}

// ---------------------------------------------------------------------------------
// Volumetric clouds (half res): a ray-marched cumulus layer, lit by the sun. Rendered
// for every pixel as if it were sky; the lighting pass composites it over sky pixels.
//   s7 = tiling cloud noise (r: Perlin-Worley, g/b/a: Worley octaves)
//   out.rgb = in-scattered light, out.a = transmittance
// ---------------------------------------------------------------------------------
float cloudShape(float3 p, float heightFrac) {
    float3 wind = float3(u_Proj2.z * 9.0, 0.0, u_Proj2.z * 3.5) * (0.4 + u_Nature.z);
    float4 n = tex3Dlod(s7, float4((p + wind) / 5600.0, 0));
    float shape = n.r * 0.75 + n.g * 0.25;
    // Flat, dense bottoms and rounded, thinning tops.
    float profile = smoothstep(0.0, 0.12, heightFrac) * smoothstep(1.0, 0.45, heightFrac);
    float coverage = u_Clouds.y;
    return saturate((shape * profile - (1.0 - coverage)) / max(coverage, 0.05));
}

float cloudDensity(float3 p, float heightFrac) {
    float d = cloudShape(p, heightFrac);
    if (d <= 0.0) return 0.0;
    // Erode the edges with finer noise (wisps).
    float3 wind = float3(u_Proj2.z * 16.0, -u_Proj2.z * 3.0, u_Proj2.z * 6.0) * (0.4 + u_Nature.z);
    float4 n = tex3Dlod(s7, float4((p + wind) / 1300.0, 0));
    float erode = (1.0 - (n.g * 0.5 + n.b * 0.3 + n.a * 0.2)) * 0.45;
    return saturate((d - erode) / (1.0 - erode));
}

float4 PS_Clouds(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {
    float3 rd = viewToWorldDir(normalize(viewPosition(uv, 1.0)));
    float base = u_Clouds.z, thickness = u_Clouds.w;
    float camY = u_UpView.w;
    if (rd.y < 0.012 || camY > base) return float4(0, 0, 0, 1);
    float t0 = (base - camY) / rd.y;
    float t1 = min((base + thickness - camY) / rd.y, t0 + thickness * 5.0);
    float steps = u_Quality.z;
    float dt = (t1 - t0) / steps;
    float t = t0 + dt * ign(vpos);
    float3 cam = cameraWorld();

    bool sunKnown = u_SunView.w > 0.5;
    float3 sun = sunKnown ? u_SunWorld.xyz : float3(0, 1, 0);
    float mu = dot(rd, sun);
    // Dual-lobe phase: bright silver lining towards the sun, some back scattering.
    float phase = (henyeyGreenstein(mu, 0.6) * 0.7 + henyeyGreenstein(mu, -0.2) * 0.3) * 4.0 * PI;
    float day = sunKnown ? u_SunWorld.w : 0.2;
    float3 sunLight = u_SunColor.rgb / max(luma(u_SunColor.rgb), 0.1) * 1.8 * day;
    float4 skyAvg = tex2Dlod(s4, float4(0.5, 0.5, 0, 0));
    float3 ambient = lerp(skyAvg.rgb, u_SkyColor.rgb * 0.45, 0.5) * (0.35 + 0.65 * day);

    float transmittance = 1.0;
    float3 scattered = 0;
    [loop] for (int i = 0; i < (int)steps; i++) {
        float3 pos = cam + rd * t;
        float hf = (pos.y - base) / thickness;
        float d = cloudDensity(pos, hf);
        if (d > 0.003) {
            // Optical depth towards the sun (4 growing steps through the coarse shape).
            float od = 0.0;
            [unroll] for (int j = 1; j <= 4; j++) {
                float3 lp = pos + sun * (j * j * 55.0);
                od += cloudShape(lp, (lp.y - base) / thickness) * (j * 2 - 1) * 55.0;
            }
            // Beer's law plus a softer term standing in for multiple scattering.
            float lightT = max(exp(-od * 0.012), exp(-od * 0.003) * 0.35);
            float powder = 1.0 - exp(-d * 6.0);
            float3 light = sunLight * lightT * phase * lerp(0.6, 1.0, powder) + ambient * lerp(0.45, 1.1, hf);
            float stepT = exp(-0.03 * d * dt);
            scattered += transmittance * light * (1.0 - stepT);
            transmittance *= stepT;
            if (transmittance < 0.02) break;
        }
        t += dt;
    }
    // Far clouds dissolve into the horizon haze.
    float fade = exp(-t0 / 22000.0) * smoothstep(0.012, 0.07, rd.y);
    return float4(scattered * fade, lerp(1.0, transmittance, fade));
}

// ---------------------------------------------------------------------------------
// Surfaces: grass, wet roads, puddles, water. The masks are shared by the reflection
// pass and the lighting pass.
// ---------------------------------------------------------------------------------
float grassMask(float3 c, float3 nWorld, float emissive) {
    float greenness = (c.g - max(c.r, c.b)) / max(c.g, 1e-3);
    return smoothstep(0.22, 0.42, greenness) * smoothstep(0.55, 0.85, nWorld.y) * (1.0 - emissive) * step(c.g, 0.6);
}

float puddleMask(float3 world, float flatness, float grass) {
    if (u_Weather.z <= 0.0) return 0.0;
    float n = noise2(world.xz * 0.16) * 0.65 + noise2(world.xz * 0.55 + 13.0) * 0.35;
    float threshold = 0.8 - u_Weather.z * 0.32;
    return smoothstep(threshold, threshold + 0.18, n) * flatness * (1.0 - grass) * saturate(u_Weather.x * 2.0);
}

// Open water (TMUF Island/Bay/Coast): flat, blue.
float waterMask(float3 c, float3 nWorld, float3 world, float z, float emissive) {
    if (u_Weather.w <= 0.0 || u_Water.z == 0.0) return 0.0;
    float level = smoothstep(0.93, 0.99, nWorld.y) * (1.0 - emissive) * u_Weather.w;
    if (u_Water.z > 0.5) {
        // The game tells where its water is (the height its own water shaders use): every
        // flat surface at that height is water - pools, rivers, the sea.
        // Far away the depth gets too coarse to tell the water from the ground 1 m above.
        float d = min(abs(world.y - u_Water.x), abs(world.y - u_Water.y));
        return smoothstep(0.2, 0.05, d) * saturate((350.0 - z) / 100.0) * level;
    }
    // Without the engine hooks: guess from the colour.
    float blue = (c.b - max(c.r, c.g * 0.8)) / max(c.b, 1e-3);
    return smoothstep(0.15, 0.35, blue) * level;
}

// Expanding rings where raindrops hit standing water. Returns the surface slope.
float2 rippleSlope(float2 xz, float t) {
    float2 g = 0;
    [unroll] for (int k = 0; k < 2; k++) {
        float2 q = xz * 2.3 + k * 3.71;
        float2 cell = floor(q);
        float2 f = frac(q) - 0.5;
        float h = hash12(cell + k * 17.0);
        float2 center = (float2(h, hash12(cell + 5.3)) - 0.5) * 0.4;
        float phase = frac(t * 1.2 + h * 7.0);
        float2 d = f - center;
        float dl = length(d);
        float x = dl - phase * 0.5;
        float wave = cos(x * 45.0) * exp(-x * x * 300.0) * (1.0 - phase);
        g += d / max(dl, 1e-3) * wave;
    }
    return g * 0.3;
}

// Gentle swell plus fine chop for open water.
float2 waveSlope(float2 xz, float t) {
    float2 g = 0;
    float2 d0 = float2(0.8, 0.6), d1 = float2(-0.4, 0.92), d2 = float2(0.97, -0.24), d3 = float2(-0.7, -0.7);
    g += d0 * cos(dot(d0, xz) * 0.9 + t * 1.3) * 0.05;
    g += d1 * cos(dot(d1, xz) * 1.7 + t * 1.9) * 0.035;
    g += d2 * cos(dot(d2, xz) * 3.1 + t * 2.6) * 0.025;
    g += d3 * cos(dot(d3, xz) * 5.3 + t * 3.4) * 0.015;
    float2 q = xz * 2.0 + t * 0.6;
    g += float2(noise2(q + float2(0.08, 0)) - noise2(q - float2(0.08, 0)), noise2(q + float2(0, 0.08)) - noise2(q - float2(0, 0.08))) * 0.35;
    return g;
}

// Short grass blades seen through a few height layers (parallax), swaying in the wind.
// Returns a brightness factor for the grass albedo.
float grassBlades(float3 ground, float3 rd, float t) {
    const float H = 0.09;
    float cosView = max(-rd.y, 0.12);
    [loop] for (int k = 0; k < 6; k++) {
        float h = H * (1.0 - (k + 0.5) / 6.0);
        float3 p = ground - rd * (h / cosView);
        float2 q = p.xz / 0.04;
        float2 cell = floor(q);
        float2 f = frac(q);
        float r1 = hash12(cell), r2 = hash12(cell + 7.31);
        float bladeH = H * (0.5 + 0.5 * r1);
        if (h < bladeH) {
            float k01 = h / bladeH;
            float2 sway = float2(sin(t * 1.9 + cell.x * 0.37 + cell.y * 0.11), cos(t * 1.4 + cell.y * 0.29)) * (0.1 + 0.25 * u_Nature.z) * k01 * k01;
            float2 center = 0.5 + (float2(r1, r2) - 0.5) * 0.5 + sway;
            float width = 0.3 * (1.0 - k01 * 0.85);
            if (length(f - center) < width) return lerp(0.62, 1.22, k01) * (0.9 + 0.2 * r2);
        }
    }
    return 0.5; // looking between the blades: shaded ground
}

float3 grassDetail(float3 c, float3 world, float3 rd, float dist, float mask) {
    // Patches of fresher and drier grass.
    float n = noise2(world.xz * 0.045) * 0.6 + noise2(world.xz * 0.17 + 7.0) * 0.4;
    float3 g = c * lerp(0.86, 1.12, n);
    g = lerp(g, g * float3(1.18, 1.06, 0.72), smoothstep(0.62, 0.85, n) * 0.35);
    // Mowing stripes, 8 m wide along the block grid. Like real mowed grass they swap
    // bright and dark depending on which way you look along them.
    if (u_Nature.y > 0.0) {
        float stripe = clamp(sin(world.x * PI / 8.0) * 4.0, -1.0, 1.0);
        float2 v = normalize(rd.xz + 1e-4);
        g *= 1.0 + (stripe * v.y * 0.15 + 0.02) * u_Nature.y;
    }
    // Blades close to the camera, faded out before they would shimmer.
    if (u_Nature.x > 0.0) {
        float footprint = dist * 2.0 / (abs(u_Proj.y) * u_Screen.y); // metres per pixel
        float fade = smoothstep(0.03, 0.012, footprint);
        if (fade > 0.0) g *= lerp(1.0, grassBlades(world, rd, u_Proj2.z), fade * u_Nature.x);
    }
    return lerp(c, g, mask);
}

// Snow lying on everything that faces up: snow fields on the grass and open ground, a
// thin, even layer on the track and everything built (thicker in soft drifts) so the road
// stays readable, little on painted surfaces (cars, coloured borders), none on lights,
// water and steep walls. Returns the coverage; `snow` is the snow's colour before the
// scene's lighting. Only the surface itself decides: nothing pops in while driving.
float snowCover(float3 c, float3 world, float3 nWorld, float dist, float grass, float emissive, float water, out float3 snow) {
    snow = 0;
    float cover = u_Snow.y;
    if (cover <= 0.0) return 0.0;
    float up = smoothstep(0.35, 0.8, nWorld.y);
    if (up <= 0.0) return 0.0;
    float peak = max(c.r, max(c.g, c.b));
    float sat = (peak - min(c.r, min(c.g, c.b))) / max(peak, 1e-3);
    float built = 1.0 - grass;
    float colourful = smoothstep(0.3, 0.55, sat) * built;
    // Fields: closed at full cover, bare patches below. Far away the noise blends into its
    // average (no shimmer).
    float patches = noise2(world.xz * 0.25) * 0.65 + noise2(world.xz * 1.3 + 7.0) * 0.35;
    patches = lerp(patches, 0.5, smoothstep(80.0, 200.0, dist));
    float field = smoothstep(0.85 - cover, 1.15 - cover, patches + 0.3);
    // Built surfaces: an even layer, thicker where the wind piled it up. Only slow noise,
    // so it never turns into a speckle pattern.
    float drift = noise2(world.xz * 0.12 + 3.0) * 0.6 + noise2(world.xz * 0.45) * 0.4;
    float thin = cover * (0.45 + 0.35 * smoothstep(0.3, 0.8, drift));
    float mask = saturate(lerp(field, thin, built) * lerp(1.0, 0.15, colourful) * up * (1.0 - emissive) * (1.0 - water));
    if (mask <= 0.0) return 0.0;
    // Drifts: soft dunes and wind ripples shade the fields a little.
    float dune = noise2(world.xz * 0.07 + float2(3.1, 1.7)) - noise2(world.xz * 0.07 + float2(3.4, 1.7));
    float ripple = sin(dot(world.xz, float2(0.8, 0.6)) * 3.0 + noise2(world.xz * 0.5) * 6.0);
    float shade = 1.0 + dune * 0.35 + ripple * 0.02 * (1.0 - smoothstep(10.0, 40.0, dist)) * grass;
    // The game's own light and shadow on the surface carry over (dimmed snow in its shade).
    shade *= lerp(0.72, 1.0, saturate(luma(c) * 4.0));
    snow = float3(0.86, 0.9, 0.96) * shade;
    return mask;
}

// The colour of driving snow: the overcast light, whitened.
float3 snowHaze(float3 sky) {
    return luma(sky) * float3(0.95, 0.97, 1.0) * 1.25 + 0.02;
}

// Snow glinting in the sun: single crystals catch it, a few at a time, changing with the
// view. `lit` = direct sun on the surface.
float3 snowSparkle(float3 world, float3 rd, float dist, float lit) {
    if (lit <= 0.0 || dist > 30.0) return 0;
    float2 cell = floor(world.xz * 30.0);
    float h = hash12(cell + floor(rd.xz * 40.0) * 0.37);
    return u_SunColor.rgb * step(0.996, h) * lit * 3.0 * (1.0 - dist / 30.0);
}

// Distant snow: soft flakes drifting on three cylinders around the camera (12 - 30 m),
// beyond the snow particles. Returns the coverage.
float snowVeil(float3 rd, float sceneDist) {
    float az = atan2(rd.x, rd.z);
    float horiz = max(sqrt(saturate(1.0 - rd.y * rd.y)), 0.05);
    float elevation = rd.y / horiz;
    float mppScale = 2.0 / (abs(u_Proj.y) * u_Screen.y); // metres per pixel at 1 m
    float t = u_Proj2.z;
    float acc = 0.0;
    [unroll] for (int l = 0; l < 3; l++) {
        float d = l == 0 ? 12.0 : (l == 1 ? 19.0 : 30.0);
        if (sceneDist > d) {
            float2 q = float2(az * d, elevation * d);
            q.x += t * u_Nature.z * (2.0 + l) + sin(q.y * 0.7 + t * 0.9 + l) * 0.3; // drifting
            q.y += t * (1.1 + l * 0.15);                                              // falling
            float2 cellSize = float2(0.55, 0.55);
            float2 cell = floor(q / cellSize);
            float2 f = frac(q / cellSize) - 0.5;
            float h = hash12(cell + l * 17.0);
            if (h < saturate(u_Snow.x * 0.5) * 0.9) {
                float2 centre = float2(frac(h * 91.7), frac(h * 47.3)) * 0.6 - 0.3;
                float size = max(0.012, d * mppScale * 0.8) / cellSize.x;
                float r = length(f - centre) / size;
                acc += saturate(1.2 - r) * (0.75 - l * 0.18);
            }
        }
    }
    return saturate(acc);
}

// Distant rain: streaks on three cylinders around the camera (13 - 32 m away), beyond the
// rain particles, hidden behind closer geometry. Returns the streak coverage.
float rainStreaks(float3 rd, float sceneDist) {
    float az = atan2(rd.x, rd.z);
    float horiz = max(sqrt(saturate(1.0 - rd.y * rd.y)), 0.05);
    float elevation = rd.y / horiz;
    float mppScale = 2.0 / (abs(u_Proj.y) * u_Screen.y); // metres per pixel at 1 m
    float acc = 0.0;
    [unroll] for (int l = 0; l < 3; l++) {
        float d = l == 0 ? 13.0 : (l == 1 ? 20.0 : 32.0);
        if (sceneDist > d) {
            float2 q = float2(az * d, elevation * d);
            q.x += q.y * 0.12 * u_Nature.z;            // wind slant
            q.y += u_Proj2.z * (7.5 + l * 0.6);        // falling
            float2 cellSize = float2(0.16, 1.25);
            float2 cell = floor(q / cellSize);
            float2 f = frac(q / cellSize);
            float h = hash12(cell + l * 31.0);
            if (h < u_Weather.y * 0.85) {
                // Everything else about the drop comes from the same hash.
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

float3 skyReflection(float3 r, float4 skyAvg, float daylight) {
    float3 zenith = u_SkyColor.rgb * (0.2 + 0.8 * daylight) * 0.8;
    return lerp(skyAvg.rgb, zenith, saturate(r.y * 1.5));
}

// ---------------------------------------------------------------------------------
// Screen-space reflections (half res) for wet roads, puddles and water.
//   s0 = scene colour (sRGB), s1 = full normal/depth, s2 = half normal/depth
//   out.rgb = reflected colour (linear), out.a = hit confidence
// ---------------------------------------------------------------------------------
float4 PS_Reflect(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {
    float4 nd = tex2Dlod(s2, float4(uv, 0, 0));
    if (nd.w >= SKY_Z) return 0;
    float3 nWorld = viewToWorldDir(nd.xyz);
    bool glossy = (u_Weather.x > 0.0 || u_Nature.w > 0.0) && nWorld.y > 0.5;
    bool water = u_Weather.w > 0.0 && u_Water.z != 0.0 && nWorld.y > 0.9;
    if (!glossy && !water) return 0;

    float3 p = viewPosition(uv, nd.w);
    float3 r = reflect(normalize(p), nd.xyz);
    float maxDist = min(120.0, nd.w * 1.5 + 25.0);
    float noise = ign(vpos);
    float tPrev = 0.05;
    // A polished dry track is a mirror: finer steps so the reflection holds together.
    float steps = u_Nature.w > 1.0 ? 32.0 : 16.0;
    [loop] for (int i = 1; i <= (int)steps; i++) {
        float t = (i - 1 + noise) / steps;
        t = t * t * maxDist + 0.1;
        float3 q = p + r * t;
        if (q.z < 0.25) break;
        float2 quv = projectToUV(q);
        if (any(quv < 0.0) || any(quv > 1.0)) break;
        float delta = q.z - tex2Dlod(s1, float4(quv, 0, 0)).w;
        if (delta > 0.0 && delta < 0.4 + t * 0.1) {
            // Refine between the last two steps.
            float a = tPrev, b = t;
            [unroll] for (int k = 0; k < 6; k++) {
                float m = (a + b) * 0.5;
                float3 qm = p + r * m;
                if (qm.z - tex2Dlod(s1, float4(projectToUV(qm), 0, 0)).w > 0.0) b = m; else a = m;
            }
            quv = projectToUV(p + r * b);
            float edge = min(min(quv.x, 1.0 - quv.x), min(quv.y, 1.0 - quv.y));
            float confidence = saturate(edge * 10.0) * saturate((1.0 - t / maxDist) * 3.0);
            float3 hit = toLinear(tex2Dlod(s0, float4(quv, 0, 0)).rgb);
            // Light sources stay bright in the reflection.
            float peak = max(hit.r, max(hit.g, hit.b));
            hit *= 1.0 + u_Rays.w * 0.5 * smoothstep(0.75, 1.0, peak);
            return float4(hit, confidence);
        }
        tPrev = t;
    }
    return 0;
}

// ---------------------------------------------------------------------------------
// Pass 4: lighting composite -> HDR (full res)
//   s0 = scene colour (sRGB), s1 = full normal/depth, s2 = AO/shadow (half, blurred),
//   s3 = half normal/depth, s4 = sky average (1x1), s5 = custom sky, s6 = clouds,
//   s8 = reflections (half), s9 = neon light spill (quarter)
// ---------------------------------------------------------------------------------
float3 upsampleOcclusion(float2 uv, float z, float3 n) {
    // Joint bilateral upsample from half resolution.
    float2 halfTexel = u_Screen.zw * 2.0;
    float2 base = (floor(uv / halfTexel - 0.5) + 0.5) * halfTexel;
    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int j = 0; j < 2; j++) {
        [unroll] for (int i = 0; i < 2; i++) {
            float2 suv = base + float2(i, j) * halfTexel;
            float4 nd = tex2Dlod(s3, float4(suv, 0, 0));
            float2 f = 1.0 - abs(uv - suv) / halfTexel;
            float w = max(f.x * f.y, 0.001);
            w *= exp(-abs(nd.w - z) / (0.03 * z + 0.05));
            w *= pow(saturate(dot(nd.xyz, n)), 8.0) + 0.002;
            sum += tex2Dlod(s2, float4(suv, 0, 0)).rgb * w;
            wsum += w;
        }
    }
    return wsum > 1e-4 ? sum / wsum : 1.0;
}

// Rain on every surface, not only the road: thin streams running down steep wet surfaces
// (walls, barriers, the car's flanks) and small splash rings on everything that faces the
// sky (barrier tops, the car's roof). Puddles have their own ripples.
float3 rainOnSurfaces(float3 c, float3 world, float3 nWorld, float dist, float3 skyLight, float puddle) {
    float rain = saturate(u_Weather.y) * saturate(u_Weather.x * 1.5);
    if (rain <= 0.0 || dist > 40.0) return c;
    float t = u_Proj2.z;
    float fade = 1.0 - smoothstep(20.0, 40.0, dist);
    float steep = 1.0 - smoothstep(0.35, 0.7, abs(nWorld.y));
    if (steep > 0.0) {
        float3 tangent = normalize(cross(float3(0, 1, 0), nWorld) + 1e-5);
        float u = dot(world, tangent) * 6.0; // a stream every ~16 cm at most
        float col = floor(u);
        float h = hash12(float2(col, 3.7));
        float x = frac(u) - 0.5 - (h - 0.5) * 0.4 + sin(world.y * 7.0 + h * 6.0) * 0.08;
        float seg = frac(world.y * 1.5 + t * (0.8 + h) + h * 10.0);
        float stream = smoothstep(0.09, 0.0, abs(x)) * smoothstep(0.0, 0.1, seg) * smoothstep(0.75, 0.3, seg) * step(0.55, h);
        c = lerp(c, c * 0.6 + skyLight * 0.35, stream * steep * fade * rain);
    }
    float up = smoothstep(0.5, 0.8, nWorld.y) * (1.0 - puddle);
    if (up > 0.0) {
        float2 q = world.xz * 5.0; // 20 cm cells, one drop at a time each
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

float3 upsampleGI(float2 uv, float z, float3 n) { // from quarter res, depths from s3 (half res)
    float2 halfTexel = u_Screen.zw * 4.0;
    float2 base = (floor(uv / halfTexel - 0.5) + 0.5) * halfTexel;
    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int j = 0; j < 2; j++) {
        [unroll] for (int i = 0; i < 2; i++) {
            float2 suv = base + float2(i, j) * halfTexel;
            float4 nd = tex2Dlod(s3, float4(suv, 0, 0));
            float2 f = 1.0 - abs(uv - suv) / halfTexel;
            float w = max(f.x * f.y, 0.001);
            w *= exp(-abs(nd.w - z) / (0.03 * z + 0.05));
            w *= pow(saturate(dot(nd.xyz, n)), 8.0) + 0.002;
            sum += tex2Dlod(s11, float4(suv, 0, 0)).rgb * w;
            wsum += w;
        }
    }
    return wsum > 1e-4 ? sum / wsum : 0.0;
}

// Light sources in the game's LDR image: lamps and neon strips. Returns 0..1.
//   c = linear colour, nWorld = world normal, rd = view ray (to tell sun glare from lights)
float emissiveAmount(float2 uv, float3 c, float3 nWorld, float3 rd, float dist) {
    // Far away the game's mipmaps blend a strip with its dark frame: it gets dimmer, so
    // the bar for "bright" comes down with distance.
    float low = lerp(0.5, 0.28, saturate((dist - 20.0) / 150.0));
    float peak = max(c.r, max(c.g, c.b));
    if (peak <= low) return 0.0;
    float sat = (peak - min(c.r, min(c.g, c.b))) / max(peak, 1e-3);
    // Erode: single bright texels (specular glints on asphalt lit by floodlights) are
    // texture detail, not lights. A pixel survives if it continues along some axis, so
    // thin lines - the neon strips far down the track - stay lights.
    float along = 0.0;
    [unroll] for (int e = 0; e < 4; e++) {
        float2 eo = (e == 0 ? float2(1, 0) : (e == 1 ? float2(0, 1) : (e == 2 ? float2(1, 1) : float2(1, -1)))) * u_Screen.zw;
        float3 ea = toLinear(tex2Dlod(s0, float4(uv + eo, 0, 0)).rgb);
        float3 eb = toLinear(tex2Dlod(s0, float4(uv - eo, 0, 0)).rgb);
        along = max(along, min(max(ea.r, max(ea.g, ea.b)), max(eb.r, max(eb.g, eb.b))));
    }
    peak = min(peak, along);
    float surround = 0.0;
    float2 r = float2(0.012 * u_Screen.y * u_Screen.z, 0.012);
    [unroll] for (int k = 0; k < 8; k++) {
        float a = k * (PI / 4.0) + 0.39;
        float3 sc = toLinear(tex2Dlod(s0, float4(uv + float2(cos(a), sin(a)) * r, 0, 0)).rgb);
        surround += max(sc.r, max(sc.g, sc.b));
    }
    surround /= 8.0;
    // Clearly brighter than the surroundings (lamps at night) ...
    float contrast = saturate((peak - surround) * 3.0) * pow(smoothstep(0.75, 1.0, peak), 2.0);
    // ... or a strongly coloured bright strip (neon), which glows in daylight too.
    float neon = smoothstep(0.5, 0.8, sat) * smoothstep(low, low + 0.35, peak);
    float amount = max(contrast, neon * 0.75);
    // The sun's glare is a reflection, not a light: white, and either on the road (facing
    // up) or on any surface that mirrors the sun towards the camera (roofs, glass).
    // Sunset glare is orange, so in the mirror direction only clearly saturated colour
    // (neon) stays a light.
    float white = 1.0 - smoothstep(0.3, 0.6, sat);
    float mirror = u_Light2.y > 0.5 ? smoothstep(0.75, 0.95, dot(reflect(rd, nWorld), u_SunWorld.xyz)) : 0.0;
    float glare = max(smoothstep(0.7, 0.9, nWorld.y) * white, mirror * (1.0 - smoothstep(0.6, 0.9, sat)));
    return amount * (1.0 - glare);
}

// The game bakes its sun into the image: lit sides, and white specular glare where a
// surface mirrors the sun. Day-for-night (a night sky over a day map) takes it back out.
//   Returns the factor for the colour.
float removeGameSun(float3 c, float3 nWorld, float3 rd, float sat, float night) {
    if (night <= 0.0 || u_Light2.y < 0.5) return 1.0;
    float3 sun = u_SunWorld.xyz;
    float above = saturate(sun.y * 6.0 + 0.4);
    // Direct light: the sun side was lit with about twice the ambient.
    float direct = saturate(dot(nWorld, sun)) * above;
    float k = 1.0 / (1.0 + direct * 1.1 * night);
    // Glare: bright, white and in the mirror direction of the sun.
    float mirror = pow(saturate(dot(reflect(rd, nWorld), sun)), 4.0) * above;
    // (Sunset glare is orange: only clearly coloured light - neon - is safe.)
    float glare = mirror * (1.0 - smoothstep(0.45, 0.8, sat)) * smoothstep(0.15, 0.5, luma(c));
    // Glare comes down to the level of plain lit asphalt - not below it, or it would leave
    // dark patches. Moonlight is flat: what's still bright gets compressed.
    float l = luma(c) * k;
    float cap = lerp(1.0, min(1.0, 0.11 / max(l, 1e-3)), saturate(glare * 2.0) * night);
    float flatten = 1.0 / (1.0 + 2.5 * night * l * cap);
    return k * cap * flatten;
}

float4 PS_Lighting(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {
    float3 srgb = tex2Dlod(s0, float4(uv, 0, 0)).rgb;
    float3 c = toLinear(srgb);
    float3 albedo = c;
    float4 nd = tex2Dlod(s1, float4(uv, 0, 0));
    float3 rdView = normalize(viewPosition(uv, 1.0));
    float3 rd = viewToWorldDir(rdView);
    float daylight = u_SunWorld.w;
    float4 skyAvg = tex2Dlod(s4, float4(0.5, 0.5, 0, 0));

    if (nd.w >= SKY_Z) {
        if (u_Sky.x > 0.5) {
            c = tex2Dlod(s5, float4(uv, 0, 0)).rgb;
        } else {
            // Game sky: richer gradient, glow around the sun.
            float horizon = 1.0 - saturate(rd.y * 3.0);
            float3 graded = lerp(c, c * c / max(luma(c), 1e-3), 0.35); // deepen saturation
            graded *= 1.0 + horizon * 0.25;
            c = lerp(c, graded, u_Atmo.w);
            if (u_SunView.w > 0.5) {
                float mu = dot(rd, u_SunWorld.xyz);
                c += sunScatter(rd, u_Rays.z * 0.5) * daylight;
                // Sun disc and a tight halo (feeds the bloom).
                c += u_SunColor.rgb * (smoothstep(0.9994, 0.99975, mu) * 1.8 + pow(saturate(mu), 900.0) * 0.35) * u_Rays.z * daylight;
            }
        }
        // Day-for-night on the game's own sky (storm, manual night setting).
        if (u_Sky.x < 0.5) c = lerp(c, luma(c) * float3(0.85, 0.9, 1.0), saturate(u_Sky.y * 1.6)) * lerp(1.0, 0.3, u_Sky.y);
        if (u_Clouds.x > 0.0) {
            float4 cloud = tex2Dlod(s6, float4(uv, 0, 0));
            // Lightning lights the cloud layer from inside.
            float3 inner = float3(0.75, 0.8, 1.0) * u_Light2.z * (1.0 - cloud.a) * 1.4;
            c = c * lerp(1.0, cloud.a, u_Clouds.x) + (cloud.rgb * lerp(1.0, 0.45, u_Sky.y) + inner) * u_Clouds.x;
        }
        c += float3(0.6, 0.65, 0.85) * u_Light2.z * 0.3 * saturate(rd.y * 3.0 + 0.3);
        c += lightningBolt(rd);
        if (u_SunView.w > 0.5) c += volumetricLight(uv, rd, daylight).rgb;
        if (u_Weather.y > 0.0) c += (skyAvg.rgb * 0.8 + 0.03) * rainStreaks(rd, SKY_Z) * 1.0;
        if (u_Snow.x > 0.0) {
            c = lerp(c, snowHaze(skyAvg.rgb), saturate(u_Snow.x * 0.35) * (1.0 - saturate(rd.y * 1.5)));
            c = lerp(c, snowHaze(skyAvg.rgb), snowVeil(rd, SKY_Z) * 0.7);
        }
        return float4(c, 1);
    }

    float3 n = nd.xyz;
    float3 p = viewPosition(uv, nd.w);
    float3 occ = upsampleOcclusion(uv, nd.w, n);
    float dist = length(p);

    // Low-frequency normal for relighting: averages neighbours on the same surface so
    // reconstruction noise (z-fighting decals, mesh seams) doesn't flicker the sun term.
    float3 nLight = n;
    float2 o = u_Screen.zw * 3.0;
    [unroll] for (int k = 0; k < 4; k++) {
        float2 dir = k == 0 ? float2(1, 1) : (k == 1 ? float2(-1, 1) : (k == 2 ? float2(1, -1) : float2(-1, -1)));
        float4 s4 = tex2Dlod(s3, float4(uv + dir * o, 0, 0));
        nLight += s4.xyz * (abs(s4.w - nd.w) < 0.02 * nd.w ? 1.0 : 0.0);
    }
    nLight = normalize(nLight);
    float3 nWorld = viewToWorldDir(nLight);
    float3 world = worldPosition(p);
    float flatness = smoothstep(0.8, 0.97, nWorld.y);

    // Expand the LDR image back toward HDR: light sources glow.
    float emissive = emissiveAmount(uv, c, nWorld, rd, dist);
    float3 glow = c * u_Rays.w * emissive;
    // White lights and screens glow less than coloured ones, or they bleach to flat white.
    float peakC = max(c.r, max(c.g, c.b));
    glow *= lerp(0.6, 1.0, smoothstep(0.25, 0.6, (peakC - min(c.r, min(c.g, c.b))) / max(peakC, 1e-3)));

    // The game bakes a bright sun glare into the asphalt; tame it so the road doesn't
    // wash out to white (it's a reflection, it gets its sun highlight further down).
    float sat = (max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b))) / max(max(c.r, max(c.g, c.b)), 1e-3);
    // Below the tonemapper's knee the asphalt keeps its texture contrast.
    float glare = smoothstep(0.6, 0.9, nWorld.y) * (1.0 - emissive) * smoothstep(0.18, 0.55, luma(c)) * (1.0 - smoothstep(0.25, 0.5, sat));
    c *= lerp(1.0, 0.6, glare);
    c *= removeGameSun(c, nWorld, rd, sat, u_Sky.y);

    // --- Grass ---
    float grass = grassMask(c, nWorld, emissive);
    if (u_Nature.x + u_Nature.y > 0.0 && grass > 0.0) c = grassDetail(c, world, rd, dist, grass);

    // --- Wet surfaces, puddles, water ---
    float wet = u_Weather.x * (1.0 - grass * 0.5) * (1.0 - emissive);
    float puddle = puddleMask(world, flatness, grass) * (1.0 - emissive);
    float water = waterMask(c, nWorld, world, nd.w, emissive);
    // --- Snow on the ground (dry, matte: no wet sheen or puddles under it) ---
    float3 snowColour;
    float snow = snowCover(c, world, nWorld, dist, grass, emissive, water, snowColour);
    if (snow > 0.0) {
        c = lerp(c, snowColour, snow);
        albedo = lerp(albedo, snowColour, snow);
        wet *= 1.0 - snow;
        puddle *= 1.0 - snow;
    }
    float reflectivity = max(max(wet * lerp(0.12, 0.55, flatness), puddle), water);
    // Dry reflections: 0..1 a glossy sheen, 1..2 polished like a showroom floor (objects and
    // checkpoints mirror clearly in the dry track).
    float polish = saturate(u_Nature.w - 1.0);
    float dry = flatness * (1.0 - grass) * (1.0 - emissive) * (1.0 - snow);
    reflectivity = max(reflectivity, min(u_Nature.w, 1.0) * dry * lerp(0.35, 1.0, polish));
    polish *= dry;
    float2 slope = 0;
    if (reflectivity > 0.0) {
        float t = u_Proj2.z;
        if (u_Weather.y > 0.0) slope = rippleSlope(world.xz, t) * u_Weather.y * (puddle + water + wet * 0.25);
        if (water > 0.0) slope += waveSlope(world.xz, t) * water;
        // Rough wet asphalt breaks the reflection up.
        if (wet > 0.0) {
            float2 q = world.xz * 6.0;
            slope += (float2(noise2(q), noise2(q + 9.7)) - 0.5) * 0.12 * wet * (1.0 - puddle);
        }
    }
    float3 nRefl = normalize(nWorld + float3(-slope.x, 0.0, -slope.y));
    // Refraction: the pool floor seen through moving waves, a little bluer with depth.
    if (water > 0.0) {
        float2 refr = worldToViewDir(float3(slope.x, 0.0, slope.y)).xy * float2(1, -1) * 0.03;
        float3 below = toLinear(tex2Dlod(s0, float4(uv + refr, 0, 0)).rgb) * (luma(c) / max(luma(albedo), 1e-3));
        c = lerp(c, below * float3(0.82, 0.95, 1.0), water * 0.85);
    }
    // Water fills the pores: wet asphalt and puddles are darker.
    c *= lerp(1.0, 0.62, wet * lerp(0.4, 1.0, flatness)) * lerp(1.0, 0.6, puddle) * lerp(1.0, 0.55, water);

    // --- Sun and sky light ---
    float ao = lerp(1.0, occ.r, u_Light.x);
    float sunVis = occ.g;
    float shadow = 1.0;
    if (u_SunView.w > 0.5) {
        float ndl = saturate(dot(nLight, u_SunView.xyz));
        shadow = lerp(1.0, sunVis, u_Light.z * saturate(ndl * 4.0));
        // In shadow: lose direct light and take on the sky's colour.
        float3 skyTint = u_SkyColor.rgb / max(luma(u_SkyColor.rgb), 1e-3);
        float shadowed = (1.0 - shadow);
        c *= lerp(1.0, 0.32, shadowed);
        c *= lerp(float3(1, 1, 1), skyTint, u_SkyColor.w * saturate(shadowed + (1.0 - ndl) * 0.35));
        // In sunlight: warm directional light.
        float lit = ndl * shadow * daylight;
        float3 sunTint = u_SunColor.rgb / max(luma(u_SunColor.rgb), 1e-3);
        c *= lerp(float3(1, 1, 1), sunTint * 1.15, u_SunColor.w * lit);
        // Grass is translucent: back-lit blades glow.
        if (grass > 0.0) c += c * sunTint * pow(saturate(dot(rd, u_SunWorld.xyz)), 4.0) * grass * lit * 0.6;
        if (snow > 0.0) c += snowSparkle(world, rd, dist, lit * u_SunColor.w) * snow;
    }
    c *= ao;
    // Ambient occlusion also tints toward the sky colour (bounce from the sky dome).
    c *= lerp(u_SkyColor.rgb / max(luma(u_SkyColor.rgb), 1e-3), float3(1, 1, 1), lerp(1.0, ao, 0.5 * u_SkyColor.w));

    // Neon light spilling onto nearby surfaces (screen-space, blurred light buffer).
    if (u_Light2.x > 0.0) {
        float3 spill = tex2Dlod(s9, float4(uv, 0, 0)).rgb;
        // Far away the glow would shrink to nothing: it gets stronger with distance.
        c += spill * u_Light2.x * 4.0 * lerp(1.0, 2.0, saturate(dist / 120.0)) * lerp(0.15, 1.0, saturate(luma(c) * 5.0)) * ao * (1.0 - emissive);
    }

    // --- Reflections ---
    if (reflectivity > 0.0) {
        float3 r = reflect(rd, nRefl);
        float NdotV = saturate(dot(nRefl, -rd));
        float smoothness = max(max(max(puddle, water), wet * 0.35), polish);
        float2 distort = worldToViewDir(float3(slope.x, 0.0, slope.y)).xy * float2(1, -1) * 0.04;
        float4 ssr = tex2Dlod(s8, float4(uv + distort, 0, 0));
        // Rough wet asphalt and the polished dry track blur the reflection (5 taps): no
        // speckles from the half-res ray hits. Puddles and water stay sharp.
        float soften = max(polish, wet * (1.0 - max(puddle, water)));
        if (soften > 0.0) {
            float2 o = u_Screen.zw * 3.0;
            float4 soft = ssr * 2.0 + tex2Dlod(s8, float4(uv + float2(o.x, 0), 0, 0)) + tex2Dlod(s8, float4(uv - float2(o.x, 0), 0, 0)) +
                          tex2Dlod(s8, float4(uv + float2(0, o.y), 0, 0)) + tex2Dlod(s8, float4(uv - float2(0, o.y), 0, 0));
            ssr = lerp(ssr, soft / 6.0, soften);
        }
        // Standing water reads as a mirror even from above (the eye adapts to it): strongly
        // where it mirrors objects and lights, gently where it only mirrors the sky (a pale
        // sky everywhere made puddles look like camouflage patches).
        float F = max(0.02 + 0.98 * pow(1.0 - NdotV, 5.0), max(puddle, water) * lerp(0.1, 0.4, ssr.a));
        F = max(F, polish * lerp(0.05, 0.55, ssr.a));
        float3 refl = lerp(skyReflection(r, skyAvg, daylight), ssr.rgb, ssr.a);
        c = lerp(c, refl, saturate(F * reflectivity * lerp(0.6, 1.0, smoothness)));
        // Sun glint (GGX): sharp on puddles and water, broad on wet asphalt.
        if (u_SunView.w > 0.5) {
            float rough = lerp(0.35, 0.05, smoothness);
            float a2 = rough * rough * rough * rough;
            float3 h = normalize(u_SunWorld.xyz - rd);
            float nh = saturate(dot(nRefl, h));
            float dd = nh * nh * (a2 - 1.0) + 1.0;
            float spec = min(a2 / (PI * dd * dd) * F * 0.25, 30.0) * saturate(dot(nRefl, u_SunWorld.xyz));
            c += u_SunColor.rgb * spec * reflectivity * shadow * daylight * 0.5;
        }
    }

    c = rainOnSurfaces(c, world, nWorld, dist, skyAvg.rgb * 0.8 + 0.04, puddle);

    // One bounce of light: the surface's own colour (the game's image is the best albedo
    // there is) times the light bounced onto it, less where it's occluded. The game's image
    // is already shaded (darker than the true albedo), hence the boost.
    if (u_Volume.z > 0.0) c += albedo * upsampleGI(uv, nd.w, n) * u_Volume.z * 2.5 * lerp(0.5, 1.0, occ.r);

    // Aerial perspective: exponential height fog lit by the sky and the sun.
    float density = u_Atmo.x * 0.0009 + u_Snow.x * 0.005;
    float falloff = max(u_Atmo.y * 0.012, 1e-4);
    float camY = u_UpView.w;
    float rdy = rd.y;
    float fogAmount = density * exp(-camY * falloff) * (1.0 - exp(-dist * rdy * falloff)) / (abs(rdy) > 1e-4 ? rdy * falloff : 1e-4 * falloff);
    if (abs(rdy) <= 1e-4) fogAmount = density * exp(-camY * falloff) * dist;
    fogAmount = 1.0 - exp(-max(fogAmount, 0.0));
    float3 fogColor = lerp(u_SkyColor.rgb * 0.7 * (0.3 + 0.7 * daylight), skyAvg.rgb, skyAvg.a * 0.75);
    fogColor = lerp(fogColor, snowHaze(skyAvg.rgb), saturate(u_Snow.x * 0.6));
    float4 volume = volumetricLight(uv, rd, daylight);
    if (u_SunView.w > 0.5) fogColor += sunScatter(rd, u_Atmo.z * 0.6) * daylight * volume.a;
    c = lerp(c, fogColor, saturate(fogAmount));
    if (u_SunView.w > 0.5) c += volume.rgb;

    // Day-for-night for the night skies: darker, cooler, less saturated - but lights keep glowing.
    float night = u_Sky.y;
    float3 unlit = c;
    c = lerp(c, luma(c) * float3(0.55, 0.72, 1.0), night * 0.55) * lerp(1.0, 0.16, night);
    // A black hole in the sky is the brightest light around: warm light on everything that
    // faces it, and the haze glows towards it.
    float4 bh = blackHoleLight();
    if (bh.w > 0.0) {
        float3 bhColor = float3(1.0, 0.8, 0.6) * bh.w;
        c += unlit * bhColor * saturate(dot(nWorld, bh.xyz) * 0.8 + 0.2) * 0.35;
        c += bhColor * pow(saturate(dot(rd, bh.xyz)), 6.0) * saturate(fogAmount * 2.0 + 0.05) * 0.4;
    }
    c += glow * (1.0 - 0.6 * saturate(fogAmount)); // lights shine through the haze
    // Lightning: a cold flash over everything (surfaces facing up and the haze most).
    if (u_Light2.z > 0.0) c += (c * 2.5 + fogColor * 0.4 * fogAmount + 0.01) * float3(0.8, 0.85, 1.0) * u_Light2.z * (0.6 + 0.4 * saturate(nWorld.y));

    if (u_Weather.y > 0.0) c += (skyAvg.rgb * 0.8 + u_SunColor.rgb * daylight * 0.15 + 0.03) * rainStreaks(rd, dist) * 1.0;
    if (u_Snow.x > 0.0) c = lerp(c, snowHaze(skyAvg.rgb), snowVeil(rd, dist) * 0.7);
    return float4(c, 1);
}

// ---------------------------------------------------------------------------------
// Neon light spill: coloured light sources light up the surfaces around them.
// Quarter-res light buffer, blurred over a world-space radius, depth-aware.
// ---------------------------------------------------------------------------------
// s0 = scene colour (sRGB, bilinear), s1 = full normal/depth. out.rgb = light, out.a = view z
float4 PS_SpillDown(float2 uv : TEXCOORD0) : COLOR0 {
    float3 sum = 0, strongest = 0;
    float zmin = SKY_Z;
    // Four bilinear taps cover the whole 4x4 block, so thin far-away strips aren't missed.
    [unroll] for (int j = 0; j < 2; j++) {
        [unroll] for (int i = 0; i < 2; i++) {
            float2 suv = uv + (float2(i, j) * 2.0 - 1.0) * u_Screen.zw;
            float3 c = toLinear(tex2Dlod(s0, float4(suv, 0, 0)).rgb);
            float4 nd = tex2Dlod(s1, float4(suv, 0, 0));
            float peak = max(c.r, max(c.g, c.b));
            float sat = (peak - min(c.r, min(c.g, c.b))) / max(peak, 1e-3);
            // Strongly coloured, bright: neon. (White lamps light things too, but white
            // also means sun glare and white paint - leave those out.)
            float low = lerp(0.45, 0.28, saturate((nd.w - 20.0) / 150.0));
            float e = smoothstep(0.45, 0.8, sat) * smoothstep(low, low + 0.4, peak) * (nd.w < SKY_Z ? 1.0 : 0.0);
            sum += c * e;
            strongest = max(strongest, c * e);
            zmin = min(zmin, nd.w);
        }
    }
    // Half average, half brightest: a strip one pixel wide still lights its surroundings.
    return float4(lerp(sum * 0.25, strongest, 0.5), zmin);
}

// s0 = light buffer, u_Pass0.xy = direction * texel. Radius scales with 1/z (about 3 m).
float4 PS_SpillBlur(float2 uv : TEXCOORD0) : COLOR0 {
    float4 center = tex2Dlod(s0, float4(uv, 0, 0));
    float z = min(center.a, 2000.0);
    float pxPerMetre = abs(u_Proj.y) * u_Screen.y * 0.25 * 0.5 / z;  // quarter-res pixels
    // At least a few pixels: far away the glow must stay visible, not shrink to nothing.
    float stepPx = clamp(3.0 * pxPerMetre / 6.0, 1.6, 12.0);
    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int i = -6; i <= 6; i++) {
        float4 s = tex2Dlod(s0, float4(uv + u_Pass0.xy * i * stepPx, 0, 0));
        float w = exp(-i * i / 18.0) * exp(-abs(s.a - center.a) / (0.25 * z + 0.5));
        sum += s.rgb * w;
        wsum += w;
    }
    // Normalised by the kernel, not by the depth-rejected weights: light from a strip
    // must fall off, not be renormalised up.
    return float4(sum / 6.4, center.a);
}

// ---------------------------------------------------------------------------------
// Cinematic: depth of field and camera motion blur (HDR, before bloom).
// ---------------------------------------------------------------------------------
// Auto focus (1x1): what's in the middle of the frame, a little below the centre (the car
// or the road ahead), smoothed over time.
//   s0 = full normal/depth, s1 = previous focus, u_Pass0.x = blend, u_Pass0.y = reset
float4 PS_Focus(float2 uv : TEXCOORD0) : COLOR0 {
    float focus = u_Cine.z;
    if (focus <= 0.0) {
        float inv = 0.0;
        [unroll] for (int j = -1; j <= 1; j++) {
            [unroll] for (int i = -1; i <= 1; i++) {
                float z = tex2Dlod(s0, float4(0.5 + i * 0.05, 0.56 + j * 0.05, 0, 0)).w;
                inv += 1.0 / min(z, 3000.0);
            }
        }
        focus = 9.0 / inv;
    }
    float previous = tex2Dlod(s1, float4(0.5, 0.5, 0, 0)).r;
    if (u_Pass0.y > 0.5 || !(previous > 0.0 && previous < 1e5)) previous = focus;
    return float4(lerp(previous, focus, u_Pass0.x), 0, 0, 1);
}

// Signed blur radius in pixels (negative = in front of the focus plane).
float circleOfConfusion(float z, float focus) {
    float maxPx = u_Cine.w * u_Screen.y / 1080.0;
    return clamp((1.0 - focus / max(z, 0.1)) * u_Cine.y * 2.5, -1.0, 1.0) * maxPx;
}

// Gathered bokeh (half res). s0 = HDR, s1 = half normal/depth, s2 = focus (1x1)
// out.rgb = blurred colour, out.a = how much blur covers this pixel (0..1)
float4 PS_DofBlur(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {
    float focus = tex2Dlod(s2, float4(0.5, 0.5, 0, 0)).r;
    float z = tex2Dlod(s1, float4(uv, 0, 0)).w;
    float coc = abs(circleOfConfusion(z, focus));
    float maxPx = u_Cine.w * u_Screen.y / 1080.0;
    float3 sum = tex2Dlod(s0, float4(uv, 0, 0)).rgb;
    float wsum = 1.0;
    float foreground = 0.0;
    float rot = ign(vpos) * 6.2832;
    const int N = 16;
    // Golden-angle spiral; the direction is rotated incrementally (no sin/cos per tap).
    float2 dir = float2(cos(rot), sin(rot));
    const float2x2 golden = float2x2(-0.737369, -0.675490, 0.675490, -0.737369);
    [loop] for (int i = 0; i < N; i++) {
        float r = sqrt((i + 0.5) / N) * maxPx;
        dir = mul(golden, dir);
        float2 suv = uv + dir * r * u_Screen.zw;
        float sz = tex2Dlod(s1, float4(suv, 0, 0)).w;
        float scoc = abs(circleOfConfusion(sz, focus));
        // A sample counts if its blur disc reaches this pixel. Sharp foreground can't be
        // pulled over by blurry background.
        bool inFront = sz < z;
        float reach = inFront ? scoc : min(scoc, coc);
        float w = saturate(reach - r + 1.0);
        if (inFront) foreground = max(foreground, w * saturate(scoc / maxPx * 4.0));
        sum += tex2Dlod(s0, float4(suv, 0, 0)).rgb * w;
        wsum += w;
    }
    return float4(sum / wsum, max(saturate(coc / maxPx * 4.0), foreground));
}

float3 dofComposite(float2 uv) {
    float3 sharp = tex2Dlod(s0, float4(uv, 0, 0)).rgb;
    if (u_Cine.y <= 0.0) return sharp;
    float4 blur = tex2Dlod(s1, float4(uv, 0, 0));
    return lerp(sharp, blur.rgb, smoothstep(0.05, 0.35, blur.a));
}

// DOF composite + camera motion blur (full res).
//   s0 = HDR (sharp), s1 = DOF (half), s2 = full normal/depth
float4 PS_Cinematic(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {
    float3 c = dofComposite(uv);
    if (u_Cine.x <= 0.0 || u_Temporal.y < 0.5) return float4(c, 1);
    float z = tex2Dlod(s2, float4(uv, 0, 0)).w;
    float prevZ;
    float2 prevUV = reproject(worldPosition(viewPosition(uv, min(z, 20000.0))), prevZ);
    // Velocity from the camera motion; anything close to a chase camera (the car) moves
    // with it and stays sharp.
    float2 vel = (uv - prevUV) * u_Cine.x * smoothstep(5.0, 14.0, z);
    float len = length(vel);
    if (len > 0.025) vel *= 0.025 / len;
    if (len * u_Screen.x < 0.75) return float4(c, 1);
    float jitter = ign(vpos) - 0.5;
    float3 sum = c;
    float wsum = 1.0;
    [loop] for (int i = 0; i < 10; i++) {
        float2 suv = saturate(uv + vel * ((i + 0.5 + jitter) / 10.0 - 0.5));
        float sz = tex2Dlod(s2, float4(suv, 0, 0)).w;
        // Don't smear the (sharp) car into the moving background.
        float w = (sz < z * 0.8 && sz < 14.0) ? 0.0 : 1.0;
        sum += dofComposite(suv) * w;
        wsum += w;
    }
    return float4(sum / wsum, 1);
}

// ---------------------------------------------------------------------------------
// Volumetric light shafts (half res): occluder mask then radial blur toward the sun.
// ---------------------------------------------------------------------------------
// s0 = HDR, s1 = full normal/depth
float4 PS_RayMask(float2 uv : TEXCOORD0) : COLOR0 {
    float z = tex2Dlod(s1, float4(uv, 0, 0)).w;
    if (z < SKY_Z) return 0;
    float3 rd = viewToWorldDir(normalize(viewPosition(uv, 1.0)));
    float mu = saturate(dot(rd, u_SunWorld.xyz));
    float3 c = tex2Dlod(s0, float4(uv, 0, 0)).rgb;
    float window = pow(mu, 12.0) * 0.6 + pow(mu, 120.0) * 0.45;
    return float4(min(c, 1.5) * window + u_SunColor.rgb * pow(mu, 40.0) * 0.25, 1);
}

// s0 = mask/previous blur, u_Pass0.x = step scale, u_Pass0.y = decay
float4 PS_RayBlur(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {
    const int SAMPLES = 28;
    float2 delta = (uv - u_SunScreen.xy) * u_Pass0.x / SAMPLES;
    float2 suv = uv - delta * ign(vpos);
    float3 sum = 0;
    float weight = 1.0;
    float wsum = 0;
    [loop] for (int i = 0; i < SAMPLES; i++) {
        sum += tex2Dlod(s0, float4(suv, 0, 0)).rgb * weight;
        wsum += weight;
        weight *= u_Pass0.y;
        suv -= delta;
    }
    return float4(sum / max(wsum, 1e-3), 1);
}

// ---------------------------------------------------------------------------------
// Bloom: 13-tap downsample (Karis average on the first level), tent upsample.
// ---------------------------------------------------------------------------------
float3 karis(float3 a, float3 b, float3 c, float3 d) {
    float wa = 1.0 / (1.0 + luma(a)), wb = 1.0 / (1.0 + luma(b));
    float wc = 1.0 / (1.0 + luma(c)), wd = 1.0 / (1.0 + luma(d));
    return (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
}

// s0 = source, u_Pass0.xy = source texel, u_Pass0.z = first level (Karis + threshold)
float4 PS_BloomDown(float2 uv : TEXCOORD0) : COLOR0 {
    float2 t = u_Pass0.xy;
    float3 a = tex2D(s0, uv + t * float2(-2, -2)).rgb;
    float3 b = tex2D(s0, uv + t * float2(0, -2)).rgb;
    float3 c = tex2D(s0, uv + t * float2(2, -2)).rgb;
    float3 d = tex2D(s0, uv + t * float2(-1, -1)).rgb;
    float3 e = tex2D(s0, uv + t * float2(1, -1)).rgb;
    float3 f = tex2D(s0, uv + t * float2(-2, 0)).rgb;
    float3 g = tex2D(s0, uv).rgb;
    float3 h = tex2D(s0, uv + t * float2(2, 0)).rgb;
    float3 i = tex2D(s0, uv + t * float2(-1, 1)).rgb;
    float3 j = tex2D(s0, uv + t * float2(1, 1)).rgb;
    float3 k = tex2D(s0, uv + t * float2(-2, 2)).rgb;
    float3 l = tex2D(s0, uv + t * float2(0, 2)).rgb;
    float3 m = tex2D(s0, uv + t * float2(2, 2)).rgb;
    float3 result;
    if (u_Pass0.z > 0.5) {
        result = karis(d, e, i, j) * 0.5 + karis(a, b, f, g) * 0.125 + karis(b, c, g, h) * 0.125 +
                 karis(f, g, k, l) * 0.125 + karis(g, h, l, m) * 0.125;
        // Soft threshold: keep mostly the bright part, but let a little of everything glow.
        float br = max(result.r, max(result.g, result.b));
        float soft = clamp(br - 0.6, 0.0, 0.8);
        soft = soft * soft / (4.0 * 0.4 + 1e-4);
        float contrib = max(soft, br - 1.0) / max(br, 1e-4);
        result *= lerp(0.08, 1.0, saturate(contrib));
    } else {
        result = (d + e + i + j) * 0.125 + (a + c + k + m) * 0.03125 + (b + f + h + l) * 0.0625 + g * 0.125;
    }
    return float4(result, 1);
}

// s0 = lower (smaller) level, s1 = current level, u_Pass0.xy = lower texel, u_Pass0.z = radius
float4 PS_BloomUp(float2 uv : TEXCOORD0) : COLOR0 {
    float2 t = u_Pass0.xy;
    float3 s = tex2D(s0, uv + t * float2(-1, -1)).rgb + tex2D(s0, uv + t * float2(1, -1)).rgb +
               tex2D(s0, uv + t * float2(-1, 1)).rgb + tex2D(s0, uv + t * float2(1, 1)).rgb;
    s += (tex2D(s0, uv + t * float2(0, -1)).rgb + tex2D(s0, uv + t * float2(-1, 0)).rgb +
          tex2D(s0, uv + t * float2(1, 0)).rgb + tex2D(s0, uv + t * float2(0, 1)).rgb) * 2.0;
    s += tex2D(s0, uv).rgb * 4.0;
    s /= 16.0;
    return float4(tex2D(s1, uv).rgb + s * u_Pass0.z, 1);
}

// ---------------------------------------------------------------------------------
// Auto exposure: average log luminance (1x1) and temporal adaptation.
// ---------------------------------------------------------------------------------
// s0 = small bloom level (HDR), out.r = average log2 luminance
float4 PS_Luminance(float2 uv : TEXCOORD0) : COLOR0 {
    float sum = 0;
    float wsum = 0;
    [loop] for (int y = 0; y < 12; y++) {
        [loop] for (int x = 0; x < 16; x++) {
            float2 suv = float2((x + 0.5) / 16.0, (y + 0.5) / 12.0);
            float l = luma(tex2Dlod(s0, float4(suv, 0, 0)).rgb);
            float2 d = suv - 0.5;
            float w = 1.0 - dot(d, d) * 1.6; // centre-weighted metering
            sum += log2(max(l, 1e-4)) * w;
            wsum += w;
        }
    }
    return float4(sum / wsum, 0, 0, 1);
}

// s0 = current average, s1 = previous adapted, u_Pass0.x = blend factor
float4 PS_Adapt(float2 uv : TEXCOORD0) : COLOR0 {
    float current = tex2Dlod(s0, float4(0.5, 0.5, 0, 0)).r;
    float previous = tex2Dlod(s1, float4(0.5, 0.5, 0, 0)).r;
    if (u_Pass0.y > 0.5 || !(abs(previous) < 100.0)) previous = current;
    return float4(lerp(previous, current, u_Pass0.x), 0, 0, 1);
}

// ---------------------------------------------------------------------------------
// Final: combine, expose, tonemap, grade (full res -> LDR with luma in alpha)
//   s0 = HDR, s1 = bloom (1/2 res), s2 = light shafts (half), s3 = adapted log lum (1x1),
//   s4 = full normal/depth (debug), s5 = AO/shadow (debug), s6 = bloom 1/8 (lens flare)
// ---------------------------------------------------------------------------------
float3 shoulderCurve(float3 x) {
    // Linear below the knee (the game's mid-tones stay as they were), then a long
    // exponential shoulder that only reaches white for real light sources.
    const float knee = 0.5;
    float3 over = max(x - knee, 0.0);
    float3 shoulder = knee + (1.0 - knee) * (1.0 - exp(-over / ((1.0 - knee) * 1.7)));
    return x < knee ? x : shoulder;
}
float3 tonemap(float3 x) {
    // Half per channel (bright lights desaturate toward white), half on the brightest
    // channel (keeps the hue of sunlit surfaces and neon instead of bleaching them).
    float peak = max(x.r, max(x.g, x.b));
    float3 hue = x * (shoulderCurve(peak.xxx).x / max(peak, 1e-5));
    return lerp(shoulderCurve(x), hue, 0.5);
}

float3 grade(float3 c) {
    // White balance (simple temperature / tint gains).
    float temp = u_Grade1.y, tnt = u_Grade1.z;
    c *= float3(1.0 + temp * 0.18, 1.0 - tnt * 0.1, 1.0 - temp * 0.18);

    // Contrast around mid grey in log space.
    float3 lc = log2(max(c, 1e-5));
    lc = (lc - log2(0.18)) * u_Grade0.z + log2(0.18);
    c = exp2(lc);

    // Lift / gamma / gain.
    c = c * u_Grade2.z + u_Grade2.x * (1.0 - c);
    c = pow(max(c, 0.0), 1.0 / u_Grade2.y);

    // Vibrance (boosts dull colours more) and saturation.
    float l = luma(c);
    float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
    float sat = (mx - mn) / max(mx, 1e-4);
    c = lerp(l.xxx, c, 1.0 + u_Grade1.x * (1.0 - sat));
    c = lerp(l.xxx, c, u_Grade0.w);

    // Split toning: cool shadows, warm highlights.
    float3 coolTint = u_SkyColor.rgb / max(luma(u_SkyColor.rgb), 1e-3);
    float3 warmTint = u_SunColor.rgb / max(luma(u_SunColor.rgb), 1e-3);
    float w = smoothstep(0.0, 0.6, l);
    c *= lerp(float3(1, 1, 1), lerp(coolTint, warmTint, w), u_Grade1.w * 0.25);
    return max(c, 0.0);
}

float3 flareSource(float2 uv) {
    // Only the brightest parts (sun, lamps) produce ghosts.
    float3 c = tex2Dlod(s6, float4(uv, 0, 0)).rgb;
    return max(c - 2.5, 0.0);
}

float3 lensFlare(float2 uv) {
    float3 sum = 0;
    float2 toCenter = 0.5 - uv;
    [unroll] for (int i = 1; i <= 4; i++) {
        float2 g = uv + toCenter * (i * 0.5);
        float fade = pow(saturate(1.0 - length(g - 0.5) / 0.7), 2.0);
        float3 tint = i == 1 ? float3(1.0, 0.7, 0.4) : (i == 2 ? float3(0.45, 0.75, 1.0) : (i == 3 ? float3(0.75, 1.0, 0.55) : float3(1.0, 0.55, 0.85)));
        sum += flareSource(g) * fade * tint * (1.0 / i);
    }
    return sum;
}

float4 PS_Final(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {
    int debugView = (int)(u_Post.w + 0.5);
    if (debugView == 1) {
        float z = tex2D(s4, uv).w;
        return float4(z >= SKY_Z ? float3(0.4, 0.6, 1.0) : saturate(log2(z) / 10.0).xxx, 1);
    }
    if (debugView == 2) return float4(tex2D(s4, uv).xyz * 0.5 + 0.5, 1);
    if (debugView == 3) return float4(tex2D(s5, uv).rrr, 1);
    if (debugView == 4) return float4(tex2D(s5, uv).ggg, 1);
    if (debugView == 5) return float4(toSRGB(tex2D(s2, uv).rgb), 1);
    if (debugView == 6) return float4(toSRGB(tex2D(s1, uv).rgb), 1);
    if (debugView == 7) return float4(tex2D(s5, uv).bbb, 1);

    // Chromatic aberration toward the screen edges.
    float2 d = uv - 0.5;
    float2 ca = d * dot(d, d) * u_Bloom.w * 0.03;
    float3 c = tex2D(s0, uv).rgb;
    if (u_Bloom.w > 0.0) {
        c.r = tex2D(s0, uv - ca).r;
        c.b = tex2D(s0, uv + ca).b;
    }

    float3 bloom = tex2D(s1, uv).rgb;
    c = lerp(c, bloom, u_Bloom.x);

    // Light shafts.
    if (u_SunScreen.w > 0.0) {
        float3 rays = tex2D(s2, uv).rgb;
        c += rays * u_Rays.x * u_SunScreen.z * 0.3;
    }
    // Ghosts are only plausible for point-like sources: limit them to the visible sun.
    if (u_Bloom.z * u_SunScreen.z > 0.0) c += lensFlare(uv) * u_Bloom.z * 0.35 * u_SunScreen.z;

    // Exposure: manual EV and (partial) auto exposure toward middle grey.
    float avgLog = tex2D(s3, float2(0.5, 0.5)).r;
    float autoEv = clamp(log2(0.2) - avgLog, -1.5, 0.15) * u_Grade0.y;
    c *= u_Grade0.x * exp2(autoEv);

    c = grade(c);
    c = tonemap(c);

    // Vignette.
    float vig = 1.0 - dot(d, d) * u_Grade2.w * 1.6;
    c *= saturate(vig);

    float3 srgb = toSRGB(c);
    // Film grain + dithering (also hides banding of the 8-bit output).
    float grain = hash12(vpos + frac(u_Proj2.z) * 1000.0) - 0.5;
    float grainAmount = u_Post.x * smoothstep(0.0, 0.45, luma(srgb)) * (1.2 - luma(srgb));
    srgb += grain * (grainAmount + 1.0 / 255.0);
    srgb = saturate(srgb);
    return float4(srgb, luma(srgb));
}

// ---------------------------------------------------------------------------------
// FXAA 3.11 (quality, ~preset 12) - s0 = LDR with luma in alpha, u_Pass0.xy = texel
// ---------------------------------------------------------------------------------
float4 PS_FXAA(float2 uv : TEXCOORD0) : COLOR0 {
    float2 t = u_Pass0.xy;
    float4 rgbyM = tex2Dlod(s0, float4(uv, 0, 0));
    if (u_Post.z < 0.5) return rgbyM;
    float lumaM = rgbyM.w;
    float lumaS = tex2Dlod(s0, float4(uv + float2(0, t.y), 0, 0)).w;
    float lumaE = tex2Dlod(s0, float4(uv + float2(t.x, 0), 0, 0)).w;
    float lumaN = tex2Dlod(s0, float4(uv - float2(0, t.y), 0, 0)).w;
    float lumaW = tex2Dlod(s0, float4(uv - float2(t.x, 0), 0, 0)).w;
    float maxSM = max(lumaS, lumaM), minSM = min(lumaS, lumaM);
    float maxESM = max(lumaE, maxSM), minESM = min(lumaE, minSM);
    float maxWN = max(lumaN, lumaW), minWN = min(lumaN, lumaW);
    float rangeMax = max(maxWN, maxESM), rangeMin = min(minWN, minESM);
    float range = rangeMax - rangeMin;
    if (range < max(0.0312, rangeMax * 0.125)) return rgbyM;

    float lumaNW = tex2Dlod(s0, float4(uv + float2(-t.x, -t.y), 0, 0)).w;
    float lumaSE = tex2Dlod(s0, float4(uv + float2(t.x, t.y), 0, 0)).w;
    float lumaNE = tex2Dlod(s0, float4(uv + float2(t.x, -t.y), 0, 0)).w;
    float lumaSW = tex2Dlod(s0, float4(uv + float2(-t.x, t.y), 0, 0)).w;
    float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;
    float subpixRcpRange = 1.0 / range;
    float subpixNSWE = lumaNS + lumaWE;
    float edgeHorz1 = -2.0 * lumaM + lumaNS, edgeVert1 = -2.0 * lumaM + lumaWE;
    float lumaNESE = lumaNE + lumaSE, lumaNWNE = lumaNW + lumaNE;
    float edgeHorz2 = -2.0 * lumaE + lumaNESE, edgeVert2 = -2.0 * lumaN + lumaNWNE;
    float lumaNWSW = lumaNW + lumaSW, lumaSWSE = lumaSW + lumaSE;
    float edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2);
    float edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);
    float edgeHorz3 = -2.0 * lumaW + lumaNWSW, edgeVert3 = -2.0 * lumaS + lumaSWSE;
    float edgeHorz = abs(edgeHorz3) + edgeHorz4, edgeVert = abs(edgeVert3) + edgeVert4;
    float subpixNWSWNESE = lumaNWSW + lumaNESE;
    float lengthSign = t.x;
    bool horzSpan = edgeHorz >= edgeVert;
    float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
    if (!horzSpan) { lumaN = lumaW; lumaS = lumaE; } else lengthSign = t.y;
    float subpixB = (subpixA * (1.0 / 12.0)) - lumaM;
    float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM, lumaSS = lumaS + lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) lengthSign = -lengthSign;
    float subpixC = saturate(abs(subpixB) * subpixRcpRange);
    float2 posB = uv;
    float2 offNP = horzSpan ? float2(t.x, 0) : float2(0, t.y);
    if (!horzSpan) posB.x += lengthSign * 0.5; else posB.y += lengthSign * 0.5;
    float2 posN = posB - offNP, posP = posB + offNP;
    float subpixD = -2.0 * subpixC + 3.0;
    float lumaEndN = tex2Dlod(s0, float4(posN, 0, 0)).w;
    float subpixE = subpixC * subpixC;
    float lumaEndP = tex2Dlod(s0, float4(posP, 0, 0)).w;
    if (!pairN) lumaNN = lumaSS;
    float gradientScaled = gradient * 0.25;
    float lumaMM = lumaM - lumaNN * 0.5;
    float subpixF = subpixD * subpixE;
    bool lumaMLTZero = lumaMM < 0.0;
    lumaEndN -= lumaNN * 0.5;
    lumaEndP -= lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled, doneP = abs(lumaEndP) >= gradientScaled;
    static const float steps[10] = {1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0, 8.0, 8.0};
    [loop] for (int i = 1; i < 10 && !(doneN && doneP); i++) {
        if (!doneN) { posN -= offNP * steps[i]; lumaEndN = tex2Dlod(s0, float4(posN, 0, 0)).w - lumaNN * 0.5; doneN = abs(lumaEndN) >= gradientScaled; }
        if (!doneP) { posP += offNP * steps[i]; lumaEndP = tex2Dlod(s0, float4(posP, 0, 0)).w - lumaNN * 0.5; doneP = abs(lumaEndP) >= gradientScaled; }
    }
    float dstN = horzSpan ? uv.x - posN.x : uv.y - posN.y;
    float dstP = horzSpan ? posP.x - uv.x : posP.y - uv.y;
    bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
    float spanLength = dstP + dstN;
    bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    float spanLengthRcp = 1.0 / spanLength;
    bool directionN = dstN < dstP;
    float dst = min(dstN, dstP);
    bool goodSpan = directionN ? goodSpanN : goodSpanP;
    float subpixG = subpixF * subpixF;
    float pixelOffset = (dst * (-spanLengthRcp)) + 0.5;
    float subpixH = subpixG * 0.75;
    float pixelOffsetGood = goodSpan ? pixelOffset : 0.0;
    float pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
    if (!horzSpan) uv.x += pixelOffsetSubpix * lengthSign; else uv.y += pixelOffsetSubpix * lengthSign;
    return float4(tex2Dlod(s0, float4(uv, 0, 0)).rgb, lumaM);
}

// ---------------------------------------------------------------------------------
// AMD FidelityFX CAS (contrast adaptive sharpening) - s0 = LDR, u_Pass0.xy = texel
// ---------------------------------------------------------------------------------
// Rain drops on the lens. Two kinds, like on a real camera in the rain:
//  - small droplets that land, sit still and slowly dry off,
//  - bigger drops that run down the lens, wobbling, with a thin wet trail behind them.
// Water on a lens is clear: it only bends the image behind it (no dark rim, no "bubble").
//   returns uv offset (xy) and a brightness factor (z, ~1)
float3 lensDrops(float2 uv) {
    float3 r = float3(0, 0, 1);
    float amount = u_Volume.w;
    if (amount <= 0.0) return r;
    float aspect = u_Screen.x * u_Screen.w;
    float2 p = float2(uv.x * aspect, uv.y); // square units
    float t = u_Proj2.z;

    // Small droplets, sitting still.
    {
        float scale = 26.0;
        float2 q = p * scale;
        float2 cell = floor(q);
        float h = hash12(cell + 91.7);
        float cycle = t * (0.03 + 0.04 * h) + h * 11.0;
        float life = frac(cycle);
        if (hash12(cell + floor(cycle) * 7.7) < amount * 0.32) {
            float2 centre = (float2(hash12(cell + 1.7), hash12(cell + 4.3)) - 0.5) * 0.5;
            float2 d = frac(q) - 0.5 - centre;
            float ang = atan2(d.y, d.x);
            float radius = lerp(0.12, 0.3, hash12(cell + 8.1)) * smoothstep(0.0, 0.03, life) * (1.0 - smoothstep(0.6, 1.0, life));
            float k = length(d) / max(radius, 1e-4) * (1.0 + 0.12 * sin(ang * 3.0 + h * 20.0));
            if (k < 1.0) {
                float bulge = sqrt(1.0 - k * k); // the drop's dome
                r.xy = -d / scale * float2(1.0 / aspect, 1.0) * 1.4 * bulge;
                r.z = 1.0 + 0.06 * bulge - 0.12 * smoothstep(0.85, 1.0, k);
            }
        }
    }

    // Running drops: one per column at most, sliding down at their own pace.
    {
        float scale = 9.0;
        float col = floor(p.x * scale);
        float h = hash12(float2(col, 17.3));
        float speed = 0.06 + 0.12 * h;
        float cycle = t * speed + h * 5.0;
        if (hash12(float2(col, floor(cycle) * 3.1)) < amount * 0.5) {
            float y = frac(cycle) * 1.4 - 0.2;                 // top to bottom of the screen
            float cx = (col + 0.5 + (h - 0.5) * 0.5) / scale + sin(y * 9.0 + h * 30.0) * 0.006;
            float radius = lerp(0.012, 0.022, frac(h * 7.3));
            float2 d = float2(p.x - cx, p.y - y);
            d.y *= d.y > 0.0 ? 0.8 : 1.3;                       // heavier at the bottom
            float k = length(d) / radius;
            if (k < 1.0) {
                float bulge = sqrt(1.0 - k * k);
                r.xy = -d * float2(1.0 / aspect, 1.0) * 1.8 * bulge;
                r.z = 1.0 + 0.08 * bulge - 0.12 * smoothstep(0.85, 1.0, k);
            } else if (p.y < y && p.y > y - 0.25) {
                // The wet trail it leaves: a thin streak that dries from the top.
                float along = (y - p.y) / 0.25;
                float tx = cx + sin(p.y * 9.0 + h * 30.0) * 0.006 - sin(y * 9.0 + h * 30.0) * 0.006;
                float w = radius * 0.3 * (1.0 - along);
                float trail = smoothstep(w, 0.0, abs(p.x - tx)) * (1.0 - along);
                r.x += (p.x - tx) * trail * 0.6 / aspect;
                r.z *= 1.0 + 0.03 * trail;
            }
        }
    }
    return r;
}

float4 PS_Sharpen(float2 uv : TEXCOORD0) : COLOR0 {
    float3 drop = lensDrops(uv);
    uv += drop.xy;
    float2 t = u_Pass0.xy;
    float3 b = tex2Dlod(s0, float4(uv + float2(0, -t.y), 0, 0)).rgb;
    float3 d = tex2Dlod(s0, float4(uv + float2(-t.x, 0), 0, 0)).rgb;
    float3 e = tex2Dlod(s0, float4(uv, 0, 0)).rgb;
    float3 f = tex2Dlod(s0, float4(uv + float2(t.x, 0), 0, 0)).rgb;
    float3 h = tex2Dlod(s0, float4(uv + float2(0, t.y), 0, 0)).rgb;
    if (u_Post.y <= 0.001) return float4(e * drop.z, 1);
    float3 mn = min(min(min(d, e), min(f, b)), h);
    float3 mx = max(max(max(d, e), max(f, b)), h);
    float3 amp = saturate(min(mn, 2.0 - mx) / max(mx, 1e-4));
    amp = sqrt(amp);
    float peak = -1.0 / lerp(8.0, 5.0, saturate(u_Post.y));
    float3 w = amp * peak;
    float3 c = (b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w);
    return float4(saturate(c * drop.z), 1);
}

// Utility: raw depth (INTZ) -> R32F, used for frame captures.
float4 PS_CopyDepth(float2 uv : TEXCOORD0) : COLOR0 {
    return float4(tex2Dlod(s0, float4(uv, 0, 0)).r, 0, 0, 1);
}

// ---------------------------------------------------------------------------------
// Temporal anti-aliasing / stabilisation (full res, after FXAA, before sharpening).
// The game's projection can't be jittered, so this mainly calms flicker (thin lines,
// AO/shadow noise, specular sparkle); camera motion adds sub-pixel samples on its own.
//   s0 = current (sRGB), s1 = history (rgb, a = view z), s2 = full normal/depth
//   out.rgb = resolved colour, out.a = view z (for the next frame's depth check)
// ---------------------------------------------------------------------------------
float3 toYCoCg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 fromYCoCg(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

// 5-tap Catmull-Rom history fetch (sharper than bilinear, s1 must be filtered).
float3 historyCatmullRom(float2 uv) {
    float2 pos = uv * u_Screen.xy;
    float2 p1 = floor(pos - 0.5) + 0.5;
    float2 f = pos - p1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 t0 = (p1 - 1.0) * u_Screen.zw;
    float2 t3 = (p1 + 2.0) * u_Screen.zw;
    float2 t12 = (p1 + w2 / w12) * u_Screen.zw;
    float3 r = tex2Dlod(s1, float4(t12.x, t0.y, 0, 0)).rgb * (w12.x * w0.y);
    r += tex2Dlod(s1, float4(t0.x, t12.y, 0, 0)).rgb * (w0.x * w12.y);
    r += tex2Dlod(s1, float4(t12.x, t12.y, 0, 0)).rgb * (w12.x * w12.y);
    r += tex2Dlod(s1, float4(t3.x, t12.y, 0, 0)).rgb * (w3.x * w12.y);
    r += tex2Dlod(s1, float4(t12.x, t3.y, 0, 0)).rgb * (w12.x * w3.y);
    float w = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / w, 0.0);
}

// u_Pass0.xy = this frame's jitter, u_Pass0.zw = the history's jitter (uv shift of the image;
// 0 without jitter). The history is kept unjittered: the current frame is read back shifted.
float4 PS_TAA(float2 uv : TEXCOORD0) : COLOR0 {
    float2 cuv = uv + u_Pass0.xy;
    float3 current = tex2Dlod(s0, float4(cuv, 0, 0)).rgb;
    float z = tex2Dlod(s2, float4(cuv, 0, 0)).w;
    if (u_Temporal.y < 0.5) return float4(current, z);

    float prevZ;
    float2 prevUV = reproject(worldPosition(viewPosition(cuv, min(z, 20000.0))), prevZ) - u_Pass0.zw;
    if (any(prevUV < 0.0) || any(prevUV > 1.0)) return float4(current, z);

    // Neighbourhood statistics of the current frame (variance clipping in YCoCg), 3x3.
    float3 m1 = 0, m2 = 0;
    [unroll] for (int y = -1; y <= 1; y++) {
        [unroll] for (int x = -1; x <= 1; x++) {
            float3 s = toYCoCg(tex2Dlod(s0, float4(cuv + float2(x, y) * u_Screen.zw, 0, 0)).rgb);
            m1 += s;
            m2 += s * s;
        }
    }
    m1 /= 9.0;
    m2 /= 9.0;
    float3 sigma = sqrt(max(m2 - m1 * m1, 0.0));
    // Standing still the history is trusted more: the jittered samples and the noise of the
    // effects add up to a calm image (fine grates and reflections stop shimmering). In motion
    // the box gets tight again, against ghosting.
    float speed = length((uv - prevUV) * u_Screen.xy);
    float still = 1.0 - saturate(speed / 3.0);
    float gamma = lerp(1.25, 2.25, still);
    float3 boxMin = m1 - sigma * gamma, boxMax = m1 + sigma * gamma;

    float3 history = toYCoCg(historyCatmullRom(prevUV));
    // Clip towards the box centre.
    float3 centre = (boxMin + boxMax) * 0.5, extent = (boxMax - boxMin) * 0.5 + 1e-4;
    float3 v = history - centre;
    float3 a = abs(v / extent);
    float m = max(a.x, max(a.y, a.z));
    if (m > 1.0) history = centre + v / m;

    float weight = lerp(0.1, 0.06, still);
    // Disocclusion and moving objects (the car, opponents): the history must have seen
    // this surface at the depth the reprojection expects.
    if (z < SKY_Z) {
        float historyZ = tex2Dlod(s1, float4(prevUV, 0, 0)).a;
        float error = abs(historyZ - prevZ) / max(prevZ, 0.1);
        weight = lerp(weight, 1.0, smoothstep(0.02, 0.08, error));
    }
    // Fast motion: favour the current frame (less smearing).
    weight = max(weight, saturate(speed / 60.0) * 0.4);
    return float4(fromYCoCg(lerp(history, toYCoCg(current), weight)), z);
}

// Plain copy (TAA history -> output when nothing else follows).
float4 PS_Copy(float2 uv : TEXCOORD0) : COLOR0 {
    float3 drop = lensDrops(uv);
    return float4(tex2Dlod(s0, float4(uv + drop.xy, 0, 0)).rgb * drop.z, 1);
}

// ---------------------------------------------------------------------------------
// Rain particles: drops and splashes are real geometry (pipeline.cpp draws them with its
// own vertex shader), blended additively over the finished image (after TAA).
//   s0 = full normal/depth, s1 = scene colour (sRGB), s2 = sky average (1x1),
//   s3 = neon light (quarter), s4 = adapted exposure (1x1), u_Pass0.x = intensity
//   data.xy = position in the quad, data.z = view z, data.w = fade / splash age
// ---------------------------------------------------------------------------------
float3 rainLight(float2 uv) {
    // A drop is a tiny lens: it shows the sky and, upside down, what's behind it.
    // At night the street lights and neon in it are what makes rain visible.
    float3 sky = tex2Dlod(s2, float4(0.5, 0.5, 0, 0)).rgb;
    float3 behind = toLinear(tex2Dlod(s1, float4(uv + float2(0.0, -0.02), 0, 0)).rgb);
    float3 neon = tex2Dlod(s3, float4(uv, 0, 0)).rgb;
    float peak = max(behind.r, max(behind.g, behind.b));
    float3 light = sky * 0.8 + behind * 0.25 + behind * smoothstep(0.6, 1.0, peak) * u_Rays.w * 0.5 + neon * 4.0 + 0.015;
    // To display space with the frame's exposure (the image is already tonemapped).
    float autoEv = clamp(log2(0.2) - tex2Dlod(s4, float4(0.5, 0.5, 0, 0)).r, -1.5, 0.15) * u_Grade0.y;
    float night = u_Sky.y;
    light *= lerp(1.0, 0.25, night);
    return toSRGB(tonemap(light * u_Grade0.x * exp2(autoEv)));
}

float4 PS_RainDrop(float4 data : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {
    float2 uv = (vpos + 0.5) * u_Screen.zw;
    clip(tex2Dlod(s0, float4(uv, 0, 0)).w - data.z);
    float across = saturate(1.0 - abs(data.x));
    float along = smoothstep(0.0, 0.3, data.y) * smoothstep(1.0, 0.5, data.y);
    return float4(rainLight(uv) * (across * along * data.w * u_Pass0.x), 0);
}

// Spray mist behind the car: soft puffs that thin out as they spread. u_Pass0.x = amount
float4 PS_Spray(float4 data : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {
    float2 uv = (vpos + 0.5) * u_Screen.zw;
    clip(tex2Dlod(s0, float4(uv, 0, 0)).w - (data.z * 0.985 - 0.05));
    float r2 = dot(data.xy, data.xy);
    float puff = saturate(1.0 - r2);
    puff *= puff;
    float a = puff * pow(1.0 - data.w, 2.0) * u_Pass0.x * 0.05; // hundreds overlap: each one is faint
    return float4(rainLight(uv) * a, 0);
}

// Neon trail, one point: where the car is (found in the depth buffer below the screen
// centre, like the spray), lowered to the middle of the tyres.
//   s0 = full normal/depth, u_Pass0.x = time stamp (< 0: a new line starts here)
float4 PS_TrailPoint(float2 uv : TEXCOORD0) : COLOR0 {
    float z0 = tex2Dlod(s0, float4(0.5, 0.6, 0, 0)).w;
    float z1 = tex2Dlod(s0, float4(0.5, 0.65, 0, 0)).w;
    float z2 = tex2Dlod(s0, float4(0.5, 0.7, 0, 0)).w;
    float z = min(z0, min(z1, z2));
    if (z < 1.5 || z > 16.0) return 0; // no car in front of the camera
    float y = z == z0 ? 0.6 : (z == z1 ? 0.65 : 0.7);
    float3 car = worldPosition(viewPosition(float2(0.5, y), z));
    float3 up = viewToWorldDir(float3(0, 1, 0));
    float3 fwd = viewToWorldDir(float3(0, 0, 1));
    return float4(car - up * 0.5 - fwd * 0.25, u_Pass0.x);
}

// The trail ribbon: a bright core with a soft glow, additive in HDR (it blooms).
//   s0 = full normal/depth, u_Pass0.rgb = colour * strength
float4 PS_Trail(float4 data : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {
    float2 uv = (vpos + 0.5) * u_Screen.zw;
    clip(tex2Dlod(s0, float4(uv, 0, 0)).w - data.z * 0.995);
    float x = data.x;
    float light = exp(-x * x * 10.0) * 2.5 + exp(-x * x * 2.5) * 0.4;
    return float4(u_Pass0.rgb * light * data.y, 0);
}

// Snow flakes: soft discs, stretched into short streaks when they rush past the camera.
// Premultiplied alpha (blended over the image, white on white doesn't glow).
//   data.xy = across (-1..1), along (in radii, 0..len), data.z = view z, data.w = opacity
//   len = streak length in radii
float4 PS_SnowFlake(float4 data : TEXCOORD0, float len : TEXCOORD1, float2 vpos : VPOS) : COLOR0 {
    float2 uv = (vpos + 0.5) * u_Screen.zw;
    clip(tex2Dlod(s0, float4(uv, 0, 0)).w - data.z);
    float along = data.y - clamp(data.y, 0.0, len);
    float r = length(float2(data.x, along));
    float a = smoothstep(1.0, 0.35, r) * data.w * u_Pass0.x;
    // Overcast light from all around, a touch of the sun; lights and neon at night.
    float3 sky = tex2Dlod(s2, float4(0.5, 0.5, 0, 0)).rgb;
    float3 neon = tex2Dlod(s3, float4(uv, 0, 0)).rgb;
    float3 light = luma(sky) * float3(0.95, 0.97, 1.0) * 1.3 + u_SunColor.rgb * u_SunWorld.w * 0.25 + neon * 3.0 + 0.02;
    float autoEv = clamp(log2(0.2) - tex2Dlod(s4, float4(0.5, 0.5, 0, 0)).r, -1.5, 0.15) * u_Grade0.y;
    light *= lerp(1.0, 0.3, u_Sky.y);
    float3 colour = toSRGB(tonemap(light * u_Grade0.x * exp2(autoEv)));
    // Lit by the same light as the snow around it: never darker than what is behind (no grey
    // smudges on a white field), a little brighter on dark surfaces.
    float behind = luma(tex2Dlod(s1, float4(uv, 0, 0)).rgb);
    colour = max(colour, behind * 1.08 + 0.03);
    return float4(colour * a, a);
}

float4 PS_RainSplash(float4 data : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {
    float2 uv = (vpos + 0.5) * u_Screen.zw;
    clip(tex2Dlod(s0, float4(uv, 0, 0)).w - (data.z * 0.985 - 0.05));
    float p = data.w;           // age, 0..1
    float2 q = data.xy;         // x: -1..1 across, y: 0..1 up
    // Crown: a thin wall of water that opens up and rises, then falls apart.
    float h = 0.55 * sin(saturate(p * 2.5) * PI * 0.5) * (1.0 - 0.6 * p);
    float r = 0.2 + 0.55 * sqrt(p);
    float wall = exp(-pow((abs(q.x) - r * (0.7 + 0.3 * q.y / max(h, 0.05))) * 8.0, 2.0));
    wall *= smoothstep(h, h * 0.6, q.y) * saturate(q.y / max(h * 0.3, 0.02));
    // Droplets thrown out on short arcs.
    float drops = 0.0;
    [unroll] for (int k = 0; k < 4; k++) {
        float dx = (k - 1.5) / 1.5;
        float2 pos = float2(dx * (0.25 + 0.7 * p), 3.2 * p * (1.0 - p) * (0.55 + 0.25 * frac(k * 0.618 + 0.3)));
        drops += smoothstep(0.1, 0.03, length((q - pos) * float2(1.0, 1.4)));
    }
    // Flash where the drop hits the ground.
    float hit = exp(-q.y * 14.0 - q.x * q.x * 5.0) * saturate(1.0 - p * 2.5);
    float a = (wall * 0.7 + drops * 0.8 + hit) * (1.0 - p) * u_Pass0.x;
    return float4(rainLight(uv) * a * 0.7, 0);
}
