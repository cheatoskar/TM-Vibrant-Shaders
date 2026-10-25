// TM Vibrant Shaders for ManiaPlanet: the car's headlights (CarHeadlights, shader model 5), the TMNF
// mod's PS_Headlights (shaders/tmvs.hlsl) on the engine's G-buffer. Two spotlights on the front of
// the car the camera follows (its pose from the engine's constants, effects_headlights.cpp), added
// to the HDR image right after the lighting composite (blend one, one), before depth of field,
// motion blur and TAA:
//   - the surfaces: each lamp lights what is in its cone, by distance and the angle of the surface
//     to it, with the engine's albedo and normals, and its glint with the engine's own specular
//     model (Block_DefReadP1_p: (N.H)^n x n x MSpecular.x^2 x SpecScale, n = MSpecular.w x
//     SpecMaxExp + 1); behind the haze as the composite's surfaces are
//   - the haze: 16 samples along the view ray, each in the lamps' cones, scattered toward the eye
//     (the beams show in thick fog and as a faint cone in clear air)
//   - the glare: looking into a lamp (the camera in front of the car) a core and a halo, covered
//     where something nearer hides the lamp
// No shadows: the light goes through walls (as in TMNF). The lamps' cone points ahead and a little
// down, so the car's own body behind them stays dark.
//   b4: h_Head0 (strength, cos of the cone's edge, cos of its bright core, range m), h_Head1 (left
//       lamp, world; w the glare), h_Head2 (right lamp; w the beam in the air), h_Head3 (the beams'
//       axis, world), h_Spec (the engine's SpecMaxExp, SpecScale x its light's w (0: unknown, no
//       glint), 0, 0)
// entries: PS_Headlights:ps_5_0
#include "common.hlsli"

cbuffer HeadlightParams : register(b4)
{
    float4 h_Head0;
    float4 h_Head1;
    float4 h_Head2;
    float4 h_Head3;
    float4 h_Spec;
};

// TMNF's: the light on surfaces and in the air, in TMNF's units (u_Misc.z turns them into ours).
static const float HEAD_SURFACE = 1.2;
static const float HEAD_BEAM = 90.0;
static const float3 HEAD_COLOUR = float3(1.0, 0.93, 0.8);

// Inverse square past a fifth of the range (so Headlight range moves how far it carries: at 90 m
// a wall 50 m out still gets about an eighth of the light near the car), a smooth end at the range.
float headSpot(float3 q, float3 lamp)
{
    float3 v = q - lamp;
    float d2 = dot(v, v);
    float cone = smoothstep(h_Head0.y, h_Head0.z, dot(v, h_Head3.xyz) * rsqrt(max(d2, 1e-4)));
    float r2 = d2 / (h_Head0.w * h_Head0.w);
    float reach = saturate(1.0 - r2 * r2);
    return cone * reach * reach / (1.0 + r2 * 24.0);
}

// Strong only in front of the beam, near its axis; a point of light far away: its angle shrinks
// with the distance (a lamp 1 m wide at 10 m), its brightness falls and is mostly gone past 60-100 m.
float headGlare(float3 rd, float3 cam, float3 lamp)
{
    float3 w = lamp - cam;
    float dl = max(length(w), 1e-3);
    float3 wd = w / dl;
    float3 lv = worldToViewDir(w);
    float2 luv = projectToUV(float3(lv.xy, max(lv.z, 1e-3)));
    // Hidden behind something nearer (the car's own nose is within a metre of the lamp).
    float seen = 0.0;
    if (lv.z > 0.1)
        seen = all(luv > 0.0) && all(luv < 1.0) ? saturate(engineZ(luv) - lv.z + 1.0) : 1.0;
    float onAxis = saturate(dot(h_Head3.xyz, -wd));
    float facing = smoothstep(h_Head0.y, 1.0, onAxis);
    facing *= facing;
    float k = max(dl * 0.1, 1.0);
    float x = 1.0 - saturate(dot(rd, wd));
    float fade = lerp(1.0, 0.08, smoothstep(40.0, 100.0, dl)) / (1.0 + dl * dl * 0.0004);
    float glare = exp(-x * 12000.0 * k * k) * 4.0 + 2.0 / pow(1.0 + x * 800.0 * k * k, 1.5);
    return min(glare, 8.0) * facing * seen * fade;
}

