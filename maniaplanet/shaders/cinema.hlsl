// TM Vibrant Shaders for ManiaPlanet: depth of field, motion blur and temporal anti-aliasing on the
// HDR image before the tone map (shader model 5). Ported from the TMNF mod (shaders/tmvs.hlsl
// PS_Focus, PS_DofBlur, PS_Cinematic, PS_TAA).
//
// What differs from TMNF: ManiaPlanet hands over its motion vectors (TMapMotion, R16G16 float, t13).
// Its DeferredCameraMotion_p writes uv - previous uv of every pixel (in screen fractions, y down),
// and the cars write their own motion on top: the car the camera follows moves with it and stays
// sharp by itself. A capture while driving shows them pointing outward from the centre, the car at
// about 0. Without them the camera reprojection stands in, as in TMNF.
//
// Per frame (effects_cinema.cpp): [PS_Focus (1x1) -> PS_DofBlur x2] -> PS_Cinematic (DOF composite
// and motion blur) -> PS_TAA (two targets: the image, and the history with its view z).
// entries: PS_Focus:ps_5_0 PS_DofBlur:ps_5_0 PS_Cinematic:ps_5_0 PS_TAA:ps_5_0
#include "common.hlsli"

// Where this pixel was in the previous frame (uv). prevZ: its view z then (camera reprojection,
// for the history's depth check).
float2 previousUV(float2 uv, float z, out float prevZ)
{
    float2 cam = reproject(worldPosition(viewPosition(uv, min(z, 20000.0))), prevZ);
    float2 prev = cam;
    if (u_Engine.w > 0.5) prev = uv - t_motion.SampleLevel(g_point, uv, 0).xy;
    return prev;
}

// ---------------------------------------------------------------------------------
// Auto focus (1x1): what's in the middle of the frame, below the centre (the car or the road
// ahead), smoothed over time. ManiaPlanet's chase cameras show the car lower than TMNF's (its
// middle at about 0.7 of the height, TMNF 0.56): measured there. t0 = previous focus.
//   u_Pass0.x = blend, u_Pass0.y = reset, u_Pass0.zw = the point clicked in the photo mode (uv),
//   negative = the car / the road ahead. u_Pass1: DofAutofocus Car, the player's car (world, about
//   its middle), w = 1.
// ---------------------------------------------------------------------------------
float4 PS_Focus(VSOut i) : SV_Target
{
    float focus = u_Cine.z;
    if (focus <= 0.0)
    {
        float inv = 0.0, nearest = 3000.0;
        bool clicked = u_Pass0.z >= 0.0;
        float2 at = clicked ? u_Pass0.zw : float2(0.5, 0.68);
        float spread = clicked ? 0.006 : 0.05;
        [unroll] for (int j = -1; j <= 1; j++)
        {
            [unroll] for (int k = -1; k <= 1; k++)
            {
                float z = min(engineZ(saturate(at + float2(k, j) * spread)), 3000.0);
                inv += 1.0 / z;
                nearest = min(nearest, z);
            }
        }
        // A clicked point: the mean around it. Auto focus: the nearest thing in the window, the
        // car (the mean took the road behind it in too: the car came out blurred).
        focus = clicked ? 9.0 / inv : max(nearest, 1.5);
        // DofAutofocus Car (TMNF's): projected with this frame's camera; off screen or behind the
        // camera the middle above stays. The car's own pixels: a 5x5 grid over a metre each side
        // of its middle, only depths close to the car's distance count (the road beside it, the
        // sky, a pillar in front don't). None of them (all of it hidden): the car's distance
        // itself, so it is sharp when it comes out.
        if (!clicked && u_Pass1.w > 0.5)
        {
            float3 car = worldToViewDir(u_Pass1.xyz - cameraWorld());
            float2 carAt = projectToUV(car);
            if (car.z > 0.5 && all(abs(carAt - 0.5) < 0.47))
            {
                float2 step = float2(u_Proj.x, u_Proj.y) * 0.25 / car.z; // 0.5 m in uv
                float tolerance = max(2.5, 0.1 * car.z);
                float carInv = 0.0, count = 0.0;
                [unroll] for (int cj = -2; cj <= 2; cj++)
                {
                    [unroll] for (int ci = -2; ci <= 2; ci++)
                    {
                        float z = engineZ(saturate(carAt + float2(ci, cj) * step));
                        if (abs(z - car.z) < tolerance)
                        {
                            carInv += 1.0 / z;
                            count += 1.0;
                        }
                    }
                }
                focus = count > 0.0 ? count / carInv : car.z;
            }
        }
    }
    float previous = t0_tex(float2(0.5, 0.5)).r;
    if (u_Pass0.y > 0.5 || !(previous > 0.0 && previous < 1e5)) previous = focus;
    return float4(lerp(previous, focus, u_Pass0.x), 0, 0, 1);
}

// Signed blur radius in full-res pixels (negative = in front of the focus plane).
float circleOfConfusion(float z, float focus)
{
    float maxPx = u_Cine.w * u_Screen.y / 1080.0;
    return clamp((1.0 - focus / max(z, 0.1)) * u_Cine.y * 2.5, -1.0, 1.0) * maxPx;
}

