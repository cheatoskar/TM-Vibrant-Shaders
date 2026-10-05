#include "hook.h"
#include "proxy.h"
#include "tracer.h"
#include "log.h"
#include <windows.h>
#include <d3d9.h>

#pragma comment(lib, "d3d9.lib")

namespace tmshaders {
namespace hook {
namespace {

// IDirect3DDevice9 vtable slots.
enum Slot : int {
    kReset = 16,
    kPresent = 17,
    kCreateTexture = 23,
    kCreateRenderTarget = 28,
    kCreateDepthStencilSurface = 29,
    kStretchRect = 34,
    kSetRenderTarget = 37,
    kSetDepthStencilSurface = 39,
    kBeginScene = 41,
    kEndScene = 42,
    kClear = 43,
    kSetTransform = 44,
    kSetViewport = 47,
    kSetLight = 51,
    kLightEnable = 53,
    kSetTexture = 65,
    kDrawPrimitive = 81,
    kDrawIndexedPrimitive = 82,
    kDrawPrimitiveUP = 83,
    kDrawIndexedPrimitiveUP = 84,
    kSetVertexShader = 92,
    kSetVertexShaderConstantF = 94,
    kSetPixelShader = 107,
    kSetPixelShaderConstantF = 109,
    kSlotCount = 119
};

void** g_vtable = nullptr;
void* g_original[kSlotCount] = {};
DeviceCallbacks g_callbacks;
int g_internal = 0;

template <typename Fn>
Fn original(Slot slot) {
    return reinterpret_cast<Fn>(g_original[slot]);
}

bool writePointer(void** slot, void* value) {
    DWORD previous = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &previous)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), previous, &previous);
    return true;
}

bool tracingGame() {
    return g_internal == 0 && tracer::tracing();
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
    tracer::event("  %s prims=%u z=%lu zw=%lu blend=%lu cw=%lx vs=%p ps=%p", kind, primitives, z, zw, blend, cw,
                  static_cast<void*>(vs), static_cast<void*>(ps));
    if (vs) vs->Release();
    if (ps) ps->Release();
}

// --- Detours ---------------------------------------------------------------

HRESULT APIENTRY resetDetour(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    if (g_callbacks.preReset) g_callbacks.preReset();
    if (params && g_callbacks.adjustPresentParams) g_callbacks.adjustPresentParams(params);
    HRESULT hr = original<HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*)>(kReset)(device, params);
    TMVS_LOG("Reset -> 0x%08lx (%ux%u fmt=%s ms=%d autoDepth=%d %s windowed=%d)", hr, params->BackBufferWidth,
             params->BackBufferHeight, tracer::formatName(params->BackBufferFormat), params->MultiSampleType,
             params->EnableAutoDepthStencil, tracer::formatName(params->AutoDepthStencilFormat), params->Windowed);
    if (SUCCEEDED(hr) && g_callbacks.postReset) {
        g_internal++;
        g_callbacks.postReset(device);
        g_internal--;
    }
    return hr;
}

HRESULT APIENTRY presentDetour(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty) {
    if (g_callbacks.present) {
        g_internal++;
        g_callbacks.present(device);
        g_internal--;
    }
    if (tracingGame()) tracer::event("Present");
    tracer::onPresent(device);
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*)>(kPresent)(
        device, src, dst, window, dirty);
}

