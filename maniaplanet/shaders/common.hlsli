// TM Vibrant Shaders for ManiaPlanet: what every effect pass shares (shader model 5).
//
// The constants keep the TMNF mod's names and meanings (shaders/tmvs.hlsl, c0-c44), so its
// effects carry over almost line for line. effects.cpp fills them from the settings and the
// engine's camera. What differs from TMNF:
//   - The engine's depth is reversed and infinite: depth = near / z (sky = 0). linearDepth()
//     turns it into view z like in TMNF (SKY_Z for the sky).
//   - D3D11 has no half-pixel offset: uv and ndc map straight onto each other.
//   - View space is the engine's camera space (WorldToCamera): z forward, x mirrored (P00 < 0).
//     The engine's G-buffer normals are in it, so they need no conversion.
//   - The engine's HDR image is not exposed yet: the passes work on hdr * engineExposure(), the
//     same "display linear" units as TMNF's linear colour.
//
// Slots:
//   b0 Frame (u_*), b1 PassParams (u_Pass0, u_Pass1), b2 the engine's tone-map constants (exposure uv)
//   t0-t7 the pass's own inputs
//   t8 depth, t9 normals, t10 albedo, t11 specular, t12 light mask, t13 motion, t14 HDR,
//   t15 exposure (the engine's, bound once per frame; any may be missing: see u_Engine)
//   s0 linear clamp, s1 point clamp, s2 linear wrap, s3 point wrap

cbuffer Frame : register(b0)
{
    float4 u_Screen;    // c0: w, h, 1/w, 1/h (full resolution)
    float4 u_Proj;      // c1: P00, P11, P20, P21
    float4 u_Proj2;     // c2: P22, P32 (depth = P22 + P32 / z), time (s), frame index
    float4 u_ViewToW0;  // c3: camera x axis in the world, camera world x
    float4 u_ViewToW1;  // c4: camera y axis, camera world y
    float4 u_ViewToW2;  // c5: camera z axis (forward), camera world z
    float4 u_SunView;   // c6: towards the sun (view space), sun known
    float4 u_SunWorld;  // c7: towards the sun (world space), daylight factor
    float4 u_SunScreen; // c8: sun uv, on-screen fade, in front of camera
    float4 u_SunColor;  // c9: rgb, sun light strength (SunLight)
    float4 u_SkyColor;  // c10: rgb, ambient tint strength
    float4 u_Light;     // c11: AO strength, AO radius (m), shadow strength, shadow length (m)
    float4 u_Atmo;      // c12: fog density, height falloff, sun scatter, sky enhance
    float4 u_Rays;      // c13: god rays, decay, sun glow, highlight boost
    float4 u_Bloom;     // c14: bloom, radius, lens flare, chromatic aberration
    float4 u_Grade0;    // c15: exposure mul, auto exposure, contrast, saturation
    float4 u_Grade1;    // c16: vibrance, temperature, tint, split toning
    float4 u_Grade2;    // c17: lift, gamma, gain, vignette
    float4 u_Post;      // c18: film grain, sharpen, fxaa, debug view
    float4 u_UpView;    // c19: world up in view space, camera height
    float4 u_Sky;       // c20: sky mode, night amount, sky rotation (rad), sky brightness
    float4 u_Sky2;      // c21: cloud amount, star amount, effect size, planet size
    float4 u_Quality;   // c22: AO samples, shadow steps, cloud steps, long shadow steps
    float4 u_Weather;   // c23: wetness, rain, puddles, water surfaces
    float4 u_Cine;      // c24: motion blur, depth of field, focus distance (0 = auto), max blur (px at 1080p)
    float4 u_Nature;    // c25: grass detail, mowing stripes, wind, dry reflections
    float4 u_Clouds;    // c26: volumetric clouds, coverage, base height (m), thickness (m)
    float4 u_Planet;    // c27: planet type, view, azimuth, elevation (rad)
    float4 u_PrevView0; // c28: world -> previous frame's view space, row 0 (rotation, translation)
    float4 u_PrevView1; // c29: row 1
    float4 u_PrevView2; // c30: row 2
    float4 u_PrevProj;  // c31: previous frame's P00, P11, P20, P21
    float4 u_Engine;    // c32: engine textures present: normals, albedo, light mask, motion
    float4 u_Engine2;   // c33: engine sun known (1; -1 = night map, the moon), specular present, ambient strength, 0
    float4 u_Temporal;  // c34: TAA on, history valid, noise frame (0..63), 0
    float4 u_Ambient;   // c35: the engine's ambient light (rgb, linear), 0
    float4 u_Light2;    // c36: neon light, game sun (1 known, -1 night map), lightning flash, lightning bolt
    float4 u_Volume;    // c37: volumetric light strength, march range (m), global illumination, lens drops
    float4 u_Water;     // c38: water heights (world y): low, high; z: kind (0 none known); w: reach (photo mode 3)
    float4 u_Snow;      // c39: snowfall, snow cover, wind direction (world x, z)
    float4 u_BlackHole; // c40: black hole direction (world, turns with the sky), size (0 = none)
    float4 u_Sky3;      // c41: aurora speed, snow flakes on the lens, reflection blur, 1 / haze distance
    float4 u_Lut;       // c42: (unused here: the look runs in hdr.hlsl)
    float4 u_SunOut;    // c43: how much of the engine's sun comes out (night skies), engine sun direction (world)
    float4 u_Mat;       // c44: 0, real night (GameNight), a night sky replaces the game's, tone knee
    float4 u_EngineSun; // c45: the engine's sun colour (rgb, linear, as it lights with it), 1 = known
    float4 u_Fog;       // c46: the engine's fog colour (rgb, linear), 0
    float4 u_Misc;      // c47: engine exposure is applied in the input (1) or read here (0), photo mode,
                        //      TMNF colours -> display linear here (sky.hlsli tmnfScale), 0
};

