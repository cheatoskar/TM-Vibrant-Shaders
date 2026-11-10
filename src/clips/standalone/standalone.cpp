// TM Clips, the standalone mod (TMClips.dll, loaded by the TrackMania ModLoader): the clips of
// src/clips with a Present hook and a small window of its own. With TM Vibrant Shaders in the same
// game it does nothing: TM Vibrant Shaders records and shows the settings (clips.h says how they
// find out about each other).
//
// The hooks: a NULLREF Direct3D 9 device (no window shown, no depth buffer, so TM Vibrant Shaders
// and other mods leave it alone) gives the addresses of the device's Present and Reset, which every
// device shares; those two get an inline jump (src/inlinehook.cpp, which chains with other mods'
// hooks in either order). Nothing in the game's own code is touched.
#include "clips/clips.h"
#include "clips/settings.h"
#include "inlinehook.h"
#include "keys.h"
#include "log.h"
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <string>

extern "C" IMAGE_DOS_HEADER __ImageBase;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#ifndef TMCLIPS_VERSION
#define TMCLIPS_VERSION "0.0.0"
#endif

namespace tmshaders {
namespace clips {
namespace standalone {
namespace {

using PresentFn = HRESULT(APIENTRY*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn = HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
inlinehook::Hook g_presentHook, g_resetHook;
PresentFn g_present = nullptr;
ResetFn g_reset = nullptr;
IDirect3DDevice9* g_device = nullptr; // the game's (the first hardware device that presents)

// The window.
bool g_imgui = false;
HWND g_hwnd = nullptr;
WNDPROC g_wndProc = nullptr;
bool g_open = false;
bool g_menuDown = false;
bool g_swallowed[5] = {};

const wchar_t* const kMouseOwner = L"TMMods.MouseOwner"; // shared with TM Vibrant Shaders and 100% TMX
HANDLE self() {
    return reinterpret_cast<HANDLE>(&__ImageBase);
}
void publishMouse(bool want) {
    if (!g_hwnd) return;
    HANDLE owner = GetPropW(g_hwnd, kMouseOwner);
    if (want && !owner) SetPropW(g_hwnd, kMouseOwner, self());
    else if (!want && owner == self()) RemovePropW(g_hwnd, kMouseOwner);
}
bool mouseOurs() {
    HANDLE owner = GetPropW(g_hwnd, kMouseOwner);
    return !owner || owner == self();
}

int mouseButton(UINT msg, WPARAM wparam, bool& up) {
    up = false;
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: return 0;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: return 1;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: return 2;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: return GET_XBUTTON_WPARAM(wparam) == XBUTTON2 ? 4 : 3;
    case WM_LBUTTONUP: up = true; return 0;
    case WM_RBUTTONUP: up = true; return 1;
    case WM_MBUTTONUP: up = true; return 2;
    case WM_XBUTTONUP: up = true; return GET_XBUTTON_WPARAM(wparam) == XBUTTON2 ? 4 : 3;
    }
    return -1;
}

bool ours(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!g_open || idle() || !ImGui::GetCurrentContext()) return false;
    if (msg == WM_KEYDOWN && wparam == VK_ESCAPE) {
        g_open = false;
        return true;
    }
    if (msg == WM_KEYUP && wparam == VK_ESCAPE) return true;
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && !mouseOurs()) return false;
    ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && msg != WM_MOUSEMOVE && ImGui::GetIO().WantCaptureMouse) return true;
    if ((msg == WM_KEYDOWN || msg == WM_CHAR) && ImGui::GetIO().WantCaptureKeyboard) return true;
    return false;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    bool up = false;
    const int button = mouseButton(msg, wparam, up);
    bool taken = ours(hwnd, msg, wparam, lparam);
    if (button >= 0) {
        // A release goes where its press went.
        if (up) {
            taken = g_swallowed[button];
            g_swallowed[button] = false;
        } else {
            g_swallowed[button] = taken;
        }
    }
    if (taken) return 0;
    return CallWindowProcW(g_wndProc, hwnd, msg, wparam, lparam);
}

void initImGui(IDirect3DDevice9* device) {
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (FAILED(device->GetCreationParameters(&cp)) || !cp.hFocusWindow) return;
    g_hwnd = cp.hFocusWindow;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; // the game drives its own cursor
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX9_Init(device);
    g_wndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wndProc)));
    g_imgui = true;
}

