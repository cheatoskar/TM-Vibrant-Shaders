#pragma once
#include <d3d9.h>

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

// One indexed draw of the main camera, with the game's state bound. view / projection: the
// camera's (SetTransform). Only opaque draws of static materials should come here.
void recordDraw(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start, UINT count,
                const float view[16], const float projection[16]);

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

// How many meshes are kept now (tests, the log).
unsigned keptMeshes();

} // namespace casters
} // namespace tmshaders
