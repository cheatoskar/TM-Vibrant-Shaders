// TM Vibrant Shaders for ManiaPlanet: soft sun shadows (PCSS) in place of the engine's sun shadow
// pass, Tech3/DeferredShadowPssm_p. src/softshadow.cpp swaps it in at that draw, with all of the
// engine's state left as it is: same inputs, same target (the light mask, f90), same stencil.
//
// The engine's pass (disassembled from its GpuCache, variants with 1-5 splits picked by their
// texture rectangles; the ones that pick by view depth stay the engine's):
//   P = camera-space position from its linear depth (t0) and RasterToCamera (c0-c2)
//   static maps t1, t2 (4096^2, ~1 m texels): matrices c5-c7 and c8-c10, blended by c3.x
//   splits: one 6400 x 1280 atlas t3 (five 1280^2 tiles), matrices from c11 on (3 rows each),
//     each with its rectangle (SplitTcMins, SplitTcMaxs); the last (finest) split that holds
//     the pixel is used, no blend between splits
//   one bilinear comparison per map (s0, s1: ref >= stored is lit, larger = nearer the light);
//     at grazing views (N.V < 0.5) up to 8 taps along the pixel's footprint on the surface
//   no bias in the shader: the maps are drawn with a depth bias
//   out = lit^2 into all four channels (only red reaches the light mask)
// Optional maps per variant: PSplitOpacity (scales the split's light), StaticSprite (bicubic,
// scales it too). Their slots move the face normal (VNormalInC): t4, t5, t6 or t7.
//
// The replacement keeps all of that and only changes how the split atlas is read:
//   1. blocker search (gathers of the raw depths) in the finest split that holds the search disc
//      at no more than u_soft1.y texels;
//   2. penumbra radius = blocker distance (m, along the light) * tan(half the light's angle);
//   3. filter (gathers, bilinear weights) in the finest split that holds that disc at no more
//      than u_soft1.z texels: sharp at contact in the fine split, wide penumbras in coarser ones;
//   4. every texel is compared with the receiver's own plane there (from the face normal), so a
//      wide filter doesn't shadow the surface it lies on (no acne, no extra bias: no peter-panning);
//   5. no blocker found: the smallest filter in the engine's split (thin casters stay sharp).
// The static maps (beyond the splits) and the opacity and sprite maps are read as the engine does.

cbuffer ShaderP : register(b0)
{
    float4 c[36]; // the engine's ShaderP (its size depends on the split count)
};
cbuffer SceneP : register(b13)
{
    float4 scene[7]; // [6] = GbxP_RasterPos_SxyTwz_TexCoord
};
cbuffer SoftShadowP : register(b5)
{
    float4 u_soft0; // tan(half the light's angle), longest blocker distance (m), splits, mode (0 soft, 1 the engine's, 2 debug)
    float4 u_soft1; // smallest filter radius, largest search radius, largest filter radius (texels), depth epsilon
};

Texture2D<float> t_depth : register(t0);
Texture2D<float> t_static0 : register(t1);
Texture2D<float> t_static1 : register(t2);
Texture2D<float> t_splits : register(t3);
SamplerComparisonState s_static : register(s0);
SamplerComparisonState s_splits : register(s1);
SamplerState s_point : register(s5); // the mod's: point, clamp

#if SOFT_SPRITE
Texture2D<float> t_sprite0 : register(t4);
Texture2D<float> t_sprite1 : register(t5);
SamplerComparisonState s_sprite : register(s2);
#if SOFT_OPACITY
Texture2D t_opacity : register(t6);
SamplerState s_opacity : register(s3);
Texture2D t_normal : register(t7);
#else
Texture2D t_normal : register(t6);
#endif
#elif SOFT_OPACITY
Texture2D t_opacity : register(t4);
SamplerState s_opacity : register(s2);
Texture2D t_normal : register(t5);
#else
Texture2D t_normal : register(t4);
#endif

static const int kSearchTaps = 8;
static const int kFilterTaps = 12;
static const float kGoldenAngle = 2.39996323;
// The face normal is 8 bits per channel: the receiver's plane may be off by about a degree, so a
// tap a meter away gets 3 cm more room along the light (besides u_soft1.w, quantisation).
static const float kPlaneSlack = 0.03;

