// TM Vibrant Shaders for ManiaPlanet: the weather stage's passes (shader model 5), ported from the
// TMNF mod (shaders/tmvs.hlsl, src/pipeline.cpp kRainVS):
//   PS_Reflect: screen-space reflections (half res) for wet roads, puddles and the polished track
//   VS/PS_RainDrop, VS/PS_RainSplash, VS/PS_SnowFlake: particles drawn into the HDR image
//   PS_LensDrops: drops and flakes on the lens
// Particles have no vertex buffer: every particle is 6 vertices (two triangles), its seed is a
// hash of its index (SV_VertexID / 6), its corner SV_VertexID % 6.
// entries: PS_Reflect:ps_5_0 VS_RainDrop:vs_5_0 PS_RainDrop:ps_5_0 VS_RainSplash:vs_5_0 PS_RainSplash:ps_5_0 VS_SnowFlake:vs_5_0 PS_SnowFlake:ps_5_0 PS_LensDrops:ps_5_0
#include "common.hlsli"

// The particles' own constants (effects_weather.cpp), vertex and pixel shaders.
cbuffer WeatherParams : register(b4)
{
    float4 w_Streak; // streak of a drop in the world (m, without a previous frame), time (s, wrapped)
    float4 w_Box;    // rain box size, box height, fall speed (m/s), min width (px)
    float4 w_Wind;   // wind x, z (m/s), splashes per second per particle, splash box size (m)
    float4 w_Motion; // shutter (frames), previous frame valid, frame time (s), longest streak (ndc)
    float4 w_Snow;   // snow wind x, z (m/s), fall speed (m/s), 0
    float4 w_Light;  // 0, 0, 0, intensity of the particles drawn
};

// ---------------------------------------------------------------------------------
// Screen-space reflections (half res).
//   t0 = half normal/depth. Full-res depth and colour are the engine's (t8, t14).
//   out.rgb = reflected colour (display linear), out.a = hit confidence
// ---------------------------------------------------------------------------------
float4 PS_Reflect(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float4 nd = t0_tex(uv);
    if (nd.w >= SKY_Z) return 0;
    float3 nWorld = viewToWorldDir(nd.xyz);
    bool glossy = (u_Weather.x > 0.0 || u_Nature.w > 0.0) && nWorld.y > 0.5;
    if (!glossy) return 0;

    float3 p = viewPosition(uv, nd.w);
    float3 n = nd.xyz;
    float3 r = reflect(normalize(p), n);
    float maxDist = min(120.0 * u_Water.w, p.z * 1.5 + 25.0);
    // Photo mode (u_Water.w 3): twice the steps and little noise (a still image shows the gaps
    // between coarse steps as dashed lines).
    bool photo = u_Water.w > 1.5;
    float noise = ign(i.pos.xy) * (photo ? 0.25 : 0.5);
    float tPrev = 0.05;
    float steps = (u_Nature.w > 1.0 ? 48.0 : 32.0) * (photo ? 2.0 : 1.0);
    [loop] for (int k = 1; k <= (int)steps; k++)
    {
        float t = (k - 1 + noise) / steps;
        t = t * t * maxDist + 0.1;
        float3 q = p + r * t;
        if (q.z < 0.1) break;
        float2 quv = projectToUV(q);
        if (any(quv < 0.0) || any(quv > 1.0)) break;
        float sceneZ = engineZ(quv);
        float delta = q.z - sceneZ;
        if (delta > 0.0 && delta < 0.4 + t * 0.1)
        {
            // Refine between the last two steps.
            float a = tPrev, b = t;
            [unroll] for (int m = 0; m < 6; m++)
            {
                float mid = (a + b) * 0.5;
                float3 qm = p + r * mid;
                if (qm.z - engineZ(projectToUV(qm)) > 0.0) b = mid;
                else a = mid;
            }
            quv = projectToUV(p + r * b);
            float edge = min(min(quv.x, 1.0 - quv.x), min(quv.y, 1.0 - quv.y));
            float confidence = saturate(edge * 10.0) * saturate((1.0 - t / maxDist) * 3.0);
            // The engine's image is HDR already: lamps and neon stay bright in the reflection.
            // (Limited, so a reflected sun glint doesn't turn into a white blot.)
            float3 hit = min(engineColour(quv), 32.0);
            return float4(hit, confidence);
        }
        tPrev = t;
    }
    return 0;
}

