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
float4 u_Quality   : register(c22); // AO samples, shadow steps, 0, 0

// Per-pass constants
float4 u_Pass0 : register(c32);
float4 u_Pass1 : register(c33);

sampler2D s0 : register(s0);
sampler2D s1 : register(s1);
sampler2D s2 : register(s2);
sampler2D s3 : register(s3);
sampler2D s4 : register(s4);
sampler2D s5 : register(s5);
sampler2D s6 : register(s6);

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
float3 cameraWorld() { return float3(u_ViewToW0.w, u_ViewToW1.w, u_ViewToW2.w); }

float ign(float2 pixel) {
    // Interleaved gradient noise, animated per frame.
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
// Pass 2: ambient occlusion + ray-marched sun shadows (half res)
//   s0 = half normal/depth, s1 = full normal/depth
//   out.r = AO visibility, out.g = sun visibility, out.a = 1
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
    return float4(ao, sunVis, 0, 1);
}

// Depth-aware separable blur of AO/shadow (half res).
//   s0 = AO/shadow, s1 = half normal/depth, u_Pass0.xy = direction * texel
float4 PS_BilateralBlur(float2 uv : TEXCOORD0) : COLOR0 {
    float4 centerND = tex2Dlod(s1, float4(uv, 0, 0));
    float2 sum = 0;
    float wsum = 0;
    [unroll] for (int i = -4; i <= 4; i++) {
        float2 suv = uv + u_Pass0.xy * i;
        float4 nd = tex2Dlod(s1, float4(suv, 0, 0));
        float w = exp(-i * i / 10.0);
        w *= exp(-abs(nd.w - centerND.w) / (0.02 * centerND.w + 0.05));
        w *= pow(saturate(dot(nd.xyz, centerND.xyz)), 4.0) + 0.001;
        sum += tex2Dlod(s0, float4(suv, 0, 0)).rg * w;
        wsum += w;
    }
    return float4(sum / max(wsum, 1e-4), 0, 1);
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

// Point stars on a 3D grid, anti-aliased, with gentle twinkle.
float3 stars(float3 rd, float density) {
    float3 sum = 0;
    [loop] for (int layer = 0; layer < 2; layer++) {
        float scale = layer == 0 ? 160.0 : 380.0;
        float3 p = rd * scale;
        float3 cell = floor(p);
        float h = hash13(cell + layer * 71.0);
        if (h > 1.0 - 0.06 * density) {
            float3 center = cell + 0.5 + (float3(hash13(cell + 3.1), hash13(cell + 5.7), hash13(cell + 9.3)) - 0.5) * 0.6;
            float d = length(p - center);
            float size = layer == 0 ? 0.11 : 0.07;
            float b = smoothstep(size, 0.0, d) * (layer == 0 ? 2.5 : 1.0);
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
    return galaxy(rd) + stars(rd, u_Sky2.y) + float3(0.004, 0.006, 0.012);
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
    float density = saturate(gas * 1.6 - 0.25) * lanes;
    density *= smoothstep(inner * 0.9, inner * 1.15, r) * smoothstep(outer, outer * 0.55, r);

    // Temperature falls off outward: white-hot inside, deep orange at the rim.
    float heat = pow(inner / r, 1.6);
    float3 col = lerp(float3(1.0, 0.32, 0.07), float3(1.0, 0.86, 0.68), saturate(heat * 1.15));
    col = lerp(col, float3(0.85, 0.9, 1.0), saturate(heat - 0.75) * 1.5);
    // Relativistic beaming: the side orbiting towards the camera is much brighter.
    float v = min(sqrt(0.5 / max(r - 1.0, 0.5)), 0.7);
    float3 orbit = normalize(cross(n, x));
    float g = sqrt(1.0 - v * v) / (1.0 - v * dot(orbit, -dir));
    float beaming = clamp(g * g * g, 0.12, 3.5);
    float redshift = sqrt(saturate(1.0 - 1.0 / r));
    float3 emission = col * (0.35 + heat * 3.5) * beaming * redshift;
    return float4(emission * density, saturate(density * 1.4));
}

// Black hole after Interstellar's Gargantua: light rays are traced through Schwarzschild
// space-time, so the shadow, the photon ring and the disk lensed over and under the hole
// all come out of the bending itself.
float3 blackHoleSky(float3 rd) {
    float3 bh = blackHoleDirection();
    float3x3 frame = skyFrame(bh);
    float dist = 70.0 / u_Sky2.z;               // camera distance in Schwarzschild radii
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
    float cone = asin(min(traceRadius / dist, 0.99));
    if (angle < cone) {
        // Straight to the trace sphere, then integrate the bent ray inside it.
        float3 p = -bh * dist;
        float b = dot(p, rd);
        float c = dot(p, p) - traceRadius * traceRadius;
        p += rd * (-b - sqrt(max(b * b - c, 0.0)));
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
    } else {
        // Weak field: background bent toward the hole by 2 rs / b.
        float deflect = 2.0 / (dist * sin(angle));
        outDir = normalize(rd + normalize(bh - rd * cosA) * deflect);
        col = spaceBackground(outDir);
    }
    // Soft glow of the inner disk scattered around the hole (feeds the bloom).
    float glow = exp(-angle * dist / 9.0) * smoothstep(2.3, 3.4, angle * dist);
    return col + float3(1.0, 0.75, 0.5) * glow * 0.18;
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
            float3 ringCol = float3(0.92, 0.82, 0.66) * (0.35 + 0.65 * abs(dot(light, ringN))) * shadow;
            result = float4(ringCol * 0.7 * density, density * 0.85);
        }
    }
    if (tPlanet < 1e8 && (result.a < 0.999)) {
        float3 x = rd * tPlanet;
        float3 nrm = normalize(x - center);
        float lat = dot(nrm, ringN);
        float band = noise3(float3(lat * 14.0, 0.0, 1.0)) * 0.6 + noise3(float3(lat * 45.0 + nrm.x * 0.6, 2.0, 0.0)) * 0.4;
        float3 albedo = lerp(float3(0.78, 0.66, 0.46), float3(0.97, 0.9, 0.72), band);
        albedo = lerp(albedo, float3(0.62, 0.68, 0.72), smoothstep(0.75, 0.95, abs(lat)));  // bluish poles
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

// Aurora: a folded curtain line (level set of a warped noise field) sampled on stacked
// altitude slices. Many jittered slices blend into smooth vertical veils.
float noise2(float2 p) {
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash12(i), b = hash12(i + float2(1, 0)), c = hash12(i + float2(0, 1)), d = hash12(i + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float auroraCurtain(float2 p, float t) {
    float2 w = float2(noise2(p * 0.45 + float2(t, 0.0)), noise2(p * 0.45 + float2(5.2, 1.3 - t)));
    p += (w - 0.5) * 3.2;
    float n = noise2(p * 0.7) * 0.7 + noise2(p * 1.6 + 9.1) * 0.3;
    float activity = smoothstep(0.4, 0.75, noise2(p * 0.12 + float2(3.7, t * 0.5)));  // patches, not a full sky
    return exp(-abs(n - 0.5) * 26.0) * activity;
}

float3 auroraSky(float3 rd) {
    float3 col = spaceBackground(rd) * 0.7;
    if (rd.y <= 0.0) return col;
    float ca = cos(u_Sky.z), sa = sin(u_Sky.z);
    float2 flat = float2(rd.x * ca - rd.z * sa, rd.x * sa + rd.z * ca);
    float jitter = hash13(rd * 4096.0);
    float t = u_Proj2.z * 0.02;
    float3 acc = 0, avg = 0;
    [loop] for (int i = 0; i < 40; i++) {
        float fi = i + jitter;
        float height = 1.0 + pow(fi, 1.4) * 0.012;
        float2 p = flat * height / (rd.y * 1.6 + 0.12) * 1.6;
        float curtain = auroraCurtain(p, t);
        float3 tint = lerp(float3(0.12, 1.0, 0.4), float3(0.15, 0.65, 0.95), saturate((fi - 8.0) / 24.0));
        tint = lerp(tint, float3(0.75, 0.25, 0.9), saturate((fi - 22.0) / 18.0));
        avg = lerp(avg, tint * curtain, 0.45);
        acc += avg * exp2(-fi * 0.085) * smoothstep(0.0, 4.0, fi);
    }
    col += acc * 0.2 * smoothstep(0.0, 0.12, rd.y);
    return col;
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

float3 customSky(float3 rd) {
    int mode = (int)(u_Sky.x + 0.5);
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
    } else if (mode == 3) {
        c = blackHoleSky(rd);
        float3 pdir = normalize(rotateY(float3(-0.62, 0.14, 0.8), u_Sky.z));
        float4 planet = ringedPlanet(rd, normalize(blackHoleDirection() * 0.45 - pdir * 0.75 + float3(0, 0.35, 0)));
        c = c * (1.0 - planet.a) + planet.rgb;
    } else {
        c = auroraSky(rd);
    }
    // Below the horizon: dark ground haze instead of mirrored sky.
    c = lerp(c, c * 0.15 + float3(0.01, 0.012, 0.02), smoothstep(0.0, -0.08, rd.y) * (mode == 1 ? 0.0 : 1.0));
    return c * u_Sky.w;
}

// Pass: custom sky (full res, only where depth = sky). s0 = full normal/depth
float4 PS_Sky(float2 uv : TEXCOORD0) : COLOR0 {
    if (tex2Dlod(s0, float4(uv, 0, 0)).w < SKY_Z) return 0;
    float3 rd = viewToWorldDir(normalize(viewPosition(uv, 1.0)));
    return float4(min(customSky(rd), 12.0), 1);
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
// Pass 4: lighting composite -> HDR (full res)
//   s0 = scene colour (sRGB), s1 = full normal/depth, s2 = AO/shadow (half, blurred),
//   s3 = half normal/depth, s4 = sky average (1x1), s5 = custom sky
// ---------------------------------------------------------------------------------
float2 upsampleOcclusion(float2 uv, float z, float3 n) {
    // Joint bilateral upsample from half resolution.
    float2 halfTexel = u_Screen.zw * 2.0;
    float2 base = (floor(uv / halfTexel - 0.5) + 0.5) * halfTexel;
    float2 sum = 0;
    float wsum = 0;
    [unroll] for (int j = 0; j < 2; j++) {
        [unroll] for (int i = 0; i < 2; i++) {
            float2 suv = base + float2(i, j) * halfTexel;
            float4 nd = tex2Dlod(s3, float4(suv, 0, 0));
            float2 f = 1.0 - abs(uv - suv) / halfTexel;
            float w = max(f.x * f.y, 0.001);
            w *= exp(-abs(nd.w - z) / (0.03 * z + 0.05));
            w *= pow(saturate(dot(nd.xyz, n)), 8.0) + 0.002;
            sum += tex2Dlod(s2, float4(suv, 0, 0)).rg * w;
            wsum += w;
        }
    }
    return wsum > 1e-4 ? sum / wsum : 1.0;
}

float4 PS_Lighting(float2 uv : TEXCOORD0) : COLOR0 {
    float3 srgb = tex2Dlod(s0, float4(uv, 0, 0)).rgb;
    float3 c = toLinear(srgb);
    float4 nd = tex2Dlod(s1, float4(uv, 0, 0));
    float3 rdView = normalize(viewPosition(uv, 1.0));
    float3 rd = viewToWorldDir(rdView);
    float daylight = u_SunWorld.w;

    // Expand the LDR image back toward HDR: near-white geometry pixels are light sources
    // (lamps, neon strips, reflections). The sky only gets this close to the sun.
    // Large bright areas (sun glare on the road, white walls) are surfaces, not lights:
    // only pixels clearly brighter than their surroundings (neon strips, lamps) qualify.
    float peak = max(c.r, max(c.g, c.b));
    float emissive = 0.0;
    if (nd.w < SKY_Z && peak > 0.6) {
        // Erode: a light covers a few pixels. Single bright texels (specular glints on
        // asphalt lit by floodlights) are texture detail and must not turn into lights.
        [unroll] for (int e = 0; e < 4; e++) {
            float2 eo = float2(e == 0 || e == 2 ? -1.5 : 1.5, e < 2 ? -1.5 : 1.5) * u_Screen.zw;
            float3 ec = toLinear(tex2Dlod(s0, float4(uv + eo, 0, 0)).rgb);
            peak = min(peak, max(ec.r, max(ec.g, ec.b)));
        }
        float surround = 0.0;
        float2 r = float2(0.012 * u_Screen.y * u_Screen.z, 0.012);
        [unroll] for (int k = 0; k < 8; k++) {
            float a = k * (PI / 4.0) + 0.39;
            float3 sc = toLinear(tex2Dlod(s0, float4(uv + float2(cos(a), sin(a)) * r, 0, 0)).rgb);
            surround += max(sc.r, max(sc.g, sc.b));
        }
        surround /= 8.0;
        emissive = saturate((peak - surround) * 3.0) * pow(smoothstep(0.75, 1.0, peak), 2.0);
    }
    float3 glow = c * u_Rays.w * emissive;

    if (nd.w >= SKY_Z && u_Sky.x > 0.5) return float4(tex2Dlod(s5, float4(uv, 0, 0)).rgb, 1);
    if (nd.w >= SKY_Z) {
        // Sky: richer gradient, glow around the sun.
        float horizon = 1.0 - saturate(rd.y * 3.0);
        float3 graded = lerp(c, c * c / max(luma(c), 1e-3), 0.35); // deepen saturation
        graded *= 1.0 + horizon * 0.25;
        c = lerp(c, graded, u_Atmo.w);
        if (u_SunView.w > 0.5) {
            float mu = dot(rd, u_SunWorld.xyz);
            c += sunScatter(rd, u_Rays.z * 0.5) * daylight;
            c += u_SunColor.rgb * smoothstep(0.9994, 0.99975, mu) * 2.0 * u_Rays.z * daylight; // sun disk
        }
        return float4(c, 1);
    }

    float3 n = nd.xyz;
    float3 p = viewPosition(uv, nd.w);
    float2 occ = upsampleOcclusion(uv, nd.w, n);

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
    float ao = lerp(1.0, occ.r, u_Light.x);
    float sunVis = occ.g;

    if (u_SunView.w > 0.5) {
        float ndl = saturate(dot(nLight, u_SunView.xyz));
        float shadow = lerp(1.0, sunVis, u_Light.z * saturate(ndl * 4.0));
        // In shadow: lose direct light and take on the sky's colour.
        float3 skyTint = u_SkyColor.rgb / max(luma(u_SkyColor.rgb), 1e-3);
        float shadowed = (1.0 - shadow);
        c *= lerp(1.0, 0.36, shadowed);
        c *= lerp(float3(1, 1, 1), skyTint, u_SkyColor.w * saturate(shadowed + (1.0 - ndl) * 0.35));
        // In sunlight: warm directional light.
        float lit = ndl * shadow * daylight;
        float3 sunTint = u_SunColor.rgb / max(luma(u_SunColor.rgb), 1e-3);
        c *= lerp(float3(1, 1, 1), sunTint * 1.12, u_SunColor.w * lit);
    }
    c *= ao;
    // Ambient occlusion also tints toward the sky colour (bounce from the sky dome).
    c *= lerp(u_SkyColor.rgb / max(luma(u_SkyColor.rgb), 1e-3), float3(1, 1, 1), lerp(1.0, ao, 0.5 * u_SkyColor.w));

    // Aerial perspective: exponential height fog lit by the sky and the sun.
    float dist = length(p);
    float density = u_Atmo.x * 0.0009;
    float falloff = max(u_Atmo.y * 0.012, 1e-4);
    float camY = u_UpView.w;
    float rdy = rd.y;
    float fogAmount = density * exp(-camY * falloff) * (1.0 - exp(-dist * rdy * falloff)) / (abs(rdy) > 1e-4 ? rdy * falloff : 1e-4 * falloff);
    if (abs(rdy) <= 1e-4) fogAmount = density * exp(-camY * falloff) * dist;
    fogAmount = 1.0 - exp(-max(fogAmount, 0.0));
    float4 skyAvg = tex2Dlod(s4, float4(0.5, 0.5, 0, 0));
    float3 fogColor = lerp(u_SkyColor.rgb * 0.7 * (0.3 + 0.7 * daylight), skyAvg.rgb, skyAvg.a * 0.75);
    if (u_SunView.w > 0.5) fogColor += sunScatter(rd, u_Atmo.z * 0.6) * daylight;
    c = lerp(c, fogColor, saturate(fogAmount));

    // Day-for-night for the night skies: darker, cooler, less saturated - but lights keep glowing.
    float night = u_Sky.y;
    c = lerp(c, luma(c) * float3(0.55, 0.72, 1.0), night * 0.55) * lerp(1.0, 0.16, night);
    c += glow * (1.0 - saturate(fogAmount));

    return float4(c, 1);
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
    float window = pow(mu, 12.0) * 0.6 + pow(mu, 120.0) * 0.8;
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
float4 PS_Sharpen(float2 uv : TEXCOORD0) : COLOR0 {
    float2 t = u_Pass0.xy;
    float3 b = tex2Dlod(s0, float4(uv + float2(0, -t.y), 0, 0)).rgb;
    float3 d = tex2Dlod(s0, float4(uv + float2(-t.x, 0), 0, 0)).rgb;
    float3 e = tex2Dlod(s0, float4(uv, 0, 0)).rgb;
    float3 f = tex2Dlod(s0, float4(uv + float2(t.x, 0), 0, 0)).rgb;
    float3 h = tex2Dlod(s0, float4(uv + float2(0, t.y), 0, 0)).rgb;
    if (u_Post.y <= 0.001) return float4(e, 1);
    float3 mn = min(min(min(d, e), min(f, b)), h);
    float3 mx = max(max(max(d, e), max(f, b)), h);
    float3 amp = saturate(min(mn, 2.0 - mx) / max(mx, 1e-4));
    amp = sqrt(amp);
    float peak = -1.0 / lerp(8.0, 5.0, saturate(u_Post.y));
    float3 w = amp * peak;
    float3 c = (b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w);
    return float4(saturate(c), 1);
}

// Utility: raw depth (INTZ) -> R32F, used for frame captures.
float4 PS_CopyDepth(float2 uv : TEXCOORD0) : COLOR0 {
    return float4(tex2Dlod(s0, float4(uv, 0, 0)).r, 0, 0, 1);
}
