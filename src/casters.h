#pragma once
#include "gfx.h"
#include <d3d9.h>
#include <vector>

// CasterShadows: light-space depth maps drawn from the game's own static geometry, for the
// long shadows of the sun and the black hole.
//
// The game culls per camera, so geometry off screen is never drawn for us. Instead every
// opaque draw of the main camera is remembered once it proved static (the same mesh at the
// same place in two frames half a second apart): its buffers, its draw call and its place in
// the world. The place comes from the game's own vertex constants: c0..c3 hold
// GbxVisualPrCamera (mesh -> clip) in the game's material shaders; clip x, y and w undo the
// camera's projection without its near plane, the view's inverse gives mesh -> world.
// The remembered meshes are drawn into two maps: one over the whole track (coarse, drawn once
// until the light turns or the track grows past it) and one around the camera (fine, drawn
// again in the background when the camera has moved on). Shadows don't pop with the camera,
// keep the meshes' real shape (overhangs, thin poles) and reach as far as the maps.
namespace tmshaders {
namespace casters {

// One map, for the shaders. rows: world -> map u, v (0..1, texel centres included) and depth
// (0..1 as stored in the map).
struct MapInfo {
    IDirect3DTexture9* texture = nullptr; // INTZ: depth along the light, 0..1, in .r; null: none
    float rows[3][4] = {};
    float texel[2] = {};  // size of a texel in uv
    float bias = 0.0f;    // depth (0..1 units) a receiver must lie behind the stored depth
};

struct Maps {
    MapInfo nearMap; // around the camera, fine
    MapInfo farMap;  // the whole track, coarse
};

// Every indexed draw of the main camera asks first (before looking at its state): false, it isn't
// checked this frame (a later frame checks it, see frameDone) or the recording is paused.
bool takeTurn();

// One indexed draw of the main camera, with the game's state bound. view / projection: the
// camera's (SetTransform). Only opaque draws of static materials should come here.
void recordDraw(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start, UINT count,
                const float view[16], const float projection[16]);

// Once a frame (Present): the next frame's share of the draws to check (recordDraw spreads its
// work over frames on maps with many draws).
void frameDone();
// While no pipeline uses the maps (long shadows and volumetric light off, no sun), the draws
// aren't checked at all: what is kept stays, recording goes on where it was when they're used again.
void pauseRecording(bool paused);
// CPU time of the recording and of drawing the maps since the start, ms (the perf log).
double cpuMs();

// Draws what is new into the maps (inside the pipeline's state save). sunWorld: towards the
// light; camera: world position. False while neither map is complete (the height map's
// shadows stay).
bool update(IDirect3DDevice9* device, const float sunWorld[3], const float camera[3], Maps& out);

// A new map, a block removed in the editor, or the setting went off: every remembered mesh is
// let go (the game's buffers with it).
// `log`: say so in the log (the editor removes blocks one by one: not every time).
void reset(const char* why, bool log = true);
// Device reset or shutdown: also the maps themselves (default pool).
void releaseDevice();

// The caster maps' shaders, for gfx::precompile (compiled ahead, off the render thread).
void runtimeShaders(std::vector<gfx::ShaderSource>& out);

// How many meshes are kept now (tests, the log).
unsigned keptMeshes();

} // namespace casters
} // namespace tmshaders