// ---------------------------------------------------------------------------------
// Particles
// ---------------------------------------------------------------------------------
struct ParticleOut
{
    float4 pos : SV_Position;
    float4 data : TEXCOORD0;
    float len : TEXCOORD1;
};

float4 particleSeed(uint particle)
{
    float2 p = float2(particle % 4096u, particle / 4096u) + 0.5;
    return float4(hash12(p + 0.13), hash12(p + 17.71), hash12(p + 41.37), hash12(p + 73.19));
}

// The quad's corner: x -1..1 across, y 0..1 along. Two triangles: corners 0 1 2 and 2 1 3.
float2 particleCorner(uint vertex)
{
    static const uint kQuad[6] = {0u, 1u, 2u, 2u, 1u, 3u};
    uint c = kQuad[vertex % 6u];
    return float2((c & 1u) ? 1.0 : -1.0, (c & 2u) ? 1.0 : 0.0);
}

float3 toView(float3 w) { return worldToViewDir(w - cameraWorld()); }

float4 toClip(float3 v)
{
    return float4(v.x * u_Proj.x + v.z * u_Proj.z, v.y * u_Proj.y + v.z * u_Proj.w, v.z * u_Proj2.x + u_Proj2.y, v.z);
}

float2 toNdc(float3 v) { return float2(v.x * u_Proj.x / v.z + u_Proj.z, v.y * u_Proj.y / v.z + u_Proj.w); }

// Where a world point was on screen a frame ago (ndc); false when it was behind the camera.
bool previousNdc(float3 w, out float2 ndc)
{
    float4 p = float4(w, 1.0);
    float3 v = float3(dot(p, u_PrevView0), dot(p, u_PrevView1), dot(p, u_PrevView2));
    ndc = float2(v.x * u_PrevProj.x / v.z + u_PrevProj.z, v.y * u_PrevProj.y / v.z + u_PrevProj.w);
    return v.z > 0.25;
}

// The engine's view z at a screen point (vertex shaders: no sampler, a texel load).
float sceneZAt(float2 uv)
{
    uint2 px = (uint2)clamp(uv * u_Screen.xy, 0.0, u_Screen.xy - 1.0);
    return linearDepth(t_depth.Load(int3(px, 0)));
}

float3 sceneNormalAt(float2 uv)
{
    uint2 px = (uint2)clamp(uv * u_Screen.xy, 0.0, u_Screen.xy - 1.0);
    float3 n = t_normal.Load(int3(px, 0)).xyz * 2.0 - 1.0;
    return n * rsqrt(max(dot(n, n), 1e-8));
}

// Drops: a box around the camera that is fixed in the world (wrapped as the camera moves), each
// drop a screen-aligned streak along its motion relative to the camera (where it was a frame ago).
ParticleOut VS_RainDrop(uint id : SV_VertexID)
{
    ParticleOut o;
    o.pos = float4(0, 0, -2, 1);
    o.data = 0;
    o.len = 0;
    float4 seed = particleSeed(id / 6u);
    float2 corner = particleCorner(id);
    float3 box = float3(w_Box.x, w_Box.y, w_Box.x);
    float3 velocity = float3(w_Wind.x, -w_Box.z * (0.85 + 0.3 * seed.w), w_Wind.y);
    float3 cam = cameraWorld();
    float3 local = (frac((seed.xyz * box + velocity * w_Streak.w - cam) / box) - 0.5) * box;
    float3 w = cam + local;
    float3 va = toView(w);
    if (va.z < 0.4) return o;
    float4 ca = toClip(va);
    float2 na = ca.xy / ca.w, nb;
    float2 prev;
    if (w_Motion.y > 0.5 && previousNdc(w - velocity * w_Motion.z, prev))
    {
        float2 move = (prev - na) * w_Motion.x;
        float l = length(move);
        nb = na + (l > w_Motion.w ? move * (w_Motion.w / l) : move);
    }
    else
    {
        float3 vb = toView(w + w_Streak.xyz);
        if (vb.z < 0.4) return o;
        nb = toNdc(vb);
    }
    float2 pxScale = 2.0 * u_Screen.zw; // ndc per pixel
    float2 d = (nb - na) / pxScale;
    float len = length(d);
    float2 dir = len > 1e-3 ? d / len : float2(0.0, 1.0);
    if (len < 4.0) nb = na + dir * 4.0 * pxScale; // a drop is never a dot
    float z = va.z;
    // Real width, about 2.5 mm; thinner than the minimum means fainter, not wider.
    float widthPx = 0.0025 * abs(u_Proj.y) / (pxScale.y * z);
    float shown = max(widthPx, w_Box.w);
    float2 n = lerp(na, nb, corner.y) + float2(-dir.y, dir.x) * corner.x * shown * 0.5 * pxScale;
    o.pos = float4(n * ca.w, ca.z, ca.w);
    float edge = saturate(2.0 - 4.0 * max(abs(local.x), abs(local.z)) / box.x); // hide the box edges
    // Drops right in front of the lens are out of focus, far ones fade into the rain haze.
    float depth = smoothstep(0.7, 2.5, z) * exp(-z / 28.0) * (0.55 + 0.9 * frac(seed.w * 7.31));
    o.data = float4(corner.x, corner.y, z, edge * depth * sqrt(saturate(widthPx / shown)));
    return o;
}

