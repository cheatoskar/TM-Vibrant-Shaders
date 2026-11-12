#include "modules/hostbridge.h"
#include "modules/modules.h"
#include "clips/clips.h"
#include "clips/settings.h"
#include "config.h"
#include "hook.h"
#include "horizon.h"
#include "keys.h"
#include "log.h"
#include "menu.h"
#include "overlay.h"
#include "plugin.h"
#include "imgui.h"
#include "horizon_plugin.h"
#include "horizon_imgui.h" // HZ_ICON_*
#include <windows.h>
#include <d3d9.h>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace tmshaders {
namespace hostbridge {
namespace {

using modules::Id;

struct Slot {
    const HorizonHostApi* host = nullptr; // Init .. Shutdown
    bool attached = false;
    bool enabled = false;
    std::vector<std::string> panels; // keys registered
    bool flagSet = false;            // our capture flag is on through this module
};
Slot g_slots[modules::kCount];

bool g_claimed = false;   // claimInstance ran
bool g_refused = false;   // the standalone DLL runs here: the modules stay off
std::string g_owner;      // which copy runs (for the refused note)
bool g_booted = false;
std::wstring g_dataRoot;  // setDataRoot (tests)
ImGuiMemAllocFunc g_hostAlloc = nullptr;
ImGuiMemFreeFunc g_hostFree = nullptr;
void* g_hostAllocUser = nullptr;
bool g_captureWanted = false; // horizon::capture's last answer (the flag follows the pre_present module)
float g_lastUiScale = 0.0f;   // what the last page drawn for the host got (ABI 1.4 ui_metrics; the tests read it)
float g_lastUiRoom = 0.0f;

bool validModule(int module) {
    return module >= 0 && module < modules::kCount;
}

// The module whose device calls the core takes: the lowest attached one (device, Reset), the lowest
// attached and enabled one (pre_present: the host calls it only for enabled plugins).
int deviceOwner() {
    for (int i = 0; i < modules::kCount; i++) {
        if (g_slots[i].attached) return i;
    }
    return -1;
}

int presentOwner() {
    for (int i = 0; i < modules::kCount; i++) {
        if (g_slots[i].attached && g_slots[i].enabled) return i;
    }
    return -1;
}

const HorizonHostApi* anyHost() {
    const int owner = presentOwner() >= 0 ? presentOwner() : deviceOwner();
    return owner >= 0 ? g_slots[owner].host : nullptr;
}

void hostLog(int module, int level, const char* text) {
    if (validModule(module) && g_slots[module].host) g_slots[module].host->log(g_slots[module].host->plugin, level, "%s", text);
}

int moduleOf(void* user) {
    return static_cast<int>(reinterpret_cast<intptr_t>(user));
}

// --- Device callbacks (HorizonDeviceCallbacks), registered by every module ---------------------

void HORIZON_CALL devCreated(void*, void* device, void* params) {
    // Every module's call: the same device again is ignored (hook::hostedDeviceCreated).
    hook::hostedDeviceCreated(static_cast<IDirect3DDevice9*>(device), static_cast<const D3DPRESENT_PARAMETERS*>(params));
}

void HORIZON_CALL devAdjust(void* user, void* params) {
    if (moduleOf(user) == deviceOwner()) hook::hostedAdjustPresentParams(static_cast<D3DPRESENT_PARAMETERS*>(params));
}

void HORIZON_CALL devBeforeReset(void* user, void* device) {
    if (moduleOf(user) == deviceOwner()) hook::hostedBeforeReset(static_cast<IDirect3DDevice9*>(device));
}

void HORIZON_CALL devAfterReset(void* user, void* device, int32_t hr) {
    if (moduleOf(user) == deviceOwner()) hook::hostedAfterReset(static_cast<IDirect3DDevice9*>(device), static_cast<HRESULT>(hr));
}

void HORIZON_CALL devPrePresent(void* user, void* device) {
    if (moduleOf(user) == presentOwner()) hook::hostedPresent(static_cast<IDirect3DDevice9*>(device));
}

// --- Horizon's interface state and capture flag for src/horizon.cpp --------------------------

uint32_t hostedUiState() {
    const HorizonHostApi* host = anyHost();
    return host && host->menu_open && host->menu_open() ? horizon::kMenuOpen : 0u;
}

// The flag is set through the module that takes pre_present; a module that stops being it takes its
// flag back.
void syncCaptureFlag() {
    const int owner = presentOwner();
    for (int i = 0; i < modules::kCount; i++) {
        Slot& s = g_slots[i];
        const bool want = g_captureWanted && i == owner;
        if (s.flagSet == want || !s.host) continue;
        s.host->set_host_flag(s.host->plugin, HORIZON_HOST_FLAG_CAPTURE, want ? 1 : 0);
        s.flagSet = want;
    }
}

void hostedCapture(bool active) {
    g_captureWanted = active;
    syncCaptureFlag();
}

// --- Hotkeys --------------------------------------------------------------------------------

enum HotkeyKind : int { kKey = 0, kKeyShifted = 1, kClip = 2, kOpenPage = 3 };

// Horizon Host 0.2 with plugin ABI 1.3 (docs/plugin-abi.md "ABI 1.3", docs/spec-open-page.md):
// open_page(plugin, panel_key) opens Horizon's menu on the plugin's page (again: closes it). It is the first
// 1.3 field, right after the 1.2 table. Null on an older host.
typedef HorizonResult(HORIZON_CALL* OpenPageFn)(HorizonPluginHandle plugin, const char* panelKey);
OpenPageFn openPageOf(const HorizonHostApi* host) {
    if (!host || host->abi_minor < 3 || host->size < offsetof(HorizonHostApi, open_page) + sizeof(OpenPageFn)) return nullptr;
    return host->open_page;
}

void HORIZON_CALL onHotkey(void* user) {
    const intptr_t v = reinterpret_cast<intptr_t>(user);
    const int kind = static_cast<int>(v >> 16), value = static_cast<int>(v & 0xffff);
    if (kind == kKey) keys::push(static_cast<keys::Action>(value));
    else if (kind == kKeyShifted) keys::pushShifted(static_cast<keys::Action>(value));
    else if (kind == kClip) clips::run(static_cast<clips::Action>(value));
    else if (kind == kOpenPage && value >= 0 && value < modules::kCount && g_slots[value].host) {
        if (OpenPageFn open = openPageOf(g_slots[value].host)) open(g_slots[value].host->plugin, nullptr);
    }
}

void* hotkeyUser(int kind, int value) {
    return reinterpret_cast<void*>(static_cast<intptr_t>((kind << 16) | value));
}

// The key a 1.5 settings.ini (or the defaults) gives an action, as Horizon's key text.
std::string keyOf(keys::Action a) {
    return keys::name(keys::get(a));
}

void registerHotkeys(int module) {
    const HorizonHostApi* host = g_slots[module].host;
    int wanted = 0, done = 0;
    auto add = [&](const char* action, const char* name, const std::string& key, void* user) {
        wanted++;
        if (host->register_hotkey(host->plugin, action, name, key.c_str(), onHotkey, user) > 0) done++;
    };
    auto addKey = [&](const char* action, const char* name, keys::Action a) {
        if (!keys::info(a).available) return;
        add(action, name, keyOf(a), hotkeyUser(kKey, static_cast<int>(a)));
    };
    if (module == modules::kShaders) {
        // The 1.5 menu key (F8, or yours) opens TMVS's page in Horizon's menu, where the host can do that.
        if (openPageOf(host)) add("menu", "Open TMVS page", keyOf(keys::Action::Menu), hotkeyUser(kOpenPage, module));
        addKey("effects", "Effects on/off", keys::Action::Effects);
        addKey("photo-split", "Compare split", keys::Action::PhotoSplit);
        addKey("reload", "Reload shaders", keys::Action::Reload);
        addKey("trace", "Trace", keys::Action::Trace);
    } else if (module == modules::kCapture) {
        addKey("photo", "Photo mode", keys::Action::Photo);
        addKey("photo-panel", "Photo panel", keys::Action::PhotoPanel);
        addKey("capture", "Photo / capture", keys::Action::Capture);
        // Shift+ the capture key: a hi-res photo in photo mode. Its own entry, so it can be rebound.
        const std::string capture = keyOf(keys::Action::Capture);
        add("photo-hires", "Hi-res photo", capture.empty() ? std::string() : "Shift+" + capture,
            hotkeyUser(kKeyShifted, static_cast<int>(keys::Action::Capture)));
        // The clips' keys from clips.ini (1.5's Clips page set them there).
        clips::Settings cs;
        clips::load(cs);
        add("clip-last-run", "Save last run", cs.lastRunKey, hotkeyUser(kClip, static_cast<int>(clips::Action::SaveLastRun)));
        add("clip-run-so-far", "Save run so far", cs.runSoFarKey, hotkeyUser(kClip, static_cast<int>(clips::Action::SaveRunSoFar)));
        add("clip-last", "Save last seconds", cs.saveKey, hotkeyUser(kClip, static_cast<int>(clips::Action::SaveLast)));
    }
    if (wanted) {
        char line[160];
        snprintf(line, sizeof(line), "%d of %d hotkeys registered (the keys from settings.ini and clips.ini)", done, wanted);
        hostLog(module, done == wanted ? HORIZON_LOG_INFO : HORIZON_LOG_WARN, line);
        TMVS_LOG("modules: %s: %s", modules::pluginId(static_cast<Id>(module)), line);
    }
}

// --- Panels -----------------------------------------------------------------------------------

struct PanelPage {
    int module;
    int page; // menu::pages
    const char* key;
    const char* title;
    uint32_t icon;
};

const PanelPage kPanels[] = {
    {modules::kShaders, menu::pages::kShaders, "shaders", "Shaders", HZ_ICON_LAYERS},
    {modules::kShaders, menu::pages::kCustomize, "customize", "Customize", HZ_ICON_SLIDERS_HORIZONTAL},
    {modules::kShaders, menu::pages::kStudio, "studio", "Studio", HZ_ICON_LAYOUT_GRID},
    {modules::kShaders, menu::pages::kMaps, "maps", "Maps", HZ_ICON_MAP},
    {modules::kShaders, menu::pages::kPerformance, "performance", "Performance", HZ_ICON_MONITOR},
    {modules::kShaders, menu::pages::kMisc, "misc", "Misc", HZ_ICON_FLAG},
    {modules::kShaders, menu::pages::kAbout, "about", "About", HZ_ICON_INFO},
    {modules::kCapture, menu::pages::kClips, "clips", "Clips", HZ_ICON_FILM},
    {modules::kCapture, menu::pages::kVideo, "video", "Video", HZ_ICON_PLAY},
};

// Each module's Settings tab (DrawMenu).
int settingsPage(int module) {
    switch (module) {
    case modules::kShaders: return menu::pages::kSettings;
    case modules::kCapture: return menu::pages::kCaptureSettings;
    case modules::kUpscaling: return menu::pages::kUpscaling;
    case modules::kPacks: return menu::pages::kPacks;
    default: return -1;
    }
}

// Our own ImGui context and allocator while one of the host's draws borrows the globals.
struct OwnContext {
    ImGuiContext* context;
    ImGuiMemAllocFunc alloc;
    ImGuiMemFreeFunc free;
    void* user;
};

void restoreOwn(const OwnContext* own) {
    ImGui::SetCurrentContext(own->context);
    ImGui::SetAllocatorFunctions(own->alloc, own->free, own->user);
}

// The draw, with our context given back however it ends. Horizon Host 0.3 catches an exception in a
// plugin's DrawMenu or panel (C++ or an access violation), switches the plugin off and draws on: without
// the __finally our DLL's ImGui would keep pointing at the host's context, and our own menu, toasts and
// photo hints would draw into it. No C++ objects in this function (__try).
void drawGuarded(const OwnContext* own, void (*draw)(void*), void* arg) {
    __try {
        draw(arg);
    } __finally {
        if (AbnormalTermination()) overlay::abortPageForHorizon();
        restoreOwn(own);
    }
}

// The host calls this inside its own ImGui frame: its context lives in the host's copy of Dear ImGui.
// Ours (this DLL's) is pointed at it, with the host's allocator, for the draw only (memory either side
// allocates in that context is freed by the host), and given back its own afterwards.
template <typename Fn>
void inHostContext(const HorizonHostApi* host, Fn draw) {
    if (!host || !g_hostAlloc) return;
    OwnContext own{ImGui::GetCurrentContext(), nullptr, nullptr, nullptr};
    ImGui::GetAllocatorFunctions(&own.alloc, &own.free, &own.user);
    ImGui::SetAllocatorFunctions(g_hostAlloc, g_hostFree, g_hostAllocUser);
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(host->imgui_context()));
    drawGuarded(&own, [](void* fn) { (*static_cast<Fn*>(fn))(); }, &draw);
}

// The host's title font for our headings (its atlas: ours can't be drawn into its lists), null before
// its first frame or from a host without one.
ImFont* hostTitleFont(const HorizonHostApi* host) {
    const HorizonStyle* style = host && host->style ? host->style() : nullptr;
    if (!style || style->size < offsetof(HorizonStyle, font_title) + sizeof(void*)) return nullptr;
    return static_cast<ImFont*>(style->font_title);
}

// Horizon Host 0.3 with plugin ABI 1.4 (docs/plugin-abi.md "ABI 1.4"): the menu size, the player's
// content scale and the room of the container calling us right now (the menu page, a popped-out panel,
// a free plugin window; the host sets page_w/page_h before each call). Null on an older host.
const HorizonUiMetrics* uiMetricsOf(const HorizonHostApi* host) {
    if (!host || host->abi_minor < 4 || host->size < offsetof(HorizonHostApi, ui_metrics) + sizeof(void*) || !host->ui_metrics) return nullptr;
    const HorizonUiMetrics* m = host->ui_metrics();
    return m && m->size >= sizeof(HorizonUiMetrics) ? m : nullptr;
}

// The host's UI scale: ui_metrics (1.4), else its style's scale (screen height / 1080), else 0 (ours).
float hostScale(const HorizonHostApi* host) {
    if (const HorizonUiMetrics* m = uiMetricsOf(host)) return m->scale;
    const HorizonStyle* style = host && host->style ? host->style() : nullptr;
    if (style && style->size >= offsetof(HorizonStyle, scale) + sizeof(float)) return style->scale;
    return 0.0f;
}

// ready: overlay::readyForHorizon(), asked in our own context before the switch.
void drawPage(int page, bool ready, const HorizonHostApi* host) {
    const HorizonUiMetrics* m = uiMetricsOf(host);
    const float room = m && m->page_w > 0.0f ? m->page_w : 0.0f;
    g_lastUiScale = hostScale(host);
    g_lastUiRoom = room;
    if (!ready || !overlay::drawPageForHorizon(page, hostTitleFont(host), g_lastUiScale, room))
        ImGui::TextDisabled("Starts with the game's first frame.");
}

void HORIZON_CALL drawPanel(void* user) {
    const PanelPage& p = kPanels[reinterpret_cast<intptr_t>(user)];
    const bool ready = overlay::readyForHorizon();
    const HorizonHostApi* host = g_slots[p.module].host;
    inHostContext(host, [&p, ready, host] { drawPage(p.page, ready, host); });
}

void registerPanels(int module) {
    const HorizonHostApi* host = g_slots[module].host;
    for (size_t i = 0; i < sizeof(kPanels) / sizeof(kPanels[0]); i++) {
        const PanelPage& p = kPanels[i];
        if (p.module != module) continue;
        HorizonPanelDef def{};
        def.size = sizeof(def);
        def.key = p.key;
        def.title = p.title;
        def.icon = p.icon;
        def.flags = HORIZON_PANEL_MENU_PAGE;
        def.draw = drawPanel;
        def.user = reinterpret_cast<void*>(static_cast<intptr_t>(i));
        if (host->register_panel(host->plugin, &def) == HORIZON_OK) g_slots[module].panels.push_back(p.key);
    }
}

// --- Module states -----------------------------------------------------------------------------

// What the core's features follow, after every attach, detach and switch.
void apply() {
    if (!g_booted) return;
    Config::get().setShadersBlocked(!modules::on(modules::kShaders));
    if (modules::attached(modules::kCapture)) plugin::startClips();
    clips::setAllowed(modules::on(modules::kCapture));
    syncCaptureFlag();
    (void)modules::takeChanges();
}

bool checkHost(const HorizonHostApi* host) {
    return host && host->abi == HORIZON_ABI && host->abi_minor >= 2 && host->size >= offsetof(HorizonHostApi, host_flags) + sizeof(void*) &&
           host->register_panel && host->set_host_flag && host->set_device_callbacks && host->register_hotkey;
}

bool checkImGui(const HorizonHostApi* host) {
    if (!host->imgui_version || host->imgui_version() != IMGUI_VERSION_NUM || !host->imgui_context || !host->imgui_allocators) return false;
    void* alloc = nullptr;
    void* release = nullptr;
    void* user = nullptr;
    host->imgui_allocators(&alloc, &release, &user);
    if (!alloc || !release) return false;
    g_hostAlloc = reinterpret_cast<ImGuiMemAllocFunc>(alloc);
    g_hostFree = reinterpret_cast<ImGuiMemFreeFunc>(release);
    g_hostAllocUser = user;
    return true;
}

DWORD WINAPI precompile(LPVOID) {
    plugin::boot(); // booted already: the shaders compiled ahead, on this thread (the game doesn't wait)
    return 0;
}

// The first attach: TM Vibrant Shaders for this process, booted as Horizon modules.
bool claimAndBoot(const HorizonHostApi* host) {
    if (!g_claimed) {
        g_claimed = true;
        if (!g_dataRoot.empty()) {
            log::setDataDir(g_dataRoot + L"\\TMVS");
            clips::setDocumentsRoot(g_dataRoot);
        }
        modules::setHosted();
        if (!plugin::claimInstance()) {
            g_refused = true;
            g_owner = plugin::activeOwner();
            char line[200];
            snprintf(line, sizeof(line), "TM Vibrant Shaders already runs in this game as %s: it stays in charge, the modules stay off",
                     g_owner.c_str());
            host->log(host->plugin, HORIZON_LOG_WARN, "%s", line);
            return false;
        }
        plugin::ensureBooted();
        g_booted = true;
        if (HANDLE thread = CreateThread(nullptr, 0, precompile, nullptr, 0, nullptr)) CloseHandle(thread);
        keys::setHostOwned(true);
        clips::setHostKeys(true);
        horizon::useHosted(hostedUiState, hostedCapture, 0);
        TMVS_LOG("modules: Horizon Host's plugin API %u.%u: pages as its panels, hotkeys in its table, the F8 menu off", host->abi,
                 host->abi_minor);
    }
    return !g_refused;
}

} // namespace