HRESULT APIENTRY createTextureDetour(IDirect3DDevice9* device, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format,
                                     D3DPOOL pool, IDirect3DTexture9** out, HANDLE* shared) {
    HRESULT hr = original<HRESULT(APIENTRY*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**,
                                             HANDLE*)>(kCreateTexture)(device, w, h, levels, usage, format, pool, out, shared);
    if (g_internal == 0 && (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) {
        TMVS_LOG("CreateTexture %ux%u levels=%u usage=0x%lx fmt=%s -> %p", w, h, levels, usage, tracer::formatName(format),
                 SUCCEEDED(hr) && out ? static_cast<void*>(*out) : nullptr);
    }
    return hr;
}

HRESULT APIENTRY createRenderTargetDetour(IDirect3DDevice9* device, UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms,
                                          DWORD quality, BOOL lockable, IDirect3DSurface9** out, HANDLE* shared) {
    HRESULT hr = original<HRESULT(APIENTRY*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                             IDirect3DSurface9**, HANDLE*)>(kCreateRenderTarget)(device, w, h, format, ms, quality,
                                                                                                 lockable, out, shared);
    if (g_internal == 0) {
        TMVS_LOG("CreateRenderTarget %ux%u fmt=%s ms=%d -> %p", w, h, tracer::formatName(format), ms,
                 SUCCEEDED(hr) && out ? static_cast<void*>(*out) : nullptr);
    }
    return hr;
}

HRESULT APIENTRY createDepthStencilDetour(IDirect3DDevice9* device, UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms,
                                          DWORD quality, BOOL discard, IDirect3DSurface9** out, HANDLE* shared) {
    if (g_internal == 0 && g_callbacks.createDepthStencil) {
        HRESULT replaced = E_NOTIMPL;
        if (g_callbacks.createDepthStencil(device, w, h, format, ms, out, &replaced)) return replaced;
    }
    HRESULT hr = original<HRESULT(APIENTRY*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                             IDirect3DSurface9**, HANDLE*)>(kCreateDepthStencilSurface)(device, w, h, format, ms,
                                                                                                         quality, discard, out,
                                                                                                         shared);
    if (g_internal == 0) {
        TMVS_LOG("CreateDepthStencilSurface %ux%u fmt=%s ms=%d -> %p", w, h, tracer::formatName(format), ms,
                 SUCCEEDED(hr) && out ? static_cast<void*>(*out) : nullptr);
    }
    return hr;
}

HRESULT APIENTRY stretchRectDetour(IDirect3DDevice9* device, IDirect3DSurface9* src, const RECT* srcRect, IDirect3DSurface9* dst,
                                   const RECT* dstRect, D3DTEXTUREFILTERTYPE filter) {
    if (tracingGame()) tracer::event("StretchRect %s -> %s", tracer::describe(src), tracer::describe(dst));
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*,
                                       D3DTEXTUREFILTERTYPE)>(kStretchRect)(device, src, srcRect, dst, dstRect, filter);
}

HRESULT APIENTRY setRenderTargetDetour(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface) {
    if (tracingGame()) tracer::event("SetRenderTarget %lu %s", index, tracer::describe(surface));
    if (g_internal == 0 && g_callbacks.setRenderTarget) g_callbacks.setRenderTarget(index, surface);
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*)>(kSetRenderTarget)(device, index, surface);
}

HRESULT APIENTRY setDepthStencilDetour(IDirect3DDevice9* device, IDirect3DSurface9* surface) {
    if (tracingGame()) tracer::event("SetDepthStencilSurface %s", tracer::describe(surface));
    if (g_internal == 0 && g_callbacks.setDepthStencil) surface = g_callbacks.setDepthStencil(surface);
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, IDirect3DSurface9*)>(kSetDepthStencilSurface)(device, surface);
}

HRESULT APIENTRY beginSceneDetour(IDirect3DDevice9* device) {
    if (tracingGame()) tracer::event("BeginScene");
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*)>(kBeginScene)(device);
}

HRESULT APIENTRY endSceneDetour(IDirect3DDevice9* device) {
    if (tracingGame()) tracer::event("EndScene");
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*)>(kEndScene)(device);
}

HRESULT APIENTRY clearDetour(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z,
                             DWORD stencil) {
    if (tracingGame()) tracer::event("Clear rects=%lu flags=%lx color=%08lx z=%.3f s=%lu", count, flags, color, z, stencil);
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD)>(kClear)(
        device, count, rects, flags, color, z, stencil);
}

