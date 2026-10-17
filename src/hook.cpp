#include "hook.h"
#include "inlinehook.h"
#include "gameshaders.h"
#include "proxy.h"
#include "tracer.h"
#include "log.h"
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <intrin.h>
#include <vector>

#pragma comment(lib, "d3d9.lib")

namespace tmshaders {
namespace hook {
namespace {

// IDirect3DDevice9 vtable slots.
enum Slot : int {
    kReset = 16,
    kPresent = 17,
    kCreateTexture = 23,
    kCreateVolumeTexture = 24,
    kCreateCubeTexture = 25,
    kCreateVertexBuffer = 26,
    kCreateIndexBuffer = 27,
    kCreateRenderTarget = 28,
    kCreateDepthStencilSurface = 29,
    kStretchRect = 34,
    kSetRenderTarget = 37,
    kGetRenderTarget = 38,
    kSetDepthStencilSurface = 39,
    kBeginScene = 41,
    kEndScene = 42,
    kClear = 43,
    kSetTransform = 44,
    kSetViewport = 47,
    kGetViewport = 48,
    kSetLight = 51,
    kLightEnable = 53,
    kSetTexture = 65,
    kSetScissorRect = 75,
    kGetScissorRect = 76,
    kDrawPrimitive = 81,
    kDrawIndexedPrimitive = 82,
    kDrawPrimitiveUP = 83,
    kDrawIndexedPrimitiveUP = 84,
    kSetVertexShader = 92,
    kSetVertexShaderConstantF = 94,
    kCreatePixelShader = 106,
    kSetPixelShader = 107,
    kSetPixelShaderConstantF = 109,
    kSlotCount = 119
};

// How the device methods are hooked. Inline (a jump at each method's first bytes, inlinehook.h):
// the device's method table can be rewritten under us (with CenterSpeed loaded, all of the game's
// device table held d3d9's own functions again after the first frame: no Present, no keys, no
// effects), the code of the methods stays. A method whose first bytes can't be moved falls back to
// its vtable slot, and that slot is written again when the table is found rewritten (diagFrame).
//
// Per slot, every method address hooked (one, unless devices with different method tables came
// along), with the trampoline that calls on into it.
struct SlotHooks {
    static constexpr int kMax = 4;
    inlinehook::Hook hooks[kMax];
    int count = 0;
};
SlotHooks g_slots[kSlotCount];
bool g_inline = false;               // at least one method hooked inline
void** g_vtable = nullptr;           // the game device's table (vtable fallback slots are written there)
bool g_vtableSlot[kSlotCount] = {};  // this slot is hooked through g_vtable
void* g_tableOriginal[kSlotCount] = {}; // what such a slot held before us
void* g_original[kSlotCount] = {};   // what the game's device calls on into (trampoline or the table's old pointer)
DeviceCallbacks g_callbacks;
int g_internal = 0;
IDirect3DDevice9* g_device = nullptr; // the game's device, for the memory report
// The device the game created (null until then). Other devices (another mod's helper device,
// CenterSpeed's dummy, Twinkie's) run into the same hooked code: they pass through untouched.
IDirect3DDevice9* g_gameDevice = nullptr;
bool foreignDevice(IDirect3DDevice9* device) { return g_gameDevice && device != g_gameDevice; }

// A call of the game's own (not the plugin's, not another device's): the callbacks see it.
bool gameCall(IDirect3DDevice9* device) { return g_internal == 0 && !foreignDevice(device); }

// What this device's call continues into: the trampoline of the method its table points to (when
// one slot has more than one), else the game device's.
void* originalFor(IDirect3DDevice9* device, Slot slot) {
    const SlotHooks& s = g_slots[slot];
    if (s.count > 1) {
        void** table = *reinterpret_cast<void***>(device);
        for (int i = 0; i < s.count; i++) {
            if (s.hooks[i].function == table[slot]) return s.hooks[i].trampoline;
        }
    }
    return g_original[slot];
}

// --- Hook diagnostics ("hook diag:" in the log) -------------------------------
// teout's hang with CenterSpeed (kiero + MinHook inline hooks): after the first frame no Present,
// no lost device and no Reset reached our detours any more. Either the game's thread blocks in
// the Present chain, or its calls go around our vtable entries. Logging only: nothing is re-patched.

// Reads memory that may be gone (another mod's code, a freed table) without crashing.
bool readPointer(const void* address, void** out) {
    __try {
        *out = *static_cast<void* const*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readByte(const void* address, unsigned char* out) {
    __try {
        *out = *static_cast<const unsigned char*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// "d3d9.dll+0x1234" for an address in a module, else the kind of memory ("heap/private").
void describeAddress(const void* address, char* out, size_t size) {
    HMODULE module = nullptr;
    if (address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      static_cast<LPCWSTR>(address), &module) &&
        module) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* name = strrchr(path, '\\');
        name = name ? name + 1 : path;
        snprintf(out, size, "%s+0x%lx", name,
                 static_cast<unsigned long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)));
        return;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (address && VirtualQuery(address, &info, sizeof(info)) == sizeof(info) && info.State == MEM_COMMIT) {
        snprintf(out, size, "%p (%s, no module)", address,
                 info.Type == MEM_PRIVATE ? "heap/private" : info.Type == MEM_MAPPED ? "mapped" : "image");
        return;
    }
    snprintf(out, size, "%p (not readable)", address);
}

// A pointer with its first code byte: E9 (jmp) or FF 25 (jmp [ ]) there is someone's inline hook.
void describeCode(const void* address, char* out, size_t size) {
    char where[160];
    describeAddress(address, where, sizeof(where));
    unsigned char first = 0;
    if (readByte(address, &first)) {
        snprintf(out, size, "%s first byte %02X%s", where, first, first == 0xE9 || first == 0xFF ? " (a jump: hooked inline?)" : "");
    } else {
        snprintf(out, size, "%s (code not readable)", where);
    }
}

// Presents of the game's device that reached presentDetour (counted at its entry).
volatile LONG g_diagPresents = 0;

// At most a few diagnostic lines a minute, so a broken state can't flood the log.
bool diagLineAllowed() {
    static DWORD s_windowStart = 0;
    static int s_lines = 0;
    const DWORD now = GetTickCount();
    if (now - s_windowStart > 60000) {
        s_windowStart = now;
        s_lines = 0;
    }
    return ++s_lines <= 4;
}

// The game's draw calls: how many per frame, and how many with software vertex processing
// (the device is a mixed one): is the CPU side of the game worth working on? Logged twice,
// after about 1000 and 10000 frames, then no longer counted.
struct DrawStats {
    unsigned frames = 0;
    unsigned draws = 0;
    unsigned software = 0;
    unsigned primitives = 0;
} g_drawStats;
bool g_countDraws = true;

void countDraw(IDirect3DDevice9* device, UINT primitives) {
    if (!g_countDraws || !gameCall(device)) return;
    g_drawStats.draws++;
    g_drawStats.primitives += primitives;
    if (device->GetSoftwareVertexProcessing()) g_drawStats.software++;
}

void drawStatsFrame() {
    if (!g_countDraws || ++g_drawStats.frames % 1000 != 0) return;
    const DrawStats& d = g_drawStats;
    TMVS_LOG("draws: %.0f per frame, %.0f%% with software vertex processing, %.0f primitives per frame (%u frames)",
             static_cast<double>(d.draws) / d.frames, d.draws ? 100.0 * d.software / d.draws : 0.0, static_cast<double>(d.primitives) / d.frames,
             d.frames);
    if (d.frames >= 10000) g_countDraws = false;
}

template <typename Fn>
Fn original(IDirect3DDevice9* device, Slot slot) {
    return reinterpret_cast<Fn>(originalFor(device, slot));
}

bool writePointer(void** slot, void* value) {
    DWORD previous = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &previous)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), previous, &previous);
    return true;
}

bool tracingGame(IDirect3DDevice9* device) {
    return gameCall(device) && tracer::tracing();
}

template <typename Shader>
uint64_t shaderHash(Shader* shader) {
    if (!shader) return 0;
    UINT size = 0;
    if (FAILED(shader->GetFunction(nullptr, &size)) || size < 4) return 0;
    std::vector<unsigned char> code(size);
    if (FAILED(shader->GetFunction(code.data(), &size))) return 0;
    return gameshaders::bytecodeHash(code.data(), size);
}

void traceDrawState(IDirect3DDevice9* device, const char* kind, UINT primitives) {
    DWORD z = 0, zw = 0, blend = 0, cw = 0;
    device->GetRenderState(D3DRS_ZENABLE, &z);
    device->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    device->GetRenderState(D3DRS_COLORWRITEENABLE, &cw);
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    device->GetVertexShader(&vs);
    device->GetPixelShader(&ps);
    // Alpha-tested draws (fences, grids: CasterAlphaTest keeps them) say so, with ref and func.
    DWORD alphaTest = 0, alphaRef = 0, alphaFunc = 0;
    device->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
    device->GetRenderState(D3DRS_ALPHAREF, &alphaRef);
    device->GetRenderState(D3DRS_ALPHAFUNC, &alphaFunc);
    char alpha[40] = "";
    if (alphaTest) snprintf(alpha, sizeof(alpha), " alphatest=%lu/%lu", alphaRef, alphaFunc);
    // Vertices in screen pixels (XYZRHW, or POSITIONT in the declaration) are marked: no viewport
    // applies to them (RenderScale has to scale them itself).
    bool screen = false;
    if (!vs) {
        DWORD fvf = 0;
        device->GetFVF(&fvf);
        screen = (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
        IDirect3DVertexDeclaration9* decl = nullptr;
        device->GetVertexDeclaration(&decl);
        if (decl) {
            D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1] = {};
            UINT n = MAXD3DDECLLENGTH + 1;
            if (SUCCEEDED(decl->GetDeclaration(elements, &n))) {
                for (UINT i = 0; i < n && elements[i].Stream != 0xFF; i++) screen = screen || elements[i].Usage == D3DDECLUSAGE_POSITIONT;
            }
            decl->Release();
        }
    }
    if (screen) strncat(alpha, " screen", sizeof(alpha) - strlen(alpha) - 1);
    // With the bytecode hashes the draws can be matched to the dumped sources (gameshaders).
    tracer::event("  %s prims=%u z=%lu zw=%lu blend=%lu cw=%lx%s vs=%p#%016llx ps=%p#%016llx", kind, primitives, z, zw, blend, cw, alpha,
                  static_cast<void*>(vs), static_cast<unsigned long long>(shaderHash(vs)), static_cast<void*>(ps),
                  static_cast<unsigned long long>(shaderHash(ps)));
    if (vs) vs->Release();
    if (ps) ps->Release();
}

// --- Memory report ---------------------------------------------------------
// TmForever.exe is 32-bit without the large-address-aware flag: everything in the
// process (the game's textures, every DLL, overlays like recorders) shares 2 GB of
// address space. When it runs out, the game's textures fail to load and it draws
// untextured (white) surfaces. Failed creations are logged with the memory left.

const char* resultName(HRESULT hr) {
    switch (hr) {
        case E_OUTOFMEMORY: return "E_OUTOFMEMORY (process address space)";
        case D3DERR_OUTOFVIDEOMEMORY: return "D3DERR_OUTOFVIDEOMEMORY (video memory)";
        case D3DERR_INVALIDCALL: return "D3DERR_INVALIDCALL";
        case D3DERR_NOTAVAILABLE: return "D3DERR_NOTAVAILABLE";
        case D3DERR_DEVICELOST: return "D3DERR_DEVICELOST";
        case D3DERR_DEVICENOTRESET: return "D3DERR_DEVICENOTRESET";
        case D3DERR_DRIVERINTERNALERROR: return "D3DERR_DRIVERINTERNALERROR";
        default: return "error";
    }
}

void formatMemory(char* out, size_t size, IDirect3DDevice9* device) {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    GlobalMemoryStatusEx(&status);
    // Fragmentation matters as much as the total: a texture needs one contiguous block.
    size_t largest = 0;
    MEMORY_BASIC_INFORMATION info{};
    for (const char* p = nullptr; VirtualQuery(p, &info, sizeof(info)) == sizeof(info);) {
        if (info.State == MEM_FREE && info.RegionSize > largest) largest = info.RegionSize;
        const char* next = static_cast<const char*>(info.BaseAddress) + info.RegionSize;
        if (next <= p) break;
        p = next;
    }
    const unsigned textureMem = device ? device->GetAvailableTextureMem() : 0;
    snprintf(out, size, "address space %llu of %llu MB free (largest block %zu MB), video memory ~%u MB free (D3D estimate)",
             status.ullAvailVirtual >> 20, status.ullTotalVirtual >> 20, largest >> 20, textureMem >> 20);
}

void logCreateFailure(IDirect3DDevice9* device, const char* what, HRESULT hr) {
    static LONG s_failures = 0;
    const LONG n = InterlockedIncrement(&s_failures);
    // The first ones tell the story; after that, a reminder now and then.
    if (n > 20 && n % 200 != 0) return;
    char memory[200];
    formatMemory(memory, sizeof(memory), device);
    TMVS_LOG("memory: %s %s failed: 0x%08lx %s; %s; %ld failures so far", g_internal ? "mod" : "game", what, hr, resultName(hr),
             memory, n);
}

// --- Detours ---------------------------------------------------------------

HRESULT APIENTRY resetDetour(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    if (foreignDevice(device)) return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*)>(device, kReset)(device, params);
    if (g_callbacks.preReset) g_callbacks.preReset();
    if (params && g_callbacks.adjustPresentParams) g_callbacks.adjustPresentParams(params);
    // The call goes on through every hook on Reset: with CenterSpeed, ours then its (or the other
    // way round), each freeing its own D3DPOOL_DEFAULT resources before d3d9 resets. One left
    // alive (a hook the call went around) makes every Reset fail: the game keeps retrying, the
    // window stays white or black.
    HRESULT hr = original<HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*)>(device, kReset)(device, params);
    static unsigned s_failed = 0; // failed Resets in a row
    if (FAILED(hr) && s_failed++ > 0) return hr; // logged at the first of the series
    TMVS_LOG("Reset -> 0x%08lx (%ux%u fmt=%s ms=%d autoDepth=%d %s windowed=%d)", hr, params->BackBufferWidth,
             params->BackBufferHeight, tracer::formatName(params->BackBufferFormat), params->MultiSampleType,
             params->EnableAutoDepthStencil, tracer::formatName(params->AutoDepthStencilFormat), params->Windowed);
    if (FAILED(hr)) {
        const HRESULT coop = device->TestCooperativeLevel();
        char entry[220] = "-";
        const SlotHooks& r = g_slots[kReset];
        if (r.count) {
            if (inlinehook::intact(r.hooks[0])) snprintf(entry, sizeof(entry), "our jump");
            else describeCode(r.hooks[0].site, entry, sizeof(entry));
        }
        TMVS_LOG("Reset failed: 0x%08lx %s, TestCooperativeLevel 0x%08lx (%s); Reset's entry: %s. Usually a D3DPOOL_DEFAULT "
                 "resource still alive (this mod's or another's); the next failures in a row are not logged",
                 hr, resultName(hr), coop, coop == D3DERR_DEVICENOTRESET ? "not reset" : coop == D3DERR_DEVICELOST ? "lost" : coop == D3D_OK ? "ok" : "other",
                 entry);
        logMemory("after the failed reset");
        return hr;
    }
    if (s_failed) TMVS_LOG("Reset: worked again after %u failed in a row", s_failed);
    s_failed = 0;
    g_device = device; // the first reset comes before the first Present
    logMemory("after reset");
    if (SUCCEEDED(hr) && g_callbacks.postReset) {
        g_internal++;
        g_callbacks.postReset(device);
        g_internal--;
    }
    return hr;
}