int attach(int module, const HorizonHostApi* host) {
    if (!validModule(module)) return kBadModule;
    if (!checkHost(host)) return kOldHost;
    if (g_slots[module].attached) return kAlready;
    if (!checkImGui(host)) {
        host->log(host->plugin, HORIZON_LOG_ERROR, "Dear ImGui differs: the host's %d, ours %d", host->imgui_version ? host->imgui_version() : 0,
                  IMGUI_VERSION_NUM);
        return kImGuiDiffers;
    }
    Slot& slot = g_slots[module];
    slot.host = host; // also when refused: DrawMenu says why
    if (!claimAndBoot(host)) return kStandaloneActive;
    slot.attached = true;
    slot.enabled = true; // the host loads (and calls Init for) enabled plugins only
    modules::setAttached(static_cast<Id>(module), true);
    modules::setEnabled(static_cast<Id>(module), true);

    HorizonDeviceCallbacks dev{};
    dev.size = sizeof(dev);
    dev.user = reinterpret_cast<void*>(static_cast<intptr_t>(module));
    dev.device_created = devCreated;
    dev.adjust_present_params = devAdjust;
    dev.before_reset = devBeforeReset;
    dev.after_reset = devAfterReset;
    dev.pre_present = devPrePresent;
    host->set_device_callbacks(host->plugin, &dev); // a device there already: device_created comes now
    registerHotkeys(module);
    registerPanels(module);
    apply();
    TMVS_LOG("modules: %s attached (%zu panels)", modules::pluginId(static_cast<Id>(module)), slot.panels.size());
    return kAttached;
}

