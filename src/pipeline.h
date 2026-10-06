#pragma once
#include "gfx.h"
#include "settings.h"
#include <d3d9.h>
#include <string>
#include <vector>

namespace tmshaders {

// The post-processing pipeline. Device-agnostic: used by the game plugin and by the
// offline previewer (tools/preview), which feeds it recorded frames.
class Pipeline {
public:
    struct Inputs {
        IDirect3DTexture9* color = nullptr; // scene colour (sRGB, 8 bit)
        IDirect3DTexture9* depth = nullptr; // raw hardware depth in .r (INTZ or R32F)
        UINT width = 0;
        UINT height = 0;
        float view[16] = {};
        float projection[16] = {};
        float sunDirection[3] = {0.0f, 1.0f, 0.0f}; // world space, towards the sun
        bool sunKnown = false;
        float sunColor[3] = {1.0f, 1.0f, 1.0f};     // the game's light colour
        bool sunColorKnown = false;
        float time = 0.0f;
        // false for extra cameras in the same frame (replay camera blends): they must not
        // touch temporal history (TAA, motion blur, exposure, focus).
        bool temporal = true;
    };

    // shaderDir: folder containing tmvs.hlsl; empty = embedded copy.
    bool init(IDirect3DDevice9* device, const std::wstring& shaderDir);
    bool ready() const { return m_ready; }
    void release();
    bool reloadShaders(IDirect3DDevice9* device);
    const std::string& lastError() const { return m_lastError; }

    // Device state is saved/restored around the pipeline (render targets excluded).
    void beginStateSave(IDirect3DDevice9* device);
    void endStateSave(IDirect3DDevice9* device);

    // Writes the final image to `output` (must not be inputs.color).
    void render(IDirect3DDevice9* device, const Inputs& inputs, const Settings& settings, IDirect3DSurface9* output);

    // Capture helpers.
    bool readColor(IDirect3DDevice9* device, IDirect3DSurface9* surface, std::vector<uint32_t>& out);
    bool readDepth(IDirect3DDevice9* device, IDirect3DTexture9* depth, std::vector<float>& out);

    // Resets temporal state (exposure, TAA, motion blur, long-shadow height map).
    void resetHistory() {
        m_historyValid = false;
        m_havePrevious = false;
        m_heightValid = false;
    }

    // GPU timing per pass with timestamp queries (results arrive a few frames late).
    void setProfiling(bool enabled) { m_profiling = enabled; }
    void collectProfile(bool wait); // wait = block until every issued frame is resolved
    int passCount() const { return kPassCount; }
    static const char* passName(int pass);
    float passTime(int pass) const { return m_passMs[pass]; } // smoothed milliseconds
    float totalTime() const { return m_totalMs; }

private:
    enum Pass {
        kLinearDepth,
        kPrepare,
        kDownsampleND,
        kHeightMerge,
        kHeightSplat,
        kOcclusionShadow,
        kBilateralBlur,
        kSky,
        kSkyAverage,
        kClouds,
        kReflect,
        kSpillDown,
        kSpillBlur,
        kLighting,
        kRainDrop,
        kRainSplash,
        kFocus,
        kDofBlur,
        kCinematic,
        kRayMask,
        kRayBlur,
        kBloomDown,
        kBloomUp,
        kLuminance,
        kAdapt,
        kFinal,
        kFXAA,
        kTAA,
        kSharpen,
        kCopy,
        kCopyDepth,
        kPassCount
    };

    static constexpr int kBloomLevels = 6;

    bool compileAll(IDirect3DDevice9* device);
    bool ensureTargets(IDirect3DDevice9* device, UINT width, UINT height);
    void destroyTargets();
    void setFrameConstants(IDirect3DDevice9* device, const Inputs& inputs, const Settings& settings);
    void runPass(IDirect3DDevice9* device, Pass pass, gfx::Target& target);
    void runPass(IDirect3DDevice9* device, Pass pass, IDirect3DSurface9* target, UINT width, UINT height);
    void bind(IDirect3DDevice9* device, int stage, IDirect3DBaseTexture9* texture, bool linear);
    void passConstants(IDirect3DDevice9* device, float a, float b, float c = 0.0f, float d = 0.0f);
    bool ensureHeightMap(IDirect3DDevice9* device);
    void updateHeightMap(IDirect3DDevice9* device, const Inputs& inputs, const Settings& settings);
    bool ensureCloudNoise(IDirect3DDevice9* device);
    bool ensureRain(IDirect3DDevice9* device);
    void drawRain(IDirect3DDevice9* device, const Inputs& inputs, const Settings& settings, float dt, bool splashes,
                  IDirect3DSurface9* target);
    bool detectCameraCut(const Inputs& inputs) const;