HRESULT APIENTRY presentDetour(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty) {
    if (foreignDevice(device)) {
        return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*)>(device, kPresent)(
            device, src, dst, window, dirty);
    }
    InterlockedIncrement(&g_diagPresents);
    g_device = device;
    if (g_callbacks.present) {
        g_internal++;
        g_callbacks.present(device);
        g_internal--;
    }
    if (tracingGame(device)) tracer::event("Present");
    tracer::onPresent(device);
    drawStatsFrame();
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*)>(device, kPresent)(
        device, src, dst, window, dirty);
}

HRESULT APIENTRY createTextureDetour(IDirect3DDevice9* device, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format,
                                     D3DPOOL pool, IDirect3DTexture9** out, HANDLE* shared) {
    HRESULT hr = original<HRESULT(APIENTRY*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**,
                                             HANDLE*)>(device, kCreateTexture)(device, w, h, levels, usage, format, pool, out, shared);
    if (FAILED(hr)) {
        char what[96];
        snprintf(what, sizeof(what), "CreateTexture %ux%u %s pool=%d", w, h, tracer::formatName(format), pool);
        logCreateFailure(device, what, hr);
    }
    if (gameCall(device) && (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) {
        TMVS_LOG("CreateTexture %ux%u levels=%u usage=0x%lx fmt=%s -> %p", w, h, levels, usage, tracer::formatName(format),
                 SUCCEEDED(hr) && out ? static_cast<void*>(*out) : nullptr);
    }
    return hr;
}

HRESULT APIENTRY createVolumeTextureDetour(IDirect3DDevice9* device, UINT w, UINT h, UINT d, UINT levels, DWORD usage,
                                           D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture9** out, HANDLE* shared) {
    HRESULT hr = original<HRESULT(APIENTRY*)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                             IDirect3DVolumeTexture9**, HANDLE*)>(device, kCreateVolumeTexture)(device, w, h, d, levels, usage,
                                                                                                      format, pool, out, shared);
    if (FAILED(hr)) {
        char what[96];
        snprintf(what, sizeof(what), "CreateVolumeTexture %ux%ux%u %s pool=%d", w, h, d, tracer::formatName(format), pool);
        logCreateFailure(device, what, hr);
    }
    return hr;
}