// ---------------------------------------------------------------------------------
// Gathered bokeh. t0 = HDR (bilinear), t1 = half normal/depth (half-res pass), t2 = focus (1x1).
//   u_Pass0.x = samples (16, photo mode 64), u_Pass0.y = how much each pixel's spiral is turned
//   at random, u_Pass0.z = radius (1; 0.3 for the smoothing pass), u_Pass0.w = 1: full res (the
//   depth from the engine's depth buffer).
//   out.rgb = blurred colour, out.a = how much blur covers this pixel (0..1)
// ---------------------------------------------------------------------------------
float dofDepth(float2 uv)
{
    float z;
    if (u_Pass0.w > 0.5) z = engineZ(uv);
    else z = t1_tex(uv).w;
    return z;
}

float4 PS_DofBlur(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float focus = t2_tex(float2(0.5, 0.5)).r;
    float z = dofDepth(uv);
    float coc = abs(circleOfConfusion(z, focus));
    float maxPx = u_Cine.w * u_Screen.y / 1080.0;
    float reachPx = maxPx * u_Pass0.z;
    float3 sum = t0_texL(uv).rgb;
    float wsum = 1.0;
    float foreground = 0.0;
    float rot = ign(i.pos.xy) * 6.2832 * u_Pass0.y;
    int n = (int)u_Pass0.x;
    // Golden-angle spiral; the direction is turned step by step (no sin/cos per tap).
    float2 dir = float2(cos(rot), sin(rot));
    const float2x2 golden = float2x2(-0.737369, -0.675490, 0.675490, -0.737369);
    [loop] for (int k = 0; k < n; k++)
    {
        float r = sqrt((k + 0.5) / n) * reachPx;
        dir = mul(golden, dir);
        float2 suv = uv + dir * r * u_Screen.zw;
        float sz = dofDepth(suv);
        float scoc = abs(circleOfConfusion(sz, focus));
        // A sample counts if its blur disc reaches this pixel. Sharp foreground can't be pulled
        // over by blurry background.
        bool inFront = sz < z;
        float reach = inFront ? scoc : min(scoc, coc);
        float w = saturate(reach - r + 1.0);
        if (inFront) foreground = max(foreground, w * saturate(scoc / maxPx * 4.0));
        sum += t0_texL(suv).rgb * w;
        wsum += w;
    }
    return float4(sum / wsum, max(saturate(coc / maxPx * 4.0), foreground));
}

// ---------------------------------------------------------------------------------
// DOF composite and motion blur (full res). t0 = HDR (sharp), t1 = DOF blur.
//   u_Pass0.x = depth of field on, u_Pass0.y = motion blur (shutter: 1 = one frame of motion)
// ---------------------------------------------------------------------------------
float3 dofComposite(float2 uv)
{
    float3 sharp = t0_tex(uv).rgb;
    float3 c = sharp;
    if (u_Pass0.x > 0.5)
    {
        float4 blur = t1_texL(uv);
        c = lerp(sharp, blur.rgb, smoothstep(0.05, 0.35, blur.a));
    }
    return c;
}

float4 PS_Cinematic(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float3 c = dofComposite(uv);
    int debug = (int)(u_Post.w + 0.5);
    float z = engineZ(uv);
    if (debug == 40)
    {
        // Blur size: red in front of the focus, blue behind, black sharp.
        float coc = circleOfConfusion(z, t2_tex(float2(0.5, 0.5)).r) / max(u_Cine.w * u_Screen.y / 1080.0, 1e-3);
        return float4(saturate(-coc), 0, saturate(coc), 1);
    }
    if (debug == 41)
    {
        // Motion: 1/50 of the screen per frame = full colour.
        float prevZ;
        float2 v = (uv - previousUV(uv, z, prevZ)) * 50.0;
        return float4(saturate(v.x) + saturate(-v.y) * 0.5, saturate(-v.x) + saturate(-v.y) * 0.5, saturate(v.y), 1);
    }
    if (u_Pass0.y <= 0.0) return float4(c, 1);
    float prevZ;
    float2 vel = (uv - previousUV(uv, z, prevZ)) * u_Pass0.y;
    // The camera reprojection doesn't know the car moves with the camera: keep what's close sharp.
    if (u_Engine.w < 0.5) vel *= smoothstep(5.0, 14.0, z);
    float len = length(vel);
    if (len > 0.025) vel *= 0.025 / len;
    if (len * u_Screen.x < 0.75) return float4(c, 1);
    float jitter = ign(i.pos.xy) - 0.5;
    float3 sum = c;
    float wsum = 1.0;
    [loop] for (int k = 0; k < 10; k++)
    {
        float2 suv = saturate(uv + vel * ((k + 0.5 + jitter) / 10.0 - 0.5));
        float sz = engineZ(suv);
        // Don't smear the (sharp) car into the moving background.
        float w = (sz < z * 0.8 && sz < 14.0) ? 0.0 : 1.0;
        sum += dofComposite(suv) * w;
        wsum += w;
    }
    return float4(sum / wsum, 1);
}

