#pragma once
#include <d3d9.h>

// Where the frame's time goes in game, written to the log every 2000 shaded frames: the frame
// rate, the GPU time of the game's 3D scene (its main camera), of the mod (from the end of that
// camera to the shaded image, the copies around the pipeline included) and of the pipeline's
// most expensive passes, and the mod's CPU time on the render thread. GPU timestamps
// (results read a few frames later, never waited for).
//
// The GPU times are spans on the GPU's clock: while the GPU waits for the CPU (a CPU-bound
// frame) the game's span includes that wait. "rest" is the GPU frame less both spans: the
// game's shadow maps, HUD and menus, and idle time; near zero means the GPU is the limit.
namespace tmshaders {
class Pipeline;
namespace perflog {

enum class Mark {
    GameBegin, // the main camera starts drawing
    GameEnd,   // it ends (before the mod's copies and the pipeline)
    ModEnd,    // the shaded image is done
    Count
};

// At the main camera's begin and end (first main camera of the frame only).
void mark(IDirect3DDevice9* device, Mark mark);
// CPU time the mod spent this frame outside the game (the pipeline's render calls).
void addCpu(double ms);
// Once a frame, at Present. `pipeline`: its smoothed per-pass GPU times. `castersCpuMs`: the
// recording's cumulative CPU time (casters::cpuMs).
void frame(IDirect3DDevice9* device, const Pipeline& pipeline, bool shaded, double castersCpuMs);
// Before a device reset: the queries go (default pool).
void release();
// Whether this frame's disjoint bracket is open (from frame() to the next Present): the
// pipeline's profiling then opens none of its own inside it.
bool bracketOpen();

} // namespace perflog
} // namespace tmshaders
