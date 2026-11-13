#pragma once
#include <d3d9.h>

// Where the frame's time goes in game, written to the log every 2000 shaded frames: the frame
// rate, the game's Cpu/Gpu synchro, the game thread's CPU time split up (perfcpu.h: the game's
// logic between frames, its rendering, the mod's own time and the part of it blocked on the GPU,
// the frame end with Present and the game's wait for the GPU, the frame start), the GPU time of
// the game's 3D scene (its main camera), of the mod (from the end of that camera to the shaded
// image, the copies around the pipeline included) and of the pipeline's most expensive passes.
// GPU timestamps (results read a few frames later, never waited for).
//
//   perf: 48 FPS (20.86 ms a frame) over 2000 shaded frames, Cpu/Gpu synchro Immediate; CPU ms a
//   frame: game logic 6.10, game rendering 2.40, TMVS 1.50 (pipeline 0.19, caster recording 0.28,
//   at the frame end 0.60; blocked 0.02), frame end 10.56 (Present 0.30, the rest the game's wait
//   for the GPU), frame start 0.30; GPU ms a frame: game 3D 1.91, mod 6.23 (passes 6.42: ...;
//   caster maps 0.00), rest of the GPU frame 14.68; game 3D drawing calls 1.95
//
// The CPU parts add up to the frame time. A long "frame end" with "Immediate" is the CPU waiting
// for the GPU to finish the frame; "Present" shows only where the mod hooks Present itself (not
// under Horizon Host, which calls the driver's Present after the mod's work).
//
// The GPU times are spans on the GPU's clock: while the GPU waits for the CPU (a CPU-bound
// frame) the game's span includes that wait. "rest" is the GPU frame less both spans: the
// game's shadow maps, HUD and menus, and idle time; near zero means the GPU is the limit. The
// game's main camera is also timed on the CPU (its drawing calls, the mod's hooks in them): when
// that is about as long as its GPU span, the GPU was waiting for the calls.
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
// RenderScale: this frame's main camera is drawn at that scale (the line says on how many frames:
// a window mixes scaled and full-size frames when the scale switches, e.g. for an F12 capture).
void renderScaled(float scale);
// Once a frame, at Present. `pipeline`: its smoothed per-pass GPU times. `castersCpuMs`: the
// recording's cumulative CPU time (casters::cpuMs).
void frame(IDirect3DDevice9* device, const Pipeline& pipeline, bool shaded, double castersCpuMs);
// false: no GPU timestamp queries at all (the log keeps the frame rate and the CPU times).
void setGpuTimestamps(bool on);
// Before a device reset: the queries go (default pool).
void release();
// Whether this frame's disjoint bracket is open (from frame() to the next Present): the
// pipeline's profiling then opens none of its own inside it.
bool bracketOpen();

} // namespace perflog
} // namespace tmshaders