// The light of the air (overcast): the engine's fog colour (display linear), else the preset's sky.
float3 airSky()
{
    float3 fog = u_Fog.rgb * engineExposure();
    return luma(fog) > 1e-4 ? fog : u_SkyColor.rgb * 0.6;
}

// The light a drop or a flake catches: the sky, what is behind it (a drop is a tiny lens), the
// lamps and neon around it (the bright parts of the image). Display linear, before the tone map.
float3 airLight(float2 uv)
{
    float3 behind = min(t0_texL(uv + float2(0.0, -0.02)).rgb, 16.0);
    float peak = max(behind.r, max(behind.g, behind.b));
    float bright = smoothstep(0.6, 3.0, peak);
    float3 light = airSky() * 0.8 + behind * 0.25 + behind * bright * 0.6 + 0.015;
    return light * lerp(1.0, 0.25, u_Sky.y);
}

float4 PS_RainDrop(ParticleOut i) : SV_Target
{
    float2 uv = i.pos.xy * u_Screen.zw;
    clip(engineZ(uv) - i.data.z);
    float across = saturate(1.0 - abs(i.data.x));
    float along = smoothstep(0.0, 0.3, i.data.y) * smoothstep(1.0, 0.5, i.data.y);
    return float4(airLight(uv) * (across * along * i.data.w * w_Light.w), 0);
}

// Splashes: random spots in a box ahead of the camera (fixed in the world: they stay put while
// the camera moves). TMNF found the ground there in its height map; here a vertical ray is
// marched down through the depth buffer until it goes behind the visible surface, which must
// face up (the engine's normal) for a splash.
ParticleOut VS_RainSplash(uint id : SV_VertexID)
{
    ParticleOut o;
    o.pos = float4(0, 0, -2, 1);
    o.data = 0;
    o.len = 0;
    float4 seed = particleSeed(id / 6u + 50000u);
    float2 corner = particleCorner(id);
    float cycle = w_Streak.w * w_Wind.z * (0.8 + 0.4 * seed.w) + seed.z;
    float k = floor(cycle), age = frac(cycle) / 0.3;
    if (age > 1.0) return o;
    float2 r = frac(sin(float2(k * 12.9898 + seed.x * 78.233, k * 39.3468 + seed.y * 11.135)) * 43758.5453);
    float size = w_Wind.w;
    float3 cam = cameraWorld();
    float2 ahead = cam.xz + normalize(u_ViewToW2.xz + 1e-4) * size * 0.4;
    float2 xz = ahead + (frac((r * size - ahead) / size) - 0.5) * size;
    // Down from above the camera: the first step that ends behind the visible surface.
    float top = cam.y + 2.0, bottom = cam.y - 60.0;
    float found = -1e5;
    float yPrev = top;
    [loop] for (int s = 1; s <= 20; s++)
    {
        float f = s / 20.0;
        float y = lerp(top, bottom, f * f);
        float3 v = toView(float3(xz.x, y, xz.y));
        if (v.z < 0.6) { yPrev = y; continue; }
        float2 suv = ndcToUV(toNdc(v));
        if (any(suv < 0.0) || any(suv > 1.0)) break;
        float sceneZ = sceneZAt(suv);
        if (v.z > sceneZ)
        {
            // Behind the surface: between this step and the last one (refined).
            float a = yPrev, b = y;
            [unroll] for (int m = 0; m < 5; m++)
            {
                float mid = (a + b) * 0.5;
                float3 vm = toView(float3(xz.x, mid, xz.y));
                float2 muv = ndcToUV(toNdc(vm));
                if (vm.z > sceneZAt(muv)) b = mid; else a = mid;
            }
            found = (a + b) * 0.5;
            break;
        }
        yPrev = y;
    }
    if (found < -1e4) return o;
    float3 v = toView(float3(xz.x, found, xz.y));
    if (v.z < 0.6) return o;
    float2 suv = ndcToUV(toNdc(v));
    // Only where this spot is the surface you see (not a wall seen from the side) and it faces up.
    if (abs(sceneZAt(suv) - v.z) > 0.25 + v.z * 0.02) return o;
    if (viewToWorldDir(sceneNormalAt(suv)).y < 0.7) return o;
    float sz = 0.08 + 0.06 * seed.w;
    float3 up = u_UpView.xyz;
    float3 right = normalize(float3(1.0, 0.0, 0.0) - up * up.x);
    o.pos = toClip(v + right * corner.x * sz * 1.7 + up * corner.y * sz * 1.6);
    o.data = float4(corner.x, corner.y, v.z, age);
    return o;
}

