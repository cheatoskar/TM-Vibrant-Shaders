#pragma once
// The game's main camera for photo zoom, hi-res photo tiles and TAA jitter (Settings page, panel
// ManiaPlanet, *Game camera*; off by default: it writes into the game's memory).
//
// ManiaPlanet's CHmsCamera (class 0x06001000, 0x338 bytes) has a FovRect (reflection: FovRectEnable
// at 0x30C, FovRectMin / FovRectMax at 0x2F0 / 0x2F8, read through its getter's jump table): where
// the camera's whole field of view lands, in the coordinates of its DrawRect (per render thread at
// 0x190 + 0x28 x the thread's slot). The engine's frustum (0x1401CEBF0) maps FovRect onto DrawRect
// and takes the part of the view that falls into DrawRect; its own tiled render (0x1405D08C0) sets
// FovRect for each tile the same way. So a FovRect larger than DrawRect zooms in, a shifted one
// moves the view, and the projection, culling and every constant derived from them (SceneV, the
// passes' RasterToCamera, the shadow splits) follow, as the engine computes them itself.
// The main camera: CGameCtnApp::GameCamera (0x2E8, CGameCamera 0x03023000) -> SceneCamera (0xF0,
// CSceneCamera 0x0A00A000, made in its constructor) -> its CHmsCamera (0x48, made in CSceneCamera's
// constructor), each checked by its class id. The setters' code (0x1401CE940 / 0x1401CE960) is
// checked first: another build keeps everything off.
//
// What the engine did is measured, not assumed: the SceneV projection of every frame (P00 / P11 the
// scale, P02 / P12 the offset in NDC) against the one before the change. Which way FovRect's axes
// run against the screen is learned from that (the log says so), and TAA takes the jitter the
// projection really has.
#include <cstdint>

namespace tmvs_mp {
namespace gamecamera {

// NDC' = scale x NDC + offset (x right, y up), applied to the main camera's view.
struct Want {
    float scale[2] = {1.0f, 1.0f};
    float offset[2] = {0.0f, 0.0f};
    bool jitter = false; // the offset is a TAA jitter (sub-pixel)
    bool any() const { return jitter || scale[0] != 1.0f || scale[1] != 1.0f || offset[0] != 0.0f || offset[1] != 0.0f; }
    bool scaled() const { return scale[0] != 1.0f || scale[1] != 1.0f; }
};

// Once a frame at Present (render thread): `allowed` is the Game camera option. Writes the FovRect
// for the next frame, or puts the game's own back when nothing is wanted (or it's not allowed).
void apply(const Want& want, bool allowed);

// The main camera's projection of the frame just rendered (row-major camera -> clip, the decoded
// SceneV CameraProjection), once a frame with a camera.
void observe(const float* projection);

// The last frame's projection is the wanted one (to the settle frames of a hi-res tile): its scale
// and offset match `want` (the offset by the axes' learned direction).
bool matches(const Want& want);

// TAA: this frame's jitter as the image's uv shift (x right, y down), measured from the projection;
// 0 when the mod isn't jittering (or a tile or zoom offset is in it).
void jitterUv(const float* projection, float out[2]);

// The mod's FovRect is in the camera now.
bool active();
// A line for the menu's status (ManiaPlanet panel).
const char* status();

} // namespace gamecamera
} // namespace tmvs_mp