bool menuKeyPressed() {
    const std::string s_key = menuKey();
    keys::Binding b;
    if (!keys::parse(s_key.c_str(), b) || !b.vk) return false;
    const bool down = (GetAsyncKeyState(static_cast<int>(b.vk)) & 0x8000) != 0;
    unsigned mods = 0;
    if (GetAsyncKeyState(VK_SHIFT) < 0) mods |= keys::kShift;
    if (GetAsyncKeyState(VK_CONTROL) < 0) mods |= keys::kCtrl;
    if (GetAsyncKeyState(VK_MENU) < 0) mods |= keys::kAlt;
    HWND front = GetForegroundWindow();
    DWORD pid = 0;
    if (front) GetWindowThreadProcessId(front, &pid);
    const bool fire = down && !g_menuDown && mods == b.mods && pid == GetCurrentProcessId();
    g_menuDown = down;
    return fire;
}

// ImGui's DX9 renderer leaves some of the game's fixed-function state as it finds it (TM Vibrant
// Shaders found a texture transform left by the replay viewer): those are set to defaults and back.
struct CleanState {
    IDirect3DDevice9* d;
    DWORD transform = 0, coords = 0, srgbTex = 0, srgbWrite = 0, writeMask = 0;
    explicit CleanState(IDirect3DDevice9* device) : d(device) {
        d->GetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, &transform);
        d->GetTextureStageState(0, D3DTSS_TEXCOORDINDEX, &coords);
        d->GetSamplerState(0, D3DSAMP_SRGBTEXTURE, &srgbTex);
        d->GetRenderState(D3DRS_SRGBWRITEENABLE, &srgbWrite);
        d->GetRenderState(D3DRS_COLORWRITEENABLE, &writeMask);
        d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    }
    ~CleanState() {
        d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, transform);
        d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, coords);
        d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, srgbTex);
        d->SetRenderState(D3DRS_SRGBWRITEENABLE, srgbWrite);
        d->SetRenderState(D3DRS_COLORWRITEENABLE, writeMask);
    }
};

void drawOverlay(IDirect3DDevice9* device) {
    if (menuKeyPressed()) {
        g_open = !g_open;
        TMVS_LOG("window: %s", g_open ? "open" : "closed");
    }
    const bool toast = toastActive();
    if (!g_open && !toast) {
        publishMouse(false);
        return;
    }
    if (!g_imgui) initImGui(device);
    if (!g_imgui) return;
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = std::max(1.0f, io.DisplaySize.y / 1080.0f);
    io.MouseDrawCursor = g_open && io.WantCaptureMouse && mouseOurs();
    ImGui::NewFrame();
    if (toast) drawToast();
    if (g_open) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(460.0f * io.FontGlobalScale, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("TM Clips " TMCLIPS_VERSION, &g_open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
        ImGui::PushItemWidth(-140.0f * io.FontGlobalScale);
        drawPanel();
        ImGui::PopItemWidth();
        ImGui::End();
    }
    ImGui::EndFrame();
    publishMouse(g_open && io.WantCaptureMouse);
    if (SUCCEEDED(device->BeginScene())) {
        ImGui::Render();
        {
            CleanState clean(device);
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        }
        device->EndScene();
    }
}

bool gameDevice(IDirect3DDevice9* device) {
    if (g_device) return device == g_device;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (FAILED(device->GetCreationParameters(&cp)) || cp.DeviceType != D3DDEVTYPE_HAL || !cp.hFocusWindow) return false;
    g_device = device;
    IDirect3DSurface9* back = nullptr;
    D3DSURFACE_DESC desc{};
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
        back->GetDesc(&desc);
        back->Release();
    }
    TMVS_LOG("hook: the game's device %p presents (%ux%u)", device, desc.Width, desc.Height);
    return true;
}

