// TMVS 2.0 module test (development tool): the four Horizon module plugins and tmvs-core.dll from this
// build, loaded by a fake Horizon Host (tools/fakehost.h) on a real D3D9 device in a hidden window, no game.
// Each scenario runs in a child process of its own (the core is one per process, as in the game):
//   load               Info/Init of the four plugins, the panels and hotkeys each registers (the 1.5 keys
//                      from settings.ini and clips.ini taken over), the shared settings.ini read (preset),
//                      device creation with MSAA taken out, frames, a Reset, every page drawn in the host's
//                      ImGui context, live switching (TMVS off blocks the effects without writing Enabled,
//                      the pre_present owner moves), Capture alone recording a clip (an MP4 from the save
//                      key, without the TMVS module), Shutdown, FreeLibrary and loading them again.
//   standalone-first   the standalone DLL holds TM Vibrant Shaders' instance (its mutex and owner text,
//                      as claimInstance makes them): the modules load, stay off, their page says why.
//   modules-first      the modules hold it; the standalone TMVibrantShaders.dll loaded after them stays
//                      passive (no boot: nothing written to Documents\TrackMania\TMVS).
// Everything is written under <exe folder>\moduletest\<scenario> (TMVS_Core_SetDataRoot), never into the
// player's Documents. Usage: tmvs_moduletest [scenario ...] (none: all). Exit code 0 = every check passed.
#include "imgui.h"
#include "fakehost.h"
#include <d3d9.h>
#include <filesystem>
#include <fstream>
#include <shlobj.h>
#include <string>
#include <vector>

#pragma comment(lib, "d3d9.lib")

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) g_failures++;
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

std::wstring exeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    return dir.substr(0, dir.find_last_of(L"\\/"));
}

void writeText(const std::wstring& path, const std::string& text) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream f(path, std::ios::binary);
    f << text;
}

std::string readText(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// --- the core's test exports ------------------------------------------------------------------

typedef int(__cdecl* SetDataRootFn)(const wchar_t*);
typedef int(__cdecl* QueryFn)(char*, int);
QueryFn g_query = nullptr;

std::string query() {
    char text[1024] = {};
    if (g_query) g_query(text, sizeof(text));
    return text;
}

std::string field(const char* key) {
    const std::string q = query(), k = std::string(key) + "=";
    size_t at = 0;
    while ((at = q.find(k, at)) != std::string::npos) {
        if (at == 0 || q[at - 1] == ';') {
            const size_t end = q.find(';', at);
            return q.substr(at + k.size(), end == std::string::npos ? std::string::npos : end - at - k.size());
        }
        at++;
    }
    return "?";
}

void checkField(const char* key, const char* want) {
    const std::string got = field(key);
    char line[256];
    snprintf(line, sizeof(line), "%s = %s (want %s)", key, got.c_str(), want);
    check(got == want, line);
}

HMODULE loadCore(const std::wstring& root) {
    HMODULE core = LoadLibraryW((exeDir() + L"\\tmvs-core.dll").c_str());
    if (!core) return nullptr;
    auto setRoot = reinterpret_cast<SetDataRootFn>(GetProcAddress(core, "TMVS_Core_SetDataRoot"));
    g_query = reinterpret_cast<QueryFn>(GetProcAddress(core, "TMVS_Core_Query"));
    if (!setRoot || !g_query || !setRoot(root.c_str())) return nullptr;
    return core;
}

const char* const kIds[] = {"tmvs", "tmvs-capture", "tmvs-upscaling", "tmvs-packs"};

bool loadModules() {
    for (const char* id : kIds) {
        if (!fakehost::load(exeDir(), id)) {
            printf("  could not load %s.dll\n", id);
            return false;
        }
    }
    return true;
}

// --- a hidden window with a HAL device ----------------------------------------------------------

struct Device {
    HWND window = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* dev = nullptr;
    D3DPRESENT_PARAMETERS pp{};
    D3DMULTISAMPLE_TYPE adjusted = D3DMULTISAMPLE_NONE; // what the host's adjust_present_params left
};

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

// As the game: the host's adjust_present_params before CreateDevice, device_created after it.
bool createDevice(Device& d, bool tellHost) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TmvsModuleTest";
    RegisterClassW(&wc);
    d.window = CreateWindowW(wc.lpszClassName, L"moduletest", WS_OVERLAPPEDWINDOW, 0, 0, 1280, 720, nullptr, nullptr, wc.hInstance, nullptr);
    d.d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d.window || !d.d3d) return false;
    d.pp.Windowed = TRUE;
    d.pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d.pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    d.pp.BackBufferWidth = 1280;
    d.pp.BackBufferHeight = 720;
    d.pp.hDeviceWindow = d.window;
    d.pp.EnableAutoDepthStencil = TRUE;
    d.pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    d.pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    d.pp.MultiSampleType = D3DMULTISAMPLE_4_SAMPLES; // the core takes it out (depth effects need one sample)
    if (tellHost) fakehost::adjustParams(&d.pp);
    d.adjusted = d.pp.MultiSampleType;
    if (FAILED(d.d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, d.window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &d.pp, &d.dev))) {
        d.pp.MultiSampleType = D3DMULTISAMPLE_NONE; // no 4x MSAA on this card (the refused case leaves it in)
        if (FAILED(d.d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, d.window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &d.pp, &d.dev)))
            return false;
    }
    if (tellHost) fakehost::deviceCreated(d.dev, &d.pp);
    else fakehost::g_device = d.dev;
    return true;
}

