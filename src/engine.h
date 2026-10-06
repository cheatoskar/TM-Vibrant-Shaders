#pragma once
#include <d3d9.h>

// Hooks into the TrackMania Forever "Vision" engine (CVisionViewportDx9).
//
// Addresses come from TmForever.map (build 2011-01-28, identical code in the
// TMLoader 2.12.0 executable). Every slot is verified against the expected
// function address before patching, so an unknown executable simply leaves
// the engine untouched and the plugin falls back to Present-time rendering.
namespace tmshaders {
namespace engine {

struct Matrix4 {
    float m[4][4];
};

// Camera data captured for the camera currently being rendered.
struct CameraInfo {
    void* camera = nullptr;
    Matrix4 view{};        // world -> view, row-vector convention (D3D)
    Matrix4 projection{};  // view -> clip, row-vector convention (D3D)
    float location[12]{};  // raw GmIso4 the engine passed (3x3 rotation + translation)
    bool hasView = false;
    bool hasProjection = false;
};

struct Callbacks {
    void (*frameBegin)() = nullptr;
    void (*cameraBegin)(void* camera) = nullptr;
    void (*cameraEnd)(void* camera, const CameraInfo& info) = nullptr;
    void (*overlayBegin)() = nullptr;
    void (*overlayEnd)() = nullptr;
    void (*frameEnd)() = nullptr;
};

bool install(const Callbacks& callbacks);
// Overrides the game's shadow quality (-1 = keep the game's setting).
void forceShadows(int mode);
bool active();

// Camera info of the camera currently between RenderCameraBegin/End (or the last one).
const CameraInfo& currentCamera();

// True while a MediaTracker clip plays (replays, intros, the replay editor) or the video
// export runs: the moments for motion blur and depth of field. False while driving.
bool cinematicActive();

// Sub-pixel offset (NDC) added to every perspective projection the engine computes from now
// on, for TAA: each frame samples the pixels at a slightly different spot. 0, 0 = off.
void setProjectionJitter(float x, float y);

// The two water heights of this map: the water blocks' (always the same) and the sea level
// (from the map load or the plane the game renders the water reflection for; = blockY without
// a sea). False until a map load was seen (hooks unavailable): the shaders then guess by colour.
bool waterHeights(float& blockY, float& seaY);
// Which of the cinematic hooks fired since the last call (bit 0 clip player, 1 clip viewer,
// 2 video export), for the log.
unsigned cinematicSources();

} // namespace engine
} // namespace tmshaders
