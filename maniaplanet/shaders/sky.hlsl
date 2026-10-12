// TM Vibrant Shaders for ManiaPlanet: the sky stage (shader model 5), effects_sky.cpp runs it.
// The TMNF mod's custom skies, volumetric clouds, sky average, lightning, light shafts and lens
// flare. Generated parts: the TMNF functions in the middle are copied from shaders/tmvs.hlsl.
//
// Per frame, before the lighting composite:
//   [PS_AuroraHalf (half, aurora only)] -> [PS_Clouds (half)] -> PS_Sky* (full: the finished sky,
//   clouds and lightning in it) -> PS_SkyAverage (1x1, the haze's colour)
// After the composite, weather and cinema (only with the sun on screen):
//   PS_RayMask (half) -> PS_RayBlur x2 -> PS_FlareDown x2 (quarter, eighth) -> PS_SunPost (full)
// entries: PS_SkyGame:ps_5_0 PS_SkyClear:ps_5_0 PS_SkyStars:ps_5_0 PS_SkyBlackHole:ps_5_0 PS_SkyAurora:ps_5_0 PS_SkyRing:ps_5_0 PS_AuroraHalf:ps_5_0 PS_SkyAverage:ps_5_0 PS_Clouds:ps_5_0 PS_RayMask:ps_5_0 PS_RayBlur:ps_5_0 PS_FlareDown:ps_5_0 PS_SunPost:ps_5_0
#include "common.hlsli"
#include "sky.hlsli"

// Tiling cloud noise (r: Perlin-Worley, g/b/a: Worley octaves), as TMNF's s7. Above the slots the
// other passes use (t0-t15), so it never overlaps them.
Texture3D g_noise : register(t16);

