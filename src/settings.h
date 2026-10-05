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

    // Lighting (screen-space, reconstructed from depth)
    float aoStrength = 1.0f;
    float aoRadius = 1.6f;          // metres
    float shadowStrength = 0.75f;   // screen-space sun shadows
    float shadowLength = 9.0f;      // metres
    float sunLight = 0.45f;         // directional sun relighting (N.L)
    float ambientTint = 0.55f;      // sky-coloured fill in shadow
    float sunColor[3] = {1.00f, 0.86f, 0.66f};
    float gameSunColor = 0.7f;      // how much of the game's own light colour (time of day) to use
    float skyColor[3] = {0.55f, 0.72f, 1.00f};
    float sunElevationOverride = -1.0f; // degrees; < 0 uses the game's light
    float sunAzimuthOverride = 0.0f;

    // Sky & atmosphere
    int skyMode = 0;               // 0 game, 1 clear sky, 2 starry night, 3 black hole, 4 aurora
    float skyNight = -1.0f;        // scene darkening for night skies (< 0 = automatic)
    float skyRotation = 0.0f;      // degrees: rotates galaxy / black hole / aurora
    float skyBrightness = 1.0f;
    float cloudAmount = 0.5f;
    float starAmount = 1.0f;
    float skyEffectSize = 1.0f;    // black hole size
    float planetSize = 0.0f;       // ringed planet in the space skies (0 = none)
    float skyEnhance = 0.6f;
    float sunGlow = 0.7f;
    float fogDensity = 0.35f;
    float fogHeightFalloff = 0.6f;
    float fogSunScatter = 0.7f;

    // Volumetric light shafts
    float godRays = 0.55f;
    float godRayDecay = 0.965f;

    // Bloom & lens
    float bloom = 0.07f;
    float bloomRadius = 0.85f;
    float highlightBoost = 3.0f;   // LDR -> HDR expansion of light sources
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

    // Anti-aliasing & sharpening
    bool fxaa = true;
    float sharpen = 0.35f;
};

} // namespace tmshaders