cbuffer PassParams : register(b1)
{
    float4 u_Pass0;
    float4 u_Pass1;
};

// The engine's own tone-map constants (ToneMapGlobalFilmCurve): where this frame's exposure is.
cbuffer EngineTone : register(b2)
{
    float4 g_eA;
    float4 g_eD;
    float4 g_eUV;
};

// The pass's own inputs: tN_tex(uv) reads point-sampled, tN_texL(uv) bilinear (clamped).
Texture2D g_in0 : register(t0);
Texture2D g_in1 : register(t1);
Texture2D g_in2 : register(t2);
Texture2D g_in3 : register(t3);
Texture2D g_in4 : register(t4);
Texture2D g_in5 : register(t5);
Texture2D g_in6 : register(t6);
Texture2D g_in7 : register(t7);
#define t0_tex(uv) g_in0.SampleLevel(g_point, (uv), 0)
#define t1_tex(uv) g_in1.SampleLevel(g_point, (uv), 0)
#define t2_tex(uv) g_in2.SampleLevel(g_point, (uv), 0)
#define t3_tex(uv) g_in3.SampleLevel(g_point, (uv), 0)
#define t4_tex(uv) g_in4.SampleLevel(g_point, (uv), 0)
#define t5_tex(uv) g_in5.SampleLevel(g_point, (uv), 0)
#define t6_tex(uv) g_in6.SampleLevel(g_point, (uv), 0)
#define t7_tex(uv) g_in7.SampleLevel(g_point, (uv), 0)
#define t0_texL(uv) g_in0.SampleLevel(g_linear, (uv), 0)
#define t1_texL(uv) g_in1.SampleLevel(g_linear, (uv), 0)
#define t2_texL(uv) g_in2.SampleLevel(g_linear, (uv), 0)
#define t3_texL(uv) g_in3.SampleLevel(g_linear, (uv), 0)
#define t4_texL(uv) g_in4.SampleLevel(g_linear, (uv), 0)
#define t5_texL(uv) g_in5.SampleLevel(g_linear, (uv), 0)
#define t6_texL(uv) g_in6.SampleLevel(g_linear, (uv), 0)
#define t7_texL(uv) g_in7.SampleLevel(g_linear, (uv), 0)

Texture2D<float> t_depth : register(t8);
Texture2D t_normal : register(t9);
Texture2D t_albedo : register(t10);
Texture2D t_specular : register(t11);
Texture2D t_lightMask : register(t12);
Texture2D t_motion : register(t13);
Texture2D t_hdr : register(t14);
Texture2D t_exposure : register(t15);

// Volumetric light (half res: lit fraction, lit path / range), read by the composite; u_Pass1.x = present.
Texture2D g_volume : register(t16);

SamplerState g_linear : register(s0);
SamplerState g_point : register(s1);
SamplerState g_linearWrap : register(s2);
SamplerState g_pointWrap : register(s3);

static const float PI = 3.14159265;
static const float SKY_Z = 60000.0;
static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// ---------------------------------------------------------------------------------
// Helpers (as in TMNF)
// ---------------------------------------------------------------------------------
float3 toLinear(float3 c)
{
    c = max(c, 0);
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 toSRGB(float3 c)
{
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float luma(float3 c) { return dot(c, LUMA); }

// Reversed infinite depth -> view z (SKY_Z where nothing was drawn).
float linearDepth(float d)
{
    if (d <= 1e-9) return SKY_Z;
    return min(u_Proj2.y / max(d - u_Proj2.x, 1e-12), SKY_Z);
}

float2 uvToNdc(float2 uv) { return float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0); }
float2 ndcToUV(float2 ndc) { return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5); }

