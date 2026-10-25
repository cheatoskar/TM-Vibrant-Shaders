// TM Vibrant Shaders for ManiaPlanet: the weather stage's passes (shader model 5), ported from the
// TMNF mod (shaders/tmvs.hlsl, src/pipeline.cpp kRainVS):
//   PS_Reflect: screen-space reflections (half res) for wet roads, puddles and the polished track
//   VS/PS_RainDrop, VS/PS_RainSplash, VS/PS_Spray, VS/PS_SnowFlake: particles drawn into the HDR image
//   PS_LensDrops: drops and flakes on the lens
//   PS_WaterLevel, VS/PS_WaterSplash: the water near the car and what the car throws up in it
//   (WaterSplash, effects_water.cpp; TMNF: PS_WaterSplash, kRainVS waterVS)
// Particles have no vertex buffer: every particle is 6 vertices (two triangles), its seed is a
// hash of its index (SV_VertexID / 6), its corner SV_VertexID % 6.
// entries: PS_Reflect:ps_5_0 VS_RainDrop:vs_5_0 PS_RainDrop:ps_5_0 VS_RainSplash:vs_5_0 PS_RainSplash:ps_5_0 VS_Spray:vs_5_0 PS_Spray:ps_5_0 VS_SnowFlake:vs_5_0 PS_SnowFlake:ps_5_0 PS_LensDrops:ps_5_0 PS_WaterLevel:ps_5_0 VS_WaterSplash:vs_5_0 PS_WaterSplash:ps_5_0
#include "common.hlsli"

