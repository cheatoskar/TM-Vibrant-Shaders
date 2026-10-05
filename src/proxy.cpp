// Standalone mode: copied into the game folder as d3d9.dll, this DLL *is* the game's Direct3D 9,
// so no ModLoader is needed. Every export forwards to the system d3d9.dll. Direct3DCreate9 first
// makes sure the plugin is set up, so the game's very first device is already hooked.
//
// Loaded through the ModLoader under its own name, the exports are simply unused.
#include "proxy.h"
#include "plugin.h"
#include <d3d9.h>

namespace tmshaders {
namespace proxy {

HMODULE systemD3D9() {
    static HMODULE module = [] {
        wchar_t path[MAX_PATH] = {};
        const UINT n = GetSystemDirectoryW(path, MAX_PATH); // SysWOW64 for this 32-bit process
        if (!n || n > MAX_PATH - 16) return static_cast<HMODULE>(nullptr);
        wcscat_s(path, L"\\d3d9.dll");
        return LoadLibraryW(path);
    }();
    return module;
}

IDirect3D9* createDirect3D9(UINT sdkVersion) {
    using Fn = IDirect3D9*(WINAPI*)(UINT);
    HMODULE module = systemD3D9();
    auto fn = module ? reinterpret_cast<Fn>(GetProcAddress(module, "Direct3DCreate9")) : nullptr;
    return fn ? fn(sdkVersion) : nullptr;
}

} // namespace proxy
} // namespace tmshaders

using namespace tmshaders;

namespace {

// Functions forwarded as-is: resolved once, then jumped to with the caller's arguments untouched.
#define TMVS_FORWARDS(X)                                                                                                   \
    X(D3DPERF_BeginEvent)                                                                                                  \
    X(D3DPERF_EndEvent)                                                                                                    \
    X(D3DPERF_GetStatus)                                                                                                   \
    X(D3DPERF_QueryRepeatFrame)                                                                                            \
    X(D3DPERF_SetMarker)                                                                                                   \
    X(D3DPERF_SetOptions)                                                                                                  \
    X(D3DPERF_SetRegion)                                                                                                   \
    X(DebugSetLevel)                                                                                                       \
    X(DebugSetMute)                                                                                                        \
    X(Direct3D9EnableMaximizedWindowedModeShim)                                                                            \
    X(Direct3DCreate9On12)                                                                                                 \
    X(Direct3DCreate9On12Ex)                                                                                               \
    X(Direct3DShaderValidatorCreate9)                                                                                      \
    X(PSGPError)                                                                                                           \
    X(PSGPSampleTexture)

#define TMVS_DECLARE_POINTER(name) void* g_##name = nullptr;
TMVS_FORWARDS(TMVS_DECLARE_POINTER)

extern "C" void __cdecl resolveForwards() {
    static bool resolved = false;
    if (resolved) return;
    HMODULE module = proxy::systemD3D9();
#define TMVS_RESOLVE(name) g_##name = module ? reinterpret_cast<void*>(GetProcAddress(module, #name)) : nullptr;
    TMVS_FORWARDS(TMVS_RESOLVE)
    resolved = true;
}

} // namespace

extern "C" {

IDirect3D9* WINAPI TMVS_Direct3DCreate9(UINT sdkVersion) {
    plugin::ensureBooted();
    return proxy::createDirect3D9(sdkVersion);
}

HRESULT WINAPI TMVS_Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** out) {
    plugin::ensureBooted();
    using Fn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    HMODULE module = proxy::systemD3D9();
    auto fn = module ? reinterpret_cast<Fn>(GetProcAddress(module, "Direct3DCreate9Ex")) : nullptr;
    return fn ? fn(sdkVersion, out) : D3DERR_NOTAVAILABLE;
}

// Naked stubs keep every register and stack argument as the caller left them.
#define TMVS_DEFINE_STUB(name)                                                                                             \
    __declspec(naked) void TMVS_##name() {                                                                                 \
        __asm pushad                                                                                                       \
        __asm call resolveForwards                                                                                         \
        __asm popad                                                                                                        \
        __asm jmp dword ptr [g_##name]                                                                                     \
    }
TMVS_FORWARDS(TMVS_DEFINE_STUB)

} // extern "C"
