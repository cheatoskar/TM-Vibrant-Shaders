#pragma once
// A stand-in for Horizon Host's plugin side (include/horizon_plugin.h, ABI 1.2), for the module tests
// (tools/moduletest.cpp) and the Horizon menu preview (tools/hzpreview.cpp): every plugin gets its own
// HorizonHostApi table; what the plugins register (hotkeys, panels, device callbacks, host flags, log
// lines, settings) is kept for the test to look at. Modelled on horizon-host's tests/fake_host.cpp and
// src/host/plugins.cpp (the order of the device calls, device_created at once for a late registration).
// One translation unit only (no inline linkage games). Dear ImGui must be included before.
#include "horizon_plugin.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fakehost {

struct Hotkey {
    int handle;
    std::string action, name, key;
    HorizonCallback callback;
    void* user;
};

struct Panel {
    std::string key, title;
    uint32_t icon, flags;
    HorizonPanelDrawFn draw;
    void* user;
};

struct Plugin {
    std::string id, file;
    HMODULE module = nullptr;
    HorizonHostApi api{};
    const HorizonPluginInfo* info = nullptr;
    HorizonPlugin_InitFn init = nullptr;
    HorizonPlugin_ShutdownFn shutdown = nullptr;
    HorizonPlugin_DrawMenuFn drawMenu = nullptr;
    HorizonPlugin_SetEnabledFn setEnabled = nullptr;
    int initResult = -100;
    bool running = false;
    bool enabled = false;
    std::vector<Hotkey> hotkeys;
    std::vector<Panel> panels;
    bool hasDev = false;
    HorizonDeviceCallbacks dev{};
    uint32_t flags = 0;
    std::map<std::string, std::string> settings;
};

std::vector<std::unique_ptr<Plugin>> g_plugins;
std::vector<std::string> g_log;
void* g_device = nullptr;
int g_nextHotkey = 1;
std::string g_dataRoot;
int g_menuOpen = 0;

Plugin* of(HorizonPluginHandle h) {
    return reinterpret_cast<Plugin*>(h);
}