// The composite's haze over a surface `dist` away (scene.hlsl lightingPass): its surfaces are
// lerp(lit, fog, this), so light added on them is seen through 1 - this of it.
float hazeOver(float3 rd, float dist)
{
    float density = (u_Atmo.x * 0.0009 + u_Snow.x * 0.005) * u_Sky3.w;
    float falloff = max(u_Atmo.y * 0.012, 1e-4);
    float camY = u_UpView.w;
    float rdy = rd.y;
    float amount = abs(rdy) > 1e-4 ? density * exp(-camY * falloff) * (1.0 - exp(-dist * rdy * falloff)) / (rdy * falloff)
                                   : density * exp(-camY * falloff) * dist;
    return saturate(1.0 - exp(-max(amount, 0.0)));
}

float4 PS_Headlights(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float z = engineZ(uv);
    float3 rdView = normalize(viewPosition(uv, 1.0));
    float3 rd = viewToWorldDir(rdView);
    float3 cam = cameraWorld();
    bool sky = z >= SKY_Z;
    float dist = sky ? 1e5 : z / max(rdView.z, 1e-3);
    float3 lampL = h_Head1.xyz, lampR = h_Head2.xyz;
    float scale = u_Misc.z > 0.0 ? u_Misc.z : 1.0;
    float3 add = 0;
    [branch] if (!sky)
    {
        float3 world = cam + rd * dist;
        // The engine's normal, facing the camera (without its normal buffer: toward the camera).
        float3 nView = -rdView;
        if (u_Engine.x > 0.5)
        {
            nView = engineNormal(uv);
            if (dot(nView, rdView) > 0.0) nView = -nView;
        }
        float3 n = viewToWorldDir(nView);
        // The engine's material colour; a floor so black paint still shows the light a little.
        float3 albedo = (u_Engine.y > 0.5 ? engineAlbedo(uv) : min(engineColour(uv), 1.0)) * 0.9 + 0.03;
        float spotL = headSpot(world, lampL), spotR = headSpot(world, lampR);
        // The lamps sit low: a road is lit at a grazing angle, and in a game it should not be black.
        // The angle is taken from lamps 3 m higher (as TMNF).
        float3 raise = float3(0.0, 3.0, 0.0);
        float3 toL = normalize(lampL + raise - world), toR = normalize(lampR + raise - world);
        float3 surface = albedo * (spotL * (saturate(dot(n, toL)) * 0.7 + 0.3) + spotR * (saturate(dot(n, toR)) * 0.7 + 0.3));
        // The glint (wet and polished roads, paint, glass), with the lamps' real direction.
        [branch] if (h_Spec.y > 0.0 && u_Engine2.y > 0.5 && spotL + spotR > 1e-5)
        {
            float4 spec = t_specular.SampleLevel(g_point, uv, 0);
            float gloss = spec.w * h_Spec.x + 1.0;
            float amount = spec.x * spec.x * h_Spec.y;
            float3 toEye = -rd;
            float3 dL = normalize(lampL - world), dR = normalize(lampR - world);
            float gL = saturate(dot(n, dL)) * pow(max(saturate(dot(n, normalize(toEye + dL))), 1e-6), gloss);
            float gR = saturate(dot(n, dR)) * pow(max(saturate(dot(n, normalize(toEye + dR))), 1e-6), gloss);
            surface += min((spotL * gL + spotR * gR) * gloss * amount, 3.0);
        }
        add = surface * HEAD_SURFACE * HEAD_COLOUR * (h_Head0.x * scale) * (1.0 - hazeOver(rd, dist));
    }
    // The beams: the haze in the cones, forward-scattering toward the lamps.
    float reach = min(dist, h_Head0.w * 1.2);
    float noise = ign(i.pos.xy);
    float scatter = 0.0;
    [loop] for (int k = 0; k < 16; k++)
    {
        float f = (k + noise) / 16.0;
        float t = f * f * reach;
        float3 q = cam + rd * t;
        scatter += (headSpot(q, lampL) + headSpot(q, lampR)) * (f * 2.0 * reach / 16.0);
    }
    float density = (u_Atmo.x * 0.0009 + 0.0008) * u_Sky3.w;
    float phase = henyeyGreenstein(dot(h_Head3.xyz, -rd), 0.7) * 0.9 + 0.02;
    float beam = scatter * density * phase * HEAD_BEAM * h_Head2.w * h_Head0.x;
    // Seen from in front, from inside the cones (a replay camera ahead of the car), the beams
    // came out 50 times as bright as from behind and the whole picture went white: a soft limit
    // at 3 (from behind, about 1 in thick fog, they lose a few per cent).
    beam /= sqrt(1.0 + beam * beam / 9.0);
    add += beam * HEAD_COLOUR * scale;
    add += (headGlare(rd, cam, lampL) + headGlare(rd, cam, lampR)) * float3(1.0, 0.92, 0.8) * (h_Head1.w * h_Head0.x * scale);
    return float4(add, 0);
}
