/*
 * horizon_plugin.h - the Horizon Host plugin ABI (C, 32-bit x86, MIT licence).
 *
 * A Horizon plugin is a native DLL in horizon\plugins\<id>\<id>.dll (legacy games) or in the
 * plugins folder of the TMLoader product "Horizon Host" (Forever). The host (horizon\host.dll)
 * loads it, checks its ABI and calls it. Plugins never hook Present/EndScene/Reset or DirectInput
 * themselves: the host owns the Direct3D hook, the Dear ImGui context, input, hotkeys and the
 * game hooks, and hands out what a plugin needs through HorizonHostApi.
 *
 * Rules for this header
 *   - Every struct starts with its own size (or the ABI number) so it can grow at the end.
 *     Additive changes only: new fields go at the end, new events get new numbers, nothing is
 *     reordered or removed. HORIZON_ABI changes only when something can no longer stay compatible.
 *   - HORIZON_ABI_MINOR counts additive changes. A plugin built against a newer minor still loads in
 *     an older host; it checks host->size / host->abi_minor before using newer functions.
 *   - All functions are __cdecl. All strings are UTF-8 unless a name ends in _w (UTF-16, Windows paths).
 *   - Every call into a plugin happens on the game's main thread unless said otherwise.
 *
 * Documentation: docs/plugin-abi.md (guide), docs/profile-json.md (how the launcher enables plugins).
 */
#ifndef HORIZON_PLUGIN_H
#define HORIZON_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HORIZON_ABI 1
#define HORIZON_ABI_MINOR 4 /* 1: create_texture_rgba, release_texture, save_replay
                               2: panels (register_panel ...), host flags (capture)
                               3: open_page, toasts (notify), the TM Horizon bridge (bridge_request,
                                  bridge_state), HorizonDeviceCallbacks.post_present
                               4: ui_metrics (menu size, content scale, room for a plugin page) */

/* Dear ImGui version the host is built with (IMGUI_VERSION_NUM). Plugins that draw must be built
 * with exactly this version (default imconfig.h) and attach to the host's context, see
 * horizon_imgui.h. */
#define HORIZON_IMGUI_VERSION_NUM 19150 /* 1.91.5 */

#ifndef HORIZON_CALL
#define HORIZON_CALL __cdecl
#endif

#ifdef HORIZON_PLUGIN_BUILD
#define HORIZON_EXPORT __declspec(dllexport)
#else
#define HORIZON_EXPORT
#endif

/* ------------------------------------------------------------------ games (bit mask) ---------- */
#define HORIZON_GAME_ESWC     0x01u /* TrackMania Nations ESWC (unofficial 1.80 build) */
#define HORIZON_GAME_SUNRISE  0x02u /* TrackMania Sunrise (Win10 community repack) */
#define HORIZON_GAME_ORIGINAL 0x04u /* TrackMania Original (Online Patch v2 exe) */
#define HORIZON_GAME_TMNF     0x08u /* TrackMania Nations Forever (also TMLoader's combined exe) */
#define HORIZON_GAME_TMUF     0x10u /* TrackMania United Forever */
#define HORIZON_GAME_LEGACY   (HORIZON_GAME_ESWC | HORIZON_GAME_SUNRISE | HORIZON_GAME_ORIGINAL)
#define HORIZON_GAME_FOREVER  (HORIZON_GAME_TMNF | HORIZON_GAME_TMUF)
#define HORIZON_GAME_ALL      (HORIZON_GAME_LEGACY | HORIZON_GAME_FOREVER)

/* ------------------------------------------------------------------ capabilities ---------------- */
#define HORIZON_CAP_MENU        0x01u /* exports HorizonPlugin_DrawMenu (a settings page in the menu) */
#define HORIZON_CAP_OVERLAY     0x02u /* exports HorizonPlugin_DrawOverlay (always-on HUD) */
#define HORIZON_CAP_DEVICE      0x04u /* needs the Direct3D device (renderers such as TMVS) */
#define HORIZON_CAP_GAME_EVENTS 0x08u /* exports HorizonPlugin_OnEvent */
#define HORIZON_CAP_GAME_CONTROL 0x10u /* calls PlayMapFile / ReturnToMenu */