void HORIZON_CALL apiLog(HorizonPluginHandle h, int32_t level, const char* fmt, ...) {
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    const std::string line = "[" + (of(h) ? of(h)->id : std::string("?")) + "] " + (level >= HORIZON_LOG_WARN ? "WARN " : "") + text;
    g_log.push_back(line);
    printf("    host log: %s\n", line.c_str());
}
int32_t HORIZON_CALL apiGetString(HorizonPluginHandle h, const char* key, const char* fallback, char* out, uint32_t size) {
    auto it = of(h)->settings.find(key);
    const std::string v = it != of(h)->settings.end() ? it->second : (fallback ? fallback : "");
    if (out && size) strncpy_s(out, size, v.c_str(), _TRUNCATE);
    return static_cast<int32_t>(v.size());
}
void HORIZON_CALL apiSetString(HorizonPluginHandle h, const char* key, const char* value) { of(h)->settings[key] = value ? value : ""; }
int32_t HORIZON_CALL apiGetInt(HorizonPluginHandle h, const char* key, int32_t fallback) {
    auto it = of(h)->settings.find(key);
    return it != of(h)->settings.end() ? atoi(it->second.c_str()) : fallback;
}
void HORIZON_CALL apiSetInt(HorizonPluginHandle h, const char* key, int32_t v) { of(h)->settings[key] = std::to_string(v); }
float HORIZON_CALL apiGetFloat(HorizonPluginHandle h, const char* key, float fallback) {
    auto it = of(h)->settings.find(key);
    return it != of(h)->settings.end() ? static_cast<float>(atof(it->second.c_str())) : fallback;
}
void HORIZON_CALL apiSetFloat(HorizonPluginHandle h, const char* key, float v) { of(h)->settings[key] = std::to_string(v); }
const char* HORIZON_CALL apiDataFolder(HorizonPluginHandle) { return g_dataRoot.c_str(); }
const wchar_t* HORIZON_CALL apiDataFolderW(HorizonPluginHandle) { return L""; }
void* HORIZON_CALL apiImguiContext(void) { return ImGui::GetCurrentContext(); }
int32_t HORIZON_CALL apiImguiVersion(void) { return IMGUI_VERSION_NUM; }
void HORIZON_CALL apiImguiAllocators(void** a, void** f, void** u) {
    ImGuiMemAllocFunc af = nullptr;
    ImGuiMemFreeFunc ff = nullptr;
    ImGui::GetAllocatorFunctions(&af, &ff, u);
    *a = reinterpret_cast<void*>(af);
    *f = reinterpret_cast<void*>(ff);
}
const HorizonStyle* HORIZON_CALL apiStyle(void) { return nullptr; }
HorizonGameInfo g_game{sizeof(HorizonGameInfo), HORIZON_GAME_TMNF, "TrackMania Nations Forever (fake)", "2.11.26", "", 0, 0, "en", "", "", "0.2.0", 2, 0};
const HorizonGameInfo* HORIZON_CALL apiGameInfo(void) { return &g_game; }
void* HORIZON_CALL apiEnginePointer(uint32_t) { return nullptr; }
int32_t HORIZON_CALL apiRegisterHotkey(HorizonPluginHandle h, const char* action, const char* name, const char* key, HorizonCallback cb, void* user) {
    if (!action || !cb) return 0;
    Hotkey k{g_nextHotkey++, action, name ? name : "", key ? key : "", cb, user};
    of(h)->hotkeys.push_back(k);
    return k.handle;
}
const char* HORIZON_CALL apiHotkeyText(HorizonPluginHandle h, int32_t handle) {
    for (const Hotkey& k : of(h)->hotkeys)
        if (k.handle == handle) return k.key.c_str();
    return "";
}
void HORIZON_CALL apiSubscribe(HorizonPluginHandle, uint32_t) {}
void HORIZON_CALL apiPostReplaySaved(HorizonPluginHandle, const char*) {}
HorizonResult HORIZON_CALL apiPlayMap(HorizonPluginHandle, const char*, uint32_t) { return HORIZON_ERR_NOT_SUPPORTED; }
HorizonResult HORIZON_CALL apiReturnToMenu(HorizonPluginHandle) { return HORIZON_ERR_NOT_SUPPORTED; }
int32_t HORIZON_CALL apiGameState(void) { return HORIZON_STATE_MAIN_MENU; }
int32_t HORIZON_CALL apiIsOnline(void) { return 0; }
int32_t HORIZON_CALL apiActionsAllowed(HorizonPluginHandle h) { return of(h)->enabled ? 1 : 0; }
void HORIZON_CALL apiRunOnGameThread(HorizonPluginHandle, HorizonCallback cb, void* user) {
    if (cb) cb(user);
}
void* HORIZON_CALL apiDevice(void) { return g_device; }
void HORIZON_CALL apiSetDeviceCallbacks(HorizonPluginHandle h, const HorizonDeviceCallbacks* cb) {
    Plugin* p = of(h);
    p->dev = HorizonDeviceCallbacks{};
    p->hasDev = false;
    if (!cb) return;
    memcpy(&p->dev, cb, cb->size < sizeof(p->dev) ? cb->size : sizeof(p->dev));
    p->hasDev = true;
    if (g_device && p->dev.device_created) p->dev.device_created(p->dev.user, g_device, nullptr); // as the host: a device there already
}
int32_t HORIZON_CALL apiMenuOpen(void) { return g_menuOpen; }
void HORIZON_CALL apiRestartNote(HorizonPluginHandle, const char*) {}
int32_t HORIZON_CALL apiReflectOffset(const char*, const char*) { return -1; }
void* HORIZON_CALL apiReflectMethod(const char*, const char*) { return nullptr; }
uint64_t HORIZON_CALL apiCreateTexture(HorizonPluginHandle, uint32_t, uint32_t, const void*) { return 0; }
void HORIZON_CALL apiReleaseTexture(HorizonPluginHandle, uint64_t) {}
HorizonResult HORIZON_CALL apiSaveReplay(HorizonPluginHandle, const char*) { return HORIZON_ERR_NOT_SUPPORTED; }
HorizonResult HORIZON_CALL apiRegisterPanel(HorizonPluginHandle h, const HorizonPanelDef* d) {
    if (!d || !d->key || !d->title || !d->draw) return HORIZON_ERR_INVALID_ARG;
    for (Panel& p : of(h)->panels) {
        if (p.key == d->key) {
            p = Panel{d->key, d->title, d->icon, d->flags, d->draw, d->user};
            return HORIZON_OK;
        }
    }
    of(h)->panels.push_back(Panel{d->key, d->title, d->icon, d->flags, d->draw, d->user});
    return HORIZON_OK;
}
void HORIZON_CALL apiUnregisterPanel(HorizonPluginHandle h, const char* key) {
    auto& v = of(h)->panels;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i].key == key) {
            v.erase(v.begin() + static_cast<long>(i));
            return;
        }
    }
}
int32_t HORIZON_CALL apiPanelVisible(HorizonPluginHandle, const char*) { return 0; }
void HORIZON_CALL apiSetHostFlag(HorizonPluginHandle h, uint32_t flag, int32_t on) {
    if (on) of(h)->flags |= flag;
    else of(h)->flags &= ~flag;
}
uint32_t HORIZON_CALL apiHostFlags(void) {
    uint32_t f = 0;
    for (auto& p : g_plugins) f |= p->flags;
    return f;
}

