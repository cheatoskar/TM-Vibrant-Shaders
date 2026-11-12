// Horizon menu preview with the TMVS 2.0 modules (development tool): Horizon Host's own menu (its UI
// sources from horizon-host, HORIZON_HOST_DIR in CMakeLists) drawn offscreen on a hidden window's D3D9
// device, with this build's four module plugins loaded by the fake host (tools/fakehost.h) and their
// pages drawn by tmvs-core.dll inside it, as in the game. One PNG per scene:
//   tmvs-settings, tmvs-shaders, capture-settings, capture-clips, upscaling, packs, hotkeys
// Usage: tmvs_hzpreview <out folder> [scene]. Writes its settings under <exe folder>\hzpreview-data (a fresh 1.5
// style settings.ini), never into the player's Documents; reads the player's resource packs for the Packs
// page as the game would (read only).
#include <windows.h>

#include <d3d9.h>

#include <initguid.h>
#include <wincodec.h>

#ifdef small
#undef small // rpcndr.h: would break Fonts::small in ui.h
#endif
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "host/ui/ui.h"
#include "imgui.h"
#include "fakehost.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using namespace horizon::ui;

namespace {

const int kWidth = 1600, kHeight = 900;

std::wstring exeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    return dir.substr(0, dir.find_last_of(L"\\/"));
}

std::wstring widen(const std::string& s) {
    wchar_t w[MAX_PATH] = {};
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w, MAX_PATH);
    return w;
}

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

// A PNG as a managed texture (plugin icons, Horizon's logo). 0 = missing.
unsigned long long loadTexture(IDirect3DDevice9* dev, const std::wstring& path, float* aspect = nullptr) {
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return 0;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    IDirect3DTexture9* tex = nullptr;
    UINT w = 0, h = 0;
    if (SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(conv->GetSize(&w, &h)) && SUCCEEDED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
            conv->CopyPixels(nullptr, static_cast<UINT>(lr.Pitch), static_cast<UINT>(lr.Pitch) * h, static_cast<BYTE*>(lr.pBits));
            tex->UnlockRect(0);
        }
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    factory->Release();
    if (aspect && h) *aspect = static_cast<float>(w) / static_cast<float>(h);
    return reinterpret_cast<unsigned long long>(tex);
}

bool savePng(IDirect3DDevice9* dev, const std::wstring& path) {
    IDirect3DSurface9* back = nullptr;
    IDirect3DSurface9* sys = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back))) return false;
    D3DSURFACE_DESC desc;
    back->GetDesc(&desc);
    bool ok = false;
    if (SUCCEEDED(dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(dev->GetRenderTargetData(back, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            std::vector<BYTE> rgb(static_cast<size_t>(desc.Width) * desc.Height * 3);
            for (UINT y = 0; y < desc.Height; ++y) {
                const BYTE* src = static_cast<const BYTE*>(lr.pBits) + static_cast<size_t>(y) * static_cast<size_t>(lr.Pitch);
                BYTE* dst = &rgb[static_cast<size_t>(y) * desc.Width * 3];
                for (UINT x = 0; x < desc.Width; ++x) {
                    dst[x * 3 + 0] = src[x * 4 + 0];
                    dst[x * 3 + 1] = src[x * 4 + 1];
                    dst[x * 3 + 2] = src[x * 4 + 2];
                }
            }
            sys->UnlockRect();
            IWICImagingFactory* factory = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
                IWICStream* stream = nullptr;
                IWICBitmapEncoder* enc = nullptr;
                IWICBitmapFrameEncode* frame = nullptr;
                if (SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
                    SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
                    SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
                    SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(desc.Width, desc.Height))) {
                    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
                    frame->SetPixelFormat(&fmt);
                    ok = SUCCEEDED(frame->WritePixels(desc.Height, desc.Width * 3, static_cast<UINT>(rgb.size()), rgb.data())) &&
                         SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
                }
                if (frame) frame->Release();
                if (enc) enc->Release();
                if (stream) stream->Release();
                factory->Release();
            }
        }
    }
    if (sys) sys->Release();
    back->Release();
    return ok;
}