/* ------------------------------------------------------------------ plugin flags ---------------- */
/* Exactly one of SOLO_ONLY / ONLINE_OK. A plugin with neither counts as SOLO_ONLY. */
#define HORIZON_FLAG_SOLO_ONLY   0x01u /* host stops its events, overlay, hotkeys and queued calls online */
#define HORIZON_FLAG_ONLINE_OK   0x02u /* allowed online (TMX rules still apply: no CP display etc.) */
#define HORIZON_FLAG_LIVE_TOGGLE 0x04u /* can be switched on/off without a restart (exports HorizonPlugin_SetEnabled
                                          or simply works when the host stops calling it) */

/* ------------------------------------------------------------------ results --------------------- */
typedef int32_t HorizonResult;
#define HORIZON_OK                   0
#define HORIZON_ERR_NOT_SUPPORTED   -1 /* not available for this game/exe (yet) */
#define HORIZON_ERR_WRONG_STATE     -2 /* e.g. PlayMapFile outside the main menu */
#define HORIZON_ERR_BLOCKED_ONLINE  -3 /* online gate or Competition Patch run gate */
#define HORIZON_ERR_INVALID_ARG     -4
#define HORIZON_ERR_FAILED          -5
#define HORIZON_ERR_BUSY            -6 /* another call of this kind is still running */

/* ------------------------------------------------------------------ game state ------------------ */
typedef enum HorizonGameState {
    HORIZON_STATE_UNKNOWN = 0,
    HORIZON_STATE_MAIN_MENU = 1,   /* menus, no map running */
    HORIZON_STATE_SOLO_RACE = 2,   /* solo race running (campaign, single map, editor test excluded) */
    HORIZON_STATE_END_RACE = 3,    /* solo: the local player finished, end-race screen */
    HORIZON_STATE_NET_RACE = 4,    /* LAN or internet race (counts as online) */
    HORIZON_STATE_EDITOR = 5,      /* map editor (incl. its test drive) */
    HORIZON_STATE_REPLAY = 6,      /* replay viewer / replay editor / video */
    HORIZON_STATE_LOADING = 7
} HorizonGameState;

typedef enum HorizonPlayMode {
    HORIZON_MODE_UNKNOWN = 0,
    HORIZON_MODE_SOLO = 1,
    HORIZON_MODE_NET = 2,          /* LAN or internet */
    HORIZON_MODE_EDITOR_TEST = 3,
    HORIZON_MODE_REPLAY = 4
} HorizonPlayMode;

/* ------------------------------------------------------------------ events ---------------------- */
typedef enum HorizonEventType {
    HORIZON_EVENT_MAP_LOADED = 1,    /* map: the map the game now runs (race, replay or editor) */
    HORIZON_EVENT_RACE_START = 2,    /* the local player (re)starts a run (start line / restart) */
    HORIZON_EVENT_RESPAWN = 3,       /* the local player respawned at a checkpoint; respawns = count in this run */
    HORIZON_EVENT_CHECKPOINT = 4,    /* time_ms = race time at the checkpoint, checkpoint_index = 0-based */
    HORIZON_EVENT_FINISH = 5,        /* time_ms, is_local_player, respawns, play_mode, is_pb */
    HORIZON_EVENT_RACE_END = 6,      /* the game stopped recording the run and holds its replay (engine_replay);
                                        end_screen = 1: the solo end-race menu follows (auto-advance, "next map") */
    HORIZON_EVENT_MENU_ENTERED = 7,  /* back in the menus, no map running */
    HORIZON_EVENT_REPLAY_SAVED = 8,  /* path = the replay file just written (by the game or a plugin) */
    HORIZON_EVENT_ONLINE_CHANGED = 9,/* is_online changed; sent to every plugin, gated or not */
    HORIZON_EVENT_GAME_STATE = 10    /* game_state changed (HorizonGameState) */
} HorizonEventType;

/* ------------------------------------------------------------------ panels (ABI minor 2) ------- */
/* A panel is a piece of a plugin's UI the host places: a tab on the plugin's page in the menu, which
 * the player can pop out into its own window (and keep on screen with the menu closed). The host owns
 * the tab list, the windows, their options (keep open, opacity, lock layout, click-through, hide in
 * photos and videos) and saves them per plugin id + panel key. The plugin only draws the body. */