float4 PS_RainSplash(ParticleOut i) : SV_Target
{
    float2 uv = i.pos.xy * u_Screen.zw;
    clip(engineZ(uv) - (i.data.z * 0.985 - 0.05));
    float p = i.data.w;   // age, 0..1
    float2 q = i.data.xy; // x: -1..1 across, y: 0..1 up
    // Crown: a thin wall of water that opens up and rises, then falls apart.
    float h = 0.55 * sin(saturate(p * 2.5) * PI * 0.5) * (1.0 - 0.6 * p);
    float r = 0.2 + 0.55 * sqrt(p);
    float wall = exp(-pow((abs(q.x) - r * (0.7 + 0.3 * q.y / max(h, 0.05))) * 8.0, 2.0));
    wall *= smoothstep(h, h * 0.6, q.y) * saturate(q.y / max(h * 0.3, 0.02));
    // Droplets thrown out on short arcs.
    float drops = 0.0;
    [unroll] for (int k = 0; k < 4; k++)
    {
        float dx = (k - 1.5) / 1.5;
        float2 pos = float2(dx * (0.25 + 0.7 * p), 3.2 * p * (1.0 - p) * (0.55 + 0.25 * frac(k * 0.618 + 0.3)));
        drops += smoothstep(0.1, 0.03, length((q - pos) * float2(1.0, 1.4)));
    }
    // Flash where the drop hits the ground.
    float hit = exp(-q.y * 14.0 - q.x * q.x * 5.0) * saturate(1.0 - p * 2.5);
    float a = (wall * 0.7 + drops * 0.8 + hit) * (1.0 - p) * w_Light.w;
    return float4(airLight(uv) * a * 0.7, 0);
}