float3 viewPosition(float2 uv, float z)
{
    float2 ndc = uvToNdc(uv);
    return float3((ndc.x - u_Proj.z) * z / u_Proj.x, (ndc.y - u_Proj.w) * z / u_Proj.y, z);
}

float2 projectToUV(float3 p)
{
    return ndcToUV(float2(p.x * u_Proj.x / p.z + u_Proj.z, p.y * u_Proj.y / p.z + u_Proj.w));
}

float3 viewToWorldDir(float3 v) { return v.x * u_ViewToW0.xyz + v.y * u_ViewToW1.xyz + v.z * u_ViewToW2.xyz; }
float3 worldToViewDir(float3 w) { return float3(dot(w, u_ViewToW0.xyz), dot(w, u_ViewToW1.xyz), dot(w, u_ViewToW2.xyz)); }
float3 cameraWorld() { return float3(u_ViewToW0.w, u_ViewToW1.w, u_ViewToW2.w); }
float3 worldPosition(float3 viewPos) { return cameraWorld() + viewToWorldDir(viewPos); }

// Where a (static) world point was on screen in the previous frame. prevZ = its depth then.
float2 reproject(float3 world, out float prevZ)
{
    float4 w = float4(world, 1.0);
    float3 v = float3(dot(w, u_PrevView0), dot(w, u_PrevView1), dot(w, u_PrevView2));
    prevZ = v.z;
    return ndcToUV(float2(v.x * u_PrevProj.x / v.z + u_PrevProj.z, v.y * u_PrevProj.y / v.z + u_PrevProj.w));
}

float ign(float2 pixel)
{
    pixel += u_Temporal.z * 5.588238;
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float henyeyGreenstein(float cosTheta, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(abs(1.0 + g2 - 2.0 * g * cosTheta), 1.5));
}

float3 sunScatter(float3 rd, float strength)
{
    float mu = dot(rd, u_SunWorld.xyz);
    float mie = henyeyGreenstein(mu, 0.7) * 0.3 + henyeyGreenstein(mu, 0.25) * 0.4;
    return u_SunColor.rgb * mie * strength;
}

// ---------------------------------------------------------------------------------
// The engine's frame
// ---------------------------------------------------------------------------------
float engineExposure()
{
    return t_exposure.SampleLevel(g_point, g_eUV.xy, 0).z;
}

// The engine's HDR image in display-linear units (its exposure applied).
float3 engineColour(float2 uv)
{
    float3 c = min(t_hdr.SampleLevel(g_point, uv, 0).rgb, 50000.0);
    return u_Misc.x > 0.5 ? c : c * engineExposure();
}

float3 engineColourLinear(float2 uv)
{
    float3 c = min(t_hdr.SampleLevel(g_linear, uv, 0).rgb, 50000.0);
    return u_Misc.x > 0.5 ? c : c * engineExposure();
}

float engineRawDepth(float2 uv) { return t_depth.SampleLevel(g_point, uv, 0); }
float engineZ(float2 uv) { return linearDepth(engineRawDepth(uv)); }

// The engine's normal (view space, unit length). Without the normal buffer: from the depth.
float3 engineNormal(float2 uv)
{
    float3 n = t_normal.SampleLevel(g_point, uv, 0).xyz * 2.0 - 1.0;
    return n * rsqrt(max(dot(n, n), 1e-8));
}

// Material colour (the engine decodes its MDiffuse with a square: gamma 2).
float3 engineAlbedo(float2 uv)
{
    float3 a = t_albedo.SampleLevel(g_point, uv, 0).rgb;
    return a * a;
}

// The engine's own light (Block_DefReadP1_p, `ld r0.zw, t1.yzxw`): sun = colour * N.L * mask.x
// (its shadow map), ambient = mask.w * the ambient colour, both times the albedo and 2. (Its
// ambient comes from the vertex shader, from the light map on most blocks: the SceneV ambient
// stands in for it, the rest goes to "rest".) Returns (sun diffuse, ambient diffuse) in
// display-linear units, without the cloud shadows.
// The engine's sun (u_SunOut.yzw), not the effects' light (a black hole sky moves that).
void engineLight(float2 uv, float3 nView, float3 albedo, out float3 sunPart, out float3 ambientPart, out float sunShadow)
{
    float4 mask = t_lightMask.SampleLevel(g_point, uv, 0);
    float e = u_Misc.x > 0.5 ? 1.0 : engineExposure();
    float ndl = saturate(dot(nView, worldToViewDir(u_SunOut.yzw)));
    sunShadow = mask.x;
    sunPart = 2.0 * e * albedo * u_EngineSun.rgb * (ndl * mask.x) * u_EngineSun.w;
    ambientPart = 2.0 * e * albedo * u_Ambient.rgb * mask.w;
}
