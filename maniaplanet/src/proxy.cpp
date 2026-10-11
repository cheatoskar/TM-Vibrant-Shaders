// Copied next to ManiaPlanet.exe as d3d11.dll, this DLL is the game's Direct3D 11. The two device
// creation functions go to the system d3d11.dll and hook the new device; every other export jumps
// straight to the system DLL (forward.asm).
//
// Other programs that happen to load it (the launcher in the same folder) get plain forwarding.
#include "capture.h"
#include "hooks.h"
#include "log.h"
#include "settings.h"
#include <windows.h>
#include <d3d11.h>
#include <iterator>

extern "C" void* g_forward[];
void* g_forward[64] = {};

namespace tmvs_mp {
namespace proxy {
namespace {

const char* const kForwardNames[] = {
#include "forward_names.inc"
};

HMODULE g_system = nullptr;
bool g_active = false;

using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                        ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
                                                    UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
                                                    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
CreateDeviceFn g_createDevice = nullptr;
CreateDeviceAndSwapChainFn g_createDeviceAndSwapChain = nullptr;

} // namespace

// Called from DllMain: loads the system d3d11.dll by its full path (the plain name would be us).
bool init(bool active) {
    g_active = active;
    wchar_t path[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n > MAX_PATH - 16) return false;
    wcscat_s(path, L"\\d3d11.dll");
    g_system = LoadLibraryW(path);
    if (!g_system) return false;
    static_assert(std::size(kForwardNames) <= 64, "g_forward is too small");
    for (size_t i = 0; i < std::size(kForwardNames); i++) g_forward[i] = reinterpret_cast<void*>(GetProcAddress(g_system, kForwardNames[i]));
    g_createDevice = reinterpret_cast<CreateDeviceFn>(GetProcAddress(g_system, "D3D11CreateDevice"));
    g_createDeviceAndSwapChain = reinterpret_cast<CreateDeviceAndSwapChainFn>(GetProcAddress(g_system, "D3D11CreateDeviceAndSwapChain"));
    return g_createDevice && g_createDeviceAndSwapChain;
}

bool active() {
    return g_active;
}

void startOnce() {
    static bool started = false;
    if (started || !g_active) return;
    started = true;
    TMVS_LOG("TM Vibrant Shaders for ManiaPlanet %s", TMVS_MP_VERSION);
    settings::load();
}

} // namespace proxy
} // namespace tmvs_mp

using namespace tmvs_mp;

extern "C" HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                                            const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdk, ID3D11Device** device,
                                            D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** ctx) {
    if (!proxy::g_createDevice) return E_FAIL;
    proxy::startOnce();
    // The device is needed to hook it, even when the caller only asks whether it would work.
    ID3D11Device* created = nullptr;
    const HRESULT hr = proxy::g_createDevice(adapter, type, software, flags, levels, levelCount, sdk,
                                             device || !proxy::g_active ? device : &created, level, ctx);
    if (SUCCEEDED(hr) && proxy::g_active && device && *device) hooks::onDevice(*device);
    if (created) created->Release();
    return hr;
}

extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software,
                                                        UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount,
                                                        UINT sdk, const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain,
                                                        ID3D11Device** device, D3D_FEATURE_LEVEL* level,
                                                        ID3D11DeviceContext** ctx) {
    if (!proxy::g_createDeviceAndSwapChain) return E_FAIL;
    proxy::startOnce();
    const HRESULT hr = proxy::g_createDeviceAndSwapChain(adapter, type, software, flags, levels, levelCount, sdk, desc,
                                                         swapChain, device, level, ctx);
    if (SUCCEEDED(hr) && proxy::g_active) {
        if (device && *device) hooks::onDevice(*device);
        if (swapChain && *swapChain) hooks::onSwapChain(*swapChain);
    }
    return hr;
}

// For tools/harness.cpp: what the last frame did, and a capture of the next one.
extern "C" void WINAPI TmvsMpStats(int* out) {
    const hooks::Stats& s = hooks::stats();
    out[0] = static_cast<int>(s.placed);
    out[1] = s.toneMapSeen ? 1 : 0;
    out[2] = static_cast<int>(s.shadersKnown);
    out[3] = static_cast<int>(s.shadersCreated);
    out[4] = s.depthFound ? 1 : 0;
    out[5] = static_cast<int>(s.drawsLastFrame);
}

extern "C" void WINAPI TmvsMpCapture() {
    capture::request();
}