void detach(int module) {
    if (!validModule(module)) return;
    Slot& slot = g_slots[module];
    if (slot.host && slot.attached) {
        for (const std::string& key : slot.panels) slot.host->unregister_panel(slot.host->plugin, key.c_str());
        if (slot.flagSet) slot.host->set_host_flag(slot.host->plugin, HORIZON_HOST_FLAG_CAPTURE, 0);
        slot.host->set_device_callbacks(slot.host->plugin, nullptr);
    }
    const bool was = slot.attached;
    slot = Slot();
    if (!was) return;
    modules::setAttached(static_cast<Id>(module), false);
    modules::setEnabled(static_cast<Id>(module), false);
    apply();
    TMVS_LOG("modules: %s detached", modules::pluginId(static_cast<Id>(module)));
}

void setEnabled(int module, bool enabled) {
    if (!validModule(module) || !g_slots[module].attached || g_slots[module].enabled == enabled) return;
    g_slots[module].enabled = enabled;
    modules::setEnabled(static_cast<Id>(module), enabled);
    apply();
    TMVS_LOG("modules: %s switched %s in the Horizon menu", modules::pluginId(static_cast<Id>(module)), enabled ? "on" : "off");
}

void drawMenu(int module) {
    if (!validModule(module)) return;
    const HorizonHostApi* host = g_slots[module].host;
    const bool ready = !g_refused && overlay::readyForHorizon();
    inHostContext(host, [module, ready, host] {
        if (g_refused) {
            ImGui::TextWrapped("TM Vibrant Shaders already runs in this game as %s. That copy stays in charge (its menu is F8); "
                               "these modules stay off.",
                               g_owner.c_str());
            ImGui::TextDisabled("Remove one of the two to use the other.");
            return;
        }
        drawPage(settingsPage(module), ready, host);
    });
}