HRESULT APIENTRY createCubeTextureDetour(IDirect3DDevice9* device, UINT edge, UINT levels, DWORD usage, D3DFORMAT format,
                                         D3DPOOL pool, IDirect3DCubeTexture9** out, HANDLE* shared) {
    HRESULT hr = original<HRESULT(APIENTRY*)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**,
                                             HANDLE*)>(device, kCreateCubeTexture)(device, edge, levels, usage, format, pool, out, shared);
    if (FAILED(hr)) {
        char what[96];
        snprintf(what, sizeof(what), "CreateCubeTexture %u %s pool=%d", edge, tracer::formatName(format), pool);
        logCreateFailure(device, what, hr);
    }
    return hr;
}

HRESULT APIENTRY createVertexBufferDetour(IDirect3DDevice9* device, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                          IDirect3DVertexBuffer9** out, HANDLE* shared) {
    HRESULT hr = original<HRESULT(APIENTRY*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*)>(device, kCreateVertexBuffer)(device, length, usage, fvf, pool, out, shared);
    if (FAILED(hr)) {
        char what[96];
        snprintf(what, sizeof(what), "CreateVertexBuffer %u bytes pool=%d", length, pool);
        logCreateFailure(device, what, hr);
    }
    return hr;
}

HRESULT APIENTRY createIndexBufferDetour(IDirect3DDevice9* device, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                         IDirect3DIndexBuffer9** out, HANDLE* shared) {
    HRESULT hr = original<HRESULT(APIENTRY*)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*)>(device, kCreateIndexBuffer)(device, length, usage, format, pool, out, shared);
    if (FAILED(hr)) {
        char what[96];
        snprintf(what, sizeof(what), "CreateIndexBuffer %u bytes pool=%d", length, pool);
        logCreateFailure(device, what, hr);
    }
    return hr;
}