void frames(Device& d, int count, int sleepMs = 0) {
    for (int i = 0; i < count; i++) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        static int s_frame = 0;
        s_frame++;
        const D3DCOLOR colour = D3DCOLOR_XRGB((s_frame * 3) & 255, 90, 255 - ((s_frame * 5) & 255));
        d.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, colour, 1.0f, 0);
        fakehost::prePresent(d.dev);
        d.dev->Present(nullptr, nullptr, nullptr, nullptr);
        if (sleepMs) Sleep(static_cast<DWORD>(sleepMs));
    }
}

void release(Device& d) {
    if (d.dev) d.dev->Release();
    if (d.d3d) d.d3d->Release();
    if (d.window) DestroyWindow(d.window);
    d = Device();
}

// --- the host's ImGui frame -----------------------------------------------------------------------

void hostImGui() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
}

// Every panel and every Settings tab drawn inside one host frame. Returns the vertices drawn.
int drawEverything(int* panelsDrawn) {
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(1200.0f, 800.0f));
    ImGui::Begin("host menu");
    int n = 0;
    for (auto& p : fakehost::g_plugins) {
        if (!p->running) continue;
        ImGui::PushID(p->id.c_str());
        if (p->drawMenu) {
            ImGui::BeginChild("settings", ImVec2(0, 300));
            p->drawMenu();
            ImGui::EndChild();
        }
        for (const fakehost::Panel& panel : p->panels) {
            ImGui::PushID(panel.key.c_str());
            ImGui::BeginChild("panel", ImVec2(0, 300));
            panel.draw(panel.user);
            ImGui::EndChild();
            ImGui::PopID();
            n++;
        }
        ImGui::PopID();
    }
    ImGui::End();
    ImGui::Render();
    if (panelsDrawn) *panelsDrawn = n;
    return ImGui::GetDrawData()->TotalVtxCount;
}

void prepareRoot(const std::wstring& root) {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    // A 1.5 settings.ini (no SettingsVersion): its preset and its own Effects key must come over.
    writeText(root + L"\\TMVS\\settings.ini",
              "[General]\r\nPreset=Golden Hour\r\nEnabled=1\r\nCheckForUpdates=0\r\n[Keys]\r\nEffects=F5\r\n");
    // The clips: small and quick, a key of their own (taken over into Horizon's table).
    writeText(root + L"\\TMClips\\clips.ini",
              "[Clips]\r\nEnabled=1\r\nSaveOnFinish=0\r\nAdaptiveBuffer=0\r\nBufferSeconds=20\r\nKeySeconds=30\r\nHeight=720\r\nFps=30\r\n"
              "Bitrate=4\r\nSound=0\r\nSaveKey=F4\r\n");
}

std::vector<std::wstring> clipsIn(const std::wstring& dir) {
    std::vector<std::wstring> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::wstring name = e.path().filename().wstring();
        if (name.size() > 4 && name.find(L".part.") == std::wstring::npos && name.substr(name.size() - 4) == L".mp4") out.push_back(name);
    }
    return out;
}

// --- scenarios ------------------------------------------------------------------------------------