#define HORIZON_PANEL_MENU_PAGE 0x01u /* a tab on the plugin's page in the menu */
#define HORIZON_PANEL_HUD       0x02u /* starts in its own window that stays on screen with the menu closed
                                         (e.g. a timer); the player can still bring it into the menu */
#define HORIZON_PANEL_START_HIDDEN 0x04u /* with HUD: a widget that is off until the player shows it (layout mode
                                            bar, or "Show" on its tab). Host 0.3.0; older hosts show it at once */

typedef void (HORIZON_CALL* HorizonPanelDrawFn)(void* user);

typedef struct HorizonPanelDef {
    uint32_t size;               /* sizeof(HorizonPanelDef) */
    const char* key;             /* stable within the plugin, [a-z0-9-], saved with the layout; never change it */
    const char* title;           /* tab and window title, one or two words */
    uint32_t icon;               /* HZ_ICON_* codepoint from horizon_imgui.h (Lucide), 0 = none */
    uint32_t flags;              /* HORIZON_PANEL_* (neither: MENU_PAGE) */
    HorizonPanelDrawFn draw;     /* draws the body: no ImGui::Begin/End of its own, no heading */
    void* user;                  /* passed to draw */
    /* first place of its window, as fractions of the screen (0..1); w/h 0 = fit the content */
    float default_x, default_y, default_w, default_h;
} HorizonPanelDef;

/* Host flags (HorizonHostApi.set_host_flag / host_flags) */
#define HORIZON_HOST_FLAG_CAPTURE 0x01u /* a photo or video is being taken (set by e.g. TMVS): windows with
                                           "Hide in photos and videos" and the Horizon bar are hidden */

/* ------------------------------------------------------------------ toasts (ABI minor 3) ------- */
/* A toast is a short note in the corner of the screen ("Upload finished", "Map saved"). The host draws
 * them in the TM Horizon look, stacks them, hides them in photos and videos (HORIZON_HOST_FLAG_CAPTURE
 * and HorizonHost_SetCaptureActive) and holds them back while the player has "Do not disturb" on. */
#define HORIZON_TOAST_INFO  0u
#define HORIZON_TOAST_OK    1u
#define HORIZON_TOAST_WARN  2u
#define HORIZON_TOAST_ERROR 3u
#define HORIZON_TOAST_IMPORTANT 0x01u /* flags: shown even with "Do not disturb" (use sparingly) */

typedef struct HorizonToast {
    uint32_t size;               /* sizeof(HorizonToast) */
    uint32_t kind;               /* HORIZON_TOAST_* */
    const char* title;           /* one short line, required */
    const char* text;            /* optional second line, "" or NULL for none */
    uint32_t icon;               /* HZ_ICON_* codepoint, 0 = the kind's icon */
    uint32_t duration_ms;        /* 0 = 5000; at most 30000 */
    uint32_t flags;              /* HORIZON_TOAST_IMPORTANT */
} HorizonToast;

/* ------------------------------------------------------------------ TM Horizon bridge (ABI minor 3) */
/* The game process never talks to the internet. Plugins that need online data (TMX search, a record
 * replay) ask the TM Horizon launcher on the same PC through the host's bridge: plain HTTP on
 * 127.0.0.1, paired once (the player presses Allow in TM Horizon), see docs/client-bridge.md.
 * The host owns the connection, the key and a rate limit; a plugin only sends requests. */
#define HORIZON_BRIDGE_OFF        0 /* the player switched the bridge off (host settings) */
#define HORIZON_BRIDGE_SEARCHING  1 /* TM Horizon is not running (or not found yet) */
#define HORIZON_BRIDGE_PAIRING    2 /* waiting for Allow in TM Horizon */
#define HORIZON_BRIDGE_CONNECTED  3
#define HORIZON_BRIDGE_DENIED     4 /* the player pressed No; asks again after a while */

/* bridge_request answers. status: the launcher's HTTP status (200 ...), or one of these: */
#define HORIZON_BRIDGE_ERR_NOT_CONNECTED -1 /* not paired / TM Horizon not running */
#define HORIZON_BRIDGE_ERR_TIMEOUT       -2
#define HORIZON_BRIDGE_ERR_RATE_LIMIT    -3 /* too many requests from this plugin, try again later */
#define HORIZON_BRIDGE_ERR_TOO_LARGE     -4 /* answer larger than the host takes (16 MB) */
#define HORIZON_BRIDGE_ERR_CANCELLED     -5 /* the plugin was switched off before the answer came */

