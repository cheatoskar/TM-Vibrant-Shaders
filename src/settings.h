#pragma once
#include <string>

namespace tmshaders {

enum class DebugView : int {
    None = 0,
    Depth,
    Normals,
    AmbientOcclusion,
    SunShadows,
    GodRays,
    Bloom,
    LongShadows,
    Count
};

// Every tunable of the post pipeline. Presets fill this struct; the overlay edits it.
struct Settings {
    bool enabled = true;
    int debugView = 0;

    // Game integration
    bool disableGameMSAA = true; // required for the depth buffer; post AA replaces it
    int quality = 2;               // 0 low, 1 medium, 2 high (sample counts)
    bool readableGameDepth = true; // also hook the game's own depth buffers (menus, replay/video export)
    // Neon light trail behind the car, kept for the whole run (0 = off).
    float neonTrail = 0.0f;
    float trailColor[3] = {0.1f, 0.65f, 1.0f};
    float trailWidth = 0.2f;       // m
    bool trailTyres = true;        // two lines from the rear tyres, else one from the middle
    float trailDuration = 0.0f;    // s until a piece fades out, 0 = the whole run
    bool taaJitter = false;         // TAA samples a different sub-pixel spot every frame (sharper, smoother)
    bool cinematicOnlyInReplays = true; // motion blur and DOF only in replays, intros and the video export
    bool autoQuality = true;       // lower the effect quality when the frame rate drops below the target
    float targetFps = 60.0f;

    // Lighting (screen-space, reconstructed from depth)
    float aoStrength = 1.0f;
    float aoRadius = 1.6f;          // metres
    float shadowStrength = 0.95f;   // screen-space sun shadows
    float shadowLength = 9.0f;      // metres
    float longShadows = 0.8f;       // world-space height map shadows (long, off-screen casters)
    float longShadowRange = 120.0f; // metres
    float neonLight = 0.6f;         // coloured light sources light up their surroundings
    float sunLight = 0.6f;          // directional sun relighting (N.L)
    float ambientTint = 0.55f;      // sky-coloured fill in shadow
    float sunColor[3] = {1.00f, 0.86f, 0.66f};
    float gameSunColor = 0.7f;      // how much of the game's own light colour (time of day) to use
    float skyColor[3] = {0.55f, 0.72f, 1.00f};
    float sunElevationOverride = -1.0f; // degrees; < 0 uses the game's light
    float sunAzimuthOverride = 0.0f;

    // Sky & atmosphere
    int skyMode = 0;               // 0 game, 1 clear sky, 2 starry night, 3 black hole, 4 aurora, 5 ring world
    float skyNight = -1.0f;        // scene darkening for night skies (< 0 = automatic)
    float skyRotation = 0.0f;      // degrees: rotates galaxy / black hole / aurora
    float skyBrightness = 1.0f;
    float cloudAmount = 0.5f;
    float starAmount = 1.0f;
    float auroraSpeed = 1.0f;      // how fast the aurora curtains move
    float skyEffectSize = 1.0f;    // black hole size (above ~3 the camera is right at its disk, 0 = none)
    float blackHoleAzimuth = 0.0f;    // degrees, turns with the sky; the black hole lights the scene
    float blackHoleElevation = 15.6f; // degrees
    float planetSize = 0.0f;       // ringed planet in the space skies (0 = none)
    int planetType = 0;            // ring world: 0 Saturn, 1 Jupiter, 2 ice giant, 3 exotic
    int planetView = 1;            // ring world: 0 distant, 1 next to the rings, 2 on the rings
    float planetAzimuth = 35.0f;   // degrees
    float planetElevation = 14.0f; // degrees
    float volumetricClouds = 0.0f; // ray-marched cloud layer (0 = off)
    float cloudCoverage = 0.45f;
    float cloudHeight = 1600.0f;   // metres above the stadium floor
    float skyEnhance = 0.6f;
    float sunGlow = 0.9f;
    float fogDensity = 0.35f;
    float fogHeightFalloff = 0.6f;
    float fogSunScatter = 0.7f;

    // Volumetric light shafts
    float godRays = 1.5f;
    float globalIllumination = 0.5f; // one bounce of coloured light (screen space)
    float volumetricLight = 0.6f;  // sun shafts with shadows in the haze (needs the height map)
    float godRayDecay = 0.99f;

    // Bloom & lens
    float bloom = 0.09f;
    float bloomRadius = 0.85f;
    float highlightBoost = 3.5f;   // LDR -> HDR expansion of light sources
    float lensFlare = 0.25f;
    float chromaticAberration = 0.05f;
    float vignette = 0.25f;
    float filmGrain = 0.012f;

    // Exposure & tonemapping
    float exposure = 0.0f;         // EV
    float autoExposure = 0.5f;
    float contrast = 1.06f;
    float saturation = 1.08f;
    float vibrance = 0.25f;
    float temperature = 0.08f;
    float tint = 0.0f;
    float lift = 0.0f;
    float gamma = 1.0f;
    float gain = 1.0f;
    float shadowTint = 0.25f;      // split toning: cool shadows / warm highlights

    // Weather & surfaces
    float wetness = 0.0f;          // wet roads: darker, glossy, reflective
    float rain = 0.0f;             // falling rain and ripples
    float spray = 0.0f;            // water thrown up behind the car on wet roads (off by default)
    float snow = 0.0f;             // falling snow (2 = blizzard)
    float snowCover = 0.0f;        // snow lying on the ground, the grass and everything facing up
    float puddles = 0.0f;          // standing water on flat ground
    float waterSurfaces = 0.8f;    // waves, reflections and refraction on the game's water (pools, sea)
    float lightning = 0.0f;        // lightning flashes (how often)
    bool lensDrops = false;        // rain drops on the lens (only while it rains)
    float weatherSound = 0.45f;    // volume of rain and thunder (only plays with rain or lightning)
    float reflections = 0.0f;      // dry glossy reflections on the track
    float reflectionBlur = 0.35f;  // 0 = a mirror, 1 = a soft sheen (dry track)
    float grassDetail = 0.0f;      // grass blades and patches
    float mowingStripes = 0.5f;    // stadium mowing pattern
    float wind = 0.5f;

    // Cinematic (replays, video export)
    float motionBlur = 0.0f;       // fraction of the frame-to-frame camera motion
    float depthOfField = 0.0f;
    float focusDistance = 0.0f;    // metres, 0 = auto focus
    float bokehSize = 12.0f;       // max blur radius in pixels at 1080p

    // Anti-aliasing & sharpening
    bool fxaa = true;
    bool taa = false;              // temporal anti-aliasing / stabilisation
    float sharpen = 0.35f;
};

} // namespace tmshaders