// Snow: a box mostly ahead of the camera, fixed in the world (wrapped like the rain), each flake
// drifting with the wind and fluttering. Flakes rushing past the camera stretch into short streaks;
// those right at the lens are out of focus.
ParticleOut VS_SnowFlake(uint id : SV_VertexID)
{
    ParticleOut o;
    o.pos = float4(0, 0, -2, 1);
    o.data = 0;
    o.len = 0;
    float4 seed = particleSeed(id / 6u + 100000u);
    float2 corner = particleCorner(id);
    float3 box = float3(28.0, 16.0, 28.0);
    float3 cam = cameraWorld();
    float3 centre = cam + normalize(float3(u_ViewToW2.x, 0.0, u_ViewToW2.z) + 1e-5) * 9.0;
    float t = w_Streak.w;
    float gust = 0.75 + 0.25 * sin(t * 0.6 + seed.x * 2.0) + 0.15 * sin(t * 1.7 + seed.z * 5.0);
    float3 velocity = float3(w_Snow.x * gust, -w_Snow.z * (0.7 + 0.6 * seed.w), w_Snow.y * gust);
    float3 local = (frac((seed.xyz * box + velocity * t - centre) / box) - 0.5) * box;
    float phase = seed.w * 40.0;
    float sway = 0.12 + 0.35 * frac(seed.x * 17.3);
    local.xz += float2(sin(t * (1.3 + seed.x) + phase), cos(t * (1.1 + seed.z) + phase)) * sway;
    float3 w = centre + local;
    float3 va = toView(w);
    if (va.z < 0.25) return o;
    float4 ca = toClip(va);
    float2 na = ca.xy / ca.w;
    float2 nb = na;
    float2 prev;
    if (w_Motion.y > 0.5 && previousNdc(w - velocity * w_Motion.z, prev))
    {
        float2 move = (prev - na) * w_Motion.x * 0.5;
        float l = length(move);
        nb = na + (l > w_Motion.w * 0.5 ? move * (w_Motion.w * 0.5 / l) : move);
    }
    float z = va.z;
    float2 pxScale = 2.0 * u_Screen.zw;
    // Flakes of 6 - 20 mm, every tenth a clump of up to 4 cm; closer than ~1.5 m they blur into
    // larger, fainter discs.
    float kind = frac(seed.w * 13.7);
    float sizeM = kind > 0.9 ? 0.025 + 0.15 * (kind - 0.9) : 0.006 + 0.016 * kind / 0.9;
    float px = sizeM * abs(u_Proj.y) / (pxScale.y * z);
    float blur = 5.0 * saturate((1.5 - z) / 1.25);
    float r = max(px, 1.2) + blur;
    float opacity = saturate(px / 1.2) * 0.85 + 0.15;
    opacity *= 1.0 / (1.0 + blur * 0.35);
    opacity *= 0.7 + 0.3 * sin(t * (3.0 + 4.0 * seed.z) + phase);
    float2 d = (nb - na) / pxScale; // streak in pixels
    float len = length(d);
    float2 dir = len > 1e-3 ? d / len : float2(0.0, 1.0);
    float lenR = len / r;
    opacity /= 1.0 + lenR * 0.5;
    float along = corner.y * (lenR + 2.0) - 1.0;
    float2 n = na + (dir * along + float2(-dir.y, dir.x) * corner.x) * r * pxScale;
    o.pos = float4(n * ca.w, ca.z, ca.w);
    float edge = saturate((0.5 - max(abs(local.x), abs(local.z)) / box.x) * 8.0);
    edge *= saturate((0.5 - abs(local.y) / box.y) * 8.0);
    float far = exp(-z / 24.0);
    o.data = float4(corner.x, along, z, opacity * edge * far * smoothstep(0.25, 0.6, z));
    o.len = lenR;
    return o;
}

// Premultiplied alpha (blended over the image: white on white doesn't glow).
float4 PS_SnowFlake(ParticleOut i) : SV_Target
{
    float2 uv = i.pos.xy * u_Screen.zw;
    clip(engineZ(uv) - i.data.z);
    float along = i.data.y - clamp(i.data.y, 0.0, i.len);
    float r = length(float2(i.data.x, along));
    float a = smoothstep(1.0, 0.35, r) * i.data.w * w_Light.w;
    // Overcast light from all around, a touch of the sun; lamps and neon at night.
    float3 behind = min(t0_texL(uv).rgb, 16.0);
    float peak = max(behind.r, max(behind.g, behind.b));
    float3 light = luma(airSky()) * float3(0.95, 0.97, 1.0) * 1.3 + u_SunColor.rgb * u_SunWorld.w * 0.25 * (1.0 - saturate(u_SunOut.x)) +
                   behind * smoothstep(0.6, 3.0, peak) * 0.5 + 0.02;
    light *= lerp(1.0, 0.3, u_Sky.y);
    // Lit by the same light as the snow around it: never darker than what is behind.
    float3 colour = max(light, luma(behind) * 1.08 + 0.01);
    return float4(colour * a, a);
}