/* Runs on the game thread at the end of a frame. body is valid only during the call. content_type
 * is the answer's Content-Type ("application/json", "image/jpeg", ...), "" if none. */
typedef void (HORIZON_CALL* HorizonBridgeCallback)(void* user, int32_t status, const char* content_type, const void* body,
                                                   uint32_t body_len);

/* Bit for HorizonHostApi.subscribe: HORIZON_EVENT_BIT(HORIZON_EVENT_FINISH) | ... */
#define HORIZON_EVENT_BIT(type) (1u << (type))
#define HORIZON_EVENTS_ALL 0xfffffffeu

/* Times are milliseconds; -1 = unknown / not set. Strings may be "" when the host can't read them yet
 * (see docs/plugin-abi.md, "What each game fills in"). */
typedef struct HorizonMapInfo {
    uint32_t size;               /* sizeof(HorizonMapInfo) */
    const char* path;            /* full path of the map file, "" if unknown */
    const char* uid;
    const char* name;            /* the map's name with $-formatting removed */
    const char* author;
    const char* environment;
    int32_t author_time, gold_time, silver_time, bronze_time;
    uint32_t play_mode;          /* HorizonPlayMode */
    int32_t author_score;        /* stunts maps, else -1 */
    uint32_t is_stunts;
} HorizonMapInfo;

typedef struct HorizonEvent {
    uint32_t size;               /* sizeof(HorizonEvent) */
    uint32_t type;               /* HorizonEventType */
    uint32_t play_mode;          /* HorizonPlayMode at the time of the event */
    int32_t time_ms;             /* CHECKPOINT, FINISH */
    int32_t checkpoint_index;    /* CHECKPOINT */
    int32_t respawns;            /* RESPAWN, FINISH: respawns in this run, -1 unknown */
    uint32_t is_local_player;    /* FINISH (only local finishes are sent today, so 1) */
    uint32_t is_pb;              /* FINISH: better than the local best on this map in this session */
    uint32_t end_screen;         /* RACE_END */
    uint32_t is_online;          /* every event: the host's IsOnline() at the time */
    uint32_t game_state;         /* GAME_STATE: the new HorizonGameState */
    const HorizonMapInfo* map;   /* MAP_LOADED; for other events the current map or NULL */
    const char* path;            /* REPLAY_SAVED */
    const char* source;          /* REPLAY_SAVED: "game" or the plugin id that saved it */
    /* Engine objects of the running exe (opaque, per exe; NULL if unknown). Only for plugins that
     * carry their own per-exe address tables (e.g. the autosave plugin calls the game's save). */
    void* engine_race;           /* CTrackManiaRace* */
    void* engine_replay;         /* RACE_END: CGameCtnReplayRecord* the run was recorded into */
    void* engine_challenge;      /* the running map's CGameCtnChallenge* */
} HorizonEvent;

/* ------------------------------------------------------------------ game info ------------------- */
typedef struct HorizonGameInfo {
    uint32_t size;               /* sizeof(HorizonGameInfo) */
    uint32_t game;               /* one HORIZON_GAME_* bit */
    const char* game_name;       /* "TrackMania Nations ESWC" */
    const char* exe_version;     /* "0.1.7.9 (unofficial 1.80)" */
    const char* exe_sha256;      /* 64 lowercase hex chars */
    uint32_t exe_timestamp;      /* PE header TimeDateStamp */
    uintptr_t exe_base;          /* module base of the exe (rebase link addresses with it) */
    const char* language;        /* "en", "de", ... ("" unknown) */
    const char* game_folder;     /* folder of the exe, trailing backslash */
    const char* user_folder;     /* the game's user data folder (Documents\TrackMania...), "" unknown */
    const char* host_version;    /* "0.1.0" */
    uint32_t loader;             /* 1 = Legacy Loader (dinput8.dll), 2 = TMLoader product */
    uint32_t safe_mode;          /* 1: started without plugins (Shift held / launcher) - plugins never see 1 */
} HorizonGameInfo;

/* ------------------------------------------------------------------ host style ------------------ */
/* The TM Horizon look for plugin pages. Colours are RGBA 0..1 (ImVec4 layout). Fonts are ImFont*
 * from the shared atlas; NULL until the first frame has been drawn. */
