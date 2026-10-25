// TM Vibrant Shaders for ManiaPlanet: clear water (WaterClarity), inside the engine's frame
// (src/water11.cpp), not with the effects' constants.
//
// The engine draws its water in two steps. First Tech3/DeferredWaterFog_p fogs what lies under the
// water, one draw per water volume, premultiplied over its HDR image (blend one, inverse source
// alpha) by the light's path under the surface: that fog is the murk. Then it copies the image for
// its refraction and draws the surface (Tech3/Sea_DefWrite_p: refraction, reflection, Fresnel).
// The mod draws the fog draws into a buffer of its own (rgb the fog, premultiplied, a its cover)
// and puts it into the image here, before the engine's copy for the refraction: with less murk,
// red taken first so deep water turns blue-green, caustics on the bottom and foam where the
// bottom comes up to the surface. The engine's surface then refracts and reflects that as before.
// With clarity 0 the result is the engine's own fog: rgb + image * (1 - a).
//
// entries: VS_WaterFull:vs_5_0 PS_WaterCompose:ps_5_0

// b0: a copy of the fog draw's ShaderP (SCBufferP_DeferredWaterFog): its pixel -> world rays.
cbuffer EngineFogP : register(b0)
{
    float4 e_LeftTop;   // EyeToFarPlaneLeftTop, w: Depth01_Scale_Z01
    float4 e_LeftRight; // FarPlaneLeftRight, w: Depth01_Trans_Z01
    float4 e_TopBottom; // FarPlaneTopBottom
};

cbuffer ComposeParams : register(b2)
{
    float4 c_Water;  // clarity (0..1), time (s, wrapped), planes known (0..8), detail (caustics and foam, 0..1)
};

// b3: a copy of the fog draw's SceneP (the engine's per-camera pixel constants).
cbuffer EngineSceneP : register(b3)
{
    float4 s_Fog;             // 0
    float4 s_WorldToCamera[3]; // 16: world -> camera, one register per camera axis (xyz, translation)
    float4 s_EyeInWorld;      // 64
    float4 s_CameraPrRangeW;  // 80
    float4 s_RasterPos;       // 96: pixel -> 0..1
    float4 s_FaceSignAlpha;   // 112
    float4 s_LightAmbient;    // 128: ambient (linear)
    float4 s_LightRgb;        // 144: the sun's (or the moon's) colour as the engine lights with it
    float4 s_LightDir;        // 160: its direction (from the light into the scene), world
};

// b4-b11: each fog draw's ObjectP (WorldToWaterDepth: depth under its surface = dot(world, 1)), copied.
cbuffer WaterPlane0 : register(b4) { float4 g_plane0; };
cbuffer WaterPlane1 : register(b5) { float4 g_plane1; };
cbuffer WaterPlane2 : register(b6) { float4 g_plane2; };
cbuffer WaterPlane3 : register(b7) { float4 g_plane3; };
cbuffer WaterPlane4 : register(b8) { float4 g_plane4; };
cbuffer WaterPlane5 : register(b9) { float4 g_plane5; };
cbuffer WaterPlane6 : register(b10) { float4 g_plane6; };
cbuffer WaterPlane7 : register(b11) { float4 g_plane7; };

Texture2D g_fog : register(t0);          // the fog draws (rgb premultiplied, a cover)
Texture2D<float> g_depth : register(t1); // the fog draw's TMapDepth (linear: Depth01 scale and offset)
Texture2D g_albedo : register(t2);       // TMapDeferredMDiffuse (squared: the material colour)
Texture2D g_normal : register(t3);       // TMapDeferredPixelNormalInC (camera space)
Texture2D g_mask : register(t4);         // TMapDeferredLightMask (x: the sun's shadow, w: ambient)
Texture2D g_pictures : register(t6);     // the water pictures (src/waterfx.cpp: g foam)
SamplerState g_wrap : register(s0);

struct ComposeOut
{
    float4 add : SV_Target0;  // added
    float4 keep : SV_Target1; // the image times this (dual-source blending)
};