int scenarioLoad(const std::wstring& root) {
    prepareRoot(root);
    if (!loadCore(root)) {
        check(false, "tmvs-core.dll loads and has its test exports");
        return 1;
    }
    hostImGui();
    check(loadModules(), "the four module DLLs load");
    if (fakehost::g_plugins.size() != 4) return 1;
    for (auto& p : fakehost::g_plugins) {
        const HorizonPluginInfo* i = p->info;
        char line[200];
        snprintf(line, sizeof(line), "%s: Info (abi %u.%u, id %s, online-ok + live toggle, menu + device), Init %d", p->id.c_str(), i->abi,
                 i->abi_minor, i->id, p->initResult);
        check(i->abi == HORIZON_ABI && i->abi_minor == HORIZON_ABI_MINOR && p->id == i->id && (i->flags & HORIZON_FLAG_ONLINE_OK) &&
                  (i->flags & HORIZON_FLAG_LIVE_TOGGLE) && !(i->flags & HORIZON_FLAG_SOLO_ONLY) && (i->caps & HORIZON_CAP_MENU) &&
                  (i->caps & HORIZON_CAP_DEVICE) && (i->games & HORIZON_GAME_TMNF) && (i->games & HORIZON_GAME_TMUF) && p->initResult == 0,
              line);
    }
    printf("  core: %s\n", query().c_str());
    checkField("hosted", "1");
    checkField("refused", "0");
    checkField("attached", "1111");
    checkField("presentOwner", "0");
    checkField("hostKeys", "1");
    checkField("preset", "Golden Hour"); // the same settings.ini as the standalone
    checkField("settingsVersion", "1");  // a 1.5 file, taken over
    checkField("shadersBlocked", "0");

    fakehost::Plugin* tmvs = fakehost::find("tmvs");
    fakehost::Plugin* capture = fakehost::find("tmvs-capture");
    fakehost::Plugin* upscaling = fakehost::find("tmvs-upscaling");
    fakehost::Plugin* packs = fakehost::find("tmvs-packs");
    check(tmvs->panels.size() == 7 && capture->panels.size() == 2 && upscaling->panels.empty() && packs->panels.empty(),
          "panels: TMVS 7 (Shaders ... About), Capture 2 (Clips, Video), Upscaling and Packs their Settings tab only");
    const fakehost::Hotkey* effects = fakehost::hotkey(tmvs, "effects");
    check(effects && effects->key == "F5", "TMVS hotkey \"effects\" registered with the key from the 1.5 settings.ini (F5)");
    const fakehost::Hotkey* clipLast = fakehost::hotkey(capture, "clip-last");
    check(clipLast && clipLast->key == "F4", "Capture hotkey \"clip-last\" registered with the key from clips.ini (F4)");
    const fakehost::Hotkey* hires = fakehost::hotkey(capture, "photo-hires");
    check(hires && hires->key == "Shift+F12", "Capture hotkey \"photo-hires\" is Shift + the capture key");
    check(!fakehost::hotkey(tmvs, "menu") && !fakehost::hotkey(capture, "menu"), "no F8 menu key (Horizon's menu only)");
    check(upscaling->hotkeys.empty() && packs->hotkeys.empty(), "Upscaling and Packs register no hotkeys");

    Device d;
    check(createDevice(d, true), "a HAL device created through the host's calls");
    check(d.adjusted == D3DMULTISAMPLE_NONE, "adjust_present_params took MSAA out (once, through the device owner)");
    checkField("device", "1");
    frames(d, 30);
    // A Reset as the host makes it: before_reset, adjust, Reset, after_reset. Every D3DPOOL_DEFAULT
    // resource of the core must be gone before it, or it fails.
    fakehost::beforeReset(d.dev);
    fakehost::adjustParams(&d.pp);
    const HRESULT reset = d.dev->Reset(&d.pp);
    fakehost::afterReset(d.dev, reset);
    check(SUCCEEDED(reset), "Reset through the host's calls succeeds");
    frames(d, 30);

    int panels = 0;
    const int vertices = drawEverything(&panels);
    char line[160];
    snprintf(line, sizeof(line), "every page drawn in the host's ImGui context (%d panels, 4 Settings tabs, %d vertices)", panels, vertices);
    check(panels == 9 && vertices > 1000, line);

    fakehost::setEnabled(tmvs, false);
    checkField("shadersBlocked", "1");
    checkField("effects", "0");
    checkField("presentOwner", "1");
    fakehost::setEnabled(tmvs, true);
    checkField("effects", "1");
    checkField("presentOwner", "0");

    // Capture alone: the clips record without the TMVS module.
    fakehost::setEnabled(tmvs, false);
    fakehost::setEnabled(upscaling, false);
    fakehost::setEnabled(packs, false);
    checkField("clipsAllowed", "1");
    checkField("upscaling", "0");
    checkField("packs", "0");
    // The encoder starts on its own thread (a second or two): then about 3 s into the buffer, then the key.
    for (int i = 0; i < 40 && readText(root + L"\\TMVS\\tmvs.log").find("clips: recording ") == std::string::npos; i++) frames(d, 15, 16);
    frames(d, 180, 16);
    clipLast->callback(clipLast->user);
    const std::wstring clipsDir = root + L"\\Clips";
    bool saved = false;
    for (int i = 0; i < 40 && !saved; i++) {
        frames(d, 15, 16);
        saved = !clipsIn(clipsDir).empty();
    }
    const std::string log = readText(root + L"\\TMVS\\tmvs.log");
    const bool noEncoder = log.find("clips: can't record") != std::string::npos;
    if (noEncoder) {
        printf("  skip Capture without TMVS: no hardware H.264 encoder here (%s)\n", "clips: can't record");
    } else {
        check(saved, "Capture without the TMVS module: the save key wrote an MP4 into Clips");
        check(log.find("clips: save key: saving the last 30 s") != std::string::npos, "the clip save is in the core's log");
    }
    checkField("shadersBlocked", "1");

    fakehost::setEnabled(capture, false);
    checkField("clipsAllowed", "0");
    checkField("presentOwner", "-1");
    fakehost::setEnabled(capture, true);
    fakehost::setEnabled(tmvs, true);
    fakehost::setEnabled(upscaling, true);
    fakehost::setEnabled(packs, true);
    frames(d, 10);

    // Shutdown (the host at the game's end) and the DLLs unloaded; the core stays (pinned).
    fakehost::shutdownAll(true);
    checkField("attached", "0000");
    check(GetModuleHandleW(L"tmvs-core.dll") != nullptr, "tmvs-core.dll stays loaded after the modules are gone");
    check(GetModuleHandleW(L"tmvs-capture.dll") == nullptr, "the module DLLs are unloaded");
    // And loaded again in the same process (a profile switched back on): they attach again.
    check(loadModules(), "the modules load a second time");
    checkField("attached", "1111");
    check(fakehost::find("tmvs") && fakehost::find("tmvs")->panels.size() == 7, "their panels are registered again");
    frames(d, 10);
    fakehost::shutdownAll(true);
    const std::string settings = readText(root + L"\\TMVS\\settings.ini");
    check(settings.find("Enabled=0") == std::string::npos, "settings.ini never got Enabled=0 from the TMVS module being off");
    check(log.find("exclusive: this copy runs (Horizon modules (tmvs-core.dll))") != std::string::npos, "the core's log says it runs");
    release(d);
    return 0;
}

