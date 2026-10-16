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
constexpr UINT kScenePDayTime = 464; // GbxP_DayTime: x = the map's time of day (0..1, 0.5 noon)

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
    float dayTime = -1.0f; // the map's time of day from SceneP (0..1, 0.5 noon), -1 unknown
};

// The map's mood from its time of day: 0 night, 1 sunrise or sunset, 2 day; -1 unknown.
// (Stadium: night 0.10, sunrise 0.32, day 0.50, sunset 0.81. Its night "sun" is the moon,
// (0.36, 0.53, 1.16): bright enough to pass for day by its colour.)
int dayTimeMood(const Camera& c);

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
    kTexShadowSplits, // its split atlas (t3: 6400 x 1280, 16 bit, five tiles; variants with splits)
    kTexShadowNormal, // the face normal it reads (TMapVNormalInC; variants with splits), for captures
    kTexLights,       // EngineLightDraws: the light the engine drew as light (engine_lights.cpp), float
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
    unsigned track = 0;     // another number: another car taken up (the trail starts a new line)
};

// Which of the frame's cars is the player's. Ghosts and other players are drawn like it (the same
// G-buffer shader). The player's is the one the camera follows: the chase cameras hold it in the
// middle of the picture a few metres ahead (captures 029/030: 0.0001 m off the middle, 6.4 m
// ahead) at the same distance (5 mm a frame at most). Once taken up it is followed by its place
// and speed (within 3 m, also over a few frames without it); a car is taken up only where the
// camera follows one, and when the chase camera holds another car for 6 frames it takes that
// one. (It took the one that continued last frame's within 30 m, else the nearest, and a frame
// without any car forgot it: after a camera switch next to a ghost the trail went to the ghost
// and stayed with it. mp_preview --cartest plays such scenes.)
struct CarCandidate {
    float view[3];  // its place in camera space (x right, y up, z ahead)
    float world[3]; // and in the world
};
class CarTracker {
public:
    // The player's car among this frame's (an index, -1: none), now in seconds.
    int pick(const CarCandidate* cars, int count, double now);
    unsigned track() const { return m_id; }

private:
    bool m_valid = false;
    double m_seen = 0.0;
    float m_position[3] = {}, m_velocity[3] = {};
    bool m_speedKnown = false;
    unsigned m_id = 0;
    int m_otherFrames = 0;
    float m_lastView[8][3] = {}; // last frame's cars in camera space
    int m_lastSteady[8] = {};    // and how many frames each kept its distance from the camera
    int m_lastCount = 0;
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