HRESULT APIENTRY setTransformDetour(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE type, const D3DMATRIX* m) {
    if (tracingGame() && m) {
        tracer::event("SetTransform %d [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]",
                      static_cast<int>(type), m->_11, m->_12, m->_13, m->_14, m->_21, m->_22, m->_23, m->_24, m->_31, m->_32,
                      m->_33, m->_34, m->_41, m->_42, m->_43, m->_44);
    }
    if (g_internal == 0 && m && g_callbacks.setTransform) g_callbacks.setTransform(type, m);
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*)>(kSetTransform)(device, type, m);
}

HRESULT APIENTRY setLightDetour(IDirect3DDevice9* device, DWORD index, const D3DLIGHT9* light) {
    if (tracingGame() && light) {
        tracer::event("SetLight %lu type=%d dir=(%.4f %.4f %.4f) pos=(%.1f %.1f %.1f) diffuse=(%.3f %.3f %.3f) ambient=(%.3f %.3f %.3f)",
                      index, static_cast<int>(light->Type), light->Direction.x, light->Direction.y, light->Direction.z,
                      light->Position.x, light->Position.y, light->Position.z, light->Diffuse.r, light->Diffuse.g, light->Diffuse.b,
                      light->Ambient.r, light->Ambient.g, light->Ambient.b);
    }
    if (g_internal == 0 && light && g_callbacks.setLight) g_callbacks.setLight(index, light);
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, DWORD, const D3DLIGHT9*)>(kSetLight)(device, index, light);
}

HRESULT APIENTRY lightEnableDetour(IDirect3DDevice9* device, DWORD index, BOOL enable) {
    if (tracingGame()) tracer::event("LightEnable %lu %d", index, enable);
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, DWORD, BOOL)>(kLightEnable)(device, index, enable);
}

HRESULT APIENTRY setViewportDetour(IDirect3DDevice9* device, const D3DVIEWPORT9* vp) {
    if (tracingGame() && vp) {
        tracer::event("SetViewport %lu,%lu %lux%lu z=%.2f..%.2f", vp->X, vp->Y, vp->Width, vp->Height, vp->MinZ, vp->MaxZ);
    }
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, const D3DVIEWPORT9*)>(kSetViewport)(device, vp);
}

HRESULT APIENTRY setTextureDetour(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture) {
    if (tracingGame() && texture) {
        D3DSURFACE_DESC desc{};
        bool interesting = texture->GetType() != D3DRTYPE_TEXTURE ||
                           (SUCCEEDED(static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &desc)) &&
                            (desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)));
        if (interesting) tracer::event("  SetTexture %lu %s", stage, tracer::describe(texture));
    }
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*)>(kSetTexture)(device, stage, texture);
}

HRESULT APIENTRY drawPrimitiveDetour(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT start, UINT count) {
    if (tracingGame()) traceDrawState(device, "DP", count);
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT)>(kDrawPrimitive)(device, type, start, count);
}

HRESULT APIENTRY drawIndexedPrimitiveDetour(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices,
                                            UINT start, UINT count) {
    if (tracingGame()) traceDrawState(device, "DIP", count);
    if (g_internal == 0 && g_callbacks.draw) g_callbacks.draw(device);
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT)>(kDrawIndexedPrimitive)(
        device, type, base, minIndex, vertices, start, count);
}

HRESULT APIENTRY drawPrimitiveUPDetour(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) {
    if (tracingGame()) traceDrawState(device, "DPUP", count);
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT)>(kDrawPrimitiveUP)(
        device, type, count, data, stride);
}

HRESULT APIENTRY drawIndexedPrimitiveUPDetour(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT minIndex, UINT vertices,
                                              UINT count, const void* indices, D3DFORMAT indexFormat, const void* data, UINT stride) {
    if (tracingGame()) traceDrawState(device, "DIPUP", count);
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*,
                                       UINT)>(kDrawIndexedPrimitiveUP)(device, type, minIndex, vertices, count, indices, indexFormat,
                                                                       data, stride);
}

