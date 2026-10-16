#pragma once
#include <d3d9.h>

// CasterShadows (experimental): a light-space depth map drawn from the game's own static
// geometry, for the long shadows of the sun and the black hole.
//
// The game culls per camera, so geometry off screen is never drawn for us. Instead every
// opaque draw of the main camera is remembered once it proved static (the same mesh at the
// same place in two frames half a second apart): its buffers, its draw call and its place in
// the world. The place comes from the game's own vertex constants: c0..c3 hold
// GbxVisualPrCamera (mesh -> clip) in the game's material shaders, so mesh -> world is that
// times the inverse of the camera's view-projection. The remembered meshes are drawn into
// one map over the whole track, each only once (the map is kept until the light turns or the
// track grows past it): shadows don't pop with the camera, have the meshes' real shape
// (overhangs, thin poles) and reach as far as the map.
namespace tmshaders {
namespace casters {

// Where the map is, for the shaders. rows: world -> map u, v (0..1, texel centres included)
// and depth (0..1 as stored in the map).
struct MapInfo {
    IDirect3DTexture9* texture = nullptr; // INTZ: depth along the light, 0..1, in .r
    float rows[3][4] = {};
    float texel[2] = {};  // size of a texel in uv
    float bias = 0.0f;    // depth (0..1 units) a receiver must lie behind the stored depth
};

// One indexed draw of the main camera, with the game's state bound. view / projection: the
// camera's (SetTransform). Only opaque draws of static materials should come here.
void recordDraw(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start, UINT count,
                const float view[16], const float projection[16]);

// Draws what is new into the map (inside the pipeline's state save). sunWorld: towards the
// light. False while no complete map exists (the height map's shadows stay).
bool update(IDirect3DDevice9* device, const float sunWorld[3], MapInfo& out);

// A new map, or the setting went off: every remembered mesh is let go (the game's buffers
// with it).
void reset(const char* why);
// Device reset or shutdown: also the map itself (default pool).
void releaseDevice();

} // namespace casters
} // namespace tmshaders