// The particles' own constants (effects_weather.cpp), vertex and pixel shaders.
cbuffer WeatherParams : register(b4)
{
    float4 w_Streak; // streak of a drop in the world (m, without a previous frame), time (s, wrapped)
    float4 w_Box;    // rain box size, box height, fall speed (m/s), min width (px)
    float4 w_Wind;   // wind x, z (m/s), splashes per second per particle, splash box size (m)
    float4 w_Motion; // shutter (frames), previous frame valid, frame time (s), longest streak (ndc)
    float4 w_Snow;   // snow wind x, z (m/s), fall speed (m/s), 0
    float4 w_Light;  // splashes: this layer's box (m), the near box inside it (m, 0 = none), splash scale; intensity of the particles drawn
    // The spray (VS_Spray): the car the camera follows, from the engine (w_Car.w = 1), else found in
    // the depth buffer (w_CarVel then holds the camera's velocity).
    float4 w_Car;      // its position (world, its origin), 1 = from the engine
    float4 w_CarFwd;   // its forward (unit, world), w: the front axle (m along it)
    float4 w_CarRight; // its side, w: the rear axle (m along forward, < 0)
    float4 w_CarUp;    // its up, w: half the track (m)
    float4 w_CarVel;   // its velocity (world, m/s), w: the road under the tyres (m along up from its position)
    float4 w_Spray;    // speed along the road (m/s), amount, 0, 0
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
    // Two layers, as in TMNF: the near box, and a far one four times its size, reaching about
    // 110 m ahead, with larger splashes; it takes over where the near one thins out (it ended in
    // a hard line on the road). Each fades over half its box, so neither edge shows.
    float size = w_Light.x > 0.0 ? w_Light.x : w_Wind.w;
    float3 cam = cameraWorld();
    float2 forward = normalize(u_ViewToW2.xz + 1e-4);
    float2 ahead = cam.xz + forward * size * (w_Light.y > 0.0 ? 0.45 : 0.4);
    float2 xz = ahead + (frac((r * size - ahead) / size) - 0.5) * size;
    float2 edge = abs(xz - ahead) / (size * 0.5);
    float fade = 1.0 - smoothstep(0.5, 1.0, max(edge.x, edge.y));
    if (w_Light.y > 0.0)
    {
        float2 near = abs(xz - (cam.xz + forward * w_Light.y * 0.4)) / (w_Light.y * 0.5);
        fade *= smoothstep(0.45, 1.0, max(near.x, near.y));
    }
    if (fade <= 0.02) return o;
    // Down from above the camera: the first step that ends behind the visible surface.
    float top = cam.y + 2.0, bottom = cam.y - (w_Light.y > 0.0 ? 90.0 : 60.0);
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
    float sz = (0.08 + 0.06 * seed.w) * (w_Light.z > 0.0 ? w_Light.z : 1.0) * fade;
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

// Spray: water thrown up by the tyres on a wet road (or powder snow), as in TMNF (src/pipeline.cpp
// kRainVS sprayTyres). With the engine's car (w_Car.w = 1: the car the camera follows, from its
// draw's constants) from its four tyres along its own axes; without, the car is found in the depth
// buffer where the chase camera keeps it (low in the middle of the screen) and its rear tyres throw
// it. Every particle leaves a tyre, rises, falls and hangs in the air while the car drives on.

// Whether the road is where a tyre touches it: the surface seen just outside the tyre (where the
// camera looks past the wheel) lies near the contact's height. 0 at 1.2 m off or more: in the air,
// or something else in the way (car body, barrier, off screen). dh: how far above the contact it
// is (the spray stands on the road the camera sees, whatever height the car's origin has).
float roadAt(float3 contact, float3 outward, float3 up, out float dh)
{
    dh = 0.0;
    float3 v = toView(contact + outward * 0.55);
    if (v.z < 0.5) return 0.0;
    float2 uv = ndcToUV(toNdc(v));
    if (any(uv < 0.0) || any(uv > 1.0)) return 0.0;
    float3 road = worldPosition(viewPosition(uv, sceneZAt(uv)));
    dh = dot(road - contact, up);
    return saturate(1.0 - (abs(dh) - 0.8) / 0.4);
}

// One particle from the tyres. `road`: the road under the car's origin; the axles along `fwd`
// (front > 0, rear < 0); rearOnly: the depth search's car (only its tail is known); maxLift: how
// far the road the camera sees may lie from `road` (m).
ParticleOut sprayFromTyres(float4 seed, float2 corner, float3 road, float3 fwd, float3 right, float3 up, float frontAxle,
                           float rearAxle, float halfTrack, bool rearOnly, float maxLift)
{
    ParticleOut o;
    o.pos = float4(0, 0, -2, 1);
    o.data = 0;
    o.len = 0;
    // Only with the tyres on the road (slopes, banks, walls and loops included; jumps not): any of
    // them will do, a barrier may hide the road beside the others.
    float grounded = 0.0, lift = 0.0, weights = 0.0;
    [unroll] for (int k = 0; k < 4; k++)
    {
        if (rearOnly && k < 2) continue;
        float s = (k == 1 || k == 3) ? 1.0 : -1.0;
        float3 contact = road + fwd * (k < 2 ? frontAxle : rearAxle) + right * s * halfTrack;
        float dh;
        float w = roadAt(contact, right * s, up, dh);
        grounded = max(grounded, w);
        lift += w * dh;
        weights += w;
    }
    lift /= max(weights, 1e-3);
    if (grounded <= 0.0 || abs(lift) > maxLift) return o;
    road += up * lift; // onto the road the camera sees
    // Seven of ten particles leave the rear tyres, which throw most (the front ones run on the
    // water the car pushes aside and throw less, with smaller puffs).
    bool rear = rearOnly || seed.x < 0.7;
    float side = rearOnly ? (seed.x < 0.5 ? -1.0 : 1.0) : (rear ? (seed.x < 0.35 ? -1.0 : 1.0) : (seed.x < 0.85 ? -1.0 : 1.0));
    float strength = rear ? 1.0 : 0.55;
    float life = 0.6;
    float t = w_Streak.w;
    float age = frac(t / life * (0.85 + 0.3 * seed.w) + seed.z);
    float ts = age * life;
    // Gusts: each tyre throws more or less water as it rolls through wetter and drier patches (two
    // slow waves per tyre); fewer particles where it is drier.
    float wheel = side + (rear ? 0.0 : 3.0);
    float gust = 0.6 + 0.2 * sin(t * (1.7 + 0.6 * side) + wheel) + 0.15 * sin(t * (4.3 - 1.1 * side) + 2.0 * wheel);
    if (frac(seed.w * 5.31 + seed.z * 2.7) > gust * grounded * strength) return o;
    float3 contact = road + fwd * (rear ? rearAxle : frontAxle) + right * side * halfTrack;
    // The car's motion in its own axes: rolling along fwd, sliding along right.
    float3 v = w_CarVel.xyz;
    float rolling = dot(v, fwd), sliding = dot(v, right);
    float back = rolling >= 0.0 ? 1.0 : -1.0; // the tread throws to the back of the way it rolls
    float3 tyre = contact - fwd * back * 0.3 + up * 0.1;
    // Relative to the car: thrown off the tread backwards along the car's own axis and swept along
    // in its wake (keeping 3/4 of its rolling speed, so it trails close behind); sideways a sliding
    // tyre pushes the water out the way it slides (between a little behind and a third ahead of the
    // tyre: the spray flies off the leading side); fanning out, rising.
    float3 rel = -fwd * (back * (1.5 + 3.0 * seed.y) + rolling * 0.25) +
                 right * (sliding * (-0.1 + 0.45 * frac(seed.z * 9.7)) + side * (0.3 + 1.5 * frac(seed.x * 13.3))) +
                 up * (1.2 + 2.8 * frac(seed.y * 7.1));
    float3 p = tyre + rel * ts;
    p.y -= 4.9 * ts * ts;
    float h = dot(p - contact, up); // not below the road
    p += up * max(-0.05 - h, 0.0);
    float3 view = toView(p);
    if (view.z < 0.5) return o;
    float size = lerp(0.15, 1.1, sqrt(age)) * (0.7 + 0.6 * frac(seed.w * 3.7)) * (0.6 + 0.4 * gust) * (rear ? 1.0 : 0.75);
    o.pos = toClip(view + float3(corner.x, corner.y * 2.0 - 1.0, 0.0) * size);
    o.data = float4(corner.x, corner.y * 2.0 - 1.0, view.z, age);
    return o;
}

ParticleOut VS_Spray(uint id : SV_VertexID)
{
    ParticleOut o;
    o.pos = float4(0, 0, -2, 1);
    o.data = 0;
    o.len = 0;
    if (w_Spray.x < 4.0 || w_Spray.y <= 0.0) return o;
    float4 seed = particleSeed(id / 6u + 150000u);
    float2 corner = particleCorner(id);
    if (w_Car.w > 0.5)
    {
        float3 up = w_CarUp.xyz;
        return sprayFromTyres(seed, corner, w_Car.xyz + up * w_CarVel.w, w_CarFwd.xyz, w_CarRight.xyz, up, w_CarFwd.w, w_CarRight.w,
                              w_CarUp.w, false, 2.0);
    }
    // No car from the engine: the closest surface on three points below the screen centre (the
    // road right below the car would be closer still: stay above it).
    float z0 = sceneZAt(float2(0.5, 0.6));
    float z1 = sceneZAt(float2(0.5, 0.65));
    float z2 = sceneZAt(float2(0.5, 0.7));
    float bestZ = min(z0, min(z1, z2));
    float2 bestUV = float2(0.5, bestZ == z0 ? 0.6 : (bestZ == z1 ? 0.65 : 0.7));
    if (bestZ < 1.5 || bestZ > 16.0) return o;  // no car in front of the camera
    if (abs(u_ViewToW2.y) > 0.45) return o;      // camera pitched up/down: quarter pipe, loop, wall ride
    float3 car = worldPosition(viewPosition(bestUV, bestZ));
    float3 fwd = normalize(float3(u_ViewToW2.x, 0.0, u_ViewToW2.z) + 1e-5); // the camera's forward, level
    float3 up = float3(0.0, 1.0, 0.0);
    float3 right = float3(fwd.z, 0.0, -fwd.x);
    // The point found is the car's tail (rear wing, engine cover), about 0.85 m over the road
    // (capture 034: 1.0 m); the rear tyres sit a little ahead of it. roadAt puts the spray onto the
    // road the camera sees. The road beside it 0.5 m or more off that: no car there (the road
    // itself, a camera that doesn't draw the car: the cockpit).
    return sprayFromTyres(seed, corner, car - up * 0.85, fwd, right, up, 0.0, -0.2, 0.8, true, 0.5);
}

// Each puff a soft disc, fading as it ages; lit like the rain (hundreds overlap: each is faint).
float4 PS_Spray(ParticleOut i) : SV_Target
{
    float2 uv = i.pos.xy * u_Screen.zw;
    clip(engineZ(uv) - (i.data.z * 0.985 - 0.05));
    float puff = saturate(1.0 - dot(i.data.xy, i.data.xy));
    puff *= puff;
    float fade = 1.0 - i.data.w;
    float a = puff * fade * fade * w_Spray.y * 0.05;
    return float4(airLight(uv) * a * w_Light.w, 0);
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

// ---------------------------------------------------------------------------------
// Water (WaterSplash, effects_water.cpp)
// ---------------------------------------------------------------------------------
cbuffer WaterParams : register(b5)
{
    float4 wl_Level;    // water height (world y), the setting (0..1), wake (0..1: the car is in the water), drips (0..1)
    float4 wl_Ev[8];    // per entry [2e] where (world xyz), seconds since (< 0 none); [2e+1] its velocity (m/s), strength (0..1)
    float4 wl_Kind;     // x kind, y which entry, z seconds since the car left the water (< 0 none), w which ring
    float4 wl_Car;      // the car (as w_Car .. w_CarVel): position, 1 = known
    float4 wl_CarFwd;   // its forward (unit), w: the front axle (m along it)
    float4 wl_CarRight; // its side, w: the rear axle (m along forward, < 0)
    float4 wl_CarUp;    // its up, w: half the track (m)
    float4 wl_CarVel;   // its velocity (m/s), w: the road under the tyres (m along up from its position)
};

// The planes of this frame's water (water11.cpp: each fog draw's WorldToWaterDepth, depth under the
// surface = dot(world, 1)).
cbuffer WaterPlane0 : register(b6) { float4 g_wplane0; };
cbuffer WaterPlane1 : register(b7) { float4 g_wplane1; };
cbuffer WaterPlane2 : register(b8) { float4 g_wplane2; };
cbuffer WaterPlane3 : register(b9) { float4 g_wplane3; };
cbuffer WaterPlane4 : register(b10) { float4 g_wplane4; };
cbuffer WaterPlane5 : register(b11) { float4 g_wplane5; };
cbuffer WaterPlane6 : register(b12) { float4 g_wplane6; };
cbuffer WaterPlane7 : register(b13) { float4 g_wplane7; };

// The water near the car, one texel per plane: rings of points (1.6, 3.4 and 5.2 m) around the spot
// on the plane over (or under) the car. A point counts when the camera sees the plane there (the
// depth after the engine drew its surface) with the engine's water fog behind it (t0: its cover).
//   u_Pass0 = the car (world xyz), planes; u_Pass1.x = 1: the car is known
//   out: x points on the surface, y points tried, z the plane's height at the car, w the car's depth under it
float4 PS_WaterLevel(VSOut i) : SV_Target
{
    int k = (int)i.pos.x;
    if (k >= (int)u_Pass0.w || u_Pass1.x < 0.5) return 0;
    float4 planes[8] = {g_wplane0, g_wplane1, g_wplane2, g_wplane3, g_wplane4, g_wplane5, g_wplane6, g_wplane7};
    float4 pl = planes[min(k, 7)];
    float len = length(pl.xyz);
    if (len < 1e-4) return 0;
    float3 up = -pl.xyz / len;
    float3 car = u_Pass0.xyz;
    float under = dot(float4(car, 1.0), pl) / len;
    float3 spot = car + up * under;
    float3 side = normalize(cross(up, abs(up.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0)));
    float3 side2 = cross(up, side);
    float hits = 0.0, tried = 0.0;
    float3 cam = cameraWorld();
    [loop] for (int ring = 0; ring < 3; ring++)
    {
        float radius = 1.6 + ring * 1.8;
        [loop] for (int j = 0; j < 8; j++)
        {
            float a = (j + ring * 0.5) * 0.7853982;
            float3 q = spot + (side * cos(a) + side2 * sin(a)) * radius;
            float3 v = worldToViewDir(q - cam);
            if (v.z < 0.3) continue;
            float2 uv = projectToUV(v);
            if (any(uv < 0.002) || any(uv > 0.998)) continue;
            tried += 1.0;
            float z = engineZ(uv);
            float3 w = worldPosition(viewPosition(uv, z));
            float off = abs(dot(float4(w, 1.0), pl)) / len;
            float cover = g_in0.SampleLevel(g_point, uv, 0).a;
            hits += (z < SKY_Z && off < 0.2 + z * 0.004 && cover > 1e-4) ? 1.0 : 0.0;
        }
    }
    return float4(hits, tried, spot.y, under);
}

// What the car throws up in the water (TMNF's waterVS, kRainVS): drawn per kind (wl_Kind.x),
// kinds 0-1 and 5-6 as camera-facing quads, 2-4 as quads lying on the water.
//   0 droplets of an entry's fountain, 1 its mist, 2 a ring spreading over the surface (wl_Kind.w:
//   which of three), 3 its foam, 4 the wake's foam, 5 the bow's droplets, 6 the dripping when the
//   car comes out. Events are the entries still young (the newest four); the wake, the bow and the
//   drips follow the engine's car and run on the effects' clock.
struct WaterOut
{
    float4 pos : SV_Position;
    float4 data : TEXCOORD0;  // position in the quad (-1..1), view z, age (0..1)
    float4 data2 : TEXCOORD1; // kind, opacity, random
};

WaterOut waterHidden()
{
    WaterOut o;
    o.pos = float4(0, 0, -2, 1);
    o.data = 0;
    o.data2 = 0;
    return o;
}

// A camera-facing quad at world point p, `size` m from its middle to its edge.
WaterOut waterBillboard(float3 p, float2 corner, float size, float age, float kind, float opacity, float2 rnd)
{
    WaterOut o = waterHidden();
    float3 view = toView(p);
    if (view.z < 0.5 || opacity <= 0.0) return o;
    float2 c = float2(corner.x, corner.y * 2.0 - 1.0);
    o.pos = toClip(view + float3(c, 0.0) * size);
    o.data = float4(c, view.z, age);
    o.data2 = float4(kind, opacity, rnd);
    return o;
}

// A quad lying on the water around `centre`, `size` m from its middle to its edge, turned by `turn`.
WaterOut waterFlat(float3 centre, float2 corner, float size, float turn, float age, float kind, float opacity, float2 rnd)
{
    WaterOut o = waterHidden();
    if (opacity <= 0.0) return o;
    float2 c = float2(corner.x, corner.y * 2.0 - 1.0);
    float cs = cos(turn), sn = sin(turn);
    float3 view = toView(centre + float3(c.x * cs - c.y * sn, 0.0, c.x * sn + c.y * cs) * size);
    if (view.z < 0.5) return o;
    o.pos = toClip(view);
    o.data = float4(c, view.z, age);
    o.data2 = float4(kind, opacity, rnd);
    return o;
}

WaterOut VS_WaterSplash(uint id : SV_VertexID)
{
    float4 seed = particleSeed(id / 6u + 200000u);
    float2 corner = particleCorner(id);
    int kind = (int)(wl_Kind.x + 0.5);
    float level = wl_Level.x;
    float time = u_Proj2.z;
    // The setting thins every kind out (and the shader fades it by the same amount).
    if (kind != 2 && frac(seed.w * 7.31 + seed.z * 3.7) > saturate(wl_Level.y * 1.5)) return waterHidden();
    if (kind <= 3)
    {
        int e = (int)(wl_Kind.y + 0.5);
        float4 where = wl_Ev[e * 2], motion = wl_Ev[e * 2 + 1];
        float age = where.w, strength = motion.w;
        if (age < 0.0 || strength <= 0.0) return waterHidden();
        float4 sd = frac(seed + e * 0.3819 + float4(0.1, 0.7, 0.3, 0.9));
        float3 vh = float3(motion.x, 0.0, motion.z);
        float phi = sd.x * 6.2831853;
        float3 radial = float3(cos(phi), 0.0, sin(phi));
        float lean = dot(radial, normalize(vh + 1e-4)) * 0.5 + 0.5; // 1: the way the car went
        float3 spot = float3(where.x, level, where.z);
        if (kind == 0)
        {
            // The fountain: a crown of drops out and up, more of them in the car's direction, carried on by its speed.
            float t = age - sd.w * 0.12;
            if (t < 0.0) return waterHidden();
            float up = (2.0 + 6.5 * sd.y * sd.y) * (0.35 + 0.65 * strength) * (0.6 + 0.4 * lean);
            float out2 = (0.8 + 3.2 * sd.z) * (0.3 + 0.7 * strength);
            float3 v = radial * out2 + float3(0.0, up, 0.0) + vh * (0.15 + 0.35 * lean * sd.z);
            float3 p = spot + radial * 0.5 + v * t;
            p.y -= 4.9 * t * t;
            if (p.y < level) return waterHidden(); // back in the water
            float size = (0.035 + 0.075 * sd.w) * (0.7 + 0.5 * strength);
            return waterBillboard(p, corner, size, t, 0.0, 1.0, 0.0);
        }
        if (kind == 1)
        {
            // The mist: puffs that rise, spread and thin out.
            float life = 1.7;
            float t = age - sd.w * 0.2;
            if (t < 0.0 || t > life) return waterHidden();
            float k = t / life;
            float drag = (1.0 - exp(-2.0 * t)) * 0.5;
            float3 p = spot + radial * 0.4 + (radial * (1.0 + 2.5 * sd.z) + float3(0.0, 1.5 + 3.5 * sd.y, 0.0)) * (0.4 + 0.6 * strength) * drag + vh * 0.25 * t;
            float size = lerp(0.4, 2.0, sqrt(k)) * (0.6 + 0.8 * sd.x) * (0.6 + 0.6 * strength);
            return waterBillboard(p, corner, size, k, 1.0, strength * (1.0 - k) * (1.0 - k) * smoothstep(0.0, 0.1, k), 0.0);
        }
        if (kind == 2)
        {
            // Three rings, a fifth of a second apart, the first the largest and brightest.
            float ring = wl_Kind.w;
            float t = age - ring * 0.2;
            float life = 2.4;
            if (t < 0.0 || t > life) return waterHidden();
            float k = t / life;
            float radius = 0.7 + t * (2.2 + 3.5 * strength);
            float opacity = strength * pow(saturate(1.0 - k), 1.5) * smoothstep(0.0, 0.06, k) * (1.0 - ring * 0.25);
            return waterFlat(spot + float3(0.0, 0.03, 0.0), corner, radius / 0.86, 0.0, k, 2.0, opacity, frac(sd.zw + ring * 0.37));
        }
        // The foam: patches thrown out around the spot, drifting apart and fraying over a few seconds.
        float life = 4.0;
        float t = age - 0.05 - sd.w * 0.15;
        if (t < 0.0 || t > life) return waterHidden();
        float k = t / life;
        float reach = (0.4 + 2.2 * sd.y) * (0.5 + 0.8 * strength);
        float3 centre = spot + radial * reach * (0.55 + 0.45 * saturate(t * 1.2)) + vh * 0.1 * t + float3(0.0, 0.02, 0.0);
        float size = (0.5 + 0.9 * sd.z) * (0.6 + 0.6 * strength) * (0.8 + 0.4 * k);
        float opacity = strength * smoothstep(0.0, 0.08, k) * pow(saturate(1.0 - k), 1.2) * 0.9;
        return waterFlat(centre, corner, size, sd.w * 6.2831853, k, 3.0, opacity, sd.xy);
    }
    // The car in the water: the wake, the bow's droplets, the drips. Needs the engine's car.
    if (wl_Car.w < 0.5) return waterHidden();
    float3 fwdH = normalize(float3(wl_CarFwd.x, 0.0, wl_CarFwd.z) + 1e-5);
    float3 rightH = float3(fwdH.z, 0.0, -fwdH.x);
    float3 velH = float3(wl_CarVel.x, 0.0, wl_CarVel.z);
    float speed = length(velH);
    float4 sd = frac(seed + float4(0.45, 0.15, 0.85, 0.55));
    float wake = wl_Level.z;
    float3 bow = wl_Car.xyz + fwdH * (wl_CarFwd.w + 0.3);
    bow.y = level;
    float halfTrack = wl_CarUp.w;
    float side = sd.x < 0.5 ? -1.0 : 1.0;
    if (kind == 4)
    {
        if (wake <= 0.0 || speed < 1.5) return waterHidden();
        float life = 3.0;
        float age = frac(time / life * (0.85 + 0.3 * sd.w) + sd.z);
        float t = age * life;
        // From the bow in a V (a third of it from the rear, behind the car's middle), at rest on the water.
        bool rear = sd.y < 0.3;
        float3 origin = rear ? float3(wl_Car.x, level, wl_Car.z) + fwdH * wl_CarRight.w : bow;
        float spread = rear ? (sd.x - 0.5) * halfTrack * 2.0 * (1.0 + t) : side * (halfTrack * 0.8 + t * 0.45 * (0.5 + speed / 25.0));
        float3 centre = origin - velH * t + rightH * spread + float3(0.0, 0.02, 0.0);
        float size = lerp(0.45, 1.3, sqrt(age)) * (0.7 + 0.6 * sd.z);
        float opacity = wake * saturate((speed - 1.5) / 6.0) * pow(1.0 - age, 1.5) * smoothstep(0.0, 0.08, age) * 0.8;
        return waterFlat(centre, corner, size, sd.w * 6.2831853, age, 4.0, opacity, sd.xy);
    }
    if (kind == 5)
    {
        // The bow wave: droplets leaving the front of the car, to the sides, up and with it.
        if (wake <= 0.0 || speed < 3.0) return waterHidden();
        float life = 0.7;
        float age = frac(time / life * (0.85 + 0.3 * sd.w) + sd.z);
        float t = age * life;
        float f = saturate(speed / 18.0);
        float3 v = rightH * side * (1.0 + 3.0 * sd.y) * f + float3(0.0, (1.5 + 3.5 * sd.x) * f, 0.0) + velH * 0.4;
        float3 p = bow - velH * t + rightH * side * halfTrack * 0.8 + v * t;
        p.y -= 4.9 * t * t;
        if (p.y < level) return waterHidden();
        float size = 0.03 + 0.04 * sd.w;
        return waterBillboard(p, corner, size, age, 0.0, wake * saturate((speed - 3.0) / 8.0) * (1.0 - age * 0.5), 0.0);
    }
    // 6: drips off the car's underside and sides after it left the water, for three seconds, fewer and fewer.
    float since = wl_Kind.z;
    if (since < 0.0 || wl_Level.w <= 0.0) return waterHidden();
    float released = 2.5 * sd.z * sd.z;
    float age = since - released;
    if (age < 0.0 || age > 1.5) return waterHidden();
    float3 local = wl_CarFwd.xyz * lerp(wl_CarRight.w, wl_CarFwd.w, sd.x) + wl_CarRight.xyz * (sd.y - 0.5) * 2.2 * halfTrack +
                   wl_CarUp.xyz * (-0.1 + 0.6 * sd.w);
    float3 p = wl_Car.xyz + local - wl_CarVel.xyz * age * 0.5;
    p.y -= 4.9 * age * age;
    float3 road = wl_Car.xyz + wl_CarUp.xyz * wl_CarVel.w;
    if (p.y < road.y) return waterHidden();
    return waterBillboard(p, corner, 0.025 + 0.025 * sd.w, age, 0.0, wl_Level.w * (1.0 - age / 1.5) * (1.0 - since / 3.2), 0.0);
}

// Droplets, mist, rings, foam and the wake (TMNF's PS_WaterSplash), premultiplied over the image.
//   t0 = the image behind (half res, exposed: airLight), t1 = the water pictures (r droplet, g foam,
//   b and a the ripple's slope)
float4 PS_WaterSplash(WaterOut i) : SV_Target
{
    float2 uv = i.pos.xy * u_Screen.zw;
    clip(engineZ(uv) - (i.data.z * 0.985 - 0.05));
    float kind = i.data2.x;
    float2 q = i.data.xy;
    float r = length(q);
    float3 light = airLight(uv);
    float amount = i.data2.y * (0.4 + 0.6 * wl_Level.y);
    if (kind < 0.5)
    {
        // A droplet: a lens that mirrors the sky, with a bright spot.
        clip(1.0 - r);
        float d = g_in1.SampleLevel(g_linearWrap, q * 0.5 + 0.5, 0).r;
        float a = d * amount;
        return float4(light * a * 1.5 + a * 0.05, a * 0.4);
    }
    if (kind < 1.5)
    {
        // Mist: soft puffs that thin out as they spread.
        float puff = saturate(1.0 - r * r);
        float a = puff * puff * amount * 0.22;
        return float4(light * a * 1.1, a * 0.6);
    }
    if (kind < 2.5)
    {
        // A ring on the surface: a ridge that spreads and flattens, its far side catching the sky, its near
        // side dark, and a lip of foam on it. The ripple picture bends it so it is no perfect circle.
        float2 w = g_in1.SampleLevel(g_linearWrap, frac(q * 0.8 + i.data2.zw), 0).ba * 2.0 - 1.0;
        float rr = r + dot(w, q / max(r, 1e-3)) * 0.035;
        float width = 0.07 + 0.1 * i.data.w;
        float band = exp(-pow((rr - 0.86) / width, 2.0));
        float side = 0.5 + 0.5 * dot(normalize(q + 1e-4), float2(0.0, -1.0)); // the far side lights up
        float foam = g_in1.SampleLevel(g_linearWrap, frac(q * 1.3 + i.data2.zw), 0).g;
        float a = band * amount * smoothstep(0.0, 0.25, r);
        float3 colour = light * (0.45 + 1.1 * side) + light * foam * 0.8;
        return float4(colour * a * 0.7, a * 0.5);
    }
    // Foam (3) and the wake (4): a lace picture that frays away as it ages, soft at its edge.
    float f = g_in1.SampleLevel(g_linearWrap, frac(q * 0.5 + i.data2.zw), 0).g;
    float edge = smoothstep(1.0, 0.45, r);
    float erode = i.data.w * (kind < 3.5 ? 0.7 : 0.55);
    float cover = smoothstep(erode, erode + 0.3, f * edge * 1.25);
    float a = cover * amount;
    float3 colour = light * 1.35 + 0.02;
    return float4(colour * a, a);
}