// ---------------------------------------------------------------------------------
// Temporal anti-aliasing on the HDR image (full res). The projection isn't the mod's (no sub-pixel
// jitter): it calms flicker (thin lines, AO noise, specular sparkle), and camera motion adds
// sub-pixel samples on its own. Blended and clipped on compressed colour (c / (1 + luma)), so the
// sun and lamps don't decide the neighbourhood box (HDR fireflies would ghost).
//   t0 = current, t1 = history (rgb, a = view z; bilinear for the Catmull-Rom fetch)
//   u_Pass0.x = history valid
//   out0 = the image, out1 = the next history
// ---------------------------------------------------------------------------------
float3 toYCoCg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 fromYCoCg(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
float3 compressHdr(float3 c) { return c / (1.0 + luma(c)); }
float3 expandHdr(float3 c) { return c / max(1.0 - luma(c), 1e-4); }

// 5-tap Catmull-Rom history fetch (sharper than bilinear).
float3 historyCatmullRom(float2 uv)
{
    float2 pos = uv * u_Screen.xy;
    float2 p1 = floor(pos - 0.5) + 0.5;
    float2 f = pos - p1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 a = (p1 - 1.0) * u_Screen.zw;
    float2 b = (p1 + 2.0) * u_Screen.zw;
    float2 m = (p1 + w2 / w12) * u_Screen.zw;
    float3 r = t1_texL(float2(m.x, a.y)).rgb * (w12.x * w0.y);
    r += t1_texL(float2(a.x, m.y)).rgb * (w0.x * w12.y);
    r += t1_texL(float2(m.x, m.y)).rgb * (w12.x * w12.y);
    r += t1_texL(float2(b.x, m.y)).rgb * (w3.x * w12.y);
    r += t1_texL(float2(m.x, b.y)).rgb * (w12.x * w3.y);
    float w = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / w, 0.0);
}

struct TaaOut
{
    float4 colour : SV_Target0;
    float4 history : SV_Target1;
};

TaaOut PS_TAA(VSOut i)
{
    float2 uv = i.uv;
    float3 current = t0_tex(uv).rgb;
    float z = engineZ(uv);
    TaaOut o;
    o.colour = float4(current, 1);
    o.history = float4(current, z);
    if (u_Pass0.x < 0.5) return o;

    float prevZ;
    float2 prevUV = previousUV(uv, z, prevZ);
    if (any(prevUV < 0.0) || any(prevUV > 1.0)) return o;

    // Neighbourhood statistics of the current frame (variance clipping in YCoCg), 3x3.
    float3 m1 = 0, m2 = 0;
    [unroll] for (int y = -1; y <= 1; y++)
    {
        [unroll] for (int x = -1; x <= 1; x++)
        {
            float3 s = toYCoCg(compressHdr(t0_tex(uv + float2(x, y) * u_Screen.zw).rgb));
            m1 += s;
            m2 += s * s;
        }
    }
    m1 /= 9.0;
    m2 /= 9.0;
    float3 sigma = sqrt(max(m2 - m1 * m1, 0.0));
    // Standing still the history is trusted more; in motion the box gets tight against ghosting.
    float speed = length((uv - prevUV) * u_Screen.xy);
    float still = 1.0 - saturate(speed / 3.0);
    float gamma = lerp(1.25, 2.25, still);
    float3 boxMin = m1 - sigma * gamma, boxMax = m1 + sigma * gamma;

    float3 history = toYCoCg(compressHdr(historyCatmullRom(prevUV)));
    float3 centre = (boxMin + boxMax) * 0.5, extent = (boxMax - boxMin) * 0.5 + 1e-4;
    float3 v = history - centre;
    float3 a = abs(v / extent);
    float m = max(a.x, max(a.y, a.z));
    if (m > 1.0) history = centre + v / m;

    float weight = lerp(0.1, 0.06, still);
    // Disocclusion: the history must have seen this surface at the depth expected. Static
    // surfaces match the camera reprojection; what moves with the camera (the car, followed by
    // the motion vectors) keeps its depth instead.
    float historyZ = t1_tex(prevUV).a;
    if (z < SKY_Z)
    {
        float errorStatic = abs(historyZ - prevZ) / max(prevZ, 0.1);
        float errorMoving = u_Engine.w > 0.5 ? abs(historyZ - z) / max(z, 0.1) : errorStatic;
        weight = lerp(weight, 1.0, smoothstep(0.02, 0.08, min(errorStatic, errorMoving)));
    }
    else if (historyZ < SKY_Z * 0.5)
    {
        weight = 1.0; // sky where the history had geometry (the car's roof against the sky)
    }
    // Fast motion: favour the current frame (less smearing).
    weight = max(weight, saturate(speed / 60.0) * 0.4);
    float3 result = expandHdr(fromYCoCg(lerp(history, toYCoCg(compressHdr(current)), weight)));
    o.colour = float4(result, 1);
    o.history = float4(result, z);
    return o;
}