typedef struct HorizonColor { float r, g, b, a; } HorizonColor;
typedef struct HorizonStyle {
    uint32_t size;               /* sizeof(HorizonStyle) */
    HorizonColor accent, accent2, accent3, accent_ink;
    HorizonColor ok, warn, bad;
    HorizonColor bg, bg2, surface, text, text2, muted;
    float radius_sm, radius, radius_lg; /* pixels, already scaled */
    float scale;                 /* UI scale (1.0 at 1080p) */
    void* font_body;             /* ImFont* */
    void* font_title;            /* ImFont* (display font, larger) */
    void* font_mono;             /* ImFont* */
} HorizonStyle;

/* ------------------------------------------------------------------ UI metrics (ABI minor 4) --- */
/* The player sets the menu size (share of the screen) and a content scale (80-125 %) in General ->
 * Display, or with Ctrl + mouse wheel in the menu. The host then rebuilds its fonts inside the same
 * ImFont objects (HorizonStyle's font pointers stay valid) and HorizonStyle.scale changes. Values change
 * between frames: read them while drawing, don't keep copies. */
typedef struct HorizonUiMetrics {
    uint32_t size;               /* sizeof(HorizonUiMetrics) */
    float scale;                 /* = HorizonStyle.scale: screen height / 1080 (1..2) x content scale */
    float content_scale;         /* the player's content scale, 0.8 .. 1.25 */
    float display_w, display_h;  /* the game's back buffer, pixels */
    float menu_w, menu_h;        /* the Horizon menu window, pixels */
    float page_w, page_h;        /* room for a plugin page inside the menu card, pixels (0 before a page was drawn) */
} HorizonUiMetrics;

/* ------------------------------------------------------------------ device access --------------- */
/* For HORIZON_CAP_DEVICE plugins. All pointers are IDirect3DDevice9* / D3DPRESENT_PARAMETERS* passed
 * as void* so this header needs no d3d9.h. Order per frame: device plugins' pre_present -> host
 * ImGui (menu + overlays) -> Present. */
typedef struct HorizonDeviceCallbacks {
    uint32_t size;               /* sizeof(HorizonDeviceCallbacks) */
    void* user;
    void (HORIZON_CALL* device_created)(void* user, void* device, void* present_params);
    void (HORIZON_CALL* adjust_present_params)(void* user, void* present_params); /* before CreateDevice/Reset */
    void (HORIZON_CALL* before_reset)(void* user, void* device);
    void (HORIZON_CALL* after_reset)(void* user, void* device, int32_t hresult);
    void (HORIZON_CALL* pre_present)(void* user, void* device);
    /* ABI minor 3 (host->abi_minor >= 3; an older host never calls it): right after the game's Present
     * returned, with its HRESULT. For frame timing and frame pacing (a limiter that waits here lets the
     * game read input right after the wait). Keep it short; never call Present from here. */
    void (HORIZON_CALL* post_present)(void* user, void* device, int32_t hresult);
} HorizonDeviceCallbacks;

/* ------------------------------------------------------------------ host API -------------------- */
typedef struct HorizonPluginHandle_* HorizonPluginHandle;

#define HORIZON_LOG_DEBUG 0
#define HORIZON_LOG_INFO  1
#define HORIZON_LOG_WARN  2
#define HORIZON_LOG_ERROR 3

#define HORIZON_ENGINE_APP       1 /* CTrackMania* (the game/app object) */
#define HORIZON_ENGINE_RACE      2 /* current CTrackManiaRace* or NULL */
#define HORIZON_ENGINE_CHALLENGE 3 /* running CGameCtnChallenge* or NULL */

typedef void (HORIZON_CALL* HorizonCallback)(void* user);