// FSR 1 EASU (AMD FidelityFX Super Resolution, edge-adaptive spatial upsampling), for the aurora
// rendered at half resolution: as in TMNF, on t1 (point taps). size = input w, h, 1/w, 1/h.
void easuSet(inout float2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE)
{
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

void easuTap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len2, float lob, float clp, float3 c)
{
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

float3 easuSample(float2 uv, float4 size)
{
    float2 pp = uv * size.xy - 0.5;
    float2 fp = floor(pp);
    float2 f = pp - fp;
    float2 base = (fp + 0.5) * size.zw;
    #define EASU_TAP(x, y) g_in1.SampleLevel(g_point, base + float2(x, y) * size.zw, 0).rgb
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

// ---------------------------------------------------------------------------------
// From the TMNF mod (shaders/tmvs.hlsl), unchanged: the custom skies, the lightning bolt and the
// cloud shapes. They work in TMNF units (see sky.hlsli); the passes below convert.
// ---------------------------------------------------------------------------------
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
    [loop] for (int k = 0; k < 2; k++) {
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


// Accretion disk emission where a light ray crosses the disk plane.
//   x: crossing point (units of the Schwarzschild radius), dir: ray direction there,
//   n/e1/e2: disk frame. Returns rgb emission and opacity.
float4 diskSample(float3 x, float3 dir, float3 n, float3 e1, float3 e2) {
    const float inner = 3.0, outer = 11.0;
    float r = length(x);
    if (r < inner * 0.9 || r > outer) return 0;
    float phi = atan2(dot(x, e2), dot(x, e1));
    // The gas orbits towards +phi (the way the beaming below says it moves), the inner edge
    // faster (Kepler). Turned on for good, the pattern would wind up tighter every second
    // until it only flickered; instead two layers each wind up for a few seconds and start
    // over, fading into each other (flow-map style), so the streaks keep flowing.
    const float period = 6.0;
    float omega = 0.7 * pow(inner / r, 1.5);
    float cycle = u_Proj2.z / period;
    float gas = 0.0, weightSum = 0.0, weightSq = 0.0;
    [unroll] for (int layer = 0; layer < 2; layer++) {
        float c = cycle + layer * 0.5;
        float phase = frac(c);
        float w = 1.0 - abs(phase * 2.0 - 1.0);     // 0 when the layer starts over
        float a = phi - omega * phase * period + log(r) * 1.2;
        float seed = floor(c) * 7.31 + layer * 3.7;  // a fresh pattern each round
        // Filaments: long arcs along the orbit (low frequency around it), thin across it.
        float3 q = float3(cos(a) * 1.6, sin(a) * 1.6, r * 3.2 + seed);
        float n = noise3(q) * 0.6 + noise3(q * float3(2.3, 2.3, 2.9) + 5.0) * 0.4;
        gas += n * w;
        weightSum += w;
        weightSq += w * w;
    }
    // Keep the contrast while the layers fade (an even mix would be greyer: it pulsed).
    gas = 0.5 + (gas - 0.5 * weightSum) / sqrt(max(weightSq, 1e-3));
    // Bright gas with dark lanes between the filaments: the lanes are what shows the flow.
    float density = saturate(lerp(0.3, 1.1, smoothstep(0.2, 0.8, gas)));
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
    float3 emission = col * (0.6 + heat * 5.5) * beaming * redshift;
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
    // The star background is looked up once, at the end, in the bent direction (each copy of
    // it costs hundreds of instruction slots).
    float background = 1.0;
    const float traceRadius = 26.0;
    // Close up (camera inside the trace sphere) every ray is traced from the camera.
    bool inside = dist < traceRadius;
    float cone = inside ? 4.0 : asin(min(traceRadius / dist, 0.99));
    if (dist > 0.0 && angle < cone) {
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
        background = captured ? 0.0 : 1.0 - alpha;
        // Photon ring: light that circled the hole, a thin sharp line around the shadow.
        if (!captured) col += float3(1.0, 0.92, 0.82) * exp(-abs(closest - 2.65) * 9.0) * 1.6 * (1.0 - alpha);
    } else {
        // Weak field: background bent toward the hole by 2 rs / b. No hole (dist = 0): none.
        float deflect = dist > 0.0 ? 2.0 / (dist * max(sin(angle), 1e-4)) : 0.0;
        outDir = deflect > 0.0 ? normalize(rd + normalize(bh - rd * cosA + 1e-5) * deflect) : rd;
    }
    col += background * spaceBackground(outDir);
    if (dist <= 0.0) return col;
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

// Cloud bands of the planet. lat = -1..1 along the spin axis, lon = longitude (rad).
float wrapAngle(float a) { return a - 6.2831853 * floor(a / 6.2831853 + 0.5); }

float3 planetAlbedo(int type, float lat, float lon) {
    float t = u_Proj2.z * 0.004;
    // The planet turns, and neighbouring bands drift against each other (Jupiter's jets):
    // the turbulent band edges shear while it turns. The bands themselves follow latitude
    // only, so without the jets the turning would hardly show.
    // A turn in about two minutes: slow and stately, but you see it move.
    float turn = u_Proj2.z * 0.05;
    float jet = sin(lat * 15.0) * 0.035;
    float lonW = lon + turn + u_Proj2.z * jet;
    float cl = sqrt(saturate(1.0 - lat * lat));
    float3 q = float3(cos(lonW) * cl, lat, sin(lonW) * cl);   // point on the sphere, for turbulence
    // Storms ride their own band (the Great Red Spot sits in a westward jet).
    float spotLon = lon + turn + u_Proj2.z * sin(-0.38 * 15.0) * 0.035;
    // Turbulent band edges: the latitude is warped by noise.
    float warp = (noise3(q * 4.0 + float3(t, 0, 0)) - 0.5) * 0.09 + (noise3(q * 11.0 - float3(0, t, 0)) - 0.5) * 0.035;
    float l = lat + warp;
    float bands = noise3(float3(l * 9.0, 0.5, 1.5)) * 0.65 + noise3(float3(l * 27.0, 3.5, 0.5)) * 0.35;
    float fine = noise3(float3(l * 70.0, 2.0, 0.0) + float3(0.0, q.x, q.z) * 0.4);
    float3 col;
    if (type == 1) {
        // Jupiter: cream zones, brown and rust belts, the Great Red Spot.
        col = lerp(float3(0.55, 0.36, 0.24), float3(0.95, 0.88, 0.76), smoothstep(0.35, 0.65, bands));
        col = lerp(col, float3(0.78, 0.5, 0.32), smoothstep(0.55, 0.8, fine) * 0.35);
        float2 spot = float2(wrapAngle(spotLon - 1.2) * 0.55, (lat + 0.38) * 2.2);
        float swirl = noise3(float3(spot * 6.0, t * 3.0));
        float oval = smoothstep(0.32, 0.18, length(spot) + (swirl - 0.5) * 0.08);
        col = lerp(col, float3(0.8, 0.38, 0.24), oval * 0.85);
    } else if (type == 2) {
        // Ice giant: deep blue with faint bands, white methane streaks and a dark storm.
        col = lerp(float3(0.13, 0.27, 0.72), float3(0.32, 0.52, 0.92), smoothstep(0.3, 0.7, bands));
        float streak = smoothstep(0.72, 0.9, noise3(float3(l * 40.0, 7.0, 0.0) + float3(0.0, q.x, q.z) * 3.0));
        col = lerp(col, float3(0.9, 0.95, 1.0), streak * 0.6);
        float2 spot = float2(wrapAngle(spotLon + 0.8) * 0.6, (lat + 0.25) * 2.4);
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
// near = 0..1 how close the camera is (close up the rings break into clumps), foot = size of
// a pixel on the ring in planet radii (fine grooves only where they are bigger than a pixel).
float ringDensity(int type, float rr, float2 x, float near, float foot) {
    float inner = 1.3, outer = type == 2 ? 2.0 : 2.45;
    if (rr < inner || rr > outer) return 0.0;
    float bands = 0.5 + 0.5 * noise3(float3(rr * 42.0, 0.0, 0.0)) * noise3(float3(rr * 11.0, 3.0, 0.0));
    // Thousands of thin ringlets: crisp grooves instead of soft bands.
    bands *= lerp(1.0, 0.45 + 0.9 * smoothstep(0.25, 0.75, noise3(float3(rr * 300.0, 5.1, 0.7))), saturate(1.0 - foot * 300.0) * 0.6);
    bands *= lerp(1.0, 0.6 + 0.8 * noise3(float3(rr * 1100.0, 2.3, 8.1)), saturate(1.0 - foot * 1100.0) * 0.5);
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


// Ringed gas giant (sky detail for the space skies). Returns rgb and coverage.
float4 ringedPlanet(float3 rd, float3 light, float3 dir) {
    float size = u_Sky2.w;
    if (size <= 0.0) return 0;
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
        float3 albedo = planetAlbedo((int)(u_Planet.x + 0.5), lat, lon);
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
    // (No black hole: distance 0, just the stars - one copy of the background either way.)
    float3 col = blackHoleSky(rd, blackHoleDirection(), u_BlackHole.w > 0.0 ? 70.0 / (u_BlackHole.w * 0.55) : 0.0);
    // The sun as a star (with a black hole, the hole is the light).
    if (u_BlackHole.w <= 0.0) {
        float mu = dot(rd, sun);
        col += u_SunColor.rgb * (smoothstep(0.99955, 0.9998, mu) * 6.0 + pow(saturate(mu), 300.0) * 0.6 + pow(saturate(mu), 20.0) * 0.03);
    }

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
    if (u_Sky2.w <= 0.0) return col; // no planet
    int view = (int)(u_Planet.y + 0.5);
    float3 dir = dirFromAngles(u_Planet.z, u_Planet.w);
    float size = max(u_Sky2.w, 0.3);
    float dist = (view == 2 ? 2.8 : (view == 1 ? 2.6 : 5.5)) / size;
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
        float offset = view == 2 ? -0.06 : 0.15; // > 0: plane above the camera
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
        float lon = atan2(dot(nrm, cross(axis, e1)), dot(nrm, e1));
        float3 albedo = planetAlbedo(type, lat, lon);
        float ndl = dot(nrm, sun);
        float lit = smoothstep(-0.06, 0.4, ndl);
        // Shadow of the rings on the planet.
        float dl = dot(sun, ringN);
        float tl = abs(dl) > 1e-4 ? dot(center - x, ringN) / dl : -1.0;
        float3 sx = x + sun * tl - center;
        float ringShadow = tl > 0.0 ? 1.0 - ringDensity(type, length(sx), sx.xz, 0.0, 1.0) * 0.75 : 1.0;
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
        float foot = tRing * 2.0 / (abs(u_Proj.y) * u_Screen.y) / max(abs(denom), 0.02);
        float d = ringDensity(type, rr, px, saturate(1.0 - tRing / 1.2), foot);
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
    // Patches, not a full sky; more of them far out over the horizon than straight overhead.
    float activity = smoothstep(0.4 - 0.12 * saturate(length(p) / 25.0), 0.75, noise2(p * 0.12 + float2(3.7, t * 0.5)));
    float k = 26.0 / (1.0 + footprint * 12.0);
    // The fine octave is the first to alias: it fades out first.
    float fine = saturate(1.0 - footprint * 3.0);
    float n = noise2(p * 0.7) * lerp(1.0, 0.7, fine) + (noise2(p * 1.6 + 9.1) - 0.5) * 0.3 * fine + 0.15 * fine;
    return exp(-abs(n - 0.5) * k) * pow(max(k / 26.0, 0.0), 0.6) * activity;
}

// The glowing curtains, marched through `slices` layers of height (40 = full quality).
// Fewer slices take bigger steps through the same height range. jitter: 0..1 start offset,
// changing every frame so TAA smooths the banding of the low slice counts.
float3 auroraCurtains(float3 rd, float slices, float jitter, float pixAngle) {
    if (rd.y <= 0.0) return 0;
    float ca = cos(u_Sky.z), sa = sin(u_Sky.z);
    float2 flat = float2(rd.x * ca - rd.z * sa, rd.x * sa + rd.z * ca);
    float t = u_Proj2.z * 0.02 * u_Sky3.x;
    float stride = 40.0 / slices;
    float3 acc = 0, avg = 0;
    [loop] for (int i = 0; i < (int)slices; i++) {
        float fi = (i + jitter) * stride;
        float height = 1.0 + pow(max(fi, 0.0), 1.4) * 0.012;
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
    return acc * 0.2 * smoothstep(0.0, 0.06, rd.y);
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
        float4 planet = ringedPlanet(rd, moon, normalize(rotateY(float3(-0.62, 0.14, 0.8), u_Sky.z)));
        c = c * (1.0 - planet.a) + planet.rgb * 0.5;
        c += meteors(rd, saturate(u_Sky2.y));
    } else if (mode == 3) {
        c = blackHoleSky(rd, blackHoleDirection(), u_BlackHole.w > 0.0 ? 70.0 / u_BlackHole.w : 0.0);
        float3 pdir = dirFromAngles(u_Planet.z, u_Planet.w);
        float4 planet = ringedPlanet(rd, normalize(blackHoleDirection() * 0.45 - pdir * 0.75 + float3(0, 0.35, 0)), pdir);
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
    [loop] for (int k = 0; k < 2; k++) {
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

float cloudShape(float3 p, float heightFrac) {
    float3 wind = float3(u_Proj2.z * 32.0, u_Proj2.z * 4.0, u_Proj2.z * 12.0) * (0.4 + u_Nature.z * 4.0);
    float4 n = g_noise.SampleLevel(g_linearWrap, (p + wind) / 5600.0, 0);
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
    float3 wind = float3(u_Proj2.z * 50.0, -u_Proj2.z * 9.0, u_Proj2.z * 19.0) * (0.4 + u_Nature.z * 4.0);
    float4 n = g_noise.SampleLevel(g_linearWrap, (p + wind) / 1300.0, 0);
    float erode = (1.0 - (n.g * 0.5 + n.b * 0.3 + n.a * 0.2)) * 0.45;
    return saturate((d - erode) / (1.0 - erode));
}

// ---------------------------------------------------------------------------------
// The passes (ManiaPlanet). Sky pixels are where the engine drew nothing: depth 0 (reversed).
// ---------------------------------------------------------------------------------
bool isSky(float2 uv) { return engineZ(uv) >= SKY_Z; }

// What the sky stage adds over every sky (TMNF's lighting pass did it per sky pixel): the volumetric
// clouds (t2, half res, u_Pass1.x = present), lightning lighting them up from inside and glowing in
// the sky, and the bolt. c in display-linear units here, the TMNF terms scaled by tmnfScale().
float3 skyFinish(float2 uv, float3 rd, float3 c)
{
    float k = tmnfScale();
    float night = u_Sky.y;
    if (u_Clouds.x > 0.0 && u_Pass1.x > 0.5)
    {
        float4 cloud = t2_texL(uv);
        float3 inner = float3(0.75, 0.8, 1.0) * u_Light2.z * (1.0 - cloud.a) * 1.4 * k;
        c = c * lerp(1.0, cloud.a, u_Clouds.x) + (cloud.rgb * lerp(1.0, 0.45, night) + inner) * u_Clouds.x;
    }
    c += float3(0.6, 0.65, 0.85) * u_Light2.z * 0.3 * saturate(rd.y * 3.0 + 0.3) * k;
    c += lightningBolt(rd) * k;
    return c;
}

// The game's own sky (sky mode 0): a richer gradient, the glow and disc of the sun, the night
// darkening of SkyNight (TMNF PS_Lighting's sky branch).
float4 PS_SkyGame(VSOut i) : SV_Target
{
    if (!isSky(i.uv)) return 0;
    float3 rd = viewToWorldDir(normalize(viewPosition(i.uv, 1.0)));
    float3 c = engineColour(i.uv);
    float k = tmnfScale();
    float daylight = u_SunWorld.w;
    float horizon = 1.0 - saturate(rd.y * 3.0);
    float3 graded = lerp(c, c * c / max(luma(c), 1e-3), 0.35); // deepen saturation
    graded *= 1.0 + horizon * 0.25;
    c = lerp(c, graded, u_Atmo.w);
    if (u_SunView.w > 0.5)
    {
        float mu = dot(rd, u_SunWorld.xyz);
        c += sunScatter(rd, u_Rays.z * 0.5) * daylight * k;
        // Sun disc and a tight halo (feeds the bloom).
        c += u_SunColor.rgb * (smoothstep(0.9994, 0.99975, mu) * 1.8 + pow(saturate(mu), 900.0) * 0.35) * u_Rays.z * daylight * k;
    }
    // Day-for-night on the game's own sky (manual night setting).
    float night = u_Sky.y;
    c = lerp(c, luma(c) * float3(0.85, 0.9, 1.0), saturate(night * 1.6)) * lerp(1.0, 0.3, night);
    return float4(skyFinish(i.uv, rd, c), 1);
}

// A custom sky (modes 1-5). The aurora's curtains come from the half-res pass in t1 (u_Pass0 =
// its size).
float4 skyPass(float2 uv, uniform int mode)
{
    if (!isSky(uv)) return 0;
    float3 rd = viewToWorldDir(normalize(viewPosition(uv, 1.0)));
    float3 c = customSky(rd, mode);
    if (mode == 4) c += easuSample(uv, u_Pass0) * u_Sky.w;
    c = min(c, 12.0) * tmnfScale();
    return float4(skyFinish(uv, rd, c), 1);
}
float4 PS_SkyClear(VSOut i) : SV_Target { return skyPass(i.uv, 1); }
float4 PS_SkyStars(VSOut i) : SV_Target { return skyPass(i.uv, 2); }
float4 PS_SkyBlackHole(VSOut i) : SV_Target { return skyPass(i.uv, 3); }
float4 PS_SkyAurora(VSOut i) : SV_Target { return skyPass(i.uv, 4); }
float4 PS_SkyRing(VSOut i) : SV_Target { return skyPass(i.uv, 5); }

// The aurora's curtains at half resolution, only where one of the four full-res pixels below is
// sky. u_Pass0.x = slices, u_Pass0.zw = full texel. (TMNF units: the sky pass scales.)
float4 PS_AuroraHalf(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float2 o = u_Pass0.zw * 0.5;
    if (!isSky(uv + float2(-o.x, -o.y)) && !isSky(uv + float2(o.x, -o.y)) && !isSky(uv + float2(-o.x, o.y)) && !isSky(uv + float2(o.x, o.y)))
        return 0;
    float3 rd = viewToWorldDir(normalize(viewPosition(uv, 1.0)));
    float3 rdNext = viewToWorldDir(normalize(viewPosition(uv + float2(0.0, u_Pass0.w * 2.0), 1.0)));
    return float4(auroraCurtains(rd, u_Pass0.x, ign(i.pos.xy), length(rdNext - rd)), 1);
}

// Average sky colour near the horizon (1x1): the haze's colour.
//   t0 = the stage's custom sky (u_Pass1.x = present; the game's sky is read from the engine's
//   image, as TMNF read its own), t1 = last frame's average.
//   u_Pass0.x = blend towards this frame, u_Pass0.y = 1: no history
float4 PS_SkyAverage(VSOut i) : SV_Target
{
    float3 sum = 0;
    float wsum = 0;
    [loop] for (int y = 0; y < 10; y++)
    {
        [loop] for (int x = 0; x < 16; x++)
        {
            float2 suv = float2((x + 0.5) / 16.0, (y + 0.5) / 10.0 * 0.75);
            if (isSky(suv))
            {
                float3 rd = viewToWorldDir(normalize(viewPosition(suv, 1.0)));
                float w = exp(-max(rd.y, 0.0) * 6.0); // favour the horizon
                float3 skyCol = u_Pass1.x > 0.5 ? t0_tex(suv).rgb : engineColour(suv);
                sum += min(skyCol, 50.0) * w;
                wsum += w;
            }
        }
    }
    // A few sky pixels (a gap in a roof) are no average of the sky: lean on the fallback until
    // enough are seen, instead of switching hard between them.
    float3 fallback = u_SkyColor.rgb * 0.6 * tmnfScale();
    float3 avg = lerp(fallback, sum / max(wsum, 1e-4), saturate(wsum / 2.0));
    float4 current = float4(avg, saturate(wsum / 20.0));
    if (u_Pass0.y > 0.5) return current;
    return lerp(t1_tex(float2(0.5, 0.5)), current, u_Pass0.x);
}

// Volumetric clouds (half res): a ray-marched cumulus layer, lit by the sun. Rendered for every
// pixel as if it were sky; the sky passes put it over sky pixels.
//   t0 = sky average (1x1, last frame's), t16 = the cloud noise.
//   out.rgb = in-scattered light (display linear), out.a = transmittance
float4 PS_Clouds(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float3 rd = viewToWorldDir(normalize(viewPosition(uv, 1.0)));
    float base = u_Clouds.z, thickness = u_Clouds.w;
    float camY = u_UpView.w;
    if (rd.y < 0.012 || camY > base) return float4(0, 0, 0, 1);
    float t0 = (base - camY) / rd.y;
    float t1 = min((base + thickness - camY) / rd.y, t0 + thickness * 5.0);
    float steps = u_Quality.z;
    float dt = (t1 - t0) / steps;
    float t = t0 + dt * ign(i.pos.xy);
    float3 cam = cameraWorld();
    float k = tmnfScale();

    bool sunKnown = u_SunView.w > 0.5;
    float3 sun = sunKnown ? u_SunWorld.xyz : float3(0, 1, 0);
    float mu = dot(rd, sun);
    // Dual-lobe phase: bright silver lining towards the sun, some back scattering.
    float phase = (henyeyGreenstein(mu, 0.6) * 0.7 + henyeyGreenstein(mu, -0.2) * 0.3) * 4.0 * PI;
    float day = sunKnown ? u_SunWorld.w : 0.2;
    float3 sunLight = u_SunColor.rgb / max(luma(u_SunColor.rgb), 0.1) * 1.8 * day;
    // In TMNF units like the rest of the light here.
    float4 skyAvg = t0_tex(float2(0.5, 0.5));
    float3 ambient = lerp(skyAvg.rgb / k, u_SkyColor.rgb * 0.45, 0.5) * (0.35 + 0.65 * day);

    float transmittance = 1.0;
    float3 scattered = 0;
    [loop] for (int s = 0; s < (int)steps; s++)
    {
        float3 pos = cam + rd * t;
        float hf = (pos.y - base) / thickness;
        float d = cloudDensity(pos, hf);
        if (d > 0.003)
        {
            // Optical depth towards the sun (4 growing steps through the coarse shape).
            float od = 0.0;
            [unroll] for (int j = 1; j <= 4; j++)
            {
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
    return float4(scattered * fade * k, lerp(1.0, transmittance, fade));
}

// ---------------------------------------------------------------------------------
// Light shafts (half res): what of the sky is bright around the sun, blurred towards it.
// ---------------------------------------------------------------------------------
// t0 = the HDR image
float4 PS_RayMask(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    if (!isSky(uv)) return 0;
    float k = tmnfScale();
    float3 rd = viewToWorldDir(normalize(viewPosition(uv, 1.0)));
    float mu = saturate(dot(rd, u_SunWorld.xyz));
    float3 c = t0_texL(uv).rgb;
    float window = pow(mu, 12.0) * 0.6 + pow(mu, 120.0) * 0.45;
    return float4(min(c, 1.5 * k) * window + u_SunColor.rgb * pow(mu, 40.0) * 0.25 * k, 1);
}

// t0 = mask / previous blur, u_Pass0.x = step scale, u_Pass0.y = decay
float4 PS_RayBlur(VSOut i) : SV_Target
{
    const int SAMPLES = 28;
    float2 delta = (i.uv - u_SunScreen.xy) * u_Pass0.x / SAMPLES;
    float2 suv = i.uv - delta * ign(i.pos.xy);
    float3 sum = 0;
    float weight = 1.0;
    float wsum = 0;
    [loop] for (int s = 0; s < SAMPLES; s++)
    {
        sum += t0_texL(suv).rgb * weight;
        wsum += weight;
        weight *= u_Pass0.y;
        suv -= delta;
    }
    return float4(sum / max(wsum, 1e-3), 1);
}

// The lens flare's source: the bright parts of the image at an eighth of its size, like TMNF's
// third bloom level. t0 = source, u_Pass0.zw = its texel, u_Pass0.x = 1: first step (Karis
// average against fireflies, TMNF's soft threshold in its units).
float3 karis(float3 a, float3 b, float3 c, float3 d)
{
    float wa = 1.0 / (1.0 + luma(a)), wb = 1.0 / (1.0 + luma(b));
    float wc = 1.0 / (1.0 + luma(c)), wd = 1.0 / (1.0 + luma(d));
    return (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
}

float4 PS_FlareDown(VSOut i) : SV_Target
{
    float2 t = u_Pass0.zw;
    // Four bilinear taps cover the 4x4 source texels of this one.
    float3 a = t0_texL(i.uv + t * float2(-1, -1)).rgb, b = t0_texL(i.uv + t * float2(1, -1)).rgb;
    float3 c = t0_texL(i.uv + t * float2(-1, 1)).rgb, d = t0_texL(i.uv + t * float2(1, 1)).rgb;
    if (u_Pass0.x < 0.5) return float4((a + b + c + d) * 0.25, 1);
    float k = tmnfScale();
    float3 result = karis(min(a, 50.0), min(b, 50.0), min(c, 50.0), min(d, 50.0)) / k;
    float br = max(result.r, max(result.g, result.b));
    float soft = clamp(br - 0.6, 0.0, 0.8);
    soft = soft * soft / (4.0 * 0.4 + 1e-4);
    float contrib = max(soft, br - 1.0) / max(br, 1e-4);
    return float4(result * lerp(0.08, 1.0, saturate(contrib)) * k, 1);
}

// Ghosts of the brightest far parts (sun, lamps), mirrored through the centre. t2 = flare source.
float3 flareSource(float2 uv)
{
    // Only the brightest parts produce ghosts, and only far ones: a bright car close in front of
    // the camera would throw a row of copies of itself up the track.
    float k = tmnfScale();
    float3 c = t2_texL(uv).rgb;
    float z = engineZ(uv);
    return max(c - 2.5 * k, 0.0) * smoothstep(25.0, 60.0, z);
}

float3 lensFlare(float2 uv)
{
    float3 sum = 0;
    float2 toCenter = 0.5 - uv;
    [unroll] for (int g = 1; g <= 4; g++)
    {
        float2 p = uv + toCenter * (g * 0.5);
        float fade = pow(saturate(1.0 - length(p - 0.5) / 0.7), 2.0);
        float3 tint = g == 1 ? float3(1.0, 0.7, 0.4) : (g == 2 ? float3(0.45, 0.75, 1.0) : (g == 3 ? float3(0.75, 1.0, 0.55) : float3(1.0, 0.55, 0.85)));
        sum += flareSource(p) * fade * tint * (1.0 / g);
    }
    return sum;
}

// The shafts and the flare onto the HDR image (TMNF PS_Final). t0 = HDR, t1 = shafts (half),
// t2 = flare source (eighth), u_Pass0.x = shafts on, u_Pass0.y = flare on.
// Debug views: t3 = clouds, t4 = the stage's sky, t5 = sky average.
float4 PS_SunPost(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    int debug = (int)(u_Post.w + 0.5);
    if (debug == 20) return float4(t1_texL(uv).rgb, 1);
    if (debug == 21) return float4(t3_texL(uv).rgb + t3_texL(uv).a * 0.05, 1);
    if (debug == 22) return float4(t4_tex(uv).rgb, 1);
    if (debug == 23) return float4(t5_tex(float2(0.5, 0.5)).rgb, 1);
    if (debug == 24) return float4(t2_texL(uv).rgb, 1);
    float3 c = t0_tex(uv).rgb;
    if (u_Pass0.x > 0.5 && u_SunScreen.w > 0.0) c += t1_texL(uv).rgb * u_Rays.x * u_SunScreen.z * 0.3;
    // Ghosts are only plausible for point-like sources: limit them to the visible sun.
    if (u_Pass0.y > 0.5) c += lensFlare(uv) * u_Bloom.z * 0.35 * u_SunScreen.z;
    return float4(c, 1);
}