int scenarioStandaloneFirst(const std::wstring& root) {
    prepareRoot(root);
    // What the standalone's claimInstance leaves in a game where it runs as d3d9.dll.
    wchar_t name[64];
    swprintf(name, 64, L"Local\\TMVibrantShaders_%lu", GetCurrentProcessId());
    HANDLE mutex = CreateMutexW(nullptr, FALSE, name);
    swprintf(name, 64, L"Local\\TMVibrantShaders_Owner_%lu", GetCurrentProcessId());
    HANDLE owner = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 64, name);
    char* text = owner ? static_cast<char*>(MapViewOfFile(owner, FILE_MAP_WRITE, 0, 0, 64)) : nullptr;
    if (text) strcpy_s(text, 64, "standalone (d3d9.dll)");
    check(mutex && text, "the standalone's mutex and owner text are there");
    if (!loadCore(root)) return 1;
    hostImGui();
    check(loadModules(), "the four modules load");
    bool allZero = true;
    for (auto& p : fakehost::g_plugins) allZero = allZero && p->initResult == 0;
    check(allZero, "Init returns 0 (shown as loaded; the page says why they are off)");
    checkField("refused", "1");
    checkField("owner", "standalone (d3d9.dll)");
    checkField("attached", "0000");
    size_t panels = 0, hotkeys = 0;
    for (auto& p : fakehost::g_plugins) {
        panels += p->panels.size();
        hotkeys += p->hotkeys.size();
    }
    check(panels == 0 && hotkeys == 0, "no panels, no hotkeys");
    check(fakehost::logHas("already runs in this game as standalone (d3d9.dll)"), "Horizon's log says which copy runs");
    Device d;
    check(createDevice(d, true), "a device");
    check(d.adjusted == D3DMULTISAMPLE_4_SAMPLES, "the present parameters left alone (the standalone owns the device)");
    frames(d, 10);
    checkField("device", "0");
    int drawn = 0;
    check(drawEverything(&drawn) > 0, "the Settings tabs draw the note");
    check(GetFileAttributesW((root + L"\\TMVS\\tmvs.log").c_str()) == INVALID_FILE_ATTRIBUTES, "the core never booted (no tmvs.log)");
    fakehost::shutdownAll(true);
    release(d);
    return 0;
}