/* One table per plugin (plugin = this plugin's handle). Valid from Init until Shutdown returns. */
typedef struct HorizonHostApi {
    uint32_t abi;                /* HORIZON_ABI of the host */
    uint32_t size;               /* sizeof(HorizonHostApi) of the host: check before using fields added later */
    uint32_t abi_minor;          /* HORIZON_ABI_MINOR of the host */
    HorizonPluginHandle plugin;

    /* --- log: one line into horizon\logs\<start>.log, prefixed with the plugin id. printf format. */
    void (HORIZON_CALL* log)(HorizonPluginHandle plugin, int32_t level, const char* fmt, ...);

    /* --- settings: per plugin, stored by the host in horizon\settings\<id>.json (written on change). */
    int32_t (HORIZON_CALL* get_string)(HorizonPluginHandle plugin, const char* key, const char* fallback, char* out,
                                       uint32_t out_size); /* returns the length (truncated to out_size-1) */
    void (HORIZON_CALL* set_string)(HorizonPluginHandle plugin, const char* key, const char* value);
    int32_t (HORIZON_CALL* get_int)(HorizonPluginHandle plugin, const char* key, int32_t fallback);
    void (HORIZON_CALL* set_int)(HorizonPluginHandle plugin, const char* key, int32_t value);
    float (HORIZON_CALL* get_float)(HorizonPluginHandle plugin, const char* key, float fallback);
    void (HORIZON_CALL* set_float)(HorizonPluginHandle plugin, const char* key, float value);

    /* --- per-plugin data folder (history, caches): horizon\data\<id>\ with trailing backslash, created. */
    const char* (HORIZON_CALL* data_folder)(HorizonPluginHandle plugin);
    const wchar_t* (HORIZON_CALL* data_folder_w)(HorizonPluginHandle plugin);

    /* --- Dear ImGui: the host's context and allocator (attach with horizon_imgui.h). */
    void* (HORIZON_CALL* imgui_context)(void);   /* ImGuiContext* */
    int32_t (HORIZON_CALL* imgui_version)(void); /* IMGUI_VERSION_NUM of the host */
    void (HORIZON_CALL* imgui_allocators)(void** alloc_fn, void** free_fn, void** user_data);
    const HorizonStyle* (HORIZON_CALL* style)(void);

    /* --- game */
    const HorizonGameInfo* (HORIZON_CALL* game_info)(void);
    void* (HORIZON_CALL* engine_pointer)(uint32_t which); /* HORIZON_ENGINE_*, NULL if unknown */

    /* --- hotkeys: the host shows every hotkey and its conflicts in the menu, the user can rebind.
     * key_text: "F9", "Ctrl+Tab", "Shift+F11", "Insert"; "" = no default key. action: a stable id
     * ("save"), name: shown in the menu. Returns a handle > 0, or 0 on error. The callback runs on
     * the game thread once per press, not while the menu captures text, not while the plugin is off
     * or gated online. */
    int32_t (HORIZON_CALL* register_hotkey)(HorizonPluginHandle plugin, const char* action, const char* name,
                                            const char* key_text, HorizonCallback callback, void* user);
    /* The key now bound to a hotkey ("" none), for showing it in the plugin's own UI. */
    const char* (HORIZON_CALL* hotkey_text)(HorizonPluginHandle plugin, int32_t handle);

    /* --- events: which HorizonPlugin_OnEvent calls the plugin wants (HORIZON_EVENT_BIT mask). */
    void (HORIZON_CALL* subscribe)(HorizonPluginHandle plugin, uint32_t event_mask);
    /* Tell the host (and every other plugin) that this plugin wrote a replay. */
    void (HORIZON_CALL* post_replay_saved)(HorizonPluginHandle plugin, const char* path);

    /* --- game control (the per-exe work lives in the host). Return HorizonResult. */
    HorizonResult (HORIZON_CALL* play_map_file)(HorizonPluginHandle plugin, const char* path_utf8, uint32_t flags);
    HorizonResult (HORIZON_CALL* return_to_menu)(HorizonPluginHandle plugin);
    int32_t (HORIZON_CALL* get_game_state)(void); /* HorizonGameState */
    int32_t (HORIZON_CALL* is_online)(void);      /* 1 online (LAN/internet race or server browser), 0 not */
    /* 1 when this plugin may act now (not gated: online for solo-only plugins, Competition Patch run). */
    int32_t (HORIZON_CALL* actions_allowed)(HorizonPluginHandle plugin);

    /* --- threads: queue cb to run on the game's main thread at the end of a frame (safe point). Any thread.
     * Queued calls of a plugin have run (or were dropped) before its HorizonPlugin_Shutdown is called. */
    void (HORIZON_CALL* run_on_game_thread)(HorizonPluginHandle plugin, HorizonCallback callback, void* user);

    /* --- device access. device(): the game's IDirect3DDevice9* (for every plugin), NULL before creation.
     * set_device_callbacks: for HORIZON_CAP_DEVICE plugins (renderers). */
    void* (HORIZON_CALL* device)(void);
    void (HORIZON_CALL* set_device_callbacks)(HorizonPluginHandle plugin, const HorizonDeviceCallbacks* callbacks);

    /* --- menu */
    int32_t (HORIZON_CALL* menu_open)(void);      /* 1 while the Horizon menu is shown */
    void (HORIZON_CALL* request_restart_note)(HorizonPluginHandle plugin, const char* why); /* "applies after restart" */

    /* --- engine reflection (read-only, by class and member NAME; ids differ per engine version).
     * -1 / NULL when not found. */
    int32_t (HORIZON_CALL* reflect_offset)(const char* class_name, const char* member_name);
    void* (HORIZON_CALL* reflect_method)(const char* class_name, const char* member_name);

    /* ---- ABI minor 1 (check host->abi_minor >= 1 or host->size before use) ---------------------- */
    /* A texture for ImGui::Image (e.g. map thumbnails): w*h RGBA8 pixels, rows top to bottom. Returns an
     * ImTextureID (an IDirect3DTexture9* in the managed pool, survives Reset), 0 if there is no device yet
     * or it failed. Game thread only. Release it with release_texture (also before Shutdown returns). */
    uint64_t (HORIZON_CALL* create_texture_rgba)(HorizonPluginHandle plugin, uint32_t width, uint32_t height, const void* rgba);
    void (HORIZON_CALL* release_texture)(HorizonPluginHandle plugin, uint64_t texture);
    /* Saves the replay of the current/last run like the game's "Save replay", without dialogs.
     * name_utf8: file name without extension ("" = the game's default name). HORIZON_ERR_NOT_SUPPORTED
     * where the host does not do it (host 0.2.0 and later: ESWC only; Sunrise, Original and Forever return it.
     * The Autosave plugin saves on ESWC, Sunrise and Original with its own copy of the game's calls). */
    HorizonResult (HORIZON_CALL* save_replay)(HorizonPluginHandle plugin, const char* name_utf8);

    /* ---- ABI minor 2 (check host->abi_minor >= 2 before use) ------------------------------------ */
    /* Adds a panel, or replaces the one with the same key. def and its strings are copied. Game thread
     * (Init is fine). HORIZON_ERR_INVALID_ARG: no key/title/draw or a bad key. */
    HorizonResult (HORIZON_CALL* register_panel)(HorizonPluginHandle plugin, const HorizonPanelDef* def);
    void (HORIZON_CALL* unregister_panel)(HorizonPluginHandle plugin, const char* key);
    /* 1 while the panel is drawn this frame (its tab is the open one, or its window is on screen). */
    int32_t (HORIZON_CALL* panel_visible)(HorizonPluginHandle plugin, const char* key);
    /* Host flags (HORIZON_HOST_FLAG_*): each plugin sets its own; host_flags() is the OR of all. */
    void (HORIZON_CALL* set_host_flag)(HorizonPluginHandle plugin, uint32_t flag, int32_t on);
    uint32_t (HORIZON_CALL* host_flags)(void);

    /* ---- ABI minor 3 (check host->abi_minor >= 3 before use) ------------------------------------ */
    /* Order is fixed: open_page first (TMVS 2.0 reads it right after host_flags), then notify,
     * bridge_request, bridge_state. A plugin built against 1.2 never sees these fields. */
    /* Opens the Horizon menu on this plugin's page (for a "open my page" hotkey). panel_key NULL or ""
     * = the plugin's Settings tab (or the plain page), else that panel's tab; a key the plugin has no
     * panel for returns HORIZON_ERR_INVALID_ARG, the page still opens on Settings. If the menu is
     * already open on this page it closes (toggle, like the menu key). Game thread only.
     * HORIZON_ERR_WRONG_STATE while a capture (HorizonHost_SetCaptureActive) is active. */
    HorizonResult (HORIZON_CALL* open_page)(HorizonPluginHandle plugin, const char* panel_key);
    /* Shows a toast (copied). Any thread. Toasts of a plugin that is off or gated online are dropped. */
    void (HORIZON_CALL* notify)(HorizonPluginHandle plugin, const HorizonToast* toast);
    /* Sends one request to TM Horizon over the bridge. method "GET" or "POST"; path starts with "/v1/"
     * (docs/client-bridge.md lists them); body/body_len for POST (JSON), else NULL/0. The answer comes
     * later through callback (game thread), errors too. Returns HORIZON_OK when queued,
     * HORIZON_ERR_INVALID_ARG, HORIZON_ERR_BUSY (8 requests of this plugin open) or
     * HORIZON_ERR_NOT_SUPPORTED (bridge switched off), HORIZON_ERR_WRONG_STATE (not on the game
     * thread). A request over the rate limit is still HORIZON_OK; its callback gets
     * HORIZON_BRIDGE_ERR_RATE_LIMIT. Game thread. */
    HorizonResult (HORIZON_CALL* bridge_request)(HorizonPluginHandle plugin, const char* method, const char* path,
                                                 const void* body, uint32_t body_len, HorizonBridgeCallback callback,
                                                 void* user);
    int32_t (HORIZON_CALL* bridge_state)(void); /* HORIZON_BRIDGE_* */

    /* ---- ABI minor 4 (check host->abi_minor >= 4 before use) ------------------------------------ */
    /* Menu size, content scale and the room a plugin page has, so a wide page can pick fewer columns
     * instead of clipping. The pointer stays valid for the whole start. Game thread. */
    const HorizonUiMetrics* (HORIZON_CALL* ui_metrics)(void);
} HorizonHostApi;