// A shadow map seen from one pixel: a split's tile of the atlas (or a static map).
struct Window
{
    float3 tc;    // the pixel in it: u, v, depth (larger = nearer the light)
    float2 perM;  // texture units per meter across the light (u, v)
    float zPerM;  // depth units per meter along the light
    float2 grad;  // the receiver's plane: depth change per texture unit along u and v
    float4 rect;  // where a gather may go (its 2 x 2 texels stay in the tile)
};

float3 xform(float4 p, int row)
{
    return float3(dot(p, c[row]), dot(p, c[row + 1]), dot(p, c[row + 2]));
}

Window makeWindow(float4 p, float3 n, int row, float4 rect)
{
    Window w;
    float3 mu = c[row].xyz, mv = c[row + 1].xyz, mz = c[row + 2].xyz;
    w.tc = xform(p, row);
    w.perM = float2(length(mu), length(mv));
    float3 l = cross(mu, mv); // along the light: u and v don't change along it
    w.zPerM = abs(dot(mz, l)) / max(length(l), 1e-30);
    // The receiver's plane in map space: its normal there is the inverse transpose of the matrix
    // times n (the cofactors), so depth changes by -nm.x / nm.z per u and -nm.y / nm.z per v.
    // Limited to 8 m along the light per meter across it (83 degrees): grazing light is dim anyway.
    float3 nm = float3(dot(cross(mv, mz), n), dot(cross(mz, mu), n), dot(l, n));
    float nz = abs(nm.z) > 1e-30 ? nm.z : 1e-30;
    float2 gmax = 8.0 * w.zPerM / max(w.perM, 1e-30);
    w.grad = clamp(-nm.xy / nz, -gmax, gmax);
    w.rect = rect;
    return w;
}

// Split i of n (rows from 11 on; its rectangle after all the matrices).
Window splitWindow(float4 p, float3 n, int i, int splits, float2 texel)
{
    int minRow = 11 + 3 * splits;
    float4 rect = float4(c[minRow + i].xy, c[minRow + splits + i].xy - 0.5 * texel);
    return makeWindow(p, n, 11 + 3 * i, rect);
}

// Whether split i holds a disc of radiusM meters around the pixel (at least minTexels) at no
// more than maxTexels: only its position and scale, the rest of a window is made for the one chosen.
bool splitFits(float4 p, int i, int splits, float2 texel, float radiusM, float minTexels, float maxTexels)
{
    int row = 11 + 3 * i, minRow = 11 + 3 * splits;
    float3 tc = xform(p, row);
    float2 r = max(radiusM * float2(length(c[row].xyz), length(c[row + 1].xyz)), minTexels * texel);
    float4 rect = float4(c[minRow + i].xy, c[minRow + splits + i].xy - 0.5 * texel);
    return all(r <= maxTexels * texel) && all(tc.xy - r > rect.xy) && all(tc.xy + r < rect.zw) && tc.z > 0.0 && tc.z < 1.0;
}

// The 2 x 2 texels around uv: their depths, and the receiver's plane at each of them, in gather
// order (x: u0 v1, y: u1 v1, z: u1 v0, w: u0 v0). f = where uv lies between them.
void gather2x2(Window w, float2 uv, float2 size, out float4 depth, out float4 plane, out float2 f)
{
    float2 t = uv * size - 0.5;
    float2 b = floor(t);
    f = t - b;
    depth = t_splits.GatherRed(s_point, (b + 1.0) / size);
    float2 t00 = (b + 0.5) / size;
    float base = w.tc.z + dot(w.grad, t00 - w.tc.xy);
    float2 g = w.grad / size;
    plane = base + float4(g.y, g.x + g.y, g.x, 0.0);
}

float2 rotate(float2 d, float2 cs)
{
    return float2(d.x * cs.x - d.y * cs.y, d.x * cs.y + d.y * cs.x);
}