HRESULT APIENTRY createRenderTargetDetour(IDirect3DDevice9* device, UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms,
                                          DWORD quality, BOOL lockable, IDirect3DSurface9** out, HANDLE* shared) {
    HRESULT hr = original<HRESULT(APIENTRY*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                             IDirect3DSurface9**, HANDLE*)>(device, kCreateRenderTarget)(device, w, h, format, ms, quality,
                                                                                                 lockable, out, shared);
    if (gameCall(device)) {
        TMVS_LOG("CreateRenderTarget %ux%u fmt=%s ms=%d -> %p", w, h, tracer::formatName(format), ms,
                 SUCCEEDED(hr) && out ? static_cast<void*>(*out) : nullptr);
    }
    return hr;
}

HRESULT APIENTRY createDepthStencilDetour(IDirect3DDevice9* device, UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms,
                                          DWORD quality, BOOL discard, IDirect3DSurface9** out, HANDLE* shared) {
    if (gameCall(device) && g_callbacks.createDepthStencil) {
        HRESULT replaced = E_NOTIMPL;
        if (g_callbacks.createDepthStencil(device, w, h, format, ms, out, &replaced)) return replaced;
    }
    HRESULT hr = original<HRESULT(APIENTRY*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                             IDirect3DSurface9**, HANDLE*)>(device, kCreateDepthStencilSurface)(device, w, h, format, ms,
                                                                                                         quality, discard, out,
                                                                                                         shared);
    if (gameCall(device)) {
        TMVS_LOG("CreateDepthStencilSurface %ux%u fmt=%s ms=%d -> %p", w, h, tracer::formatName(format), ms,
                 SUCCEEDED(hr) && out ? static_cast<void*>(*out) : nullptr);
    }
    return hr;
}

HRESULT APIENTRY stretchRectDetour(IDirect3DDevice9* device, IDirect3DSurface9* src, const RECT* srcRect, IDirect3DSurface9* dst,
                                   const RECT* dstRect, D3DTEXTUREFILTERTYPE filter) {
    if (tracingGame(device)) tracer::event("StretchRect %s -> %s", tracer::describe(src), tracer::describe(dst));
    if (gameCall(device) && g_callbacks.stretchRects) g_callbacks.stretchRects(src, &srcRect, dst, &dstRect);
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*,
                                       D3DTEXTUREFILTERTYPE)>(device, kStretchRect)(device, src, srcRect, dst, dstRect, filter);
}

HRESULT APIENTRY setRenderTargetDetour(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface) {
    if (tracingGame(device)) tracer::event("SetRenderTarget %lu %s", index, tracer::describe(surface));
    if (gameCall(device) && g_callbacks.setRenderTarget) surface = g_callbacks.setRenderTarget(index, surface);
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*)>(device, kSetRenderTarget)(device, index, surface);
}

// Whether code at `address` belongs to another mod's DLL: not the game's exe, not this plugin, not
// Windows' own DLLs (d3d9, D3DX and the rest under the Windows folder) and not a d3d* DLL next to
// the game. Code outside any module (another hooker's trampoline) doesn't count. Cached per module
// (CenterSpeed asks on every indexed draw); each module is logged once.
bool otherModCode(const void* address) {
    struct Range {
        uintptr_t begin, end;
        bool otherMod;
    };
    static Range s_ranges[24];
    static volatile LONG s_count = 0;
    static SRWLOCK s_lock = SRWLOCK_INIT;
    const uintptr_t a = reinterpret_cast<uintptr_t>(address);
    const LONG n = s_count;
    for (LONG i = 0; i < n; i++) {
        if (a >= s_ranges[i].begin && a < s_ranges[i].end) return s_ranges[i].otherMod;
    }
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module) ||
        !module) {
        return false;
    }
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&otherModCode), &self);
    wchar_t path[MAX_PATH] = {}, windows[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    const UINT w = GetSystemWindowsDirectoryW(windows, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    const bool system = w && w < MAX_PATH && _wcsnicmp(path, windows, w) == 0;
    const bool other = module != GetModuleHandleW(nullptr) && module != self && !system && _wcsnicmp(name, L"d3d", 3) != 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(module) + dos->e_lfanew);
    AcquireSRWLockExclusive(&s_lock);
    const LONG at = s_count;
    if (at < static_cast<LONG>(sizeof(s_ranges) / sizeof(s_ranges[0]))) {
        s_ranges[at] = {reinterpret_cast<uintptr_t>(module), reinterpret_cast<uintptr_t>(module) + nt->OptionalHeader.SizeOfImage, other};
        InterlockedExchange(&s_count, at + 1);
        if (other) TMVS_LOG("hook: another mod (%ls) asks for the render target: while the game's camera draws into the float one, it is told the game's own", name);
    }
    ReleaseSRWLockExclusive(&s_lock);
    return other;
}

HRESULT APIENTRY getRenderTargetDetour(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9** out) {
    const HRESULT hr = original<HRESULT(APIENTRY*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9**)>(device, kGetRenderTarget)(device, index, out);
    // Another mod asking during the camera finds the target the game bound, not the float one
    // standing in for it (CenterSpeed only recognises the car drawn into the back buffer). The
    // game and this plugin keep getting the real one.
    if (SUCCEEDED(hr) && index == 0 && out && *out && g_callbacks.otherModRenderTarget && gameCall(device) &&
        otherModCode(_ReturnAddress())) {
        IDirect3DSurface9* shown = g_callbacks.otherModRenderTarget(*out);
        if (shown && shown != *out) {
            shown->AddRef();
            (*out)->Release();
            *out = shown;
        }
    }
    return hr;
}

HRESULT APIENTRY setDepthStencilDetour(IDirect3DDevice9* device, IDirect3DSurface9* surface) {
    if (tracingGame(device)) tracer::event("SetDepthStencilSurface %s", tracer::describe(surface));
    if (gameCall(device) && g_callbacks.setDepthStencil) surface = g_callbacks.setDepthStencil(surface);
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, IDirect3DSurface9*)>(device, kSetDepthStencilSurface)(device, surface);
}

HRESULT APIENTRY beginSceneDetour(IDirect3DDevice9* device) {
    if (tracingGame(device)) tracer::event("BeginScene");
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*)>(device, kBeginScene)(device);
}

HRESULT APIENTRY endSceneDetour(IDirect3DDevice9* device) {
    if (tracingGame(device)) tracer::event("EndScene");
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*)>(device, kEndScene)(device);
}