FILETIME writeTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data);
    return data.ftLastWriteTime;
}

int scenarioModulesFirst(const std::wstring& root) {
    prepareRoot(root);
    if (!loadCore(root)) return 1;
    hostImGui();
    check(loadModules(), "the four modules load");
    checkField("attached", "1111");
    wchar_t name[64];
    swprintf(name, 64, L"Local\\TMVibrantShaders_Owner_%lu", GetCurrentProcessId());
    std::string ownerText;
    if (HANDLE owner = OpenFileMappingW(FILE_MAP_READ, FALSE, name)) {
        if (const char* t = static_cast<const char*>(MapViewOfFile(owner, FILE_MAP_READ, 0, 0, 64))) ownerText = t;
    }
    check(ownerText == "Horizon modules (tmvs-core.dll)", "the owner text names the Horizon modules");
    swprintf(name, 64, L"Local\\TMVibrantShaders_%lu", GetCurrentProcessId());
    HANDLE held = OpenMutexW(SYNCHRONIZE, FALSE, name);
    check(held != nullptr, "the instance mutex is held");
    // The standalone DLL coming later (ModLoader or d3d9.dll): passive, only forwarding. Loaded only
    // with the mutex there (else it would boot and write to the player's Documents\TrackMania\TMVS).
    wchar_t docs[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs);
    const std::wstring playerLog = std::wstring(docs) + L"\\TrackMania\\TMVS\\tmvs.log";
    const FILETIME before = writeTime(playerLog);
    if (held) {
        HMODULE standalone = LoadLibraryW((exeDir() + L"\\TMVibrantShaders.dll").c_str());
        check(standalone != nullptr, "TMVibrantShaders.dll loads beside the modules");
        Sleep(1500); // a boot thread would have logged by now
        const FILETIME after = writeTime(playerLog);
        check(CompareFileTime(&before, &after) == 0, "it stayed passive: the player's tmvs.log untouched");
        if (standalone) FreeLibrary(standalone);
    }
    checkField("attached", "1111");
    fakehost::shutdownAll(true);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc >= 3 && !strcmp(argv[1], "--child")) {
        const std::string scenario = argv[2];
        const std::wstring root = exeDir() + L"\\moduletest\\" + std::wstring(scenario.begin(), scenario.end());
        int r = 1;
        if (scenario == "load") r = scenarioLoad(root);
        else if (scenario == "standalone-first") r = scenarioStandaloneFirst(root);
        else if (scenario == "modules-first") r = scenarioModulesFirst(root);
        printf("  %d checks, %d failed\n", g_checks, g_failures);
        // Not through the CRT's exit: the core's threads (encoder, sound) belong to a game that ends here.
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), (r == 0 && g_failures == 0) ? 0 : 1);
    }
    std::vector<std::string> scenarios;
    for (int i = 1; i < argc; i++) scenarios.push_back(argv[i]);
    if (scenarios.empty()) scenarios = {"load", "standalone-first", "modules-first"};
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    int failed = 0;
    for (const std::string& s : scenarios) {
        printf("%s\n", s.c_str());
        std::wstring cmd = L"\"" + std::wstring(self) + L"\" --child " + std::wstring(s.begin(), s.end());
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            printf("  FAIL could not start\n");
            failed++;
            continue;
        }
        WaitForSingleObject(pi.hProcess, 180000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        printf("%s: %s\n\n", s.c_str(), code == 0 ? "passed" : "FAILED");
        if (code != 0) failed++;
    }
    printf("%s\n", failed ? "moduletest: FAILED" : "moduletest: all passed");
    return failed ? 1 : 0;
}