// Interleaved gradient noise (Jimenez): the per-pixel turn of the sample discs.
float ign(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// The engine's read of the split atlas (its single tap, or its taps along the pixel's footprint).
float engineSplit(float3 tc, int split, float3 p, float3 n, float ndv)
{
    float lit;
    [branch] if (ndv < 0.5)
    {
        float3 v = normalize(p);
        float3 t = normalize(cross(cross(n, v), n)); // the view direction along the surface
        int row = 11 + 3 * split;
        float3 dir = float3(dot(t, c[row].xyz), dot(t, c[row + 1].xyz), dot(t, c[row + 2].xyz));
        dir *= p.z * c[3].w * (1.0 / ndv - 1.0);
        uint taps = 1u << (uint)min(-log2(ndv), c[3].z);
        float inv = 1.0 / taps;
        float3 q = tc + dir * (inv - 1.0);
        float3 stepq = dir * (inv * 2.0);
        float left = floor(tc.x * 5.0) * 0.2;
        float sum = 0.0, count = 0.0;
        [loop] for (uint k = 0u; k < taps; k++)
        {
            float ok = left < q.x && 0.0 < q.y ? 1.0 : 0.0;
            count += ok;
            sum += ok * t_splits.SampleCmpLevelZero(s_splits, q.xy, q.z);
            q += stepq;
        }
        lit = sum / count;
    }
    else lit = t_splits.SampleCmpLevelZero(s_splits, tc.xy, tc.z);
    return lit;
}

// The soft read of the split atlas. debug: penumbra radius (m) and the filter's split.
float softSplit(float4 p4, float3 p, float3 n, float ndv, int sel, int splits, float2 pos, out float2 debug)
{
    debug = 0.0;
    float2 size;
    t_splits.GetDimensions(size.x, size.y);
    float2 texel = 1.0 / size;
    float tanHalf = u_soft0.x, maxDist = u_soft0.y;
    float minR = u_soft1.x, maxSearch = u_soft1.y, maxFilter = u_soft1.z, eps = u_soft1.w;
    float noise = ign(pos);
    float2 turn = float2(cos(noise * 6.2831853), sin(noise * 6.2831853));

    // 1. Blockers within reach of the light's disc: the finest split that holds the search disc
    //    at no more than maxSearch texels (else the engine's split, the disc cut to that size).
    float searchM = tanHalf * maxDist;
    int searchSplit = sel;
    [loop] for (int i = splits - 1; i >= 0; i--)
    {
        if (splitFits(p4, i, splits, texel, searchM, 1.0, maxSearch))
        {
            searchSplit = i;
            break;
        }
    }
    Window s = splitWindow(p4, n, searchSplit, splits, texel);
    float2 searchR = clamp(searchM * s.perM, texel, maxSearch * texel);
    float found = 0.0, height = 0.0;
    [unroll] for (int k = 0; k < kSearchTaps; k++)
    {
        float a = k * kGoldenAngle;
        float2 d = rotate(sqrt((k + 0.5) / kSearchTaps) * float2(cos(a), sin(a)), turn);
        float2 uv = clamp(s.tc.xy + d * searchR, s.rect.xy, s.rect.zw);
        float4 depth, plane;
        float2 f;
        gather2x2(s, uv, size, depth, plane, f);
        float room = 2.0 * eps + length((uv - s.tc.xy) / max(s.perM, 1e-30)) * kPlaneSlack * s.zPerM;
        float4 blocker = depth > plane + room ? 1.0 : 0.0;
        found += dot(blocker, 1.0);
        height += dot(blocker, depth - plane); // how far above the receiver's plane, along the light
    }
    // Every texel a blocker: deep in the umbra (no filter).
    bool umbra = found >= kSearchTaps * 4.0;

    // 2. The penumbra: as wide as the light's disc seen from the receiver, at the blockers' distance.
    float distM = found > 0.0 ? min(height / found / max(s.zPerM, 1e-30), maxDist) : 0.0;
    float radiusM = distM * tanHalf;

    // 3. The filter: the finest split that holds its disc at no more than maxFilter texels
    //    (dithered, so the step to the next split leaves no line); no blocker: the engine's split.
    float limit = maxFilter * (0.75 + 0.5 * frac(noise * 7.31));
    int filterSplit = sel;
    [branch] if (found > 0.0 && !umbra)
    {
        [loop] for (int i = splits - 1; i >= 0; i--)
        {
            if (splitFits(p4, i, splits, texel, radiusM, minR, limit))
            {
                filterSplit = i;
                break;
            }
        }
    }
    Window w = s;
    [branch] if (filterSplit != searchSplit) w = splitWindow(p4, n, filterSplit, splits, texel);
    // The pixel's footprint on the surface (what the engine filters along at grazing views), in
    // meters across the light, so the filter disc becomes an ellipse along it.
    float3 v = normalize(p);
    float3 cnv = cross(n, v);
    float2 footM = 0.0;
    [branch] if (dot(cnv, cnv) > 1e-8)
    {
        float3 t = normalize(cross(cnv, n));
        int row = 11 + 3 * filterSplit;
        float2 footTc = float2(dot(t, c[row].xyz), dot(t, c[row + 1].xyz)) * (p.z * c[3].w * (1.0 / ndv - 1.0));
        footTc = clamp(footTc, -maxFilter * texel, maxFilter * texel);
        footM = footTc / max(w.perM, 1e-30);
    }
    float footLen = length(footM);
    float2 footDir = footLen > 1e-12 ? footM / footLen : float2(1.0, 0.0);
    // Radius in meters: at least minR texels along the map's coarser axis.
    float2 minM = minR * texel / max(w.perM, 1e-30);
    float rM = max(radiusM, max(minM.x, minM.y));
    float2 maxM = maxFilter * texel / max(w.perM, 1e-30);
    rM = min(rM, min(maxM.x, maxM.y) * 1.5);

    float lit = 0.0;
    [branch] if (umbra)
    {
        filterSplit = -2;
    }
    else if (found > 0.0)
    {
        [unroll] for (int k = 0; k < kFilterTaps; k++)
        {
            float a = k * kGoldenAngle;
            float2 d = rotate(sqrt((k + 0.5) / kFilterTaps) * float2(cos(a), sin(a)), turn);
            float2 offM = rM * d + footLen * dot(d, footDir) * footDir;
            float2 uv = clamp(w.tc.xy + offM * w.perM, w.rect.xy, w.rect.zw);
            float4 depth, plane;
            float2 f;
            gather2x2(w, uv, size, depth, plane, f);
            float room = eps + length(offM) * kPlaneSlack * w.zPerM;
            float4 l = depth <= plane + room ? 1.0 : 0.0;
            lit += lerp(lerp(l.w, l.z, f.x), lerp(l.x, l.y, f.x), f.y);
        }
        lit /= kFilterTaps;
    }
    else
    {
        // Nothing above the receiver within reach: the small filter only (four taps), which
        // still shows a caster too thin for the search's split.
        [unroll] for (int k = 0; k < 4; k++)
        {
            float a = k * 1.5707963 + 0.7853982;
            float2 d = rotate(0.7071068 * float2(cos(a), sin(a)), turn);
            float2 offM = rM * d + footLen * dot(d, footDir) * footDir;
            float2 uv = clamp(w.tc.xy + offM * w.perM, w.rect.xy, w.rect.zw);
            float4 depth, plane;
            float2 f;
            gather2x2(w, uv, size, depth, plane, f);
            float room = eps + length(offM) * kPlaneSlack * w.zPerM;
            float4 l = depth <= plane + room ? 1.0 : 0.0;
            lit += lerp(lerp(l.w, l.z, f.x), lerp(l.x, l.y, f.x), f.y);
        }
        lit *= 0.25;
    }
    debug = float2(radiusM, filterSplit);
    return lit;
}

#if SOFT_SPRITE
// The engine's bicubic (B-spline) read of a sprite shadow map: four bilinear comparisons.
float spriteBicubic(Texture2D<float> map, float2 uv, float ref)
{
    float2 size;
    map.GetDimensions(size.x, size.y);
    float2 inv = 1.0 / size;
    float2 t = uv * size - 0.5;
    float2 i = floor(t);
    float2 f = t - i;
    float2 w0 = (f * (f * (3.0 - f) - 3.0) + 1.0) / 6.0;
    float2 w1 = (f * f * (3.0 * f - 6.0) + 4.0) / 6.0;
    float2 w2 = (f * (f * (3.0 - 3.0 * f) + 3.0) + 1.0) / 6.0;
    float2 w3 = f * f * f / 6.0;
    float2 g0 = w0 + w1, g1 = w2 + w3;
    float2 h0 = (i - 0.5 + w1 / g0) * inv;
    float2 h1 = (i + 1.5 + w3 / g1) * inv;
    float s00 = map.SampleCmpLevelZero(s_sprite, h0, ref);
    float s10 = map.SampleCmpLevelZero(s_sprite, float2(h1.x, h0.y), ref);
    float s01 = map.SampleCmpLevelZero(s_sprite, float2(h0.x, h1.y), ref);
    float s11 = map.SampleCmpLevelZero(s_sprite, h1, ref);
    return g0.y * (g0.x * s00 + g1.x * s10) + g1.y * (g0.x * s01 + g1.x * s11);
}
#endif

float4 softShadow(float4 pos)
{
    int2 px = int2(pos.xy);
    float2 rp = pos.xy * scene[6].xy;
    float3 ray = c[0].xyz + rp.x * c[1].xyz + rp.y * c[2].xyz;
    float z01 = c[0].w * t_depth.Load(int3(px, 0)) + c[1].w;
    float3 p = ray * z01; // camera space, meters
    float4 p4 = float4(p, 1.0);
    float3 s0 = xform(p4, 5);
    float3 s1 = xform(p4, 8);
    int splits = (int)u_soft0.z;
    int mode = (int)u_soft0.w;

    // The engine's choice: the last split whose rectangle holds the pixel (its coordinates moved
    // there the way the engine does it, to the last bit).
    float3 tc = s0;
    int sel = 0;
    float inSplit = 0.0;
    int minRow = 11 + 3 * splits;
    [loop] for (int i = 0; i < splits; i++)
    {
        float3 t = xform(p4, 11 + 3 * i);
        float3 a = (t - c[minRow + i].xyz) * (c[minRow + splits + i].xyz - t);
        float inside = all(a > 0.0) ? 1.0 : 0.0;
        tc = mad(inside, t - tc, tc);
        inSplit = mad(inside, 1.0 - inSplit, inSplit);
        sel = inside > 0.0 ? i : sel;
    }

    float3 n = t_normal.Load(int3(px, 0)).xyz * 2.0 - 1.0;
    n *= rsqrt(max(dot(n, n), 1e-12));
    float ndv = max(dot(n, -normalize(p)), c[3].y);

    float staticLit = t_static0.SampleCmpLevelZero(s_static, s0.xy, s0.z);
    staticLit = lerp(staticLit, t_static1.SampleCmpLevelZero(s_static, s1.xy, s1.z), c[3].x);

    float softLit = 1.0, engineLit = 1.0;
    float2 debug = 0.0;
    [branch] if (inSplit > 0.5)
    {
        [branch] if (mode != 1) softLit = softSplit(p4, p, n, ndv, sel, splits, pos.xy, debug);
        [branch] if (mode != 0) engineLit = engineSplit(tc, sel, p, n, ndv);
    }
    float splitLit = mode == 1 ? engineLit : softLit;
#if SOFT_OPACITY
    float opacity = t_opacity.Sample(s_opacity, tc.xy).x;
    splitLit *= opacity;
    engineLit *= opacity;
#endif
#if SOFT_SPRITE
    float sprite = spriteBicubic(t_sprite0, s0.xy, s0.z);
    [branch] if (!(c[3].x < 0.01)) sprite = lerp(sprite, spriteBicubic(t_sprite1, s1.xy, s1.z), c[3].x);
    splitLit *= sprite;
    engineLit *= sprite;
#endif
    float lit = lerp(staticLit, splitLit, inSplit);
    float4 result = lit * lit;
    [branch] if (mode == 2)
    {
        // Debug (the test tool): soft, the engine's, penumbra radius (m), the filter's split
        // ((split + 2) / 8: 0.125 umbra, 0 outside the splits).
        float engine = lerp(staticLit, engineLit, inSplit);
        result = float4(lit * lit, engine * engine, saturate(debug.x * 4.0), inSplit > 0.5 ? (debug.y + 2.0) / 8.0 : 0.0);
    }
    return result;
}
