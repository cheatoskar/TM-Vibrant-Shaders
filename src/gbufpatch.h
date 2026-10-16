#pragma once
#include <windows.h>
#include <d3dcommon.h>
#include <cstddef>
#include <cstdint>
#include <string>

namespace tmshaders {
namespace gbufpatch {

// GameGBuffer: the game's material pixel shaders also write the surface's world normal and its
// distance from the eye into a second render target (COLOR1). TMNF compiles them from HLSL at
// run time (d3dx9_30 D3DXCompileShader), so the source is patched on its way to the compiler.
// The patch adds an output and one line before the shader's `return Output;`; everything the
// shader computed before stays as it was. Only shaders that name a world normal and an eye
// vector are patched; any doubt keeps the game's own shader.

// d3dx9 types, opaque: the exe's import is replaced, d3dx9's headers aren't needed.
struct Macro {
    const char* name;
    const char* definition;
};

// ID3DXInclude: two __stdcall virtual methods, no IUnknown.
struct Include {
    virtual HRESULT __stdcall Open(int type, const char* name, const void* parent, const void** data, UINT* bytes) = 0;
    virtual HRESULT __stdcall Close(const void* data) = 0;
};

// D3DXCompileShader. The blobs are ID3DXBuffer (the same layout as ID3DBlob), constants an
// ID3DXConstantTable (an IUnknown).
using CompileFn = HRESULT(WINAPI*)(const char* source, UINT length, const Macro* defines, Include* include, const char* function,
                                   const char* profile, DWORD flags, ID3DBlob** shader, ID3DBlob** errors, void** constants);

// The text transform. `patched` false: `reason` says why the shader stays as it is.
struct Patch {
    bool patched = false;
    std::string source;      // the patched source (patched only)
    std::string reason;      // why not, or what was used ("normal nNormal, eye EyeToPos")
    // The inputs the patch reads (tests): the eye vector's semantic, the normal's if it is an
    // input itself (empty: a variable of the shader).
    std::string eyeSemantic, normalSemantic;
};
Patch patchSource(const char* source, size_t length, const char* profile);

// What the compile wrapper did with one shader.
struct Outcome {
    enum Kind { Untouched, Patched, Skipped, Failed } kind = Untouched;
    std::string detail;      // reason (Skipped / Failed: with the compiler's message) or the patch used
};

// The game's compile, and with `patch` on also the patched source. The patched result replaces
// the game's only when it compiled, writes COLOR1 and keeps every constant and sampler of the
// game's shader in its register: the game sets them by the table it got. Otherwise the game
// gets exactly what its own compile returned. With `patch` off this is the plain call.
HRESULT compile(CompileFn fn, bool patch, const char* source, UINT length, const Macro* defines, Include* include, const char* function,
                const char* profile, DWORD flags, ID3DBlob** shader, ID3DBlob** errors, void** constants, Outcome* outcome);

// Bytecode helpers (shader model 2 and 3; 1.x has no instruction lengths and returns false/0).
// The size of a shader from its tokens (CreatePixelShader gets no size), 0 if unknown.
size_t bytecodeSize(const DWORD* code);
// Whether an instruction writes the colour output register `index` (oC1).
bool writesColorOutput(const void* code, size_t bytes, int index);
// Whether every constant of `original`'s constant table (name, register set, index, count) is
// the same in `patched`, and patched has no other. False without a table. `why` says what moved.
bool sameConstants(const void* original, size_t originalBytes, const void* patched, size_t patchedBytes, std::string* why);
// Instructions without comments, dcl and def (for logs and tests; texture and arithmetic alike).
int instructionCount(const void* code, size_t bytes);

} // namespace gbufpatch
} // namespace tmshaders