std::string utf8(unsigned codepoint) {
    std::string s;
    if (codepoint < 0x80) s += static_cast<char>(codepoint);
    else if (codepoint < 0x800) {
        s += static_cast<char>(0xC0 | (codepoint >> 6));
        s += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        s += static_cast<char>(0xE0 | (codepoint >> 12));
        s += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    return s;
}

Model g_model;
Actions g_actions;

// The menu's model from what the plugins registered (as src/host/overlay.cpp builds it).
void fillModel() {
    g_model = Model();
    Model& m = g_model;
    m.hostVersion = "0.2.0";
    m.gameName = "TrackMania Nations Forever";
    m.exeVersion = "2.11.26";
    m.profileName = "tmnf";
    m.menuKey = "Insert";
    m.gameState = "Main menu";
    m.barEnabled = true;
    m.menuHotkey = 1;
    m.onlineFix = -1;
    m.menuKeySuggestions = {"Insert", "F2", "#", "+", "Ctrl+Tab"};
    HotkeyView menu;
    menu.handle = 100000;
    menu.ownerId = "host";
    menu.ownerName = "Horizon";
    menu.name = "Open menu";
    menu.key = "Insert";
    m.hotkeys.push_back(menu);
    for (auto& p : fakehost::g_plugins) {
        PluginView v;
        v.id = p->id;
        v.name = p->info->name;
        v.version = p->info->version;
        v.author = p->info->author;
        v.description = p->info->description;
        v.status = p->running ? PluginStatus::On : PluginStatus::Failed;
        v.enabled = p->enabled;
        v.hasMenu = p->drawMenu != nullptr;
        v.liveToggle = true;
        for (const fakehost::Panel& panel : p->panels) {
            PanelView pv;
            pv.key = panel.key;
            pv.title = panel.title;
            pv.icon = panel.icon;
            pv.label = (panel.icon ? utf8(panel.icon) + "  " : std::string()) + panel.title;
            v.panels.push_back(pv);
        }
        m.plugins.push_back(v);
        for (const fakehost::Hotkey& k : p->hotkeys) {
            HotkeyView h;
            h.handle = k.handle;
            h.ownerId = p->id;
            h.ownerName = p->info->name;
            h.name = k.name;
            h.key = k.key;
            m.hotkeys.push_back(h);
        }
    }
    static std::vector<LogLine> log;
    log.clear();
    for (const std::string& line : fakehost::g_log) log.push_back(LogLine{1, line});
    m.log = &log;
    g_actions.drawPluginPage = [](const std::string& id) {
        if (fakehost::Plugin* p = fakehost::find(id.c_str())) {
            if (p->drawMenu) p->drawMenu();
        }
    };
    g_actions.drawPanelTab = [](const std::string& id, const std::string& key) {
        if (fakehost::Plugin* p = fakehost::find(id.c_str())) {
            for (const fakehost::Panel& panel : p->panels)
                if (panel.key == key) panel.draw(panel.user);
        }
    };
    g_actions.setPluginEnabled = [](const std::string&, bool) {};
    g_actions.beginKeyCapture = [](int) {};
    g_actions.pollKeyCapture = [](int, std::string&) { return false; };
    g_actions.rebindHotkey = [](int, const std::string&) { return true; };
    g_actions.openLogFolder = [] {};
    g_actions.openLauncher = [] {};
    g_actions.closeMenu = [] {};
}

void gameBackground() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    dl->AddRectFilledMultiColor(ImVec2(0, 0), d, IM_COL32(18, 28, 52, 255), IM_COL32(40, 22, 40, 255), IM_COL32(14, 16, 24, 255),
                                IM_COL32(10, 12, 18, 255));
    dl->AddQuadFilled(ImVec2(d.x * 0.42f, d.y * 0.45f), ImVec2(d.x * 0.58f, d.y * 0.45f), ImVec2(d.x * 0.9f, d.y), ImVec2(d.x * 0.1f, d.y),
                      IM_COL32(48, 52, 62, 255));
}

struct Scene {
    const char* name;
    const char* plugin; // null: a Horizon page
    const char* tab;    // panel key, "" = Settings
    const char* page;   // Horizon page for plugin == null
};