HRESULT APIENTRY clearDetour(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z,
                             DWORD stencil) {
    if (tracingGame(device)) tracer::event("Clear rects=%lu flags=%lx color=%08lx z=%.3f s=%lu", count, flags, color, z, stencil);
    if (gameCall(device) && g_callbacks.clear) g_callbacks.clear(device, count, rects, flags, color);
    if (gameCall(device) && g_callbacks.clearRects) rects = g_callbacks.clearRects(device, count, rects, flags, z, stencil);
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD)>(device, kClear)(
        device, count, rects, flags, color, z, stencil);
}

HRESULT APIENTRY setTransformDetour(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE type, const D3DMATRIX* m) {
    if (tracingGame(device) && m) {
        tracer::event("SetTransform %d [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]",
                      static_cast<int>(type), m->_11, m->_12, m->_13, m->_14, m->_21, m->_22, m->_23, m->_24, m->_31, m->_32,
                      m->_33, m->_34, m->_41, m->_42, m->_43, m->_44);
    }
    if (gameCall(device) && m && g_callbacks.setTransform) g_callbacks.setTransform(type, m);
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*)>(device, kSetTransform)(device, type, m);
}

HRESULT APIENTRY setLightDetour(IDirect3DDevice9* device, DWORD index, const D3DLIGHT9* light) {
    if (tracingGame(device) && light) {
        tracer::event("SetLight %lu type=%d dir=(%.4f %.4f %.4f) pos=(%.1f %.1f %.1f) diffuse=(%.3f %.3f %.3f) ambient=(%.3f %.3f %.3f)",
                      index, static_cast<int>(light->Type), light->Direction.x, light->Direction.y, light->Direction.z,
                      light->Position.x, light->Position.y, light->Position.z, light->Diffuse.r, light->Diffuse.g, light->Diffuse.b,
                      light->Ambient.r, light->Ambient.g, light->Ambient.b);
    }
    if (gameCall(device) && light && g_callbacks.setLight) g_callbacks.setLight(index, light);
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, DWORD, const D3DLIGHT9*)>(device, kSetLight)(device, index, light);
}

HRESULT APIENTRY lightEnableDetour(IDirect3DDevice9* device, DWORD index, BOOL enable) {
    if (tracingGame(device)) tracer::event("LightEnable %lu %d", index, enable);
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, DWORD, BOOL)>(device, kLightEnable)(device, index, enable);
}

HRESULT APIENTRY setViewportDetour(IDirect3DDevice9* device, const D3DVIEWPORT9* vp) {
    if (tracingGame(device) && vp) {
        tracer::event("SetViewport %lu,%lu %lux%lu z=%.2f..%.2f", vp->X, vp->Y, vp->Width, vp->Height, vp->MinZ, vp->MaxZ);
    }
    if (gameCall(device) && vp && g_callbacks.setViewport) vp = g_callbacks.setViewport(device, vp);
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, const D3DVIEWPORT9*)>(device, kSetViewport)(device, vp);
}

HRESULT APIENTRY getViewportDetour(IDirect3DDevice9* device, D3DVIEWPORT9* vp) {
    const HRESULT hr = original<HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DVIEWPORT9*)>(device, kGetViewport)(device, vp);
    if (SUCCEEDED(hr) && gameCall(device) && vp && g_callbacks.getViewport) g_callbacks.getViewport(device, vp);
    return hr;
}

HRESULT APIENTRY setScissorRectDetour(IDirect3DDevice9* device, const RECT* rect) {
    if (gameCall(device) && rect && g_callbacks.setScissorRect) rect = g_callbacks.setScissorRect(device, rect);
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, const RECT*)>(device, kSetScissorRect)(device, rect);
}

HRESULT APIENTRY getScissorRectDetour(IDirect3DDevice9* device, RECT* rect) {
    const HRESULT hr = original<HRESULT(APIENTRY*)(IDirect3DDevice9*, RECT*)>(device, kGetScissorRect)(device, rect);
    if (SUCCEEDED(hr) && gameCall(device) && rect && g_callbacks.getScissorRect) g_callbacks.getScissorRect(device, rect);
    return hr;
}

HRESULT APIENTRY setTextureDetour(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture) {
    if (tracingGame(device) && texture) {
        D3DSURFACE_DESC desc{};
        bool interesting = texture->GetType() != D3DRTYPE_TEXTURE ||
                           (SUCCEEDED(static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &desc)) &&
                            (desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)));
        if (interesting) tracer::event("  SetTexture %lu %s", stage, tracer::describe(texture));
    }
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*)>(device, kSetTexture)(device, stage, texture);
}

HRESULT APIENTRY drawPrimitiveDetour(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT start, UINT count) {
    if (tracingGame(device)) traceDrawState(device, "DP", count);
    countDraw(device, count);
    if (gameCall(device) && g_callbacks.draw) g_callbacks.draw(device);
    const HRESULT hr = original<HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT)>(device, kDrawPrimitive)(device, type, start, count);
    if (gameCall(device) && g_callbacks.drawDone) g_callbacks.drawDone(device);
    return hr;
}

HRESULT APIENTRY drawIndexedPrimitiveDetour(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices,
                                            UINT start, UINT count) {
    if (tracingGame(device)) traceDrawState(device, "DIP", count);
    countDraw(device, count);
    if (gameCall(device) && g_callbacks.draw) g_callbacks.draw(device);
    if (gameCall(device) && g_callbacks.drawIndexed) g_callbacks.drawIndexed(device, type, base, minIndex, vertices, start, count);
    const HRESULT hr = original<HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT)>(device, kDrawIndexedPrimitive)(
        device, type, base, minIndex, vertices, start, count);
    if (gameCall(device) && g_callbacks.drawDone) g_callbacks.drawDone(device);
    return hr;
}

HRESULT APIENTRY drawPrimitiveUPDetour(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) {
    if (tracingGame(device)) traceDrawState(device, "DPUP", count);
    countDraw(device, count);
    if (gameCall(device) && g_callbacks.draw) g_callbacks.draw(device);
    const HRESULT hr = original<HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT)>(device, kDrawPrimitiveUP)(
        device, type, count, data, stride);
    if (gameCall(device) && g_callbacks.drawDone) g_callbacks.drawDone(device);
    return hr;
}

HRESULT APIENTRY drawIndexedPrimitiveUPDetour(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT minIndex, UINT vertices,
                                              UINT count, const void* indices, D3DFORMAT indexFormat, const void* data, UINT stride) {
    if (tracingGame(device)) traceDrawState(device, "DIPUP", count);
    countDraw(device, count);
    if (gameCall(device) && g_callbacks.draw) g_callbacks.draw(device);
    const HRESULT hr = original<HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT,
                                                   const void*, UINT)>(device, kDrawIndexedPrimitiveUP)(device, type, minIndex, vertices, count,
                                                                                                indices, indexFormat, data, stride);
    if (gameCall(device) && g_callbacks.drawDone) g_callbacks.drawDone(device);
    return hr;
}