HRESULT APIENTRY setVertexShaderDetour(IDirect3DDevice9* device, IDirect3DVertexShader9* shader) {
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, IDirect3DVertexShader9*)>(kSetVertexShader)(device, shader);
}

HRESULT APIENTRY setVertexShaderConstantFDetour(IDirect3DDevice9* device, UINT start, const float* data, UINT count) {
    if (tracingGame() && data) {
        char line[900];
        int n = snprintf(line, sizeof(line), "  VSConst c%u x%u:", start, count);
        for (UINT i = 0; i < count * 4 && i < 16 * 4 && n < 860; i++) {
            n += snprintf(line + n, sizeof(line) - n, "%s%.4g", (i % 4 == 0) ? " | " : " ", data[i]);
        }
        tracer::event("%s", line);
    }
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, UINT, const float*, UINT)>(kSetVertexShaderConstantF)(device, start, data,
                                                                                                            count);
}

HRESULT APIENTRY setPixelShaderDetour(IDirect3DDevice9* device, IDirect3DPixelShader9* shader) {
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, IDirect3DPixelShader9*)>(kSetPixelShader)(device, shader);
}

HRESULT APIENTRY setPixelShaderConstantFDetour(IDirect3DDevice9* device, UINT start, const float* data, UINT count) {
    if (tracingGame() && data) {
        char line[600];
        int n = snprintf(line, sizeof(line), "  PSConst c%u x%u:", start, count);
        for (UINT i = 0; i < count * 4 && i < 8 * 4 && n < 560; i++) {
            n += snprintf(line + n, sizeof(line) - n, "%s%.4g", (i % 4 == 0) ? " | " : " ", data[i]);
        }
        tracer::event("%s", line);
    }
    return original<HRESULT(APIENTRY*)(IDirect3DDevice9*, UINT, const float*, UINT)>(kSetPixelShaderConstantF)(device, start, data,
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
    {kCreateRenderTarget, reinterpret_cast<void*>(&createRenderTargetDetour)},
    {kCreateDepthStencilSurface, reinterpret_cast<void*>(&createDepthStencilDetour)},
    {kStretchRect, reinterpret_cast<void*>(&stretchRectDetour)},
    {kSetRenderTarget, reinterpret_cast<void*>(&setRenderTargetDetour)},
    {kSetDepthStencilSurface, reinterpret_cast<void*>(&setDepthStencilDetour)},
    {kBeginScene, reinterpret_cast<void*>(&beginSceneDetour)},
    {kEndScene, reinterpret_cast<void*>(&endSceneDetour)},
    {kClear, reinterpret_cast<void*>(&clearDetour)},
    {kSetTransform, reinterpret_cast<void*>(&setTransformDetour)},
    {kSetViewport, reinterpret_cast<void*>(&setViewportDetour)},
    {kSetLight, reinterpret_cast<void*>(&setLightDetour)},
    {kLightEnable, reinterpret_cast<void*>(&lightEnableDetour)},
    {kSetTexture, reinterpret_cast<void*>(&setTextureDetour)},
    {kDrawPrimitive, reinterpret_cast<void*>(&drawPrimitiveDetour)},
    {kDrawIndexedPrimitive, reinterpret_cast<void*>(&drawIndexedPrimitiveDetour)},
    {kDrawPrimitiveUP, reinterpret_cast<void*>(&drawPrimitiveUPDetour)},
    {kDrawIndexedPrimitiveUP, reinterpret_cast<void*>(&drawIndexedPrimitiveUPDetour)},
    {kSetVertexShader, reinterpret_cast<void*>(&setVertexShaderDetour)},
    {kSetVertexShaderConstantF, reinterpret_cast<void*>(&setVertexShaderConstantFDetour)},
    {kSetPixelShader, reinterpret_cast<void*>(&setPixelShaderDetour)},
    {kSetPixelShaderConstantF, reinterpret_cast<void*>(&setPixelShaderConstantFDetour)},
};

bool patchDevice(IDirect3DDevice9* device) {
    void** vtable = *reinterpret_cast<void***>(device);
    if (g_vtable == vtable) return true;
    if (g_vtable) {
        // Vertex-processing modes get different driver vtables: the probe device's table is
        // not the game's. Move the hooks to the device the game actually renders with.
        TMVS_LOG("hook: moving hooks from probe vtable %p to game vtable %p", g_vtable, vtable);
        for (const Detour& d : kDetours) writePointer(&g_vtable[d.slot], g_original[d.slot]);
    }
    g_vtable = vtable;
    for (const Detour& d : kDetours) {
        g_original[d.slot] = vtable[d.slot];
        writePointer(&vtable[d.slot], d.function);
    }
    TMVS_LOG("hook: device vtable %p patched", vtable);
    return true;
}

// --- Device creation interception -------------------------------------------

using CreateDeviceFn = HRESULT(APIENTRY*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
constexpr int kCreateDeviceIndex = 16;
CreateDeviceFn g_realCreateDevice = nullptr;

HRESULT APIENTRY createDeviceDetour(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                    D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** out) {
    if (params && g_callbacks.adjustPresentParams) g_callbacks.adjustPresentParams(params);
    HRESULT hr = g_realCreateDevice(self, adapter, type, window, flags, params, out);
    if (params) {
        TMVS_LOG("CreateDevice -> 0x%08lx flags=0x%lx %ux%u fmt=%s ms=%d autoDepth=%d %s windowed=%d", hr, flags,
                 params->BackBufferWidth, params->BackBufferHeight, tracer::formatName(params->BackBufferFormat),
                 params->MultiSampleType, params->EnableAutoDepthStencil, tracer::formatName(params->AutoDepthStencilFormat),
                 params->Windowed);
    }
    if (SUCCEEDED(hr) && out && *out) {
        patchDevice(*out);
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

IDirect3DDevice9* makeProbeDevice(IDirect3D9* d3d, HWND window) {
    HWND targets[] = {window, GetDesktopWindow()};
    for (HWND target : targets) {
        D3DPRESENT_PARAMETERS params{};
        params.Windowed = TRUE;
        params.SwapEffect = D3DSWAPEFFECT_DISCARD;
        params.hDeviceWindow = target;
        params.BackBufferWidth = 16;
        params.BackBufferHeight = 16;
        IDirect3DDevice9* device = nullptr;
        DWORD flags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES | D3DCREATE_FPU_PRESERVE;
        if (SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, target, flags, &params, &device)) && device) {
            return device;
        }
    }
    return nullptr;
}

} // namespace

bool install(const DeviceCallbacks& callbacks) {
    g_callbacks = callbacks;
    if (g_vtable) return true;

    bool imported = patchImport("d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(&create9Detour),
                                reinterpret_cast<void**>(&g_realCreate9));
    TMVS_LOG("hook: Direct3DCreate9 import %s", imported ? "patched" : "not found");

    // The game may already own a device (late injection) or create it later. Device
    // vtables are shared per driver, so patching a probe device covers both cases.
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TMVSProbeWindow";
    RegisterClassExW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) return false;

    // The system d3d9 by full path: in standalone mode this DLL is "d3d9.dll" itself.
    if (IDirect3D9* d3d = proxy::createDirect3D9(D3D_SDK_VERSION)) {
        if (IDirect3DDevice9* device = makeProbeDevice(d3d, window)) {
            g_internal++;
            patchDevice(device);
            g_internal--;
            device->Release();
        }
        patchFactory(d3d);
        d3d->Release();
    }

    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return g_vtable != nullptr;
}

void remove() {
    if (!g_vtable) return;
    for (const Detour& d : kDetours) {
        if (g_original[d.slot]) writePointer(&g_vtable[d.slot], g_original[d.slot]);
    }
    g_vtable = nullptr;
}

void beginInternal() {
    g_internal++;
}

void endInternal() {
    g_internal--;
}

} // namespace hook
} // namespace tmshaders