    struct ProfileFrame {
        static constexpr int kMaxStamps = 72;
        IDirect3DQuery9* disjoint = nullptr;
        IDirect3DQuery9* frequency = nullptr;
        IDirect3DQuery9* stamps[kMaxStamps] = {};
        int pass[kMaxStamps] = {};
        int count = 0;
        bool pending = false;
    };
    static constexpr int kProfileFrames = 4;
    void profileBegin(IDirect3DDevice9* device);
    void profileMark(int pass);
    void profileEnd();
    bool profileResolve(ProfileFrame& frame, bool wait);
    void releaseProfile();

    bool m_ready = false;
    std::wstring m_shaderDir;
    std::string m_lastError;
    gfx::Quad m_quad;
    IDirect3DPixelShader9* m_shaders[kPassCount] = {};
    IDirect3DStateBlock9* m_stateBlock = nullptr;

    UINT m_width = 0;
    UINT m_height = 0;
    gfx::Target m_linearDepth;  // full: filtered view z (R32F)
    gfx::Target m_nd;           // full: normal + view z
    gfx::Target m_ndHalf;       // half
    gfx::Target m_occlusion;    // half: AO, sun visibility
    gfx::Target m_occlusionTmp; // half
    gfx::Target m_sky;          // full: custom sky (HDR)
    gfx::Target m_skyAverage;   // 1x1
    gfx::Target m_hdr;          // full HDR
    gfx::Target m_rays[2];      // half
    gfx::Target m_bloomDown[kBloomLevels];
    gfx::Target m_bloomUp[kBloomLevels];
    gfx::Target m_luminance;    // 1x1
    gfx::Target m_adapted[2];   // 1x1 ping-pong
    gfx::Target m_ldr;          // full LDR
    gfx::Target m_ldr2;         // full LDR
    gfx::Target m_depthCopy;    // capture helper (R32F)
    gfx::Target m_clouds;       // half: volumetric clouds (in-scatter, transmittance)
    gfx::Target m_reflect;      // half: screen-space reflections
    gfx::Target m_spill[2];     // quarter: neon light spill
    gfx::Target m_focus[2];     // 1x1 auto focus ping-pong
    gfx::Target m_dof;          // half: depth of field blur
    gfx::Target m_hdr2;         // full: HDR after DOF / motion blur
    gfx::Target m_taa[2];       // full: TAA history ping-pong (rgb, view z)
    int m_adaptIndex = 0;
    int m_focusIndex = 0;
    int m_taaIndex = 0;
    bool m_taaValid = false;
    bool m_temporalValid = false; // this frame continues the previous one (no camera cut)
    bool m_sunKnown = false;

    // Previous frame's camera (reprojection for TAA and motion blur).
    float m_prevView[16] = {};
    float m_prevProjection[16] = {};
    bool m_havePrevious = false;

    // Long-range shadows: world-space height map built from the depth buffer.
    static constexpr UINT kHeightMapSize = 512;
    static constexpr float kHeightMapWorld = 320.0f; // metres covered
    gfx::Target m_heightFrame;                       // this frame's splat
    gfx::Target m_heightMap[2];                         // accumulated, ping-pong
    IDirect3DSurface9* m_heightDepth = nullptr;      // keeps the highest splat per texel
    IDirect3DVertexShader9* m_splatVS = nullptr;
    IDirect3DVertexDeclaration9* m_splatDecl = nullptr;
    IDirect3DVertexBuffer9* m_splatPoints = nullptr;
    UINT m_splatCount = 0;
    int m_heightIndex = 0;
    bool m_heightValid = false;
    bool m_heightSupported = true;
    float m_heightOrigin[2] = {};

    IDirect3DVolumeTexture9* m_cloudNoise = nullptr;

    // Rain particles: drops in a box around the camera, splashes on the height map.
    static constexpr UINT kRainDrops = 7000;
    static constexpr UINT kRainSplashes = 2600;
    IDirect3DVertexShader9* m_dropVS = nullptr;
    IDirect3DVertexShader9* m_splashVS = nullptr;
    IDirect3DVertexDeclaration9* m_rainDecl = nullptr;
    IDirect3DVertexBuffer9* m_rainVB = nullptr; // drops, then splashes
    IDirect3DIndexBuffer9* m_rainIB = nullptr;
    bool m_rainSupported = true;
    float m_cameraVelocity[3] = {};             // smoothed, world m/s (rain streaks)
    bool m_historyValid = false;
    bool m_profiling = false;
    ProfileFrame m_profile[kProfileFrames];
    ProfileFrame* m_currentProfile = nullptr;
    int m_profileIndex = 0;
    float m_passMs[kPassCount] = {};
    float m_totalMs = 0.0f;
    float m_sunFade = 0.0f; // sun on screen (0 = shafts and flare invisible)
    float m_lastTime = 0.0f;
    unsigned m_frame = 0;
};

} // namespace tmshaders