/* ------------------------------------------------------------------ plugin exports --------------- */
typedef struct HorizonPluginInfo {
    uint32_t abi;                /* HORIZON_ABI the plugin was built against */
    const char* id;              /* folder/settings id: lowercase, [a-z0-9-], e.g. "eswc-autosave" */
    const char* name;
    const char* version;         /* semver */
    uint32_t games;              /* HORIZON_GAME_* mask */
    uint32_t caps;               /* HORIZON_CAP_* */
    uint32_t flags;              /* HORIZON_FLAG_* */
    const char* author;
    const char* description;     /* one or two short sentences for the tile */
    uint32_t abi_minor;          /* HORIZON_ABI_MINOR the plugin was built against */
} HorizonPluginInfo;

/* Required exports */
typedef const HorizonPluginInfo* (HORIZON_CALL* HorizonPlugin_InfoFn)(void);
typedef int32_t (HORIZON_CALL* HorizonPlugin_InitFn)(const HorizonHostApi* host); /* 0 = ok, else not loaded */
typedef void (HORIZON_CALL* HorizonPlugin_ShutdownFn)(void);
/* Optional exports, looked up by name */
typedef void (HORIZON_CALL* HorizonPlugin_DrawMenuFn)(void);    /* inside the plugin's page (an ImGui child) */
typedef void (HORIZON_CALL* HorizonPlugin_DrawOverlayFn)(void); /* every frame while enabled and not gated */
typedef void (HORIZON_CALL* HorizonPlugin_OnEventFn)(const HorizonEvent* e);
typedef void (HORIZON_CALL* HorizonPlugin_SetEnabledFn)(int32_t enabled); /* live on/off from the menu */

#define HORIZON_EXPORT_INFO        "HorizonPlugin_Info"
#define HORIZON_EXPORT_INIT        "HorizonPlugin_Init"
#define HORIZON_EXPORT_SHUTDOWN    "HorizonPlugin_Shutdown"
#define HORIZON_EXPORT_DRAW_MENU   "HorizonPlugin_DrawMenu"
#define HORIZON_EXPORT_DRAW_OVERLAY "HorizonPlugin_DrawOverlay"
#define HORIZON_EXPORT_ON_EVENT    "HorizonPlugin_OnEvent"
#define HORIZON_EXPORT_SET_ENABLED "HorizonPlugin_SetEnabled"

#ifdef HORIZON_PLUGIN_BUILD
HORIZON_EXPORT const HorizonPluginInfo* HORIZON_CALL HorizonPlugin_Info(void);
HORIZON_EXPORT int32_t HORIZON_CALL HorizonPlugin_Init(const HorizonHostApi* host);
HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_Shutdown(void);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HORIZON_PLUGIN_H */
