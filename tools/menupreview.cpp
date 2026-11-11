// Renders the F8 menu (src/menu.cpp) outside the game into PNGs, one per page and scroll position, on a
// hidden window's Direct3D 9 device: for checking layout, texts and contrast at a screen size (the menu's
// scale follows the height: 1080 -> 1.0, 1440 -> 1.33). Settings start from the defaults in a folder of
// its own (never the game's settings.ini). What needs the game is stood in for: the preset pictures come
// from thumbnails/, the GPU timings and resource packs are made up, the Misc page (minimap, split times)
// is left out.
//   tmvs_menupreview.exe <out folder> [--size 1920x1080] [--tag name] [--background file.jpg]
//                        [--repo <repo folder>] [--detached]
// Files: <out>\<tag>_<nn>_<page>[_<scroll>].png
#include "clips/clips.h"
#include "config.h"
#include "image.h"
#include "log.h"
#include "menu.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace tmshaders;

namespace {

IDirect3DDevice9* g_device = nullptr;
std::wstring g_repo;
std::map<std::string, IDirect3DTexture9*> g_pictures;

IDirect3DTexture9* texture(const std::vector<uint32_t>& pixels, int w, int h) {
    IDirect3DTexture9* t = nullptr;
    if (FAILED(g_device->CreateTexture(static_cast<UINT>(w), static_cast<UINT>(h), 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) return nullptr;
    D3DLOCKED_RECT r{};
    if (SUCCEEDED(t->LockRect(0, &r, nullptr, 0))) {
        for (int y = 0; y < h; y++) memcpy(static_cast<BYTE*>(r.pBits) + static_cast<size_t>(r.Pitch) * y, &pixels[static_cast<size_t>(w) * y], w * 4u);
        t->UnlockRect(0);
    }
    return t;
}

IDirect3DTexture9* load(const std::wstring& path) {
    std::vector<uint32_t> pixels;
    int w = 0, h = 0;
    if (!image::readRgba(path, pixels, w, h)) return nullptr;
    return texture(pixels, w, h);
}

// A built-in preset's picture: thumbnails/<name, lower case, spaces as _>.jpg.
ImTextureID picture(const std::string& preset) {
    auto it = g_pictures.find(preset);
    if (it == g_pictures.end()) {
        std::wstring file;
        for (char c : preset) file += static_cast<wchar_t>(c == ' ' ? '_' : std::tolower(static_cast<unsigned char>(c)));
        it = g_pictures.emplace(preset, load(g_repo + L"\\thumbnails\\" + file + L".jpg")).first;
    }
    return static_cast<ImTextureID>(reinterpret_cast<intptr_t>(it->second));
}

struct FakeTimings : menu::PassTimings {
    int count() const override { return 6; }
    const char* name(int pass) const override {
        static const char* const kNames[] = {"LinearDepth", "AO", "Shadows", "Bloom", "Grade", "TAA"};
        return kNames[pass];
    }
    float time(int pass) const override {
        static const float kMs[] = {0.29f, 0.62f, 0.81f, 0.33f, 0.18f, 0.21f};
        return kMs[pass];
    }
    float total() const override { return 2.44f; }
};

struct FakePacks : menu::ResourcePacks {
    std::vector<Pack> list{{"Stadium", "Stadium Clean", "StadiumClean.zip", "4.0 MB", true},
                           {"Stadium", "Night Lights HD", "NightLightsHD.zip", "12.6 MB", true}};
    const std::vector<Pack>& packs() override { return list; }
    std::vector<std::string> environments() override { return {"Stadium", "Island", "Coast"}; }
    void refresh() override {}
    ImTextureID preview(size_t, int& width, int& height) override {
        width = height = 0;
        return 0;
    }
    void openFolder(const std::string&) override {}
    std::string status() override { return "Stadium: the map's own textures"; }
    bool hooked() override { return true; }
    Live live() override { return Live(); }
    std::string applyNow() override { return std::string(); }
};

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool savePng(const std::wstring& path, int w, int h) {
    IDirect3DSurface9* back = nullptr;
    IDirect3DSurface9* mem = nullptr;
    bool ok = SUCCEEDED(g_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) &&
              SUCCEEDED(g_device->CreateOffscreenPlainSurface(static_cast<UINT>(w), static_cast<UINT>(h), D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &mem, nullptr)) &&
              SUCCEEDED(g_device->GetRenderTargetData(back, mem));
    D3DLOCKED_RECT r{};
    if (ok && SUCCEEDED(mem->LockRect(&r, nullptr, D3DLOCK_READONLY))) {
        std::vector<unsigned char> rgb(static_cast<size_t>(w) * h * 3);
        for (int y = 0; y < h; y++) {
            const BYTE* row = static_cast<const BYTE*>(r.pBits) + static_cast<size_t>(r.Pitch) * y;
            for (int x = 0; x < w; x++) {
                unsigned char* d = &rgb[(static_cast<size_t>(y) * w + x) * 3];
                d[0] = row[x * 4 + 2];
                d[1] = row[x * 4 + 1];
                d[2] = row[x * 4 + 0];
            }
        }
        mem->UnlockRect();
        ok = image::writePng(path, rgb.data(), w, h);
    } else {
        ok = false;
    }
    if (mem) mem->Release();
    if (back) back->Release();
    return ok;
}

// The menu's page child (scrolls), found by its name ("TM Vibrant Shaders/page_XXXXXXXX").
ImGuiWindow* pageChild() {
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows) {
        if (strstr(w->Name, "TM Vibrant Shaders/page_")) return w;
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("tmvs_menupreview <out folder> [--size 1920x1080] [--tag name] [--background file.jpg] [--repo folder] [--detached]\n");
        return 2;
    }
    auto wide = [](const char* s) {
        wchar_t w[MAX_PATH] = {};
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w, MAX_PATH);
        return std::wstring(w);
    };
    const std::wstring out = wide(argv[1]);
    int width = 1920, height = 1080;
    std::string tag = "1080p";
    std::wstring background;
    bool detached = false;
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        g_repo = exe;
        for (int up = 0; up < 3; up++) g_repo = g_repo.substr(0, g_repo.find_last_of(L"\\/")); // build\Release\x.exe -> repo
    }
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf_s(argv[++i], "%dx%d", &width, &height);
        else if (!strcmp(argv[i], "--tag") && i + 1 < argc) tag = argv[++i];
        else if (!strcmp(argv[i], "--background") && i + 1 < argc) background = wide(argv[++i]);
        else if (!strcmp(argv[i], "--repo") && i + 1 < argc) g_repo = wide(argv[++i]);
        else if (!strcmp(argv[i], "--detached")) detached = true;
    }
    if (background.empty()) background = g_repo + L"\\docs\\images\\realistic-start.jpg";
    CreateDirectoryW(out.c_str(), nullptr);
    // Settings from the defaults, in a folder of its own.
    const std::wstring data = out + L"\\_settings";
    CreateDirectoryW(data.c_str(), nullptr);
    DeleteFileW((data + L"\\settings.ini").c_str());
    log::setDataDir(data);
    log::setFileName(L"tmvs_menupreview.log");

    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TmvsMenuPreview";
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"menupreview", WS_POPUP, 0, 0, width, height, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.BackBufferWidth = static_cast<UINT>(width);
    pp.BackBufferHeight = static_cast<UINT>(height);
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp,
                                         &g_device))) {
        printf("no Direct3D 9 device\n");
        return 1;
    }

    Config& config = Config::get();
    config.checkUpdates = false;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    menu::setupStyle(window);
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX9_Init(g_device);
    IDirect3DTexture9* backdrop = load(background);
    IDirect3DTexture9* logo = load(g_repo + L"\\packaging\\icons\\icon_a_64.png");

    FakeTimings timings;
    FakePacks packs;
    menu::Host host;
    host.depthAvailable = true;
    host.engineHooks = true;
    host.sunKnown = true;
    host.sunDirection[1] = 1.0f;
    host.timings = &timings;
    host.picture = picture;
    host.icon = [logo](int) { return static_cast<ImTextureID>(reinterpret_cast<intptr_t>(logo)); };
    host.autoQualityReduced = [] { return static_cast<const char*>(nullptr); };
    host.mapComments = [](std::string&) { return false; };
    host.setMapComments = [](const std::string&) { return false; };
    host.fNumber = [](float dof) { return 1.4f * powf(16.0f / 1.4f, 1.0f - dof); };
    static bool splitOn = false, splitSwapped = false;
    static float splitPosition = 0.5f;
    static std::string splitPreset;
    host.splitOn = &splitOn;
    host.splitPosition = &splitPosition;
    host.splitSwapped = &splitSwapped;
    host.splitPreset = &splitPreset;
    host.photoHideCar = true;
    host.photoOn = [] {};
    host.supports = [](const Field& f) { return strncmp(f.key, "Vr", 2) != 0 && strcmp(f.key, "ToneCurve") != 0; };
    host.resourcePacks = &packs;
    host.clipsPanel = [] { clips::drawRecording(); };
    host.clipsSaving = [] { clips::drawSaving(); };
    host.horizon = true;

    auto frame = [&](float dt) {
        io.DeltaTime = dt;
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        io.DeltaTime = dt;
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        ImGui::NewFrame();
        if (backdrop) ImGui::GetBackgroundDrawList()->AddImage(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(backdrop)), ImVec2(0, 0), io.DisplaySize);
        menu::draw(host);
        ImGui::EndFrame();
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(40, 44, 52), 1.0f, 0);
        if (SUCCEEDED(g_device->BeginScene())) {
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_device->EndScene();
        }
    };

    // The page ids of src/menu.cpp (enum Page): every page but Misc (needs the game).
    struct Shot {
        int id;
        const char* name;
    };
    const Shot shots[] = {{0, "shaders"}, {1, "customize"}, {2, "studio"},   {3, "maps"},  {4, "performance"}, {7, "video"},
                          {8, "packs"},   {10, "clips"},    {5, "settings"}, {6, "about"}};
    int written = 0, n = 0;
    for (const Shot& shot : shots) {
        config.showOverlay = true;
        config.menuPage = shot.id;
        for (int i = 0; i < 6; i++) frame(i < 5 ? 1.0f / 60.0f : 1.0f); // layout settles; the switches' animations end
        ImGuiWindow* child = pageChild();
        const float max = child ? child->ScrollMax.y : 0.0f;
        const float step = child ? std::max(100.0f, child->InnerRect.GetHeight() - 60.0f) : 1.0f;
        int part = 0;
        for (float y = 0.0f;; y += step, part++) {
            if (child) ImGui::SetScrollY(child, std::min(y, max));
            frame(1.0f / 60.0f);
            frame(1.0f / 60.0f);
            wchar_t name[160];
            if (part == 0) swprintf(name, 160, L"\\%hs_%02d_%hs.png", tag.c_str(), n, shot.name);
            else swprintf(name, 160, L"\\%hs_%02d_%hs_%d.png", tag.c_str(), n, shot.name, part);
            if (savePng(out + name, width, height)) written++;
            else printf("could not write %ls\n", name);
            if (!child || y >= max || part >= 5) break;
            child = pageChild();
        }
        n++;
    }
    if (detached) {
        // Two pages in windows of their own beside the menu.
        config.menuPage = 0;
        config.menuWindows["maps"].detached = true;
        config.menuWindows["clips"].detached = true;
        for (int i = 0; i < 6; i++) frame(i < 5 ? 1.0f / 60.0f : 1.0f);
        wchar_t name[160];
        swprintf(name, 160, L"\\%hs_%02d_detached.png", tag.c_str(), n);
        if (savePng(out + name, width, height)) written++;
    }
    printf("%d pictures in %ls\n", written, out.c_str());
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    for (auto& p : g_pictures)
        if (p.second) p.second->Release();
    if (backdrop) backdrop->Release();
    if (logo) logo->Release();
    g_device->Release();
    d3d->Release();
    DestroyWindow(window);
    return 0;
}
