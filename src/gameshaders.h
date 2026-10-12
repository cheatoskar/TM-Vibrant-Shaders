#pragma once

namespace tmshaders {
namespace gameshaders {

// The game compiles its shaders at run time (d3dx9_30: D3DXCompileShader for HLSL,
// D3DXAssembleShader for asm). When the folder <data dir>\gameshaders exists, every shader
// it builds is written there once: source, defines, included files and the disassembly of
// the result. Read offline to learn how the game lights a surface (lightmap scale, whether
// its output is cut to 1.0) without guessing from pixels.
void install();

} // namespace gameshaders
} // namespace tmshaders