// A crash says where (module + offset), for a quick look with the map file.
LONG WINAPI crashed(EXCEPTION_POINTERS* e) {
    void* at = e->ExceptionRecord->ExceptionAddress;
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(at), &m);
    wchar_t name[MAX_PATH] = L"?";
    if (m) GetModuleFileNameW(m, name, MAX_PATH);
    printf("crash 0x%08lx at %ls+0x%lx\n", e->ExceptionRecord->ExceptionCode, name,
           static_cast<unsigned long>(reinterpret_cast<uintptr_t>(at) - reinterpret_cast<uintptr_t>(m)));
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter(crashed);
    if (argc < 2) {
        printf("usage: tmvs_hzpreview <out folder>\n");
        return 2;
    }
    wchar_t outWide[MAX_PATH] = {};
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, outWide, MAX_PATH);
    const std::wstring out = outWide;
    std::filesystem::create_directories(out);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // The core's data: a fresh folder with a 1.5-style settings.ini (no update check).
    const std::wstring root = exeDir() + L"\\hzpreview-data";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root + L"\\TMVS");
    std::ofstream(root + L"\\TMVS\\settings.ini", std::ios::binary) << "[General]\r\nPreset=Golden Hour\r\nCheckForUpdates=0\r\n";
    HMODULE core = LoadLibraryW((exeDir() + L"\\tmvs-core.dll").c_str());
    auto setRoot = core ? reinterpret_cast<int(__cdecl*)(const wchar_t*)>(GetProcAddress(core, "TMVS_Core_SetDataRoot")) : nullptr;
    if (!setRoot || !setRoot(root.c_str())) {
        printf("tmvs-core.dll missing next to the tool\n");
        return 3;
    }

    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, wndProc, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"TmvsHzPreview", nullptr};
    RegisterClassExW(&wc);
    RECT rc = {0, 0, kWidth, kHeight};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"hz preview", WS_OVERLAPPEDWINDOW, 0, 0, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr,
                              wc.hInstance, nullptr);

    // Horizon's ImGui (this exe's copy), as the host makes it.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    setIconFontFile(HZ_ASSETS "/lucide-subset.ttf");
    loadFonts(1.0f);
    applyStyle(1.0f);
    fakehost::g_style.size = sizeof(HorizonStyle);
    fakehost::g_style.font_body = fonts().body;
    fakehost::g_style.font_title = fonts().title;
    fakehost::g_style.font_mono = fonts().mono;
    fakehost::g_style.scale = 1.0f;

    // The plugins load before the game makes its device (the host's start), as in the game.
    for (const char* id : {"tmvs", "tmvs-capture", "tmvs-upscaling", "tmvs-packs"}) {
        if (!fakehost::load(exeDir(), id)) {
            printf("could not load %s.dll\n", id);
            return 4;
        }
    }
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = kWidth;
    pp.BackBufferHeight = kHeight;
    pp.hDeviceWindow = hwnd;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    fakehost::adjustParams(&pp);
    IDirect3DDevice9* dev = nullptr;
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) {
        printf("no HAL device\n");
        return 5;
    }
    fakehost::deviceCreated(dev, &pp);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(dev);
    float aspect = 1.0f;
    const std::string assets = HZ_ASSETS, packaging = TMVS_PACKAGING;
    const unsigned long long wordmark = loadTexture(dev, widen(assets + "/wordmark.png"), &aspect);
    setLogo(wordmark, aspect, loadTexture(dev, widen(assets + "/mark.png")));
    for (const char* id : {"tmvs", "tmvs-capture", "tmvs-upscaling", "tmvs-packs"}) setIcon(id, loadTexture(dev, widen(packaging + "/horizon/" + id + "/icon.png")));

    const Scene scenes[] = {
        {"tmvs-settings", "tmvs", "", nullptr},
        {"tmvs-shaders", "tmvs", "shaders", nullptr},
        {"tmvs-customize", "tmvs", "customize", nullptr},
        {"capture-settings", "tmvs-capture", "", nullptr},
        {"capture-clips", "tmvs-capture", "clips", nullptr},
        {"upscaling", "tmvs-upscaling", "", nullptr},
        {"packs", "tmvs-packs", "", nullptr},
        {"hotkeys", nullptr, nullptr, "hotkeys"},
    };
    int failed = 0;
    for (const Scene& sc : scenes) {
        if (argc >= 3 && strcmp(argv[2], sc.name) != 0) continue; // one scene only
        fillModel();
        if (sc.plugin) {
            showPluginPage(sc.plugin);
#ifdef HZ_HAS_SELECT_TAB
            selectPanelTab(sc.plugin, sc.tab);
#endif
        } else {
            showPage(sc.page);
        }
        bool menuOpen = true, openMenu = false;
        for (int f = 0; f < 45; ++f) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(10, 12, 18), 1.0f, 0);
            fakehost::prePresent(dev); // the core first (its Present work), then Horizon's interface
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            io.DeltaTime = 1.0f / 30.0f;
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            ImGui::NewFrame();
            gameBackground();
            menuOpen = true;
            drawMenu(g_model, g_actions, &menuOpen);
            drawBar(g_model, g_actions, false, true, &openMenu);
            ImGui::EndFrame();
            ImGui::Render();
            dev->SetRenderState(D3DRS_ZENABLE, FALSE);
            if (SUCCEEDED(dev->BeginScene())) {
                ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
                dev->EndScene();
            }
            if (f == 44) {
                const std::wstring path = out + L"\\" + std::wstring(sc.name, sc.name + strlen(sc.name)) + L".png";
                if (savePng(dev, path)) printf("saved %ls\n", path.c_str());
                else {
                    printf("FAILED %ls\n", path.c_str());
                    failed++;
                }
            }
            dev->Present(nullptr, nullptr, nullptr, nullptr);
        }
    }
    fflush(stdout);
    // Not through the CRT's exit: the core's threads belong to a game that ends here.
    TerminateProcess(GetCurrentProcess(), failed ? 1 : 0);
    return 0;
}
