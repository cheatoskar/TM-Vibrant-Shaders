// TM Vibrant Shaders for ManiaPlanet: the neon trail behind the car (shader model 5), after the
// TMNF mod's (shaders/tmvs.hlsl PS_Trail, src/pipeline.cpp trailVS). The car's position comes
// from the engine (its ObjectV at the car's G-buffer draw, effects_trail.cpp); the points are kept
// in a ring buffer texture, quad k joins point k and k + 1, widened towards the camera.
//   t0 = points (xyz, time stamp; < 0: a new line starts here, 0 = none), 1 row
//   b4: g_trail (points, next index + count of the ring, now (s), fade after (s, 0 = never)),
//       g_trail2 (sideways offset (m), half width (m), ring size, 0), g_trailColour (rgb * strength)
// entries: VS_Trail:vs_5_0 PS_Trail:ps_5_0
#include "common.hlsli"

cbuffer TrailParams : register(b4)
{
    float4 g_trail;
    float4 g_trail2;
    float4 g_trailColour;
};

Texture2D<float4> g_points : register(t0);

struct TrailOut
{
    float4 pos : SV_Position;
    float4 data : TEXCOORD0; // across (-1..1), fade, view z, metres from the car
};

float4 trailPoint(float i)
{
    float n = g_trail2.z;
    i = fmod(i + 2.0 * n, n);
    return g_points.Load(int3((int)i, 0, 0));
}

// Each point averaged with its neighbours on the same line (weights 1 2 3 4 3 2 1), symmetric so
// the newest end stays on the car.
float3 smoothPoint(float i, float4 centre)
{
    float first = g_trail.y - g_trail.x, last = g_trail.y - 1.0;
    float4 back[3], ahead[3];
    float nb = 0.0, nf = 0.0;
    bool open = centre.w > 0.0;
    [unroll] for (int k = 1; k <= 3; k++)
    {
        back[k - 1] = trailPoint(i - k);
        open = open && i - k >= first && back[k - 1].w != 0.0 && distance(back[k - 1].xyz, centre.xyz) < 12.0;
        nb += open ? 1.0 : 0.0;
        open = open && back[k - 1].w > 0.0;
    }
    open = true;
    [unroll] for (int k2 = 1; k2 <= 3; k2++)
    {
        ahead[k2 - 1] = trailPoint(i + k2);
        open = open && i + k2 <= last && ahead[k2 - 1].w > 0.0 && distance(ahead[k2 - 1].xyz, centre.xyz) < 12.0;
        nf += open ? 1.0 : 0.0;
    }
    float n = min(nb, nf);
    float3 sum = centre.xyz * 4.0;
    float weight = 4.0;
    [unroll] for (int k3 = 1; k3 <= 3; k3++)
    {
        if (k3 <= n)
        {
            sum += (back[k3 - 1].xyz + ahead[k3 - 1].xyz) * (4.0 - k3);
            weight += 2.0 * (4.0 - k3);
        }
    }
    return sum / weight;
}

// The direction of the line at point j: from its neighbours on the same line (smoothed). The
// pieces on both sides of a point then share its corners (no gaps or overlaps at the joints), and
// a car shaking in place doesn't swing the tyre lines around.
float3 trailTangent(float j, float4 pj, float3 sj)
{
    float first = g_trail.y - g_trail.x, last = g_trail.y - 1.0;
    float4 pm = trailPoint(j - 1.0), pp = trailPoint(j + 1.0);
    bool hasPrev = pj.w > 0.0 && j - 1.0 >= first && pm.w != 0.0 && distance(pm.xyz, pj.xyz) < 25.0;
    bool hasNext = j + 1.0 <= last && pp.w > 0.0 && distance(pp.xyz, pj.xyz) < 25.0;
    float3 m = hasPrev ? smoothPoint(j - 1.0, pm) : sj;
    float3 n = hasNext ? smoothPoint(j + 1.0, pp) : sj;
    return n - m;
}