void fillApi(Plugin* p) {
    HorizonHostApi& a = p->api;
    a = HorizonHostApi{};
    a.abi = HORIZON_ABI;
    a.size = sizeof(HorizonHostApi);
    a.abi_minor = HORIZON_ABI_MINOR;
    a.plugin = reinterpret_cast<HorizonPluginHandle>(p);
    a.log = apiLog;
    a.get_string = apiGetString;
    a.set_string = apiSetString;
    a.get_int = apiGetInt;
    a.set_int = apiSetInt;
    a.get_float = apiGetFloat;
    a.set_float = apiSetFloat;
    a.data_folder = apiDataFolder;
    a.data_folder_w = apiDataFolderW;
    a.imgui_context = apiImguiContext;
    a.imgui_version = apiImguiVersion;
    a.imgui_allocators = apiImguiAllocators;
    a.style = apiStyle;
    a.game_info = apiGameInfo;
    a.engine_pointer = apiEnginePointer;
    a.register_hotkey = apiRegisterHotkey;
    a.hotkey_text = apiHotkeyText;
    a.subscribe = apiSubscribe;
    a.post_replay_saved = apiPostReplaySaved;
    a.play_map_file = apiPlayMap;
    a.return_to_menu = apiReturnToMenu;
    a.get_game_state = apiGameState;
    a.is_online = apiIsOnline;
    a.actions_allowed = apiActionsAllowed;
    a.run_on_game_thread = apiRunOnGameThread;
    a.device = apiDevice;
    a.set_device_callbacks = apiSetDeviceCallbacks;
    a.menu_open = apiMenuOpen;
    a.request_restart_note = apiRestartNote;
    a.reflect_offset = apiReflectOffset;
    a.reflect_method = apiReflectMethod;
    a.create_texture_rgba = apiCreateTexture;
    a.release_texture = apiReleaseTexture;
    a.save_replay = apiSaveReplay;
    a.register_panel = apiRegisterPanel;
    a.unregister_panel = apiUnregisterPanel;
    a.panel_visible = apiPanelVisible;
    a.set_host_flag = apiSetHostFlag;
    a.host_flags = apiHostFlags;
}

// Loads <dir>\<id>.dll and calls Info and Init, as the host does. Null: the DLL or its exports are missing.
Plugin* load(const std::wstring& dir, const char* id) {
    auto p = std::make_unique<Plugin>();
    p->id = id;
    std::wstring path = dir + L"\\";
    for (const char* c = id; *c; c++) path += static_cast<wchar_t>(*c);
    path += L".dll";
    p->module = LoadLibraryW(path.c_str());
    if (!p->module) return nullptr;
    auto info = reinterpret_cast<HorizonPlugin_InfoFn>(GetProcAddress(p->module, HORIZON_EXPORT_INFO));
    p->init = reinterpret_cast<HorizonPlugin_InitFn>(GetProcAddress(p->module, HORIZON_EXPORT_INIT));
    p->shutdown = reinterpret_cast<HorizonPlugin_ShutdownFn>(GetProcAddress(p->module, HORIZON_EXPORT_SHUTDOWN));
    p->drawMenu = reinterpret_cast<HorizonPlugin_DrawMenuFn>(GetProcAddress(p->module, HORIZON_EXPORT_DRAW_MENU));
    p->setEnabled = reinterpret_cast<HorizonPlugin_SetEnabledFn>(GetProcAddress(p->module, HORIZON_EXPORT_SET_ENABLED));
    if (!info || !p->init || !p->shutdown) return nullptr;
    p->info = info();
    fillApi(p.get());
    p->enabled = true;
    p->initResult = p->init(&p->api);
    p->running = p->initResult == 0;
    g_plugins.push_back(std::move(p));
    return g_plugins.back().get();
}

void shutdownAll(bool freeLibraries) {
    for (auto& p : g_plugins) {
        if (p->running) p->shutdown();
        p->running = false;
        if (freeLibraries && p->module) FreeLibrary(p->module);
        p->module = nullptr;
    }
    g_plugins.clear();
}

Plugin* find(const char* id) {
    for (auto& p : g_plugins)
        if (p->id == id) return p.get();
    return nullptr;
}

void setEnabled(Plugin* p, bool on) {
    p->enabled = on;
    if (p->setEnabled) p->setEnabled(on ? 1 : 0);
}

Hotkey* hotkey(Plugin* p, const char* action) {
    for (Hotkey& k : p->hotkeys)
        if (k.action == action) return &k;
    return nullptr;
}

// --- The host's device calls (src/host/d3dhook.cpp + plugins.cpp) ---------------------------
void deviceCreated(void* device, void* params) {
    g_device = device;
    for (auto& p : g_plugins)
        if (p->running && p->hasDev && p->dev.device_created) p->dev.device_created(p->dev.user, device, params);
}
void adjustParams(void* params) {
    for (auto& p : g_plugins)
        if (p->running && p->hasDev && p->dev.adjust_present_params) p->dev.adjust_present_params(p->dev.user, params);
}
void beforeReset(void* device) {
    for (auto& p : g_plugins)
        if (p->running && p->hasDev && p->dev.before_reset) p->dev.before_reset(p->dev.user, device);
}
void afterReset(void* device, long hr) {
    for (auto& p : g_plugins)
        if (p->running && p->hasDev && p->dev.after_reset) p->dev.after_reset(p->dev.user, device, static_cast<int32_t>(hr));
}
void prePresent(void* device) {
    for (auto& p : g_plugins)
        if (p->running && p->enabled && p->hasDev && p->dev.pre_present) p->dev.pre_present(p->dev.user, device);
}

bool logHas(const char* text) {
    for (const std::string& l : g_log)
        if (l.find(text) != std::string::npos) return true;
    return false;
}

} // namespace fakehost
