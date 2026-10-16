#pragma once
#include <cstddef>
#include <cstdint>

namespace tmshaders {
namespace gameshaders {

// The game compiles its shaders at run time (d3dx9_30: D3DXCompileShader for HLSL,
// D3DXAssembleShader for asm). When the folder <data dir>\gameshaders exists, every shader
// it builds is written there once: source, defines, included files and the disassembly of
// the result. Read offline to learn how the game lights a surface (lightmap scale, whether
// its output is cut to 1.0) without guessing from pixels.
void install();

// A shader's identity: FNV-1a of its bytecode without the comment tokens (the constant table
// may or may not be kept). Written into each dump ("// bytecode ...") and printed for each
// draw of an F11 trace, so a trace's draws can be matched to the dumped sources.
uint64_t bytecodeHash(const void* code, size_t bytes);

} // namespace gameshaders
} // namespace tmshaders