HRESULT APIENTRY presentDetour(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty) {
    if (gameDevice(device)) {
        present(device); // the picture is taken before our window is drawn
        if (!idle()) drawOverlay(device);
        else publishMouse(false);
    }
    return g_present(device, src, dst, window, dirty);
}

HRESULT APIENTRY resetDetour(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    const bool game = device == g_device;
    if (game) {
        preReset();
        if (g_imgui) ImGui_ImplDX9_InvalidateDeviceObjects();
    }
    const HRESULT hr = g_reset(device, params);
    if (game) {
        if (g_imgui && SUCCEEDED(hr)) ImGui_ImplDX9_CreateDeviceObjects();
        postReset();
    }
    return hr;
}

bool installHooks() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = reinterpret_cast<HINSTANCE>(&__ImageBase);
    wc.lpszClassName = L"TMClipsDummy";
    RegisterClassExW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!window || !d3d) {
        TMVS_LOG("hook: no Direct3D 9 (%s)", window ? "Direct3DCreate9 failed" : "no window");
        if (d3d) d3d->Release();
        if (window) DestroyWindow(window);
        return false;
    }
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    IDirect3DDevice9* dummy = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT,
                                   &pp, &dummy);
    if (FAILED(hr) || !dummy) {
        TMVS_LOG("hook: no NULLREF device for the addresses (0x%08lx)", static_cast<unsigned long>(hr));
        d3d->Release();
        DestroyWindow(window);
        return false;
    }
    void** table = *reinterpret_cast<void***>(dummy);
    void* present = table[17];
    void* reset = table[16];
    dummy->Release();
    d3d->Release();
    DestroyWindow(window);
    const char* why = nullptr;
    if (!inlinehook::install(reset, reinterpret_cast<void*>(&resetDetour), g_resetHook, &why)) {
        TMVS_LOG("hook: Reset can't be hooked (%s)", why ? why : "?");
        return false;
    }
    g_reset = reinterpret_cast<ResetFn>(g_resetHook.trampoline);
    if (!inlinehook::install(present, reinterpret_cast<void*>(&presentDetour), g_presentHook, &why)) {
        TMVS_LOG("hook: Present can't be hooked (%s)", why ? why : "?");
        inlinehook::remove(g_resetHook);
        return false;
    }
    g_present = reinterpret_cast<PresentFn>(g_presentHook.trampoline);
    TMVS_LOG("hook: Present and Reset hooked%s%s", g_presentHook.chained ? " (Present chained onto another mod's hook)" : "",
             g_resetHook.chained ? " (Reset chained onto another mod's hook)" : "");
    return true;
}

DWORD WINAPI bootThread(LPVOID) {
    log::setDataDir(dataDir());
    log::setFileName(L"tmclips.log");
    TMVS_LOG("TM Clips %s starting", TMCLIPS_VERSION);
    // The mod loader loads the mods one after another at the start: TM Vibrant Shaders (with clips)
    // may come after us. Give it 3 s; when it is there it records and nothing here is hooked.
    for (int i = 0; i < 12; i++) {
        if (tmvsRecords()) {
            TMVS_LOG("clips: TM Vibrant Shaders runs in this game and records the clips: TM Clips stays idle (no hooks)");
            return 0;
        }
        Sleep(250);
    }
    boot(Host::Standalone);
    if (!installHooks()) TMVS_LOG("clips: no hooks, no clips");
    return 0;
}

} // namespace

bool claim() {
    wchar_t name[96];
    swprintf(name, 96, L"Local\\TMClips_Standalone_%lu", GetCurrentProcessId());
    HANDLE mutex = CreateMutexW(nullptr, FALSE, name); // kept for the life of the process
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return false; // a second copy: the first one works
    }
    return true;
}

void start() {
    if (HANDLE thread = CreateThread(nullptr, 0, bootThread, nullptr, 0, nullptr)) CloseHandle(thread);
}

} // namespace standalone
} // namespace clips
} // namespace tmshaders

extern "C" BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(reinterpret_cast<HMODULE>(&__ImageBase));
        if (tmshaders::clips::standalone::claim()) tmshaders::clips::standalone::start();
    }
    return TRUE;
}