float3 toView(float3 w)
{
    float3 d = w - cameraWorld();
    return worldToViewDir(d);
}

float4 toClip(float3 v)
{
    return float4(v.x * u_Proj.x + v.z * u_Proj.z, v.y * u_Proj.y + v.z * u_Proj.w, u_Proj2.x * v.z + u_Proj2.y, v.z);
}

TrailOut VS_Trail(uint id : SV_VertexID)
{
    TrailOut o;
    o.pos = float4(0, 0, -2, 1);
    o.data = 0;
    // Two triangles per quad: corners (x across -1/1, y along 0/1).
    uint q = id / 6, c = id % 6;
    float2 corner = float2(c == 1 || c == 2 || c == 4 ? 1.0 : -1.0, c == 2 || c == 4 || c == 5 ? 1.0 : 0.0);
    float k = (float)q;
    if (k >= g_trail.x - 1.0) return o;
    float first = g_trail.y - g_trail.x;
    float4 a = trailPoint(first + k), b = trailPoint(first + k + 1.0);
    if (a.w == 0.0 || b.w <= 0.0) return o;          // no car at one end, or b starts a new line
    if (distance(a.xyz, b.xyz) > 25.0) return o;     // teleported
    if (distance(a.xyz, b.xyz) < 1e-3) return o;
    // This corner's point, smoothed, and the line's direction there.
    float j = first + k + corner.y;
    float4 pj = corner.y > 0.5 ? b : a;
    float3 s = smoothPoint(j, pj);
    float3 d = trailTangent(j, pj, s);
    float len = length(d);
    if (len < 1e-3) return o;
    float age = g_trail.z - (corner.y > 0.5 ? b.w : abs(a.w));
    float fade = g_trail.w > 0.0 ? saturate(1.0 - age / g_trail.w) : 1.0;
    if (fade <= 0.0) return o;
    // It comes out behind the car (through the body it would flicker); only the fresh part.
    float3 car = trailPoint(g_trail.y - 1.0).xyz;
    float fromCar = lerp(distance(s, car), 100.0, saturate((age - 0.6) / 0.3));
    float3 dir = d / len;
    float3 right = cross(float3(0, 1, 0), dir);
    float rl = length(right);
    right = rl > 0.1 ? right / rl : float3(1, 0, 0);
    float3 p = s + right * g_trail2.x;
    float3 v = toView(p);
    if (v.z < 0.3) return o;
    // Right in front of the lens (a camera on the ground after a crash) it would fill the screen.
    fade *= smoothstep(0.5, 2.5, v.z);
    float3 dv = toView(p + dir) - v;
    float3 side = cross(dv, v);
    float sl = length(side);
    if (sl < 1e-5) return o;
    // At least a pixel wide (no flicker far away); thinner lines are dimmed instead.
    float pixel = v.z * 2.0 * u_Screen.z / abs(u_Proj.x);
    float halfWidth = max(g_trail2.y, pixel * 1.2);
    fade *= g_trail2.y / halfWidth;
    o.pos = toClip(v + side / sl * halfWidth * corner.x);
    o.data = float4(corner.x, fade, v.z, fromCar);
    return o;
}

// A bright core with a soft glow, additive in HDR (it blooms), hidden behind the scene.
float4 PS_Trail(TrailOut i) : SV_Target
{
    float2 uv = i.pos.xy * u_Screen.zw;
    clip(engineZ(uv) - i.data.z * 0.995);
    float x = i.data.x;
    float light = exp(-x * x * 10.0) * 2.5 + exp(-x * x * 2.5) * 0.4;
    light *= i.data.y * smoothstep(1.6, 2.6, i.data.w);
    // The colour is in TMNF's units (u_Misc.z converts, see sky.hlsli).
    return float4(g_trailColour.rgb * light * (u_Misc.z > 0.0 ? u_Misc.z : 1.0), 0);
}