HRESULT APIENTRY setVertexShaderDetour(IDirect3DDevice9* device, IDirect3DVertexShader9* shader) {
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, IDirect3DVertexShader9*)>(device, kSetVertexShader)(device, shader);
}

HRESULT APIENTRY setVertexShaderConstantFDetour(IDirect3DDevice9* device, UINT start, const float* data, UINT count) {
    if (tracingGame(device) && data) {
        char line[900];
        int n = snprintf(line, sizeof(line), "  VSConst c%u x%u:", start, count);
        for (UINT i = 0; i < count * 4 && i < 16 * 4 && n < 860; i++) {
            n += snprintf(line + n, sizeof(line) - n, "%s%.4g", (i % 4 == 0) ? " | " : " ", data[i]);
        }
        tracer::event("%s", line);
    }
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, UINT, const float*, UINT)>(device, kSetVertexShaderConstantF)(device, start, data,
                                                                                                            count);
}

HRESULT APIENTRY createPixelShaderDetour(IDirect3DDevice9* device, const DWORD* code, IDirect3DPixelShader9** shader) {
    const HRESULT hr = original<HRESULT(APIENTRY*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**)>(device, kCreatePixelShader)(device, code,
                                                                                                                 shader);
    if (SUCCEEDED(hr) && shader && *shader && !foreignDevice(device) && g_callbacks.createPixelShader) {
        g_callbacks.createPixelShader(*shader, code);
    }
    return hr;
}

HRESULT APIENTRY setPixelShaderDetour(IDirect3DDevice9* device, IDirect3DPixelShader9* shader) {
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, IDirect3DPixelShader9*)>(device, kSetPixelShader)(device, shader);
}

HRESULT APIENTRY setPixelShaderConstantFDetour(IDirect3DDevice9* device, UINT start, const float* data, UINT count) {
    if (tracingGame(device) && data) {
        char line[600];
        int n = snprintf(line, sizeof(line), "  PSConst c%u x%u:", start, count);
        for (UINT i = 0; i < count * 4 && i < 8 * 4 && n < 560; i++) {
            n += snprintf(line + n, sizeof(line) - n, "%s%.4g", (i % 4 == 0) ? " | " : " ", data[i]);
        }
        tracer::event("%s", line);
    }
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, UINT, const float*, UINT)>(device, kSetPixelShaderConstantF)(device, start, data,
                                                                                                           count);
}

struct Detour {
    Slot slot;
    void* function;
};

const Detour kDetours[] = {
    {kReset, reinterpret_cast<void*>(&resetDetour)},
    {kPresent, reinterpret_cast<void*>(&presentDetour)},
    {kCreateTexture, reinterpret_cast<void*>(&createTextureDetour)},
    {kCreateVolumeTexture, reinterpret_cast<void*>(&createVolumeTextureDetour)},
    {kCreateCubeTexture, reinterpret_cast<void*>(&createCubeTextureDetour)},
    {kCreateVertexBuffer, reinterpret_cast<void*>(&createVertexBufferDetour)},
    {kCreateIndexBuffer, reinterpret_cast<void*>(&createIndexBufferDetour)},
    {kCreateRenderTarget, reinterpret_cast<void*>(&createRenderTargetDetour)},
    {kCreateDepthStencilSurface, reinterpret_cast<void*>(&createDepthStencilDetour)},
    {kStretchRect, reinterpret_cast<void*>(&stretchRectDetour)},
    {kSetRenderTarget, reinterpret_cast<void*>(&setRenderTargetDetour)},
    {kGetRenderTarget, reinterpret_cast<void*>(&getRenderTargetDetour)},
    {kSetDepthStencilSurface, reinterpret_cast<void*>(&setDepthStencilDetour)},
    {kBeginScene, reinterpret_cast<void*>(&beginSceneDetour)},
    {kEndScene, reinterpret_cast<void*>(&endSceneDetour)},
    {kClear, reinterpret_cast<void*>(&clearDetour)},
    {kSetTransform, reinterpret_cast<void*>(&setTransformDetour)},
    {kSetViewport, reinterpret_cast<void*>(&setViewportDetour)},
    {kGetViewport, reinterpret_cast<void*>(&getViewportDetour)},
    {kSetLight, reinterpret_cast<void*>(&setLightDetour)},
    {kLightEnable, reinterpret_cast<void*>(&lightEnableDetour)},
    {kSetTexture, reinterpret_cast<void*>(&setTextureDetour)},
    {kSetScissorRect, reinterpret_cast<void*>(&setScissorRectDetour)},
    {kGetScissorRect, reinterpret_cast<void*>(&getScissorRectDetour)},
    {kDrawPrimitive, reinterpret_cast<void*>(&drawPrimitiveDetour)},
    {kDrawIndexedPrimitive, reinterpret_cast<void*>(&drawIndexedPrimitiveDetour)},
    {kDrawPrimitiveUP, reinterpret_cast<void*>(&drawPrimitiveUPDetour)},
    {kDrawIndexedPrimitiveUP, reinterpret_cast<void*>(&drawIndexedPrimitiveUPDetour)},
    {kSetVertexShader, reinterpret_cast<void*>(&setVertexShaderDetour)},
    {kSetVertexShaderConstantF, reinterpret_cast<void*>(&setVertexShaderConstantFDetour)},
    {kCreatePixelShader, reinterpret_cast<void*>(&createPixelShaderDetour)},
    {kSetPixelShader, reinterpret_cast<void*>(&setPixelShaderDetour)},
    {kSetPixelShaderConstantF, reinterpret_cast<void*>(&setPixelShaderConstantFDetour)},
};

const Detour* detourOf(Slot slot) {
    for (const Detour& d : kDetours) {
        if (d.slot == slot) return &d;
    }
    return nullptr;
}

// Puts the game's pointer back into a fallback slot, but only while our detour is still in it:
// another mod that hooked the same slot after us (its pointer in the vtable, calling ours) keeps
// its hook.
void restoreSlot(void** vtable, const Detour& d) {
    if (!g_vtableSlot[d.slot] || !g_tableOriginal[d.slot]) return;
    if (vtable[d.slot] == d.function) {
        writePointer(&vtable[d.slot], g_tableOriginal[d.slot]);
    } else {
        TMVS_LOG("hook: vtable slot %d holds another mod's hook (%p): left in place", d.slot, vtable[d.slot]);
    }
}

