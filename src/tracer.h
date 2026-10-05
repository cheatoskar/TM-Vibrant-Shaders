#pragma once
#include <d3d9.h>

// One-frame D3D9 call tracer. Arm it (F11 or tracer::arm()) and the next full
// frame (Present -> Present) is written to <Documents>\TrackMania\TMVS\trace_N.txt.
// Used to understand the engine's render flow; costs nothing while disarmed.
namespace tmshaders {
namespace tracer {

void arm(int delayFrames = 0);
bool tracing();

// Called from Present: advances the armed/active state machine.
void onPresent(IDirect3DDevice9* device);

void event(const char* fmt, ...);
const char* describe(IDirect3DSurface9* surface);
const char* describe(IDirect3DBaseTexture9* texture);
const char* formatName(D3DFORMAT format);

} // namespace tracer
} // namespace tmshaders
