#pragma once
#include <d3d11.h>
#include <cstdint>

namespace tmvs_mp {

// What the engine hands over in one frame: its camera (from the SceneV constant buffer it uploads)
// and its G-buffer, HDR image and motion vectors (found by the binding names in its shaders).

// SceneV, the engine's per-camera vertex constants (cbuffer slot 13, 704 bytes). The layout is the
// same in every shader of the GpuCache (tools/rdef.py).
constexpr UINT kSceneVSize = 704;
constexpr UINT kSceneVShadowLDir = 80;     // world -> sun shadow map (4 rows); row 2 runs along the sun's light
constexpr UINT kSceneVLightDirRgb0 = 224;
constexpr UINT kSceneVLightAmbient = 240;
constexpr UINT kSceneVLightAmbientLinear = 256;
constexpr UINT kSceneVCameraProjection = 432;
constexpr UINT kSceneVCameraMinMaxZ = 496;
constexpr UINT kSceneVWorldToCamera = 512;
constexpr UINT kSceneVWorldPrCamera = 560;
constexpr UINT kSceneVEyeInWorld = 624;

// SceneP, the per-camera pixel constants of the deferred lighting (Block_DefReadP1_p, slot 13,
// 672 bytes). Its light direction is exactly what the engine lights with: the lighting shader
// takes N.(-LightDirDirInWorld0) for the sun's N.L.
constexpr UINT kScenePSize = 672;
constexpr UINT kScenePFog = 0;
constexpr UINT kScenePLightAmbientLinear = 128;
constexpr UINT kScenePLightDirRgbLinear0 = 144;
constexpr UINT kScenePLightDirInWorld0 = 160;
constexpr UINT kScenePCloudsMin = 304;
constexpr UINT kScenePCloudsMax = 320;

struct Camera {
    bool valid = false;
    bool transposed = false;  // how WorldPrCamera is stored (found by projecting the eye)
    float viewProj[16] = {};  // world -> clip, row-major: clip = viewProj * (p, 1)
    float invViewProj[16] = {};
    float worldToCamera[12] = {}; // 3 rows of 4: camera axes in the world, translation in w
    // Camera space -> clip (CameraProjection, row-major). ManiaPlanet: reversed infinite depth
    // (clip z = near, w = view z: depth = near / z), x mirrored (P00 < 0).
    float projection[16] = {};
    float eye[3] = {};
    float sunColour[3] = {};  // the light colour (linear, can exceed 1)
    float ambient[3] = {};    // the ambient light colour (linear)
    float sunDirection[3] = {0.0f, 1.0f, 0.0f}; // towards the sun, world
    bool sunDirectionKnown = false;
    bool sunFromSceneP = false; // from the lighting pass's own constants (else the shadow matrix)
    float fog[4] = {};
    float minMaxZ[4] = {};
    unsigned char raw[kSceneVSize] = {}; // the whole buffer, for captures
    unsigned char rawP[kScenePSize] = {};
    bool hasSceneP = false;
};

enum EngineTexture : int {
    kTexHdr = 0,      // the tone map's input (R11G11B10 float)
    kTexExposure,     // the tone map's 2x2 average luminance / key value
    kTexDepth,        // the scene's depth buffer
    kTexNormal,       // TMapDeferredPixelNormalInC
    kTexDiffuse,      // TMapDeferredMDiffuse: material colour (albedo)
    kTexSpecular,     // TMapDeferredMSpecular: material specular
    kTexLightMask,    // TMapDeferredLightMask
    kTexMotion,       // TMapMotion (or the lighting pass's second target)
    kTexShadowDepth,  // the sun shadow pass's linear depth (Tech3/DeferredShadowPssm_p t0, R32 float)
    kTexShadowMap0,   // its two shadow maps (t1, t2: 4096 x 4096, 16 bit)
    kTexShadowMap1,
    kTexCount
};

// The engine's sun shadow pass (Tech3/DeferredShadowPssm_p): it turns the shadow maps into the
// light mask. Volumetric light marches the view rays through the same maps with its constants:
//   z = c[0].w * depth + c[1].w,  p = (c[0].xyz + sx c[1].xyz + sy c[2].xyz) * z
//   (sx, sy = pixel position times SceneP RasterPos_SxyTwz.xy), lit = lerp(map0(c[5..7] p),
//   map1(c[8..10] p), c[3].x), both compared through its comparison sampler.
struct EngineShadow {
    bool valid = false;                    // seen this frame
    ID3D11Buffer* constants = nullptr;     // a copy of its ShaderP (made on the GPU at its draw)
    UINT constantsSize = 0;
    ID3D11SamplerState* sampler = nullptr; // its comparison sampler (held)
    float raster[4] = {};                  // SceneP RasterPos_SxyTwz_TexCoord
};
constexpr UINT kScenePRasterPos = 96;

// The car (Tech3/CarSkin_DefWrite: its vertex constants ObjectV, 2752 bytes): VisualToCamera at
// 112 (3 rows of 4, the car's axes and origin in camera space), for the neon trail.
constexpr UINT kObjectVCarSize = 2752;
constexpr UINT kObjectVVisualToCamera = 112;
struct EngineCar {
    bool valid = false;     // seen this frame
    float position[3] = {}; // world
    float right[3] = {}, up[3] = {}, forward[3] = {}; // the car's axes in the world (x, y, z of its model)
};

const char* engineTextureName(int index);

// A typed format a shader resource view of `format` can use (typeless -> its UNORM/FLOAT form,
// depth formats -> their readable red channel). DXGI_FORMAT_UNKNOWN: not readable.
DXGI_FORMAT readableFormat(DXGI_FORMAT format);

// Builds viewProj/invViewProj, eye and sun from the raw SceneV bytes. False if they don't make sense.
bool decodeCamera(Camera& camera);
// The lighting constants (SceneP, rawP): sun direction, ambient and fog colour. False if absent.
bool decodeSceneP(Camera& camera);

} // namespace tmvs_mp
