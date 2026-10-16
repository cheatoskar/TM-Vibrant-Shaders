#pragma once
#include <d3d9.h>
#include <cstddef>
#include <cstdint>

namespace tmshaders {
namespace gameshaders {

// The game compiles its shaders at run time (d3dx9_30: D3DXCompileShader for HLSL,
// D3DXAssembleShader for asm). When the folder <data dir>\gameshaders exists, every shader
// it builds is written there once: source, defines, included files and the disassembly of
// the result. Read offline to learn how the game lights a surface (lightmap scale, whether
// its output is cut to 1.0) without guessing from pixels.
// With GameGBuffer on at the game's start, its pixel shaders are also patched on their way to
// the compiler (gbufpatch.h): those with a world normal write it into COLOR1.
void install();

// A shader's identity: FNV-1a of its bytecode without the comment tokens (the constant table
// may or may not be kept). Written into each dump ("// bytecode ...") and printed for each
// draw of an F11 trace, so a trace's draws can be matched to the dumped sources.
uint64_t bytecodeHash(const void* code, size_t bytes);

// GameGBuffer: the compile hook patches the game's pixel shaders (the setting was on at start).
bool patching();
// Every pixel shader created on the device (CreatePixelShader): notes whether it is one of the
// patched ones. A pointer the driver hands out again is noted again.
void notePixelShader(IDirect3DPixelShader9* shader, const DWORD* code);
// The shader writes the normal into COLOR1 (it was patched).
bool writesNormal(IDirect3DPixelShader9* shader);
// Shaders patched so far (the log's summary, the G-buffer's draw statistics).
int patchedCount();
// Once a frame: logs how many shaders were patched and why the others weren't, once the
// game has compiled a batch of them (a few seconds after its last compile).
void tick();

} // namespace gameshaders
} // namespace tmshaders
