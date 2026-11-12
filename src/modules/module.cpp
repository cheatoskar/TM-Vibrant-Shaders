// One of the four Horizon Host plugins of TM Vibrant Shaders 2.0 (src/modules/modules.h), built once
// per module with TMVS_MODULE_INDEX (0 tmvs, 1 tmvs-capture, 2 tmvs-upscaling, 3 tmvs-packs). It has no
// code of its own: it loads the shared core, tmvs-core.dll, and hands it the host's table. The core
// (one copy per process, however many modules load it) does the work and registers this module's
// panels, hotkeys and device callbacks with this module's plugin handle.
//
// Where the core is: loaded already (another module was first), else next to this DLL, else in the
// TMVS module's folder (horizon\plugins\tmvs\tmvs-core.dll: the release zip puts it there).
#define HORIZON_PLUGIN_BUILD
#include "horizon_plugin.h"
#include "tm_shaders_version.h"
#include <windows.h>
#include <cstdint>
#include <cwchar>

#ifndef TMVS_MODULE_INDEX
#error "TMVS_MODULE_INDEX: 0 tmvs, 1 tmvs-capture, 2 tmvs-upscaling, 3 tmvs-packs"
#endif

namespace {

constexpr int kModule = TMVS_MODULE_INDEX;
constexpr uint32_t kInterface = 1; // hostbridge::kInterface

const char* const kIds[] = {"tmvs", "tmvs-capture", "tmvs-upscaling", "tmvs-packs"};
const char* const kNames[] = {"TM Vibrant Shaders", "TMVS Capture", "TMVS Upscaling", "TMVS Packs"};
const char* const kDescriptions[] = {
    "Light, shadows, sky, weather and colour for the 2008 engine, with presets per map type.",
    "Photos, instant clips of your runs and checkpoints, and the video export. Works without the shaders.",
    "Renders the game at a lower resolution and scales it back up with AMD FSR 1. Works without the shaders.",
    "Texture mods (resource packs) on every map, chosen per environment.",
};

const HorizonPluginInfo kInfo = {HORIZON_ABI,
                                 kIds[kModule],
                                 kNames[kModule],
                                 TM_SHADERS_VERSION_A,
                                 HORIZON_GAME_TMNF | HORIZON_GAME_TMUF | HORIZON_GAME_ESWC,
                                 HORIZON_CAP_MENU | HORIZON_CAP_DEVICE,
                                 HORIZON_FLAG_ONLINE_OK | HORIZON_FLAG_LIVE_TOGGLE,
                                 "cheatoskar",
                                 kDescriptions[kModule],
                                 HORIZON_ABI_MINOR};

typedef uint32_t(__cdecl* InterfaceFn)(void);
typedef int(__cdecl* AttachFn)(int, const HorizonHostApi*);
typedef void(__cdecl* ModuleFn)(int);
typedef void(__cdecl* EnabledFn)(int, int);

const HorizonHostApi* g_host = nullptr;
AttachFn g_attach = nullptr;
ModuleFn g_detach = nullptr;
ModuleFn g_drawMenu = nullptr;
EnabledFn g_setEnabled = nullptr;
bool g_attached = false;

// This DLL's folder, with a trailing backslash.
void ownFolder(wchar_t* out, DWORD size) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ownFolder), &self);
    const DWORD n = GetModuleFileNameW(self, out, size);
    if (n == 0 || n >= size) {
        out[0] = 0;
        return;
    }
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) slash[1] = 0;
}

HMODULE loadCore() {
    if (HMODULE loaded = GetModuleHandleW(L"tmvs-core.dll")) return loaded;
    wchar_t folder[MAX_PATH] = {};
    ownFolder(folder, MAX_PATH);
    wchar_t path[MAX_PATH] = {};
    swprintf(path, MAX_PATH, L"%stmvs-core.dll", folder);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) swprintf(path, MAX_PATH, L"%s..\\tmvs\\tmvs-core.dll", folder);
    HMODULE core = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (core) {
        // Never unloaded: its hooks and threads stay for the life of the game.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(core), &pinned);
    }
    return core;
}

} // namespace

extern "C" {

HORIZON_EXPORT const HorizonPluginInfo* HORIZON_CALL HorizonPlugin_Info(void) {
    return &kInfo;
}

// 0: loaded (also when the standalone TM Vibrant Shaders runs here: the page then says so).
// 3: tmvs-core.dll is missing or from another version; 4: the host is too old (ABI 1.2 needed).
HORIZON_EXPORT int32_t HORIZON_CALL HorizonPlugin_Init(const HorizonHostApi* host) {
    g_host = host;
    HMODULE core = loadCore();
    const auto version = core ? reinterpret_cast<InterfaceFn>(GetProcAddress(core, "TMVS_Core_Interface")) : nullptr;
    g_attach = core ? reinterpret_cast<AttachFn>(GetProcAddress(core, "TMVS_Core_Attach")) : nullptr;
    g_detach = core ? reinterpret_cast<ModuleFn>(GetProcAddress(core, "TMVS_Core_Detach")) : nullptr;
    g_drawMenu = core ? reinterpret_cast<ModuleFn>(GetProcAddress(core, "TMVS_Core_DrawMenu")) : nullptr;
    g_setEnabled = core ? reinterpret_cast<EnabledFn>(GetProcAddress(core, "TMVS_Core_SetEnabled")) : nullptr;
    if (!core || !version || !g_attach || !g_detach || !g_drawMenu || !g_setEnabled || version() != kInterface) {
        host->log(host->plugin, HORIZON_LOG_ERROR, "%s",
                  !core ? "tmvs-core.dll not found (it belongs in the tmvs folder of the plugins): install all four TMVS folders"
                        : "tmvs-core.dll is from another TMVS version: install all four TMVS folders from the same release");
        g_attach = nullptr;
        return 3;
    }
    const int result = g_attach(kModule, host);
    static const char* const kWhy[] = {"attached", "attached already", "this Horizon is too old (plugin ABI 1.2 needed)",
                                       "Dear ImGui differs from Horizon's", "the standalone TM Vibrant Shaders runs in this game",
                                       "unknown module"};
    host->log(host->plugin, result == 0 ? HORIZON_LOG_INFO : HORIZON_LOG_WARN, "TM Vibrant Shaders %s: %s", TM_SHADERS_VERSION_A,
              result >= 0 && result <= 5 ? kWhy[result] : "unknown answer");
    g_attached = result == 0;
    if (result == 2) return 4;
    return 0;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_Shutdown(void) {
    if (g_detach) g_detach(kModule);
    g_attached = false;
    g_host = nullptr;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_DrawMenu(void) {
    if (g_drawMenu) g_drawMenu(kModule);
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_SetEnabled(int32_t enabled) {
    if (g_setEnabled && g_attached) g_setEnabled(kModule, enabled);
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
