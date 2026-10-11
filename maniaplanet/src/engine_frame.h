#pragma once
#include <d3d11.h>
#include <cstdint>

namespace tmvs_mp {

// What the engine hands over in one frame: its camera (from the SceneV constant buffer it uploads)
// and its G-buffer, HDR image and motion vectors (found by the binding names in its shaders).

// SceneV, the engine's per-camera vertex constants (cbuffer slot 13, 704 bytes). The layout is the
// same in every shader of the GpuCache (tools/rdef.py).
constexpr UINT kSceneVSize = 704;
constexpr UINT kSceneVLightDirRgb0 = 224;
constexpr UINT kSceneVLightAmbient = 240;
constexpr UINT kSceneVCameraProjection = 432;
constexpr UINT kSceneVCameraMinMaxZ = 496;
constexpr UINT kSceneVWorldToCamera = 512;
constexpr UINT kSceneVWorldPrCamera = 560;
constexpr UINT kSceneVEyeInWorld = 624;

struct Camera {
    bool valid = false;
    bool transposed = false;  // how WorldPrCamera is stored (found by projecting the eye)
    float viewProj[16] = {};  // world -> clip, row-major: clip = viewProj * (p, 1)
    float invViewProj[16] = {};
    float worldToCamera[12] = {}; // 3 rows of 4
    float eye[3] = {};
    float sunColour[3] = {};
    float minMaxZ[4] = {};
    unsigned char raw[kSceneVSize] = {}; // the whole buffer, for captures
};

enum EngineTexture : int {
    kTexHdr = 0,      // the tone map's input (R11G11B10 float)
    kTexExposure,     // the tone map's 2x2 average luminance / key value
    kTexDepth,        // the scene's depth buffer
    kTexNormal,       // TMapDeferredPixelNormalInC
    kTexDiffuse,      // TMapDeferredMDiffuse
    kTexSpecular,     // TMapDeferredMSpecular
    kTexLightMask,    // TMapDeferredLightMask
    kTexMotion,       // TMapMotion (or the lighting pass's second target)
    kTexCount
};

const char* engineTextureName(int index);

// A typed format a shader resource view of `format` can use (typeless -> its UNORM/FLOAT form,
// depth formats -> their readable red channel). DXGI_FORMAT_UNKNOWN: not readable.
DXGI_FORMAT readableFormat(DXGI_FORMAT format);

// Builds viewProj/invViewProj, eye and sun from the raw SceneV bytes. False if they don't make sense.
bool decodeCamera(Camera& camera);

} // namespace tmvs_mp
