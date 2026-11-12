// Standalone mode: copied into the game folder as d3d9.dll, this DLL *is* the game's Direct3D 9,
// so no ModLoader is needed. Every export forwards to the system d3d9.dll. Direct3DCreate9 first
// makes sure the plugin is set up, so the game's very first device is already hooked.
//
// Loaded through the ModLoader under its own name, the exports are simply unused.
#include "proxy.h"
#include "plugin.h"
#include <d3d9.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "version.lib")

namespace tmshaders {
namespace proxy {

namespace {

char g_source[512] = "";
bool g_chained = false;

HMODULE selfModule() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&selfModule), &self);
    return self;
}

// A string of the file's version resource (FileDescription: "dgVoodoo 2.8.2 - Direct3D9"), "" none.
void fileString(const wchar_t* path, const wchar_t* name, char* out, size_t size) {
    out[0] = '\0';
    DWORD handle = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(path, &handle);
    if (!bytes || bytes > (1u << 20)) return;
    void* data = malloc(bytes);
    if (!data) return;
    struct Translation {
        WORD language, codePage;
    }* translations = nullptr;
    UINT length = 0;
    if (GetFileVersionInfoW(path, 0, bytes, data) &&
        VerQueryValueW(data, L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &length) && length >= sizeof(Translation)) {
        wchar_t key[64];
        swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\%s", translations[0].language, translations[0].codePage, name);
        wchar_t* text = nullptr;
        if (VerQueryValueW(data, key, reinterpret_cast<void**>(&text), &length) && text && length)
            WideCharToMultiByte(CP_UTF8, 0, text, -1, out, static_cast<int>(size), nullptr, nullptr);
    }
    free(data);
}

// d3d9.chain.dll next to this DLL: loaded when it is a Direct3D 9 and not a copy of this mod.
HMODULE loadChain(char* note, size_t noteSize) {
    note[0] = '\0';
    HMODULE self = selfModule();
    wchar_t path[MAX_PATH] = {};
    const DWORD n = self ? GetModuleFileNameW(self, path, MAX_PATH) : 0;
    if (!n || n >= MAX_PATH - 20) return nullptr;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return nullptr;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"d3d9.chain.dll");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return nullptr;
    char description[160], product[64];
    fileString(path, L"FileDescription", description, sizeof(description));
    fileString(path, L"ProductName", product, sizeof(product));
    // A copy of this mod (renamed by hand) would forward to itself: not loaded at all.
    if (!strcmp(product, "TM Vibrant Shaders")) {
        snprintf(note, noteSize, "; d3d9.chain.dll left out (a copy of TM Vibrant Shaders)");
        return nullptr;
    }
    HMODULE chain = LoadLibraryW(path);
    if (!chain) {
        snprintf(note, noteSize, "; d3d9.chain.dll is there but didn't load (error %lu)", GetLastError());
        return nullptr;
    }
    const bool d3d9 = GetProcAddress(chain, "Direct3DCreate9") != nullptr;
    if (chain == self || GetProcAddress(chain, "TMVS_AttachDevice") || !d3d9) {
        snprintf(note, noteSize, "; d3d9.chain.dll left out (%s)", d3d9 ? "a copy of this mod" : "no Direct3DCreate9 in it");
        if (chain != self) FreeLibrary(chain);
        return nullptr;
    }
    snprintf(note, noteSize, "d3d9.chain.dll in the game folder (%s)", description[0] ? description : "no version description");
    return chain;
}

} // namespace

HMODULE systemD3D9() {
    static HMODULE module = [] {
        char note[256];
        if (HMODULE chain = loadChain(note, sizeof(note))) {
            g_chained = true;
            snprintf(g_source, sizeof(g_source), "%s", note);
            return chain;
        }
        wchar_t path[MAX_PATH] = {};
        const UINT n = GetSystemDirectoryW(path, MAX_PATH); // SysWOW64 for this 32-bit process
        if (!n || n > MAX_PATH - 16) return static_cast<HMODULE>(nullptr);
        wcscat_s(path, L"\\d3d9.dll");
        char narrow[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);
        snprintf(g_source, sizeof(g_source), "the system's d3d9.dll (%s)%s", narrow, note);
        return LoadLibraryW(path);
    }();
    return module;
}

const char* d3d9Source() {
    return g_source;
}

bool chained() {
    return g_chained;
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
