#include "hook.h"
#include "overlay.h"
#include "renderer.h"
#include "config.h"
#include <windows.h>
#include <d3d9.h>
#include <tlhelp32.h>
#include <string>

#pragma comment(lib, "d3d9.lib")

namespace tmshaders {
namespace hook {
namespace {

using EndSceneFn = HRESULT(APIENTRY*)(IDirect3DDevice9*);
using PresentFn = HRESULT(APIENTRY*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn = HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using CreateExFn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);

constexpr int kResetIndex = 16;
constexpr int kPresentIndex = 17;
constexpr int kEndSceneIndex = 42;

struct Patched {
    const char* name = "";
    void** vtable = nullptr;
    EndSceneFn endScene = nullptr;
    PresentFn present = nullptr;
    ResetFn reset = nullptr;
};

Patched g_tables[8];
int g_tableCount = 0;
bool g_drewOnce = false;

bool writePointer(void** slot, void* value) {
    DWORD previous = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &previous)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), previous, &previous);
    return true;
}

Patched* tableFor(void** vtable) {
    for (int i = 0; i < g_tableCount; i++) {
        if (g_tables[i].vtable == vtable) return &g_tables[i];
    }
    return nullptr;
}

Patched* tableOf(IDirect3DDevice9* device) {
    return tableFor(*reinterpret_cast<void***>(device));
}

void drawFrame(IDirect3DDevice9* device) {
    g_drewOnce = true;
    Renderer::get().render(device, Config::get().settings);
    overlay::draw(device);
}

HRESULT APIENTRY endSceneDetour(IDirect3DDevice9* device) {
    Patched* table = tableOf(device);
    if (!table) return S_OK;
    drawFrame(device);
    return table->endScene(device);
}

HRESULT APIENTRY presentDetour(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty) {
    Patched* table = tableOf(device);
    if (!table) return S_OK;
    if (!g_drewOnce) drawFrame(device);
    return table->present(device, src, dst, window, dirty);
}

HRESULT APIENTRY resetDetour(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    Patched* table = tableOf(device);
    Renderer::get().preReset();
    overlay::preReset();
    HRESULT hr = table ? table->reset(device, params) : D3DERR_INVALIDCALL;
    overlay::postReset();
    Renderer::get().postReset();
    return hr;
}

D3DPRESENT_PARAMETERS probeParams(HWND target) {
    D3DPRESENT_PARAMETERS params{};
    params.Windowed = TRUE;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = target;
    params.BackBufferFormat = D3DFMT_UNKNOWN;
    params.BackBufferWidth = 16;
    params.BackBufferHeight = 16;
    return params;
}

constexpr DWORD kProbeFlags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES | D3DCREATE_FPU_PRESERVE;

IDirect3DDevice9* makeProbeDevice(IDirect3D9* d3d, HWND window) {
    HWND targets[] = { window, GetDesktopWindow() };
    for (HWND target : targets) {
        D3DPRESENT_PARAMETERS params = probeParams(target);
        IDirect3DDevice9* device = nullptr;
        if (SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, target, kProbeFlags, &params, &device)) && device) {
            return device;
        }
    }
    return nullptr;
}

bool patchTable(const char* name, IDirect3DDevice9* device) {
    if (!device || g_tableCount >= 8) return false;
    void** vtable = *reinterpret_cast<void***>(device);
    if (tableFor(vtable)) return true;

    Patched& table = g_tables[g_tableCount];
    table.name = name;
    table.vtable = vtable;
    table.endScene = reinterpret_cast<EndSceneFn>(vtable[kEndSceneIndex]);
    table.present = reinterpret_cast<PresentFn>(vtable[kPresentIndex]);
    table.reset = reinterpret_cast<ResetFn>(vtable[kResetIndex]);
    g_tableCount++;

    return writePointer(&vtable[kEndSceneIndex], reinterpret_cast<void*>(&endSceneDetour)) &&
           writePointer(&vtable[kPresentIndex], reinterpret_cast<void*>(&presentDetour)) &&
           writePointer(&vtable[kResetIndex], reinterpret_cast<void*>(&resetDetour));
}

using CreateDeviceFn = HRESULT(APIENTRY*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
constexpr int kCreateDeviceIndex = 16;
CreateDeviceFn g_realCreateDevice = nullptr;

HRESULT APIENTRY createDeviceDetour(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags, D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** out) {
    HRESULT hr = g_realCreateDevice(self, adapter, type, window, flags, params, out);
    if (SUCCEEDED(hr) && out && *out) {
        patchTable("game", *out);
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

bool patchImport(const char* dll, const char* function, void* replacement, void** original) {
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

            *original = reinterpret_cast<void*>(addresses->u1.Function);
            return writePointer(reinterpret_cast<void**>(&addresses->u1.Function), replacement);
        }
    }
    return false;
}

} // namespace

bool install() {
    if (g_tableCount > 0) return true;

    patchImport("d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(&create9Detour), reinterpret_cast<void**>(&g_realCreate9));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TMShaderProbeWindow";
    RegisterClassExW(&wc);

    HWND window = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) return false;

    if (IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION)) {
        if (IDirect3DDevice9* device = makeProbeDevice(d3d, window)) {
            patchTable("plain", device);
            device->Release();
        }
        d3d->Release();
    }

    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return g_tableCount > 0;
}

void remove() {
    for (int i = 0; i < g_tableCount; i++) {
        Patched& table = g_tables[i];
        if (table.endScene) writePointer(&table.vtable[kEndSceneIndex], reinterpret_cast<void*>(table.endScene));
        if (table.present) writePointer(&table.vtable[kPresentIndex], reinterpret_cast<void*>(table.present));
        if (table.reset) writePointer(&table.vtable[kResetIndex], reinterpret_cast<void*>(table.reset));
    }
    g_tableCount = 0;
}

} // namespace hook
} // namespace tmshaders