// ---------------------------------------------------------------------------------
// Drops on the lens (u_Volume.w: rain) and snow flakes on it (u_Sky3.y).
//   t0 = the HDR image. Water on a lens is clear: it only bends the image behind it.
// ---------------------------------------------------------------------------------
// Returns the uv offset (xy) and a brightness factor (z, ~1).
float3 lensDrops(float2 uv)
{
    float3 r = float3(0, 0, 1);
    float amount = u_Volume.w;
    if (amount <= 0.0) return r;
    float aspect = u_Screen.x * u_Screen.w;
    float2 p = float2(uv.x * aspect, uv.y); // square units
    float t = u_Proj2.z;

    // Small droplets, sitting still, slowly drying off.
    {
        float scale = 26.0;
        float2 q = p * scale;
        float2 cell = floor(q);
        float h = hash12(cell + 91.7);
        float cycle = t * (0.03 + 0.04 * h) + h * 11.0;
        float life = frac(cycle);
        if (hash12(cell + floor(cycle) * 7.7) < amount * 0.32)
        {
            float2 centre = (float2(hash12(cell + 1.7), hash12(cell + 4.3)) - 0.5) * 0.5;
            float2 d = frac(q) - 0.5 - centre;
            float ang = atan2(d.y, d.x);
            float radius = lerp(0.12, 0.3, hash12(cell + 8.1)) * smoothstep(0.0, 0.03, life) * (1.0 - smoothstep(0.6, 1.0, life));
            float k = length(d) / max(radius, 1e-4) * (1.0 + 0.12 * sin(ang * 3.0 + h * 20.0));
            if (k < 1.0)
            {
                float bulge = sqrt(1.0 - k * k);
                r.xy = -d / scale * float2(1.0 / aspect, 1.0) * 1.4 * bulge;
                r.z = 1.0 + 0.06 * bulge - 0.12 * smoothstep(0.85, 1.0, k);
            }
        }
    }

    // Running drops: one per column at most, sliding down at their own pace, a wet trail behind.
    {
        float scale = 9.0;
        float col = floor(p.x * scale);
        float h = hash12(float2(col, 17.3));
        float speed = 0.06 + 0.12 * h;
        float cycle = t * speed + h * 5.0;
        if (hash12(float2(col, floor(cycle) * 3.1)) < amount * 0.5)
        {
            float y = frac(cycle) * 1.4 - 0.2;
            float cx = (col + 0.5 + (h - 0.5) * 0.5) / scale + sin(y * 9.0 + h * 30.0) * 0.006;
            float radius = lerp(0.012, 0.022, frac(h * 7.3));
            float2 d = float2(p.x - cx, p.y - y);
            d.y *= d.y > 0.0 ? 0.8 : 1.3;
            float k = length(d) / radius;
            if (k < 1.0)
            {
                float bulge = sqrt(1.0 - k * k);
                r.xy = -d * float2(1.0 / aspect, 1.0) * 1.8 * bulge;
                r.z = 1.0 + 0.08 * bulge - 0.12 * smoothstep(0.85, 1.0, k);
            }
            else if (p.y < y && p.y > y - 0.25)
            {
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

// Snow flakes on the lens: they land, sit out of focus (soft blurs) and melt away.
// Returns the coverage.
float lensSnow(float2 uv)
{
    float amount = u_Sky3.y;
    if (amount <= 0.0) return 0;
    float aspect = u_Screen.x * u_Screen.w;
    float2 p = float2(uv.x * aspect, uv.y);
    float t = u_Proj2.z;
    float result = 0;
    [unroll] for (int layer = 0; layer < 2; layer++)
    {
        float scale = layer == 0 ? 7.0 : 13.0;
        float2 q = p * scale + layer * 17.3;
        float2 cell = floor(q);
        float h = hash12(cell + 23.1 + layer * 5.0);
        float cycle = t * (0.12 + 0.1 * h) + h * 13.0;
        float life = frac(cycle);
        if (hash12(cell + floor(cycle) * 5.3) < amount * (layer == 0 ? 0.06 : 0.1))
        {
            float2 centre = (float2(hash12(cell + 2.9), hash12(cell + 6.1)) - 0.5) * 0.5;
            float2 d = frac(q) - 0.5 - centre;
            float ang = atan2(d.y, d.x) + h * 6.0;
            float melt = smoothstep(0.35, 1.0, life);
            float radius = lerp(0.14, 0.3, hash12(cell + 3.3)) * (1.0 - 0.6 * melt);
            float arms = 1.0 + 0.07 * (1.0 - melt) * cos(ang * 6.0);
            float k = length(d) / radius * arms;
            result = max(result, smoothstep(1.0, 0.0, k) * (1.0 - melt) * smoothstep(0.0, 0.02, life) * 0.55);
        }
    }
    return result;
}

float4 PS_LensDrops(VSOut i) : SV_Target
{
    float3 drop = lensDrops(i.uv);
    float4 c = t0_texL(i.uv + drop.xy);
    c.rgb *= drop.z;
    float flake = lensSnow(i.uv);
    if (flake > 0.0)
    {
        // A flake on the lens is lit by the scene, not glowing: white in daylight, dim at night.
        float3 colour = luma(airSky()) * float3(0.95, 0.97, 1.0) * 1.4 * lerp(0.95, 0.3, saturate(u_Sky.y)) + 0.02;
        c.rgb = lerp(c.rgb, colour, flake);
    }
    return c;
}