bool setDataRoot(const wchar_t* root) {
    if (g_claimed || !root) return false;
    g_dataRoot = root;
    return true;
}

std::string query() {
    char text[512];
    const Config& config = Config::get();
    snprintf(text, sizeof(text),
             "hosted=%d;refused=%d;owner=%s;attached=%d%d%d%d;enabled=%d%d%d%d;deviceOwner=%d;presentOwner=%d;shadersBlocked=%d;"
             "effects=%d;clipsAllowed=%d;upscaling=%d;packs=%d;hostKeys=%d;settingsVersion=%d;preset=%s;device=%d;capture=%d;"
             "uiScale=%.2f;uiRoom=%.0f",
             modules::hosted() ? 1 : 0, g_refused ? 1 : 0, g_refused ? g_owner.c_str() : (g_claimed ? "modules" : ""),
             g_slots[0].attached, g_slots[1].attached, g_slots[2].attached, g_slots[3].attached, g_slots[0].enabled, g_slots[1].enabled,
             g_slots[2].enabled, g_slots[3].enabled, deviceOwner(), presentOwner(), config.shadersBlocked() ? 1 : 0,
             config.settings.enabled ? 1 : 0, clips::allowed() ? 1 : 0, modules::on(modules::kUpscaling) ? 1 : 0,
             modules::on(modules::kPacks) ? 1 : 0, keys::hostOwned() ? 1 : 0, config.settingsVersionRead(), config.preset.c_str(),
             hook::hasGameDevice() ? 1 : 0, g_captureWanted ? 1 : 0, g_lastUiScale, g_lastUiRoom);
    return text;
}

} // namespace hostbridge
} // namespace tmshaders
