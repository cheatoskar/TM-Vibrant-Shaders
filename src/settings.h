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
    bool gameMaterials = true;     // lamps, cars, glass and grass from the game's own materials (not guessed from pixels)
    bool gameSkyClouds = false;    // night skies over sunset maps: the game's clouds and their mirror image in the sea stay (photos)
    bool gameNight = true;         // night skies over day maps: the game lights the map at midnight (map load), no baked sun
    int engineMoon = 1;            // night maps under the black hole: the game's moon 0 as it is, 1 shines from the black hole (lightmap, specular, car), 2 off (ambient and lamps stay)
    bool disableGameFx = true;     // skip the game's bloom and colour filter (launcher "FX") while the mod shades
    bool gameHdr = true;           // the game draws its 3D image into a float target: light above 1.0 keeps its texture (car body, floodlit white)
    bool engineLights = true;      // with GameHdr: light sources as the game draws them (lamp materials, its additive glows), nothing guessed from the image
    bool cleanHighlights = true;   // the game's clipped white maps to one smooth result (no frost on floodlit surfaces); off = 1.4.0
    bool casterShadows = true;     // long shadows from a light-space map of the game's own static meshes (casters.cpp)
    bool casterAlphaTest = true;   // CasterShadows also keeps alpha-tested meshes (fences, grids) and draws them with the game's alpha test: lattice shadows; off = they cast none
    bool shadeMenuBars = true;     // replay menu: the rows under its bars show the shaded image (mirrored) instead of leftovers of earlier frames
    bool cleanWater = true;        // the game's water mirrors only what it can see, unblurred (no blotches of the ramps over it); off = as before
    bool gameGBuffer = false;      // the game's material shaders also write their normal (gbufpatch.cpp): AO, shadows, reflections and light on the real normal (needs GameHdr; patched from the start)
    // Neon light trail behind the car, kept for the whole run (0 = off).
    float neonTrail = 0.0f;
    float trailColor[3] = {0.1f, 0.65f, 1.0f};
    float trailWidth = 0.2f;       // m
    bool trailTyres = true;        // two lines from the rear tyres, else one from the middle
    float trailDuration = 0.0f;    // s until a piece fades out, 0 = the whole run
    int trailKeepRuns = 0;         // earlier runs still drawn (dimmer) after a restart, 0 = only this one
    bool taaJitter = false;         // TAA samples a different sub-pixel spot every frame (sharper, smoother)
    bool cinematicOnlyInReplays = true; // motion blur and DOF only in replays, intros and the video export
    bool videoExportBest = true;   // the game's video export: full quality, effects on the video's clock, every supersample shaded
    int videoCodec = 1;            // the export's file: 0 the game's AVI (its codec dialog), 1 MP4 H.264, 2 MP4 HEVC (Media Foundation)
    float videoBitrate = 0.0f;     // MP4: Mbit/s, 0 = from the size and frame rate (4K 30 fps: 50 H.264, 30 HEVC)
    bool autoQuality = true;       // lower the effect quality when the frame rate drops below the target
    float targetFps = 60.0f;

    // Lighting (screen-space, reconstructed from depth)
    float aoStrength = 1.0f;
    float aoRadius = 1.6f;          // metres
    float shadowStrength = 0.95f;   // screen-space sun shadows
    float shadowLength = 9.0f;      // metres
    float longShadows = 0.8f;       // world-space height map shadows (long, off-screen casters)
    float longShadowRange = 120.0f; // metres
    int longShadowQuality = 1;      // 0: traced in 24 steps (as before 2026-10-16), 1: every map cell along the ray (light grid)
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
    float hazeDistance = 1.0f;      // < 1: the haze closes in nearer the camera (less visibility)

    // Volumetric light shafts
    float godRays = 1.5f;
    float globalIllumination = 0.5f; // one bounce of coloured light (screen space)
    float volumetricLight = 0.6f;  // sun shafts with shadows in the haze (needs the height map)
    float godRayDecay = 0.99f;

    // Bloom & lens
    float bloom = 0.09f;
    float bloomRadius = 0.85f;
    float highlightBoost = 3.5f;   // LDR -> HDR expansion of light sources
    float borderGlow = 1.0f;       // EngineLights: the neon strips' (road borders') glow, 1 = as the guess let them glow
    float lampGlare = 0.0f;        // EngineLights: this part of the lamps' light leaves their core and spreads wide and soft around them (0 = off)
    float lampHaze = 0.0f;         // EngineLights: the light the haze takes from the lamps glows in the air around them (1 = all of it, 0 = off)
    bool lampSkyHalos = false;     // EngineLights at night: the lamps' halos also over the sky, not cut at the edge of what the lamp sits on
    float floodlights = 1.0f;      // EngineLights: the strength of floodlights and lamps (the game's glows) and the light they spill, apart from the neon borders and HighlightBoost (1 = as before)
    float lensFlare = 0.25f;
    float chromaticAberration = 0.05f;
    float vignette = 0.25f;
    float filmGrain = 0.012f;

    // Exposure & tonemapping
    float exposure = 0.0f;         // EV
    float autoExposure = 0.5f;
    float contrast = 1.06f;
    float toneKnee = 0.5f;         // tone curve: linear up to here, then the soft shoulder (higher = brighter highlights)
    int toneCurve = 1;             // ManiaPlanet: 0 the engine's curve, 1 the mod's (above), 2 ACES
    float nightSurfaces = 0.8f;    // night maps: floodlit surfaces kept at this part of the game's white (lamps always in full)
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
    float waterSurfaces = 1.0f;   // waves, reflections and refraction on the game's water (pools, sea)
    float lightning = 0.0f;        // lightning flashes (how often)
    bool lensDrops = false;        // rain drops on the lens (only while it rains)
    bool weatherShelter = true;    // no rain or snow under roofs, bridges and in buildings
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
    int dofAutofocus = 0;          // the auto focus sits on 0 the middle of the screen, 1 the player's car (where the engine draws it)
    float bokehSize = 12.0f;       // max blur radius in pixels at 1080p

    // Anti-aliasing & sharpening
    bool fxaa = true;
    bool taa = true;               // temporal anti-aliasing / stabilisation (smooths the long shadows' steps; jitter stays off)
    float sharpen = 0.35f;
};

} // namespace tmshaders
