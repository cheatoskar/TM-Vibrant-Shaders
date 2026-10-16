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
        // The game's image from its float target (GameHdr): light above 1.0 kept. Null = only
        // the 8-bit one. The guesses (lamps, glow, spill) stay on `color`.
        IDirect3DTexture9* hdr = nullptr;
        // The light the game's additive draws added (glows, halos; float, already inside `hdr`).
        // With EngineLights it is the light source instead of the guess. Null = none.
        IDirect3DTexture9* added = nullptr;
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
        // TAA jitter of this frame's projection, as the uv shift of the image content.
        float jitter[2] = {};
        // Water surfaces from the game: block water height, sea level, state 1 = known,
        // -1 = unknown (guess from the colour).
        float water[3] = {0.0f, 0.0f, -1.0f}; // lowest, highest, kind: 1 sea, 2 Stadium water blocks, 0 none reported (measured), -1 no engine (colour guess)
        // The player drives (not a replay / intro): the neon trail records the car.
        bool driving = true;
        // Where the engine draws the player's car (world space, about the height of the wheel
        // centres): the neon trail follows it, also off screen. Unknown (no engine): found in
        // the depth buffer below the screen centre.
        bool carKnown = false;
        float carPosition[3] = {};
        // The engine gave the car's position a moment ago: a frame without it records nothing
        // (no fall back to the depth search, a new line starts after the gap).
        bool carFromEngine = false;
        // Photo mode: effects reach 3x farther, a larger height map, no car assumed in the
        // middle of the screen.
        bool photo = false;
        // The game's video export: photo mode's depth of field (full resolution, many samples,
        // no noise) for a moving camera.
        bool video = false;
        // Another sub-pixel sample of the frame just shaded (the export's supersampling, into
        // the same target): looks like that frame, reads its history (motion blur, focus,
        // exposure) but advances none of it. temporal is false.
        bool repeat = false;
        // Focus on this point of the screen (uv), -1 = the usual spot (the car, the road ahead).
        float focusPoint[2] = {-1.0f, -1.0f};
        // Hi-res photo tiles: exposure, focus and the haze colour keep their values (each tile
        // sees another part of the view).
        bool freeze = false;
        // Colour look-up table (setLut) mixed in after the grade: 0 = off, 1 = fully.
        float lut = 0.0f;
        // The stencil of the depth texture holds the game's material classes (the plugin
        // tagged the game's draws): lamps, cars, glass, grass are known, not guessed.
        bool materials = false;
        // F12's frame: the stencil holds each material's own id (1..255) instead of its class,
        // materialClasses[id] is that class (engine::Material). The mask is built from it and
        // readMaterialIds gives the ids (capture_NNN.mat).
        const unsigned char* materialClasses = nullptr;
        // The previewer: the mask itself (from capture_NNN.mat; r light, g car 1 / glass .5 /
        // grass .25, b water), instead of the stencil. Needs `materials`.
        IDirect3DTexture9* materialMask = nullptr;
    };

    // A 3D colour look-up table (A8R8G8B8, x = red, y = green, z = blue; size 0 = none).
    // Kept on the CPU and uploaded when needed (also after a device reset).
    void setLut(const std::vector<uint32_t>& texels, int size);

    // The game's materials in the stencil: classes 1..kMaterialClasses (engine::Material) in
    // the low three bits, kAddedLightBit where the game added light on top (headlight pools,
    // glows). Debug view "Materials" shows them (lamps yellow, cars red, glass cyan, water blue,
    // added light magenta).
    static constexpr int kMaterialView = 8;
    static constexpr int kMaterialClasses = 5;
    static constexpr unsigned long kAddedLightBit = 0x08;

    // The sky replaces the game's sun (night skies, the black hole): its baked light comes out
    // on day maps and its halo is hidden.
    static bool skyReplacesSun(const Settings& s) {
        return (s.skyMode >= 2 && s.skyMode != 5) || ((s.skyMode == 3 || s.skyMode == 5) && s.skyEffectSize > 0.0f);
    }

    // Neon trail: start over (race restart, new map) / start a new line (respawn).
    void resetTrail() { m_trailReset = true; }
    void breakTrail() { m_trailBreak = true; }

    // Restart or respawn: the height map starts over. The respawn camera shows the player's
    // car from outside the area kept out of the map, and the ground under the parked car is
    // never seen again to correct it: it would cast blotchy shadows around the car.
    void resetHeights() { m_heightReset = true; }
    // A restart or a new map: the roofs and bridges remembered for the rain are gone too.
    void resetCeilings() { m_ceilingValid = false; }
    // A new map: the sea level measured from the water material is the last map's.
    void resetSeaLevel() {
        m_seaKnown = false;
        m_waterMaterialSeen = false;
        m_waterPending = -1;
    }

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
    // Copies part of the scene depth (area in pixels) into `out` (R32F, the area's size): the
    // pipeline then runs on a camera that draws into part of the screen (MediaTracker editor).
    bool copyDepthArea(IDirect3DDevice9* device, IDirect3DTexture9* depth, const RECT& area, gfx::Target& out);
    // The game's image from its float target (GameHdr) into `target` (the game's own render
    // target, same size), cut to 0..1 as that 8-bit target would have it. Only inside `area`
    // (the camera's viewport): the rest of the game's target keeps what it had.
    bool resolveScene(IDirect3DDevice9* device, IDirect3DTexture9* scene, IDirect3DSurface9* target, const RECT& area);
    // Adds `added` (the light the game's additive draws put into a target of their own) into
    // the float scene `target`, inside `area`, as it is.
    bool addScene(IDirect3DDevice9* device, IDirect3DTexture9* added, IDirect3DSurface9* target, const RECT& area);
    // Reads a float colour target (A16B16G16R16F) back as raw half floats, RGBA, rows top-down.
    bool readHalfColor(IDirect3DDevice9* device, IDirect3DSurface9* surface, std::vector<uint16_t>& out);
    // The material id of every pixel (one byte, rows top-down), after a frame rendered with
    // Inputs::materialClasses.
    bool readMaterialIds(IDirect3DDevice9* device, std::vector<uint8_t>& out);

    // Resets temporal state (exposure, TAA, motion blur, long-shadow height map).
    void resetHistory() {
        m_historyValid = false;
        m_havePrevious = false;
        m_heightValid = false;
        m_skyValid = false;
        m_shelterValid = false;
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
        kShadowHeight,
        kVolumetric,
        kGI,
        kGITemporal,
        kSkyClear, // one entry per sky mode (1..5), in mode order
        kSkyStars,
        kSkyBlackHole,
        kSkyAurora,
        kSkyRing,
        kAuroraHalf,
        kSkyAverage,
        kShelter,
        kClouds,
        kReflect,
        kSpillDown,
        kSpillBlur,
        kLighting,
        kRainDrop,
        kRainSplash,
        kSpray,
        kTrailPoint,
        kTrail,
        kSnowFlake,
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
        kWaterLevel,
        kResolve,
        kLightingEngine,
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
    void updateHeightMap(IDirect3DDevice9* device, const Inputs& inputs, const Settings& settings, bool ceilings);
    bool ensureCloudNoise(IDirect3DDevice9* device);
    bool ensureLut(IDirect3DDevice9* device);
    bool buildMaterialMask(IDirect3DDevice9* device, const Inputs& in);
    bool ensureRain(IDirect3DDevice9* device);
    bool ensureTrail(IDirect3DDevice9* device);
    void drawTrail(IDirect3DDevice9* device, const Inputs& in, const Settings& s, float dt);
    void drawRain(IDirect3DDevice9* device, const Inputs& inputs, const Settings& settings, float dt, bool splashes,
                  IDirect3DSurface9* target);
    bool detectCameraCut(const Inputs& inputs) const;
    void keepRepeatCamera();
    void swapRepeatCamera();

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
    // resolveScene (cut to 0..1) and addScene (added as it is, one-one blending).
    bool copyScene(IDirect3DDevice9* device, IDirect3DTexture9* source, IDirect3DSurface9* target, const RECT& area, bool add);

    bool m_ready = false;
    std::wstring m_shaderDir;
    std::string m_lastError;
    gfx::Quad m_quad;
    IDirect3DPixelShader9* m_shaders[kPassCount] = {};
    IDirect3DStateBlock9* m_stateBlock = nullptr;
    IDirect3DPixelShader9* m_solidPS = nullptr; // one colour (c32): the "Materials" debug view
    IDirect3DPixelShader9* m_paletteLookupPS = nullptr; // material id -> its class's colour

    UINT m_width = 0;
    UINT m_height = 0;
    gfx::Target m_linearDepth;  // full: filtered view z (R32F)
    gfx::Target m_nd;           // full: normal + view z
    gfx::Target m_ndHalf;       // half
    gfx::Target m_occlusion;    // half: AO, sun visibility
    gfx::Target m_occlusionTmp; // half
    gfx::Target m_sky;          // full: custom sky (HDR)
    gfx::Target m_gi[2];        // quarter: one-bounce global illumination, ping-pong for the blur
    gfx::Target m_giHistory[2]; // quarter: GI accumulated over frames (rgb, view z in a)
    int m_giIndex = 0;
    bool m_giValid = false;
    gfx::Target m_skyAverage[2]; // 1x1 ping-pong: fog colour, eased over time
    int m_skyIndex = 0;
    bool m_skyValid = false;
    gfx::Target m_shelter[2];    // 1x1 ping-pong: roof over the camera (lens drops dry off)
    int m_shelterIndex = 0;
    bool m_shelterValid = false;
    gfx::Target m_hdr;          // full HDR
    gfx::Target m_rays[2];      // half
    gfx::Target m_bloomDown[kBloomLevels];
    gfx::Target m_bloomUp[kBloomLevels];
    gfx::Target m_luminance;    // 1x1
    gfx::Target m_adapted[2];   // 1x1 ping-pong
    gfx::Target m_waterLevel[2];          // 1x1: the water material on screen (sum of heights, count, lowest, highest)
    IDirect3DSurface9* m_waterRead = nullptr; // system memory copy of one of them
    int m_waterPending = -1;              // the one drawn and not read yet
    int m_waterIndex = 0;
    unsigned m_waterDrawn = 0;            // the frame it was drawn in
    float m_seaLevel = 0.0f;              // measured (TMUF sea: the game reports none)
    bool m_seaKnown = false;
    bool m_waterMaterialSeen = false;     // this map's water was seen as the game's water material
    bool m_materialsOn = false;           // the material mask exists this frame
    void measureSeaLevel(IDirect3DDevice9* device);
    bool buildMaterialIds(IDirect3DDevice9* device, const Inputs& in);
    gfx::Target m_materials;    // full: the game's material classes (r light, g car, b glass, a grass)
    gfx::Target m_materialIds;  // full: F12's material ids (r = id / 255)
    IDirect3DTexture9* m_materialTexture = nullptr; // this frame's mask: m_materials or Inputs::materialMask
    gfx::Target m_ldr;          // full LDR
    gfx::Target m_ldr2;         // full LDR
    gfx::Target m_depthCopy;    // capture helper (R32F)
    gfx::Target m_clouds;       // half: volumetric clouds (in-scatter, transmittance)
    gfx::Target m_reflect;      // half: screen-space reflections
    gfx::Target m_spill[2];     // quarter: neon light spill
    gfx::Target m_focus[2];     // 1x1 auto focus ping-pong
    gfx::Target m_dof;          // half: depth of field blur
    gfx::Target m_dofFull;      // full: the same in photo mode
    gfx::Target m_dofFull2;     // full: its second, smoothing pass
    gfx::Target m_dof2;         // half: the smoothing pass of the normal blur
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
    // The camera before the last temporal frame and whether that frame continued the one before:
    // repeats of that frame (Inputs::repeat) blur with the same motion.
    float m_repeatPrevView[16] = {};
    float m_repeatPrevProjection[16] = {};
    bool m_repeatHavePrevious = false;
    bool m_repeatTemporalValid = false;

    // Long-range shadows: world-space height map built from the depth buffer.
    UINT m_heightSize = 1024;
    float m_heightWorld = 640.0f;                    // metres covered (photo mode: 960)
    gfx::Target m_heightFrame;                       // this frame's splat
    gfx::Target m_heightMap[2];                         // accumulated, ping-pong
    gfx::Target m_ceilingMap[2];                        // undersides (roofs, bridges), kept, ping-pong
    gfx::Target m_shadowHeight;                      // volumetric light: shadow height per column
    IDirect3DSurface9* m_heightDepth = nullptr;      // keeps the highest splat per texel
    IDirect3DVertexShader9* m_splatVS = nullptr;
    IDirect3DVertexDeclaration9* m_splatDecl = nullptr;
    IDirect3DVertexBuffer9* m_splatPoints = nullptr;
    UINT m_splatCount = 0;
    int m_heightIndex = 0;
    bool m_heightValid = false;
    bool m_heightReset = false;
    bool m_ceilingValid = false;
    float m_heightHoldUntil = -1.0f; // no splats until the player's camera is back (or this time)
    bool m_heightSupported = true;
    bool m_heightFilter = false;     // R32F can be filtered: the long-shadow trace reads it bilinear
    bool m_shadowHeightStale = true; // the volumetric light's shadow heights need their pass again
    float m_sunWorld[3] = {};        // this frame's sun direction (c7)
    float m_shadowHeightSun[3] = {}; // the sun the shadow heights were made for
    float m_heightOrigin[2] = {};

    IDirect3DVolumeTexture9* m_cloudNoise = nullptr;
    IDirect3DVolumeTexture9* m_lut = nullptr;
    std::vector<uint32_t> m_lutTexels;
    int m_lutSize = 0;

    // Rain particles: drops in a box around the camera, splashes on the height map.
    static constexpr UINT kRainDrops = 24000;    // at rain 2 (downpour)
    static constexpr UINT kRainSplashes = 5200;
    static constexpr UINT kRainChunk = 12000;    // quads per draw (16-bit indices)
    static constexpr UINT kSprayParticles = 700;  // borrowed from the drop quads
    static constexpr UINT kSnowFlakes = 64000;    // at snow 2 (blizzard), the same quads as the drops
    IDirect3DVertexShader9* m_dropVS = nullptr;
    IDirect3DVertexShader9* m_splashVS = nullptr;
    IDirect3DVertexShader9* m_sprayVS = nullptr;
    IDirect3DVertexShader9* m_snowVS = nullptr;
    IDirect3DVertexDeclaration9* m_rainDecl = nullptr;
    IDirect3DVertexBuffer9* m_rainVB = nullptr; // drops, then splashes
    IDirect3DIndexBuffer9* m_rainIB = nullptr;
    bool m_rainSupported = true;
    // Neon trail: the car's positions in a ring buffer texture (xyz, time stamp; < 0 = a new
    // line starts here, 0 = no car found), drawn as a glowing ribbon.
    static constexpr UINT kTrailWidth = 128, kTrailHeight = 128;
    static constexpr UINT kTrailPoints = kTrailWidth * kTrailHeight; // 4.5 min at 60 points/s, less at speed (a point every metre: 100 s at 560 km/h)
    static constexpr float kTrailMinStep = 0.3f; // m: closer points are no line (the car shaking in place)
    gfx::Target m_trailPoints;
    IDirect3DVertexShader9* m_trailVS = nullptr;
    IDirect3DVertexDeclaration9* m_trailDecl = nullptr;
    IDirect3DVertexBuffer9* m_trailVB = nullptr;
    IDirect3DIndexBuffer9* m_trailIB = nullptr;
    bool m_trailSupported = true;
    bool m_trailReset = true;
    bool m_trailBreak = true;
    UINT m_trailHead = 0;       // next point to write
    UINT m_trailCount = 0;      // points written (up to kTrailPoints)
    float m_trailClock = 0.0f;  // time since the last point
    float m_trailKept[3] = {};  // where the last point was kept (engine positions only)
    bool m_trailKeptValid = false;
    float m_trailTime = 0.0f;   // time since the reset (s)
    float m_cameraVelocity[3] = {};             // smoothed, world m/s (rain streaks)
    float m_prevJitter[2] = {};                 // jitter of the TAA history's frame
    int m_boltSlot = -1000000;                  // lightning strike the bolt direction belongs to
    float m_boltAzimuth = 0.0f;
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