float4 VS_WaterFull(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// The TMNF mod's caustics (shaders/tmvs.hlsl causticLines): wavy bright lines, slowly moving.
float causticLines(float2 p, float t)
{
    float2 q = p;
    float v = 0.0;
    [unroll] for (int i = 0; i < 3; i++)
    {
        q += 0.55 * float2(sin(q.y * 1.7 + t * 0.8 + i * 1.9), cos(q.x * 1.4 - t * 0.7 + i * 2.7));
        v += 1.0 - abs(sin(q.x * 1.3 + q.y * 0.9 + i));
    }
    return pow(saturate(v * 0.3333), 3.0);
}

ComposeOut PS_WaterCompose(float4 pos : SV_Position)
{
    int3 px = int3(pos.xy, 0);
    float4 fog = g_fog.Load(px);
    // No water in front of this point (the fog draws cover only what lies under a surface; the
    // variants with caustics add their light even where the fog is still nothing).
    if (fog.a <= 1e-4 && max(fog.r, max(fog.g, fog.b)) <= 1e-6) discard;
    // Nothing that isn't a number goes into the image (the exposure and bloom average over all of it).
    if (any(!isfinite(fog))) discard;
    float clarity = c_Water.x;
    float a = clamp(fog.a, 0.0, 0.999);
    // The engine's murk as an optical depth (Beer-Lambert: a = 1 - exp(-tau)), clarity takes it down
    // (at 1 the same murk needs eight times the water); red goes first, blue last.
    float tau = -log(1.0 - a);
    float tauC = tau * lerp(1.0, 0.12, clarity);
    float3 through = exp(-tauC * lerp(float3(1.0, 1.0, 1.0), float3(1.9, 0.85, 0.6), clarity));
    // What the water scatters towards the camera: the engine's fog colour (rgb / a) for the part of
    // the murk that stays (where it is still nothing: its share). At clarity 0: exactly the engine's rgb.
    float stays = a > 1e-4 ? (1.0 - exp(-tauC)) / a : lerp(1.0, 0.12, clarity);
    float3 inscatter = fog.rgb * stays;

    float3 extra = 0.0;
    float foam = 0.0;
    float detail = clarity * c_Water.w;
    [branch] if (detail > 0.0 && c_Water.z > 0.5)
    {
        // The point under the water (the engine's own reconstruction: eye + ray * Depth01).
        float2 r = pos.xy * s_RasterPos.xy;
        float3 ray = e_LeftTop.xyz + r.x * e_LeftRight.xyz + r.y * e_TopBottom.xyz;
        float d01 = e_LeftTop.w * g_depth.Load(px) + e_LeftRight.w;
        float3 world = s_EyeInWorld.xyz + ray * d01;
        float dist = length(ray * d01);
        // How deep it lies: under the nearest surface above it (the planes of this frame's fog draws).
        float4 planes[8] = {g_plane0, g_plane1, g_plane2, g_plane3, g_plane4, g_plane5, g_plane6, g_plane7};
        float below = 1e9;
        [unroll] for (int i = 0; i < 8; i++)
        {
            float d = dot(float4(world, 1.0), planes[i]);
            // Only a plane like the engine's (a unit normal: depth in metres); any other is skipped.
            float n2 = dot(planes[i].xyz, planes[i].xyz);
            bool plane = n2 > 0.81 && n2 < 1.21 && all(isfinite(planes[i]));
            if (i < (int)c_Water.z && plane && d > -0.05 && d < below) below = d;
        }
        [branch] if (below < 1e8)
        {
            below = max(below, 0.0);
            // A pixel's size on the bottom (m): the caustics fade out once their lines are under about
            // 3 pixels (TMNF: finer detail aliased into streaks along the view from 20 m on), and far away.
            float footprint = d01 * length(e_TopBottom.xyz) * s_RasterPos.y;
            float fine = saturate(1.0 - (footprint - 0.04) / 0.12) * (1.0 - saturate((dist - 150.0) / 250.0));
            float t = c_Water.y;
            float4 mask = g_mask.Load(px);
            [branch] if (fine > 0.0)
            {
                // Caustics: the sun's light on the bottom (as the engine lit it: 2 albedo colour N.L
                // shadow), gathered into lines, less of them deeper down.
                float3 albedo = g_albedo.Load(px).rgb;
                albedo *= albedo;
                float3 n = g_normal.Load(px).xyz * 2.0 - 1.0;
                n *= rsqrt(max(dot(n, n), 1e-8));
                float3 toSun = -normalize(s_LightDir.xyz + 1e-6);
                float3 sunView = float3(dot(s_WorldToCamera[0].xyz, toSun), dot(s_WorldToCamera[1].xyz, toSun), dot(s_WorldToCamera[2].xyz, toSun));
                float3 sunLit = 2.0 * albedo * s_LightRgb.rgb * saturate(dot(n, sunView)) * mask.x;
                float c = causticLines(world.xz * 0.9, t);
                extra = sunLit * c * 1.6 * exp(-0.18 * below) * smoothstep(0.03, 0.3, below) * fine * detail;
            }
            // Foam where the bottom comes up to the surface: a lacy band along the shore and the walls.
            float lace = g_pictures.SampleLevel(g_wrap, frac(world.xz * 0.45 + float2(t * 0.011, -t * 0.007)), 0).g;
            float shore = smoothstep(0.45, 0.03, below);
            foam = saturate(shore * (0.35 + 0.8 * lace) - 0.12) * detail * (1.0 - saturate((dist - 200.0) / 200.0));
            float3 toSunW = -normalize(s_LightDir.xyz + 1e-6);
            float3 foamLight = 2.0 * 0.8 * (s_LightRgb.rgb * saturate(toSunW.y) * mask.x + s_LightAmbient.rgb * max(mask.w, 0.3));
            inscatter = lerp(inscatter, foamLight, foam);
        }
    }

    ComposeOut o;
    o.add = float4(inscatter + extra * through * (1.0 - foam), 0.0);
    o.keep = float4(through * (1.0 - foam), 1.0);
    // Should anything above give no number: the engine's own fog for this pixel.
    if (any(!isfinite(o.add.rgb)) || any(!isfinite(o.keep.rgb)))
    {
        o.add = float4(max(fog.rgb, 0.0), 0.0);
        o.keep = float4((1.0 - a).xxx, 1.0);
    }
    return o;
}