// Hooks the methods the game's device calls. Inline on the code its table points to; a method
// that can't be hooked inline goes through the table's slot instead (logged). A method already
// hooked (the same code for a later device) is left as it is.
void hookDevice(IDirect3DDevice9* device) {
    void** vtable = *reinterpret_cast<void***>(device);
    // Where the table lives (d3d9.dll's image, or the heap: one per device) and what the game's
    // Reset and Present were before us: another mod's inline hook shows as a jump.
    char table[160], reset[220], present[220];
    describeAddress(vtable, table, sizeof(table));
    describeCode(vtable[kReset], reset, sizeof(reset));
    describeCode(vtable[kPresent], present, sizeof(present));
    TMVS_LOG("hook diag: vtable %s; Reset was %s; Present was %s", table, reset, present);
    int atEntry = 0, chained = 0, slots = 0, known = 0;
    for (const Detour& d : kDetours) {
        void* function = vtable[d.slot];
        SlotHooks& s = g_slots[d.slot];
        bool done = false;
        for (int i = 0; i < s.count && !done; i++) {
            if (s.hooks[i].function == function) {
                g_original[d.slot] = s.hooks[i].trampoline; // this device's calls go on into this one
                g_vtableSlot[d.slot] = false;
                done = true;
                known++;
            }
        }
        if (done) continue;
        if (function == d.function) {
            known++; // our own detour in the table already (the fallback, same table)
            continue;
        }
        const char* why = "";
        // Code another slot shares (a common stub): a second jump would send one method's call
        // into the other's detour.
        // Through the vtable then, calling on past that jump (into its trampoline).
        bool shared = false;
        void* past = function;
        for (const Detour& other : kDetours) {
            const SlotHooks& o = g_slots[other.slot];
            for (int i = 0; i < o.count; i++) {
                if (o.hooks[i].function != function) continue;
                shared = true;
                past = o.hooks[i].trampoline;
            }
        }
        if (shared) why = "its code is another hooked method's";
        inlinehook::Hook hook;
        if (!shared && s.count < SlotHooks::kMax && inlinehook::install(function, d.function, hook, &why)) {
            s.hooks[s.count++] = hook;
            g_original[d.slot] = hook.trampoline;
            g_vtableSlot[d.slot] = false;
            g_inline = true;
            if (hook.chained) {
                chained++;
                char where[160];
                describeAddress(hook.previous, where, sizeof(where));
                TMVS_LOG("hook: slot %d starts with another mod's jump (to %s): chained, ours runs first, then theirs", d.slot, where);
            } else {
                atEntry++;
            }
            continue;
        }
        if (!shared && s.count >= SlotHooks::kMax) why = "too many method addresses for this slot";
        // Fallback: the table's slot (it may be rewritten under us: diagFrame writes it again).
        g_original[d.slot] = past;
        g_tableOriginal[d.slot] = function;
        g_vtableSlot[d.slot] = true;
        writePointer(&vtable[d.slot], d.function);
        slots++;
        char where[220];
        describeCode(function, where, sizeof(where));
        TMVS_LOG("hook: slot %d can't be hooked inline (%s; %s): hooked in the vtable instead", d.slot, why, where);
    }
    g_vtable = vtable;
    TMVS_LOG("hook: device %p methods hooked: %d inline at their entry, %d chained onto another mod's jump, %d through the vtable, "
             "%d already hooked",
             static_cast<void*>(device), atEntry, chained, slots, known);
}

// --- Device creation interception -------------------------------------------

using CreateDeviceFn = HRESULT(APIENTRY*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
constexpr int kCreateDeviceIndex = 16;
CreateDeviceFn g_realCreateDevice = nullptr;

HRESULT APIENTRY createDeviceDetour(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                    D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** out) {
    // The game renders with a hardware device that has a depth buffer. Other mods in the same
    // process (overlays such as Twinkie) create helper devices of their own - no depth, often
    // 0x0 or a reference device. Taking one of those for the game's moved every hook onto it:
    // no readable depth, F7/F8 dead.
    const bool gameDevice = type == D3DDEVTYPE_HAL && params && params->EnableAutoDepthStencil;
    if (!gameDevice) {
        HRESULT hr = g_realCreateDevice(self, adapter, type, window, flags, params, out);
        TMVS_LOG("CreateDevice -> 0x%08lx flags=0x%lx type=%d %ux%u autoDepth=%d: not the game's device, left alone", hr, flags,
                 static_cast<int>(type), params ? params->BackBufferWidth : 0, params ? params->BackBufferHeight : 0,
                 params ? params->EnableAutoDepthStencil : 0);
        return hr;
    }
    if (g_callbacks.adjustPresentParams) g_callbacks.adjustPresentParams(params);
    HRESULT hr = g_realCreateDevice(self, adapter, type, window, flags, params, out);
    if (params) {
        TMVS_LOG("CreateDevice -> 0x%08lx flags=0x%lx %ux%u fmt=%s ms=%d autoDepth=%d %s windowed=%d", hr, flags,
                 params->BackBufferWidth, params->BackBufferHeight, tracer::formatName(params->BackBufferFormat),
                 params->MultiSampleType, params->EnableAutoDepthStencil, tracer::formatName(params->AutoDepthStencilFormat),
                 params->Windowed);
    }
    if (SUCCEEDED(hr) && out && *out) {
        g_gameDevice = *out;
        hookDevice(*out);
        if (g_callbacks.deviceCreated) {
            g_internal++;
            g_callbacks.deviceCreated(*out);
            g_internal--;
        }
    }
    return hr;
}

void patchFactory(IDirect3D9* factory) {
    void** vtable = *reinterpret_cast<void***>(factory);
    if (!g_realCreateDevice) {
        g_realCreateDevice = reinterpret_cast<CreateDeviceFn>(vtable[kCreateDeviceIndex]);
        writePointer(&vtable[kCreateDeviceIndex], reinterpret_cast<void*>(&createDeviceDetour));
    }
}

using Create9Fn = IDirect3D9*(WINAPI*)(UINT);
Create9Fn g_realCreate9 = nullptr;

IDirect3D9* WINAPI create9Detour(UINT sdk) {
    IDirect3D9* factory = g_realCreate9 ? g_realCreate9(sdk) : nullptr;
    if (factory) patchFactory(factory);
    return factory;
}

bool patchImport(const char* dll, const char* function, void* replacement, void** originalOut) {
    HMODULE base = GetModuleHandleW(nullptr);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<BYTE*>(base) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;

    auto import = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(reinterpret_cast<BYTE*>(base) + dir.VirtualAddress);
    for (; import->Name; import++) {
        const char* name = reinterpret_cast<const char*>(reinterpret_cast<BYTE*>(base) + import->Name);
        if (_stricmp(name, dll) != 0) continue;

        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(reinterpret_cast<BYTE*>(base) + import->OriginalFirstThunk);
        auto addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(reinterpret_cast<BYTE*>(base) + import->FirstThunk);
        if (!import->OriginalFirstThunk) names = addresses;

        for (; names->u1.AddressOfData; names++, addresses++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto named = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(reinterpret_cast<BYTE*>(base) + names->u1.AddressOfData);
            if (strcmp(named->Name, function) != 0) continue;

            *originalOut = reinterpret_cast<void*>(addresses->u1.Function);
            return writePointer(reinterpret_cast<void**>(&addresses->u1.Function), replacement);
        }
    }
    return false;
}

} // namespace

bool patchGameImport(const char* dll, const char* function, void* replacement, void** originalOut) {
    return patchImport(dll, function, replacement, originalOut);
}

bool install(const DeviceCallbacks& callbacks) {
    g_callbacks = callbacks;
    if (g_realCreateDevice) return true;

    bool imported = patchImport("d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(&create9Detour),
                                reinterpret_cast<void**>(&g_realCreate9));
    TMVS_LOG("hook: Direct3DCreate9 import %s", imported ? "patched" : "not found");

    // The game's device is hooked when the game creates it (createDeviceDetour). The factory's
    // table is shared by every IDirect3D9, so patching our own covers the game's too. There is no
    // probe device any more: hooking a probe's methods at boot raced other mods hooking the same
    // code from their own threads in that moment (CenterSpeed), and a probe with software vertex
    // processing has other methods than the game's device.
    // The system d3d9 by full path: in standalone mode this DLL is "d3d9.dll" itself.
    if (IDirect3D9* d3d = proxy::createDirect3D9(D3D_SDK_VERSION)) {
        patchFactory(d3d);
        d3d->Release();
    }
    return g_realCreateDevice != nullptr;
}

void remove() {
    for (const Detour& d : kDetours) {
        SlotHooks& s = g_slots[d.slot];
        for (int i = 0; i < s.count; i++) {
            if (!inlinehook::remove(s.hooks[i]) && s.hooks[i].site) {
                TMVS_LOG("hook: slot %d: another mod's hook sits on ours: left in place", d.slot);
            }
        }
        s.count = 0;
        if (g_vtable) restoreSlot(g_vtable, d);
    }
    g_vtable = nullptr;
}

void logMemory(const char* when) {
    char memory[200];
    formatMemory(memory, sizeof(memory), g_device);
    TMVS_LOG("memory %s: %s", when, memory);
}

namespace {

bool hookedCode(Slot slot, void* function) {
    const SlotHooks& s = g_slots[slot];
    for (int i = 0; i < s.count; i++) {
        if (s.hooks[i].function == function) return true;
    }
    return false;
}

} // namespace

void diagFrame() {
    if (!g_gameDevice || (!g_inline && !g_vtable)) return;
    // 1. Does the game's device table still lead into our hooks? An inline-hooked slot: the table
    // points to the code we hooked (or to another mod's vtable hook, which calls on into it: only
    // logged, hooking that too would run ours twice). A vtable slot: our detour; back to the
    // game's own pointer means the table was rewritten (seen with CenterSpeed): written again.
    void* table = nullptr;
    const bool readable = readPointer(g_gameDevice, &table);
    int elsewhere = 0, rewritten = 0, first = -1;
    void* firstAt = nullptr;
    if (readable) {
        void** t = static_cast<void**>(table);
        for (const Detour& d : kDetours) {
            void* now = nullptr;
            if (!readPointer(&t[d.slot], &now) || now == d.function) continue;
            if (g_vtableSlot[d.slot]) {
                if (now == g_tableOriginal[d.slot] && t == g_vtable && writePointer(&t[d.slot], d.function)) {
                    rewritten++;
                    continue;
                }
            } else if (hookedCode(d.slot, now)) {
                continue;
            }
            elsewhere++;
            if (first < 0) {
                first = d.slot;
                firstAt = now;
            }
        }
    }
    // Reset and Present inline: our jump still at their entry, or another mod's on top of it.
    bool resetIntact = true, presentIntact = true;
    for (int i = 0; i < g_slots[kReset].count; i++) resetIntact = resetIntact && inlinehook::intact(g_slots[kReset].hooks[i]);
    for (int i = 0; i < g_slots[kPresent].count; i++) presentIntact = presentIntact && inlinehook::intact(g_slots[kPresent].hooks[i]);
    const int state = (readable ? 0 : 1) | (elsewhere ? 2 : 0) | (resetIntact ? 0 : 4) | (presentIntact ? 0 : 8);
    static int s_state = 0;
    if (rewritten && diagLineAllowed()) {
        TMVS_LOG("hook diag: the game's device table had the game's own pointers back in %d of our vtable slots: hooked again", rewritten);
    }
    if (state != s_state && diagLineAllowed()) {
        s_state = state;
        if (state == 0) {
            TMVS_LOG("hook diag: the game's device leads into our hooks again");
        } else {
            char where[220] = "-", resetAt[220] = "-", presentAt[220] = "-";
            if (elsewhere) describeCode(firstAt, where, sizeof(where));
            if (!resetIntact) describeCode(g_slots[kReset].hooks[0].site, resetAt, sizeof(resetAt));
            if (!presentIntact) describeCode(g_slots[kPresent].hooks[0].site, presentAt, sizeof(presentAt));
            TMVS_LOG("hook diag: device %p %s; %d slots point elsewhere (first slot %d: %s); Reset entry %s; Present entry %s",
                     static_cast<void*>(g_gameDevice), readable ? "readable" : "not readable", elsewhere, first, where,
                     resetIntact ? "ours" : resetAt, presentIntact ? "ours" : presentAt);
        }
    }

    // 2. Engine frames without a Present through our detour: the game draws, but its Presents go
    // around us (or never come).
    static LONG s_lastPresents = -1;
    static unsigned s_framesWithout = 0;
    static bool s_silent = false;
    const LONG presents = g_diagPresents;
    if (presents != s_lastPresents) {
        if (s_silent && diagLineAllowed()) {
            TMVS_LOG("hook diag: Presents through our hook again after %u engine frames without (%ld so far)", s_framesWithout,
                     static_cast<long>(presents));
        }
        s_lastPresents = presents;
        s_framesWithout = 0;
        s_silent = false;
    } else if (++s_framesWithout == 120 && diagLineAllowed()) {
        s_silent = true;
        TMVS_LOG("hook diag: 120 engine frames without a Present through our hook (%ld so far): the game's Presents go around "
                 "it, or it draws without presenting",
                 static_cast<long>(presents));
    }
}

void beginInternal() {
    g_internal++;
}

void endInternal() {
    g_internal--;
}

} // namespace hook
} // namespace tmshaders
