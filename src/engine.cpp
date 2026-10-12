#include "engine.h"
#include "gbx.h"
#include "hook.h"
#include "log.h"
#include <windows.h>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <cwchar>

namespace tmshaders {
namespace engine {
namespace {

// TmForever.exe comes in two builds: Nations Forever and United Forever. Same code, shifted
// addresses. Link-time addresses (preferred base 0x00400000): TMNF from TmForever.map, TMUF
// found by matching TMNF's code (absolute addresses and call targets masked) and checked
// over 256 bytes.
constexpr uintptr_t kPreferredBase = 0x00400000;

enum SlotId { kFrameBegin, kCameraBegin, kCameraEnd, kOverlayZones, kFrameEnd, kViewMatrix, kProjection, kShadowsSet, kSlotCount };
enum SiteId { kClipTracks, kClipCams, kVideoShoot, kTileHeight, kRaceReset, kRespawn, kWaterPlane, kLoadDecoration, kSetChallenge, kFreeCam, kZClip, kDrawShader, kDrawPass, kLensFlare, kReadInput, kInterfaceSet, kLoadScene, kClipTriggers, kJumpTime, kRaceTriggers, kSiteCount };

struct GameBuild {
    const char* name;
    uintptr_t viewportVTable;      // ??_7CVisionViewportDx9@@6B@
    uintptr_t slots[kSlotCount];   // the original functions in the vtable
    uintptr_t sites[kSiteCount];   // non-virtual functions, hooked inline
    uintptr_t handlers[kSiteCount]; // absolute address inside a prologue (its SEH handler, a global), or 0
    uintptr_t shaderLevel;         // word: GPU shader path (PC0..PC3)
    uintptr_t idGetString;         // const char* CMwId::GetString() const
    uintptr_t getFidFile;          // static CSystemFidFile* CSystemEngine::GetFidFile(const CMwNod*)
    uintptr_t fidFullName;         // void CSystemFidFile::GetFullName(CFastStringInt&, int, int) const
    uintptr_t emptyString;         // wchar_t*: the text of every empty CFastStringInt
    uintptr_t setString;           // void CFastStringInt::SetString(const SStringParamInt&)
    uintptr_t fxVTables[2];        // ??_7CSceneFxBloom@@6B@, ??_7CSceneFxColors@@6B@
    uintptr_t fxCanActive[2];      // their CanActive(CHmsCamera*) (vtable slot 32)
    uintptr_t systemConfig;        // CSystemConfig* CSystemConfig::s_SystemConfig
    uintptr_t clipGroupStop;       // void CGameCtnMediaClipViewer::ClipGroupStop()
    uintptr_t raceClipStop;        // void CGameRace::MediaClipStop()
    uintptr_t interpolatedLocation; // const GmIso4* CSceneMobil::GetInterpolatedLocation(CSceneSector*) const (vtable slot 32)
};

// Same in both builds: CGameCtnApp holds the current map (a reference), the map its comments.
constexpr size_t kAppChallenge = 0x198;      // CGameCtnChallenge* (CGameCtnApp::GetChallenge)
constexpr size_t kChallengeComments = 0x19c; // CFastStringInt (SHeaderThumbnail::FillHeaderUserData)

constexpr GameBuild kBuilds[] = {
    {"Nations Forever", 0x00bd10bc,
     {0x009a0990, 0x0099d750, 0x0099fe80, 0x009a2390, 0x009a4070, 0x0095ab40, 0x0095d530, 0x0095acc0},
     {0x00693e20, 0x00673e50, 0x006f4510, 0x0054e100, 0x004bedd0, 0x0047c0d0, 0x00991e30, 0x005a4f60, 0x005f6ed0, 0x00690d10, 0x0053fc80, 0x00971b10, 0x0096fd20, 0x00959260, 0x008fb190, 0x005e1d00, 0x007158e0, 0x00673cd0, 0x00574170, 0x005dee70},
     {0, 0x00aaf4e8, 0, 0, 0, 0, 0, 0x00a9cc20, 0x00aa3da8, 0, 0x00d731e0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0x00d123ba, 0x00935400, 0x0041be40, 0x0042b590, 0x00bbf7dc,
     0x00903280, {0x00ba90e4, 0x00ba1cd4}, {0x00821c10, 0x007ea7d0}, 0x00d54380, 0x006736d0, 0x005de270, 0x007b38f0},
    {"United Forever", 0x00bd109c,
     {0x009a0710, 0x0099d4d0, 0x0099fc00, 0x009a2110, 0x009a3df0, 0x0095aad0, 0x0095d440, 0x0095ac50},
     {0x00693ff0, 0x00673f70, 0x006f44e0, 0x0054e030, 0x004bea30, 0x0047bed0, 0x00991bd0, 0x005a5100, 0x005f7010, 0x00690ee0, 0x0053fab0, 0x009719e0, 0x0096fbf0, 0x009591f0, 0x008fb9c0, 0x005e1da0, 0x00715780, 0x00673df0, 0x005741f0, 0x005deef0},
     {0, 0x00aaeee8, 0, 0, 0, 0, 0, 0x00a9c620, 0x00aa37a8, 0, 0x00d74840, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0x00d1442a, 0x00935290, 0x0041be50, 0x0042b680, 0x00bbf7bc,
     0x00903a90, {0x00ba90c4, 0x00ba1cb4}, {0x00821ab0, 0x007ea3b0}, 0x00d559e0, 0x006737f0, 0x005de340, 0x007b3500},
};
const GameBuild* g_build = nullptr;

struct Slot {
    int index;
    SlotId id;
    const char* name;
};

constexpr Slot kRenderFrameBegin = {93, kFrameBegin, "RenderFrameBegin"};
constexpr Slot kRenderCameraBegin = {95, kCameraBegin, "RenderCameraBegin"};
constexpr Slot kRenderCameraEnd = {100, kCameraEnd, "RenderCameraEnd"};
constexpr Slot kRenderOverlayZones = {101, kOverlayZones, "RenderOverlayZones"};
constexpr Slot kRenderFrameEnd = {102, kFrameEnd, "RenderFrameEnd"};
constexpr Slot kComputeDriverViewMatrix = {103, kViewMatrix, "ComputeDriverViewMatrix"};
constexpr Slot kComputeDriverProjection = {104, kProjection, "ComputeDriverProjection"};
constexpr Slot kRenderShadowsSet = {76, kShadowsSet, "RenderShadowsSet"};

// Non-virtual functions, hooked inline. The prologue bytes are checked before patching and
// must be whole instructions that still work when copied (they run again in the trampoline).
// handlerAt: offset of the build's handler address in the prologue (it moves with the
// module), or -1.
struct InlineSite {
    SiteId id;
    unsigned char prologue[12];
    size_t length;
    int handlerAt;
    const char* name;
};
constexpr InlineSite kClipTracksUpdate = {kClipTracks, {0xd9, 0xe8, 0x83, 0xec, 0x08}, 5, -1, "CGameCtnMediaClipPlayer::TracksUpdate"};
constexpr InlineSite kClipViewerCams = {kClipCams, {0x6a, 0xff, 0x68, 0, 0, 0, 0}, 7, 3, "CGameCtnMediaClipViewer::UpdateCams"};
constexpr InlineSite kVideoShootSite = {kVideoShoot, {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8}, 6, -1, "CGameCtnMediaVideoShooter::DoShoot"};
// Map load (CGameCtnApp::ChallengeCreateSceneGraph): the zone gets the sea level of the
// environment's decoration, -1 when it has none.
constexpr InlineSite kWaterTileHeight = {kTileHeight, {0x55, 0x8b, 0xe9, 0xd9, 0x85, 0x08, 0x01, 0x00, 0x00}, 9, -1, "CHmsZone::WaterRenderTileHeightSet"};
// Race (re)start: the race HUD is reset. Respawn: back to the last checkpoint (or the start).
constexpr InlineSite kRaceResetSite = {kRaceReset, {0x56, 0x8b, 0xf1, 0x83, 0xbe, 0xa4, 0x01, 0x00, 0x00, 0x00}, 10, -1, "CTrackManiaRaceInterface::RaceOnReset"};
constexpr InlineSite kRespawnSite = {kRespawn, {0x53, 0x8b, 0x5c, 0x24, 0x08}, 5, -1, "CTrackManiaRace::RespawnPlayer"};
// Water blocks (Stadium pools and rivers) are terrain blocks: their surface is always here,
// 1.00 m below the ground (measured from the depth of four captures, out to 400 m: water
// 8.000, ground 9.001). The 7.94 measured before came from rays tilted by half a pixel.
constexpr float kBlockWaterY = 8.0f;
// Renders the reflection of a water plane; gets the plane equation (world space).
constexpr InlineSite kWaterPlaneSite = {kWaterPlane, {0x81, 0xec, 0xe0, 0x01, 0x00, 0x00}, 6, -1, "CVisionViewportDx9::TexRender_Water_PlaneR"};
// Map load: the map's decoration and environment (TMUF has seven; water blocks are Stadium's).
constexpr InlineSite kLoadDecorationSite = {kLoadDecoration, {0x6a, 0xff, 0x68, 0, 0, 0, 0}, 7, 3, "CGameCtnChallenge::LoadDecorationAndCollection"};
// The game sets its current map (race, replay, editor): we keep the app to find that map.
constexpr InlineSite kSetChallengeSite = {kSetChallenge, {0x6a, 0xff, 0x68, 0, 0, 0, 0}, 7, 3, "CGameCtnApp::SetChallenge"};
// The free camera (cam 7 in replays, the editor's free camera) is switched on or off. Its
// state is the int at +0x5c (the function returns at once when it doesn't change).
constexpr InlineSite kFreeCamSite = {kFreeCam, {0x8b, 0x44, 0x24, 0x04, 0x3b, 0x41, 0x5c}, 7, -1, "CGameControlCameraFree::SetIsActive"};
constexpr size_t kFreeCamActive = 0x5c;
// The camera's adaptive view distance ("ZClip" in the launcher): with +0x194 set it moves the
// far plane (+0x178) with the frame rate and +0x174 says it cuts the view. The prologue reads
// CMwCmdBufferCore::TheCoreCmdBuffer (an absolute address, at +1).
constexpr InlineSite kZClipSite = {kZClip, {0xa1, 0, 0, 0, 0, 0x8b, 0x80, 0xc0, 0x00, 0x00, 0x00}, 11, 1, "CHmsCamera::ZClipCompute"};
constexpr size_t kZClipEnabled = 0x194;
// Every draw of the 3D scene goes through one of these two, with the CPlugShader (material)
// it draws: CDx9ShaderKeeper::UpdateAndRender(shader, corpus, tree, visual) for the direct
// path, ShadeVisualForCurrentPass(SDelayedRender&) (shader at +0x10) for the sorted passes.
constexpr InlineSite kDrawShaderSite = {kDrawShader, {0x81, 0xec, 0x88, 0x01, 0x00, 0x00}, 6, -1, "CDx9ShaderKeeper::UpdateAndRender"};
constexpr InlineSite kDrawPassSite = {kDrawPass, {0x81, 0xec, 0xc8, 0x00, 0x00, 0x00}, 6, -1, "CDx9ShaderKeeper::ShadeVisualForCurrentPass"};
constexpr size_t kDelayedRenderShader = 0x10;
// Lens flares of the scene's lights, the sun's halo among them (drawn over everything in front
// of the sun): LensFlareAdd(light, position, size, testVisible, ...). A light whose GxLight
// (light +0x48 -> +0x88) answers 1 to its type (vtable +0x78) is the directional one, the sun.
// The game reads its keyboard (and pads, mouse) through DirectInput: GetDeviceState into a
// buffer, 256 bytes for the keyboard (one per DIK_ key). Not through window messages, so a
// key the menu used (Esc) has to be taken out here, or the game pauses as well.
constexpr InlineSite kReadInputSite = {kReadInput, {0x57, 0x8b, 0x7c, 0x24, 0x08}, 5, -1, "CInputPortDx8::DeviceReadHardwareCurState"};
// The game's own switch for the race / replay interface (HUD): the playground interface
// shows or hides its 2D scene (CScene2d::SetVisibleInterface). Called with the visibility the
// game wants; the photo mode answers 0 there.
constexpr InlineSite kInterfaceSetSite = {kInterfaceSet, {0x8b, 0x44, 0x24, 0x04, 0x3b, 0x41, 0x20}, 7, -1, "CGamePlaygroundInterface::InterfaceVisibleInternalSet"};
constexpr InlineSite kLensFlareSite = {kLensFlare, {0x83, 0xec, 0x48, 0x53, 0x55}, 5, -1, "CVisionViewport::LensFlareAdd"};
// Map load (also from CGameCtnDecoration::Init): the decoration jumps its day-time animation
// (sun or moon, ambient, fog, sky) to its mood's start time. A day map's lightmap holds only
// masks (sky occlusion, sun shadow); the sun's colour comes from that animation at run time.
// So with the time at midnight the game lights the map with its own night (moon), nothing is
// baked white. The map, its UID and replays are untouched: the mood's time is restored at once.
constexpr InlineSite kLoadSceneSite = {kLoadScene, {0x83, 0xec, 0x18, 0x53, 0x56, 0x8b, 0xf1, 0x8b, 0x46, 0x58}, 10, -1, "CGameCtnDecoration::LoadScene3d"};
// Every frame while driving: when the car enters a cell of the map's in-game clip triggers
// (MediaTracker: forced cameras, black screens, texts) it starts that clip. Skipped, no
// trigger clip starts; the map itself is untouched (intro and end-race clips, replays stay).
// The index of the running trigger clip is at +0x50 (-1 none).
constexpr InlineSite kClipTriggersSite = {kClipTriggers, {0x83, 0xec, 0x1c, 0x56, 0x8b, 0xf1, 0x83, 0x7e, 0x44, 0x00}, 10, -1, "CGameCtnMediaClipViewer::ClipGroupCheckTriggers"};
constexpr size_t kClipViewerTriggered = 0x50;
// The same while driving (the viewer above plays them in replays): CGameRace checks the
// player's position against the triggers every frame and starts the clip. The player keeps
// the index of the last triggered clip at +0x48 (-1 none).
constexpr InlineSite kRaceTriggersSite = {kRaceTriggers, {0x83, 0xec, 0x18, 0x53, 0x55, 0x8b, 0x6c, 0x24, 0x28}, 9, -1, "CGameRace::MediaClipCheckInGameTriggers"};
constexpr size_t kPlayerTriggered = 0x48;
// The player's car (the same function reads it there): CGamePlayer +0x28 is its CGameMobil
// (CGamePlayer::SetMobil), that one's +0x14 the CSceneMobil (the vehicle), +0x1c -1 while it
// is not in the scene. The vehicle's vtable slot 32 is GetInterpolatedLocation(sector): with
// no sector, the location of its first corpus, the one it is drawn with (between physics steps).
constexpr size_t kPlayerMobil = 0x28;
constexpr size_t kGameMobilScene = 0x14;
constexpr size_t kGameMobilSceneId = 0x1c;
constexpr int kInterpolatedLocationSlot = 32;
// Every jump of a weather animation to a time of day (LoadScene3d at map load, and a time
// set as a parameter later, e.g. when the race starts): 0 = midnight. Real night forces 0
// here too, or the later jump puts the map's own time back.
constexpr InlineSite kJumpTimeSite = {kJumpTime, {0x51, 0x56, 0x8b, 0xf1, 0xd9, 0x46, 0x28}, 7, -1, "CMotionManagerWeathers::JumpToTimeRemapped"};
constexpr size_t kDecorationMood = 0x60; // CGameCtnDecorationMood*
constexpr size_t kMoodDayTime = 0x28;    // RemappedStartDayTime: 0..1 of a day, night outside (0.25, 0.75]
constexpr size_t kZClipCuts = 0x174;

using FrameBeginFn = int(__fastcall*)(void*, void*);
using CameraBeginFn = void(__fastcall*)(void*, void*, void*, void*);
using CameraEndFn = void(__fastcall*)(void*, void*, void*);
using OverlayZonesFn = void(__fastcall*)(void*, void*, void*, int);
using FrameEndFn = void(__fastcall*)(void*, void*);
using ViewMatrixFn = void(__fastcall*)(void*, void*, void*);
using ProjectionFn = void(__fastcall*)(void*, void*, void*, const void*, void*);
using ShadowsSetFn = void(__fastcall*)(void*, void*, int);

FrameBeginFn g_frameBegin = nullptr;
CameraBeginFn g_cameraBegin = nullptr;
CameraEndFn g_cameraEnd = nullptr;
OverlayZonesFn g_overlayZones = nullptr;
FrameEndFn g_frameEnd = nullptr;
ViewMatrixFn g_viewMatrix = nullptr;
ProjectionFn g_projection = nullptr;
ShadowsSetFn g_shadowsSet = nullptr;
int g_forcedShadows = -1;

Callbacks g_callbacks;
CameraInfo g_camera;
bool g_active = false;

using TracksUpdateFn = void(__fastcall*)(void*, void*, float, float);
using UpdateCamsFn = void(__fastcall*)(void*, void*);
using DoShootFn = void(__fastcall*)(void*, void*, void*);
using WaterPlaneFn = void(__fastcall*)(void*, void*, void*, void*, const float*, unsigned long, int);
WaterPlaneFn g_waterPlane = nullptr;
float g_waterPlaneEq[4] = {};
DWORD g_lastWater = 0;          // GetTickCount of the last water reflection render
using TileHeightFn = void(__fastcall*)(void*, void*, float);
using RaceResetFn = void(__fastcall*)(void*, void*);
using RespawnFn = void(__fastcall*)(void*, void*, void*, int);
RaceResetFn g_raceReset = nullptr;
using LoadDecorationFn = void(__fastcall*)(void*, void*, const void*);
using IdGetStringFn = const char*(__fastcall*)(const void*, void*);
LoadDecorationFn g_loadDecoration = nullptr;
char g_environment[32] = {}; // "Stadium", "Speed", ... ("" = not known yet)
std::string g_mapComments;      // of the last map loaded
volatile LONG g_mapLoads = 0;
volatile LONG g_mapCloses = 0;  // the app let go of its map (back to the menu)
using SetChallengeFn = void(__fastcall*)(void*, void*, void*);
SetChallengeFn g_setChallenge = nullptr;
void* g_app = nullptr;          // CGameCtnApp (holds the current map)
RespawnFn g_respawn = nullptr;
volatile LONG g_raceResets = 0; // counters: the plugin compares them every frame
volatile LONG g_respawns = 0;
TileHeightFn g_tileHeight = nullptr;
bool g_mapLoaded = false;       // a map load was seen, so its sea level is known
float g_seaLevel = -1.0f;       // -1 = no sea
TracksUpdateFn g_tracksUpdate = nullptr;
UpdateCamsFn g_updateCams = nullptr;
DoShootFn g_doShoot = nullptr;
float g_jitter[2] = {};          // NDC offset for perspective projections (TAA)
float g_tile[4] = {1.0f, 1.0f, 0.0f, 0.0f}; // scale x, y and NDC offset x, y (hi-res photos)
float g_zoom = 1.0f;             // photo mode zoom (>= 1: the engine culls with its own, wider view)
// The camera of the shaded view: zoom and tiles only change its projection, not those of
// the reflection cameras (water planes, the cube maps of the car, which need their 90 degrees).
const void* g_mainCamera = nullptr;
bool g_loggedMainProjection = false;
bool g_hideOverlay = false;      // skip the game's HUD and menus (photo mode)
using FreeCamFn = void(__fastcall*)(void*, void*, int);
FreeCamFn g_freeCamSet = nullptr;
using ZClipFn = void(__fastcall*)(void*, void*, float, void*, void*);
ZClipFn g_zclip = nullptr;
bool g_fullViewDistance = false; // no adaptive view distance (photo mode)
using DrawShaderFn = void(__fastcall*)(void*, void*, void*, void*, void*, void*);
using DrawPassFn = void(__fastcall*)(void*, void*, void*);
DrawShaderFn g_drawShader = nullptr;
DrawPassFn g_drawPass = nullptr;
const void* g_currentShader = nullptr; // the material being drawn (null between draws)
bool g_shaderCensus = false;           // log every material once (dev: finding the lamp, glass, ... materials)
using LensFlareFn = void(__fastcall*)(void*, void*, void*, const void*, float, int, float);
LensFlareFn g_lensFlare = nullptr;
bool g_hideSunFlare = false;           // a night sky replaces the sun: its halo fades out
volatile bool g_hideGameSky = false;   // a night sky replaces the game's: its painted horizon isn't drawn
using LoadSceneFn = void(__fastcall*)(void*, void*);
LoadSceneFn g_loadScene = nullptr;
bool (*g_wantNight)(float dayTime) = nullptr; // the plugin: light this map at midnight?
using JumpTimeFn = void(__fastcall*)(void*, void*, float);
JumpTimeFn g_jumpTime = nullptr;
int g_jumpsLogged = 0;
using ClipTriggersFn = void(__fastcall*)(void*, void*);
ClipTriggersFn g_clipTriggers = nullptr;
using RaceTriggersFn = void(__fastcall*)(void*, void*, void*, void*);
RaceTriggersFn g_raceTriggers = nullptr;
void* g_racePlayer = nullptr;                  // the player the race checked last (its car)
DWORD g_racePlayerTick = 0;                    // when (a player of a race that ended is gone)
float g_mapDayTime = -1.0f;                   // the loaded map's own time of day (-1 unknown)
bool g_nightForced = false;                   // the game lights it at midnight instead
void* g_weather = nullptr;                    // this map's weather (the last to jump in time)
float g_weatherTime = -1.0f;                  // the time of day the game asked it for last
using ReadInputFn = int(__cdecl*)(void*, void*, unsigned long);
ReadInputFn g_readInput = nullptr;
volatile bool g_swallowEscape = false; // the menu or photo mode took this Esc press
using InterfaceSetFn = void(__fastcall*)(void*, void*, int);
InterfaceSetFn g_interfaceSet = nullptr;
void* g_interface = nullptr;     // the playground interface that last set its visibility
int g_interfaceWanted = 1;       // what the game wants for it
LONG g_interfaceLoads = -1;      // the map it belongs to (gone with the next one)
void* g_freeCam = nullptr;       // the free camera last switched on (null: off)
// The game's FX post-processing: CSceneFxNod::CanActive asks each effect through this slot
// whether it runs at all. Bloom and the colour filter answer with the launcher's
// "PostFxEnable" (display config +0x68).
constexpr int kFxCanActiveSlot = 32;
using FxCanActiveFn = int(__fastcall*)(void*, void*, void*);
FxCanActiveFn g_fxCanActive[2] = {};
bool g_skipGameFx = false;
DWORD g_lastCinematic = 0;      // GetTickCount of the last cinematic hook call
unsigned g_cinematicSources = 0;

uintptr_t rebase(uintptr_t linkAddress) {
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) - kPreferredBase + linkAddress;
}

bool readable(const void* p, size_t size) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return reinterpret_cast<const BYTE*>(p) + size <= reinterpret_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
}

void** vtable(const GameBuild& build) {
    return reinterpret_cast<void**>(rebase(build.viewportVTable));
}

bool verify(const GameBuild& build, const Slot& slot) {
    void** table = vtable(build);
    if (!readable(&table[slot.index], sizeof(void*))) return false;
    return reinterpret_cast<uintptr_t>(table[slot.index]) == rebase(build.slots[slot.id]);
}

template <typename Fn>
void patch(const Slot& slot, void* detour, Fn& original) {
    void** entry = &vtable(*g_build)[slot.index];
    original = reinterpret_cast<Fn>(*entry);
    DWORD old = 0;
    VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old);
    *entry = detour;
    VirtualProtect(entry, sizeof(void*), old, &old);
}

// Jumps from the function's start to the detour; the trampoline runs the copied prologue
// and jumps back behind it. Returns the trampoline (the "original" to call), or null.
void* inlineHook(const InlineSite& site, void* detour) {
    BYTE* target = reinterpret_cast<BYTE*>(rebase(g_build->sites[site.id]));
    unsigned char expected[12];
    memcpy(expected, site.prologue, sizeof(expected));
    if (site.handlerAt >= 0) {
        const uint32_t handler = static_cast<uint32_t>(rebase(g_build->handlers[site.id]));
        memcpy(expected + site.handlerAt, &handler, sizeof(handler));
    }
    if (!readable(target, site.length) || memcmp(target, expected, site.length) != 0) {
        TMVS_LOG("engine: %s does not match this executable, not hooked", site.name);
        return nullptr;
    }
    BYTE* trampoline = static_cast<BYTE*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return nullptr;
    memcpy(trampoline, target, site.length);
    trampoline[site.length] = 0xE9;
    *reinterpret_cast<int32_t*>(trampoline + site.length + 1) =
        static_cast<int32_t>((target + site.length) - (trampoline + site.length + 5));
    DWORD old = 0;
    VirtualProtect(target, site.length, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) = static_cast<int32_t>(static_cast<BYTE*>(detour) - (target + 5));
    for (size_t i = 5; i < site.length; i++) target[i] = 0x90; // nop out the rest
    VirtualProtect(target, site.length, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, site.length);
    return trampoline;
}

void markCinematic(unsigned source) {
    g_lastCinematic = GetTickCount();
    g_cinematicSources |= source;
}

void __fastcall tracksUpdateDetour(void* self, void* edx, float time, float delta) {
    markCinematic(1);
    g_tracksUpdate(self, edx, time, delta);
}

void __fastcall updateCamsDetour(void* self, void* edx) {
    markCinematic(2);
    g_updateCams(self, edx);
}

// Runs once per frame while the export shoots (a fiber that is resumed every frame).
void __fastcall doShootDetour(void* self, void* edx, void* fiber) {
    markCinematic(4);
    g_doShoot(self, edx, fiber);
}

// The game draws the reflection of each visible water plane: that plane is where its water is.
void __fastcall waterPlaneDetour(void* self, void* edx, void* bitmap, void* render, const float* plane, unsigned long flags, int mode) {
    if (plane && readable(plane, sizeof(float) * 4)) {
        memcpy(g_waterPlaneEq, plane, sizeof(g_waterPlaneEq));
        g_lastWater = GetTickCount();
    }
    g_waterPlane(self, edx, bitmap, render, plane, flags, mode);
}

// Water height from a plane (a x + b y + c z + d = 0); false unless it is level.
bool levelPlaneHeight(const float* p, float& y) {
    const float len = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (len < 1e-6f || fabsf(p[1]) / len < 0.99f) return false;
    y = -p[3] / p[1];
    return y > -5000.0f && y < 5000.0f;
}

void __fastcall raceResetDetour(void* self, void* edx) {
    InterlockedIncrement(&g_raceResets);
    g_raceReset(self, edx);
}

void __fastcall freeCamDetour(void* self, void* edx, int active) {
    if (active) g_freeCam = self;
    else if (self == g_freeCam) g_freeCam = nullptr;
    g_freeCamSet(self, edx, active);
}

int __fastcall bloomCanActiveDetour(void* self, void* edx, void* camera) {
    return g_skipGameFx ? 0 : g_fxCanActive[0](self, edx, camera);
}

int __fastcall colorsCanActiveDetour(void* self, void* edx, void* camera) {
    return g_skipGameFx ? 0 : g_fxCanActive[1](self, edx, camera);
}

// Replaces one virtual function of an FX class, after checking the slot holds the function
// the map names (another build or a patched game: left alone).
void patchFx(int which, void* detour) {
    void** entry = reinterpret_cast<void**>(rebase(g_build->fxVTables[which])) + kFxCanActiveSlot;
    if (!readable(entry, sizeof(void*)) || reinterpret_cast<uintptr_t>(*entry) != rebase(g_build->fxCanActive[which])) {
        TMVS_LOG("engine: FX vtable %d does not match this executable, not hooked", which);
        return;
    }
    g_fxCanActive[which] = reinterpret_cast<FxCanActiveFn>(*entry);
    DWORD old = 0;
    VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old);
    *entry = detour;
    VirtualProtect(entry, sizeof(void*), old, &old);
}

// Full view distance: the adaptive cut is skipped for this camera and frame, and comes back
// on its own afterwards (its state is untouched).
void __fastcall zclipDetour(void* self, void* edx, float speed, void* frustum, void* rect) {
    if (!g_fullViewDistance) {
        g_zclip(self, edx, speed, frustum, rect);
        return;
    }
    int* enabled = reinterpret_cast<int*>(static_cast<BYTE*>(self) + kZClipEnabled);
    int* cuts = reinterpret_cast<int*>(static_cast<BYTE*>(self) + kZClipCuts);
    const int wasEnabled = *enabled, wasCutting = *cuts;
    *enabled = 0;
    *cuts = 0;
    g_zclip(self, edx, speed, frustum, rect);
    *enabled = wasEnabled;
    *cuts = wasCutting;
}

bool nodFilePath(const void* nod, wchar_t* out, size_t size);

// What a game material is, from its shader file (one look per shader and map: the engine
// reuses the memory of freed shaders).
//   TSelfI ...                       self-illuminated: lamps, light panels, screens (not the additive
//                                    TAdd glows: they are drawn over whole blocks)
//   Vehicles\...RearLights           the cars' rear lights (lights)
//   Vehicles\Particle..., WheelMarks smoke, chunks, tyre marks: not the car
//   Vehicles\...Glass               car windows
//   Vehicles\...                    the cars
//   Grass ...                        grass
//   ...Sea... / ...Water...          the sea and other engine water (not in the menus)
Material classify(const wchar_t* path) {
    const wchar_t* file = wcsrchr(path, L'\\');
    file = file ? file + 1 : path;
    if (wcsstr(path, L"\\Menu")) return Material::Other; // the menu's planet and sea
    if (!_wcsnicmp(file, L"TSelfI", 6)) return Material::Light;
    if (wcsstr(path, L"Vehicles\\") || wcsstr(path, L"Vehicles/")) {
        if (wcsstr(file, L"Light")) return Material::Light;
        if (!_wcsnicmp(file, L"Particle", 8) || !_wcsnicmp(file, L"WheelMarks", 10)) return Material::Other;
        return wcsstr(file, L"Glass") ? Material::Glass : Material::Car;
    }
    if (!_wcsnicmp(file, L"Grass", 5)) return Material::Grass;
    const wchar_t* sea = wcsstr(file, L"Sea");
    if ((sea && sea[3] != L't') || wcsstr(file, L"Water")) return Material::Water; // not "Seat..."
    return Material::Other;
}

Material materialOf(const void* shader) {
    static std::unordered_map<const void*, Material> known;
    static LONG knownLoads = -1;
    if (!shader) return Material::Other;
    if (knownLoads != g_mapLoads) {
        known.clear();
        knownLoads = g_mapLoads;
    }
    const auto it = known.find(shader);
    if (it != known.end()) return it->second;
    wchar_t path[512] = {};
    const bool named = nodFilePath(shader, path, 512);
    const Material m = named ? classify(path) : Material::Other;
    if (known.size() < 8192) known.emplace(shader, m);
    return m;
}

// The environment's painted horizon: TMUF Island's IslandSkyPC1 is a ring around the map with
// the sunset painted on (it writes depth, so the mod's sky can't replace it; the sea mirrors
// it). Not Sky\SkyDay (the generic dome, at the sky's depth anyway).
bool isPaintedHorizon(const void* shader) {
    static std::unordered_map<const void*, bool> known;
    static LONG knownLoads = -1;
    if (!shader) return false;
    if (knownLoads != g_mapLoads) {
        known.clear();
        knownLoads = g_mapLoads;
    }
    const auto it = known.find(shader);
    if (it != known.end()) return it->second;
    wchar_t path[512] = {};
    bool horizon = false;
    if (nodFilePath(shader, path, 512)) {
        const wchar_t* file = wcsrchr(path, L'\\');
        file = file ? file + 1 : path;
        horizon = wcsstr(file, L"SkyPC") && !wcsstr(path, L"\\Sky\\") && !wcsstr(path, L"\\Menu");
        if (horizon) TMVS_LOG("engine: %ls is the painted horizon (left out under night skies)", path);
    }
    if (known.size() < 8192) known.emplace(shader, horizon);
    return horizon;
}

void censusShader(const void* shader) {
    static std::unordered_set<const void*> seen;
    if (!shader || seen.size() >= 4096 || !seen.insert(shader).second) return;
    wchar_t path[512] = {};
    if (nodFilePath(shader, path, 512)) TMVS_LOG("shader %p: %ls", shader, path);
    else TMVS_LOG("shader %p: (no file)", shader);
}

void __fastcall drawShaderDetour(void* self, void* edx, void* shader, void* corpus, void* tree, void* visual) {
    const void* previous = g_currentShader;
    g_currentShader = shader;
    if (g_shaderCensus) censusShader(shader);
    if (!(g_hideGameSky && isPaintedHorizon(shader))) g_drawShader(self, edx, shader, corpus, tree, visual);
    g_currentShader = previous;
}

void __fastcall drawPassDetour(void* self, void* edx, void* delayed) {
    const void* previous = g_currentShader;
    g_currentShader = *reinterpret_cast<void* const*>(static_cast<const BYTE*>(delayed) + kDelayedRenderShader);
    if (g_shaderCensus) censusShader(g_currentShader);
    if (!(g_hideGameSky && isPaintedHorizon(g_currentShader))) g_drawPass(self, edx, delayed);
    g_currentShader = previous;
}

bool isSunLight(const void* corpusLight) {
    __try {
        const BYTE* item = *reinterpret_cast<const BYTE* const*>(static_cast<const BYTE*>(corpusLight) + 0x48);
        if (!item) return false;
        void* light = *reinterpret_cast<void* const*>(item + 0x88);
        if (!light) return false;
        using TypeFn = int(__fastcall*)(void*, void*);
        const TypeFn type = *reinterpret_cast<const TypeFn*>(*reinterpret_cast<const BYTE* const*>(light) + 0x78);
        return type(light, nullptr) == 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The sun's flare is reported as hidden: the engine fades it out as when the sun goes behind
// something (no abrupt cut, and it comes back the same way).
void __fastcall lensFlareDetour(void* self, void* edx, void* light, const void* position, float size, int testVisible, float extra) {
    if (g_hideSunFlare && testVisible && isSunLight(light)) testVisible = 0;
    g_lensFlare(self, edx, light, position, size, testVisible, extra);
}

int __cdecl readInputDetour(void* device, void* state, unsigned long size) {
    const int ok = g_readInput(device, state, size);
    if (g_swallowEscape) {
        if (GetAsyncKeyState(VK_ESCAPE) < 0) {
            if (ok && state && size == 256) static_cast<BYTE*>(state)[0x01] = 0; // DIK_ESCAPE
        } else {
            g_swallowEscape = false; // released: the game may have it again
        }
    }
    return ok;
}

void __fastcall interfaceSetDetour(void* self, void* edx, int visible) {
    g_interface = self;
    g_interfaceWanted = visible;
    g_interfaceLoads = g_mapLoads;
    g_interfaceSet(self, edx, g_hideOverlay ? 0 : visible);
}

// Applies the photo mode's wish at once (the game only calls the setter when its own flags
// change). Only for the interface of the current map: an older one is freed.
void applyInterfaceVisibility() {
    if (!g_interfaceSet || !g_interface || g_interfaceLoads != g_mapLoads) return;
    __try {
        g_interfaceSet(g_interface, nullptr, g_hideOverlay ? 0 : g_interfaceWanted);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_interface = nullptr;
    }
}

void __fastcall respawnDetour(void* self, void* edx, void* player, int flag) {
    InterlockedIncrement(&g_respawns);
    g_respawn(self, edx, player, flag);
}

// The file a loaded map came from, through the engine's file system: the map's file node
// and its path on disk (GetFullName(path, 0, 0), as the game itself does before writing a
// file). Guarded: a wrong address must not take the game down with it.
using GetFidFileFn = void*(__cdecl*)(const void*);
using FidFullNameFn = void(__fastcall*)(const void*, void*, void*, int, int);
struct FastStringInt {
    uint32_t size;
    wchar_t* text; // empty: the engine's shared empty string, never null
};

bool nodFilePath(const void* nod, wchar_t* out, size_t size) {
    __try {
        const void* fid = reinterpret_cast<GetFidFileFn>(rebase(g_build->getFidFile))(nod);
        if (!fid) return false;
        // The path is appended to this string; its buffer stays with the game's allocator
        // (a few bytes per map load).
        FastStringInt name = {0, *reinterpret_cast<wchar_t**>(rebase(g_build->emptyString))};
        reinterpret_cast<FidFullNameFn>(rebase(g_build->fidFullName))(fid, nullptr, &name, 0, 0);
        if (!name.text || name.size == 0 || name.size >= size) return false;
        wcsncpy_s(out, size, name.text, _TRUNCATE);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string toUtf8(const wchar_t* text, int length) {
    if (length <= 0) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text, length, &out[0], n, nullptr, nullptr);
    return out;
}

// The comments the map holds in memory (from its file, or as the editor changed them).
bool commentsInMemory(const void* challenge, wchar_t* out, size_t size) {
    __try {
        const FastStringInt* s = reinterpret_cast<const FastStringInt*>(static_cast<const BYTE*>(challenge) + kChallengeComments);
        if (!s->text || s->size >= size) return false;
        memcpy(out, s->text, s->size * sizeof(wchar_t));
        out[s->size] = L'\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void readMapComments(const void* challenge) {
    static wchar_t text[16384];
    std::string comments;
    const char* source = "none";
    if (commentsInMemory(challenge, text, 16384) && text[0]) {
        comments = toUtf8(text, static_cast<int>(wcslen(text)));
        source = "map";
    } else {
        // Not loaded with the map: the file's header has them.
        wchar_t path[1024] = {};
        if (nodFilePath(challenge, path, 1024) && gbx::readMapComments(path, comments) && !comments.empty()) source = "file";
    }
    TMVS_LOG("engine: map loaded, comments: %s", source);
    g_mapComments = comments;
    InterlockedIncrement(&g_mapLoads);
}

void __fastcall setChallengeDetour(void* self, void* edx, void* challenge) {
    g_app = self;
    // Another map (or none: back to the menu): the objects of the last one go with it.
    g_freeCam = nullptr;
    g_weather = nullptr;
    g_racePlayer = nullptr;
    g_nightForced = false;
    g_mapDayTime = -1.0f;
    g_setChallenge(self, edx, challenge);
    if (!challenge) InterlockedIncrement(&g_mapCloses);
}

void* currentChallenge() {
    if (!g_app) return nullptr;
    void* const* slot = reinterpret_cast<void* const*>(static_cast<const BYTE*>(g_app) + kAppChallenge);
    return readable(slot, sizeof(void*)) ? *slot : nullptr;
}

using SetStringFn = void(__fastcall*)(void*, void*, const void*);
struct StringParamInt {
    const wchar_t* text;
    uint32_t size;
    int checked;
};

bool writeComments(void* challenge, const wchar_t* text, uint32_t length) {
    __try {
        const StringParamInt param = {text, length, 0};
        reinterpret_cast<SetStringFn>(rebase(g_build->setString))(static_cast<BYTE*>(challenge) + kChallengeComments, nullptr, &param);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SGameCtnIdentifier of the decoration at +0xc4: id, collection (the environment), author.
void __fastcall loadDecorationDetour(void* self, void* edx, const void* forcedMods) {
    g_loadDecoration(self, edx, forcedMods);
    readMapComments(self);
    const void* collection = static_cast<const BYTE*>(self) + 0xc8;
    if (!readable(collection, sizeof(uint32_t))) return;
    const char* name = reinterpret_cast<IdGetStringFn>(rebase(g_build->idGetString))(collection, nullptr);
    if (!name || !readable(name, 1)) return;
    if (strncmp(name, g_environment, sizeof(g_environment)) != 0) TMVS_LOG("engine: environment %s", name);
    strncpy_s(g_environment, name, _TRUNCATE);
}

void __fastcall jumpTimeDetour(void* self, void* edx, float time) {
    g_weather = self;
    g_weatherTime = time;
    const float wanted = g_nightForced ? 0.0f : time;
    if (g_jumpsLogged < 40) {
        g_jumpsLogged++;
        TMVS_LOG("engine: weather %p jumps to time of day %.3f%s", self, time, g_nightForced ? " -> 0 (real night)" : "");
    }
    g_jumpTime(self, edx, wanted);
}

void __fastcall raceTriggersDetour(void* self, void* edx, void* player, void* group) {
    // Called every frame of a race with the local (or watched) player: the neon trail's car.
    g_racePlayer = player;
    g_racePlayerTick = GetTickCount();
    g_raceTriggers(self, edx, player, group);
}

void __fastcall clipTriggersDetour(void* self, void* edx) {
    g_clipTriggers(self, edx);
}

void __fastcall loadSceneDetour(void* self, void* edx) {
    const BYTE* decoration = static_cast<const BYTE*>(self);
    void* mood = readable(decoration + kDecorationMood, sizeof(void*)) ? *reinterpret_cast<void* const*>(decoration + kDecorationMood) : nullptr;
    float* time = mood && readable(static_cast<BYTE*>(mood) + kMoodDayTime, sizeof(float)) ? reinterpret_cast<float*>(static_cast<BYTE*>(mood) + kMoodDayTime) : nullptr;
    if (!time || !(*time >= 0.0f && *time <= 1.0f)) {
        // Nothing of the last map carries over (its forced midnight, its time of day).
        g_weather = nullptr;
        g_nightForced = false;
        g_mapDayTime = -1.0f;
        g_loadScene(self, edx);
        return;
    }
    const float original = *time;
    g_mapDayTime = original;
    g_weather = nullptr; // the last map's (set again by the jumps of this load)
    g_nightForced = g_wantNight && g_wantNight(original);
    TMVS_LOG("engine: map time of day %.3f%s", original, g_nightForced ? ", lit at midnight (night sky)" : "");
    if (g_nightForced) *time = 0.0f;
    g_loadScene(self, edx);
    *time = original;
    g_weatherTime = original; // the load's jumps asked for the midnight forced above
}

void __fastcall tileHeightDetour(void* self, void* edx, float height) {
    InterlockedIncrement(&g_raceResets); // a new map
    g_tileHeight(self, edx, height);
    g_mapLoaded = true;
    g_seaLevel = height;
    if (height != -1.0f) TMVS_LOG("engine: sea level %.2f", height);
    else TMVS_LOG("engine: no sea on this map");
    hook::logMemory("at map load");
}

int g_loggedShadowMode = -2;

// The game's display settings from the launcher (CSystemConfigDisplay, the windowed or the
// fullscreen one), once: what the game itself spends its frame on.
void logDisplayConfig() {
    static bool logged = false;
    if (logged) return;
    logged = true;
    __try {
        const BYTE* config = *reinterpret_cast<const BYTE* const*>(rebase(g_build->systemConfig));
        if (!config) return;
        const BYTE* display = *reinterpret_cast<const BYTE* const*>(config + (*reinterpret_cast<const int*>(config + 0x20) ? 0x28 : 0x24));
        if (!display) return;
        const auto at = [display](size_t offset) { return *reinterpret_cast<const int*>(display + offset); };
        // Offsets from the class's parameter table (SMwParamInfos of CSystemConfigDisplay).
        TMVS_LOG("engine: game display config: ShaderQuality %d, Shadows %d, PostFxEnable %d (force bloom %d, colors %d, motion blur %d), "
                 "VertexProcess %d, ZClip %d (auto %d), GpuSync %d",
                 at(0x4c), at(0x54), at(0x68), at(0x74), at(0x6c), at(0x70), at(0x88), at(0x78), at(0x7c), at(0x8c));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        TMVS_LOG("engine: game display config not readable");
    }
}

int __fastcall frameBeginDetour(void* self, void* edx) {
    // CHmsViewport::m_Shadows lives at +0x248 (see CHmsViewport::RenderShadowsSet).
    int mode = *reinterpret_cast<const int*>(static_cast<const BYTE*>(self) + 0x248);
    if (g_forcedShadows >= 0 && mode != g_forcedShadows) {
        g_shadowsSet(self, edx, g_forcedShadows);
        mode = g_forcedShadows;
    }
    if (mode != g_loggedShadowMode) {
        g_loggedShadowMode = mode;
        TMVS_LOG("engine: shadow mode %d, shader level %u", mode,
                 static_cast<unsigned>(*reinterpret_cast<const unsigned short*>(rebase(g_build->shaderLevel))));
    }
    logDisplayConfig();
    if (g_callbacks.frameBegin) g_callbacks.frameBegin();
    return g_frameBegin(self, edx);
}

void __fastcall cameraBeginDetour(void* self, void* edx, void* camera, void* clearDesc) {
    g_camera.camera = camera;
    g_camera.hasView = false;
    g_camera.hasProjection = false;
    if (g_callbacks.cameraBegin) g_callbacks.cameraBegin(camera);
    g_cameraBegin(self, edx, camera, clearDesc);
}

void __fastcall cameraEndDetour(void* self, void* edx, void* camera) {
    g_cameraEnd(self, edx, camera);
    if (g_callbacks.cameraEnd) g_callbacks.cameraEnd(camera, g_camera);
}

void __fastcall overlayZonesDetour(void* self, void* edx, void* zones, int flags) {
    if (g_callbacks.overlayBegin) g_callbacks.overlayBegin();
    // Always drawn: in some setups (camera effects, the editors) this pass also brings the 3D
    // view to the screen. The photo mode hides only the interface (applyInterfaceVisibility).
    g_overlayZones(self, edx, zones, flags);
    if (g_callbacks.overlayEnd) g_callbacks.overlayEnd();
}

void __fastcall frameEndDetour(void* self, void* edx) {
    if (g_callbacks.frameEnd) g_callbacks.frameEnd();
    g_frameEnd(self, edx);
}

// SHmsCameraLocation: +0x00 GmIso4 camera location, +0x60 GmMat4 driver view matrix.
void __fastcall viewMatrixDetour(void* self, void* edx, void* location) {
    g_viewMatrix(self, edx, location);
    const BYTE* base = static_cast<const BYTE*>(location);
    memcpy(g_camera.location, base, sizeof(g_camera.location));
    memcpy(&g_camera.view, base + 0x60, sizeof(Matrix4));
    g_camera.hasView = true;
}

// SHmsCameraProjection: +0x00 GmMat4 projection, +0x40 transposed copy.
void __fastcall projectionDetour(void* self, void* edx, void* projection, const void* frustum, void* camera) {
    g_projection(self, edx, projection, frustum, camera);
    // TAA jitter: shift the image by a fraction of a pixel. In D3D's row-vector form P20/P21
    // move x/y in NDC. The struct holds the matrix transposed first (P20 at [0][2]), then
    // in D3D's own layout. Perspective only (shadow maps are orthographic).
    float* m = static_cast<float*>(projection);
    const bool main = camera && camera == g_mainCamera;
    if (main && !g_loggedMainProjection && (g_zoom > 1.0f || g_tile[0] != 1.0f)) {
        g_loggedMainProjection = true;
        TMVS_LOG("engine: photo zoom / tiles on the main camera's projection");
    }
    // Photo zoom: NDC scaled about the centre (x' = zoom x), so P00, P20 and P11, P21.
    if (main && g_zoom > 1.0f && m[14] == 1.0f && m[16 + 11] == 1.0f) {
        m[0] *= g_zoom;
        m[2] *= g_zoom;
        m[5] *= g_zoom;
        m[6] *= g_zoom;
        m[16 + 0] *= g_zoom;
        m[16 + 8] *= g_zoom;
        m[16 + 5] *= g_zoom;
        m[16 + 9] *= g_zoom;
    }
    // Hi-res photo tile: zoom in on one part of the view (x' = scale x + offset in NDC).
    if (main && (g_tile[0] != 1.0f || g_tile[1] != 1.0f) && m[14] == 1.0f && m[16 + 11] == 1.0f) {
        m[0] *= g_tile[0];
        m[2] = m[2] * g_tile[0] + g_tile[2];
        m[5] *= g_tile[1];
        m[6] = m[6] * g_tile[1] + g_tile[3];
        m[16 + 0] *= g_tile[0];
        m[16 + 8] = m[16 + 8] * g_tile[0] + g_tile[2];
        m[16 + 5] *= g_tile[1];
        m[16 + 9] = m[16 + 9] * g_tile[1] + g_tile[3];
    }
    if ((g_jitter[0] != 0.0f || g_jitter[1] != 0.0f) && m[14] == 1.0f && m[16 + 11] == 1.0f) {
        m[2] += g_jitter[0];
        m[6] += g_jitter[1];
        m[16 + 8] += g_jitter[0];
        m[16 + 9] += g_jitter[1];
    }
    memcpy(&g_camera.projection, projection, sizeof(Matrix4));
    g_camera.hasProjection = true;
}

// EShadow: 0 None, 1 Minimum, 2 Medium, 3 High, 4 VeryHigh, 5 Complex (assumed launcher order).
void __fastcall shadowsSetDetour(void* self, void* edx, int mode) {
    g_shadowsSet(self, edx, g_forcedShadows >= 0 ? g_forcedShadows : mode);
}

} // namespace

void forceShadows(int mode) {
    g_forcedShadows = mode;
}

bool install(const Callbacks& callbacks) {
    if (g_active) return true;

    const Slot slots[] = {kRenderFrameBegin, kRenderCameraBegin, kRenderCameraEnd, kRenderOverlayZones,
                          kRenderFrameEnd, kComputeDriverViewMatrix, kComputeDriverProjection, kRenderShadowsSet};
    for (const GameBuild& build : kBuilds) {
        bool match = true;
        for (const Slot& slot : slots) match = match && verify(build, slot);
        if (match) {
            g_build = &build;
            break;
        }
    }
    if (!g_build) {
        TMVS_LOG("engine: unknown TmForever.exe build (neither Nations nor United Forever), engine hooks disabled");
        return false;
    }

    g_callbacks = callbacks;
    patch(kRenderFrameBegin, reinterpret_cast<void*>(&frameBeginDetour), g_frameBegin);
    patch(kRenderCameraBegin, reinterpret_cast<void*>(&cameraBeginDetour), g_cameraBegin);
    patch(kRenderCameraEnd, reinterpret_cast<void*>(&cameraEndDetour), g_cameraEnd);
    patch(kRenderOverlayZones, reinterpret_cast<void*>(&overlayZonesDetour), g_overlayZones);
    patch(kRenderFrameEnd, reinterpret_cast<void*>(&frameEndDetour), g_frameEnd);
    patch(kComputeDriverViewMatrix, reinterpret_cast<void*>(&viewMatrixDetour), g_viewMatrix);
    patch(kComputeDriverProjection, reinterpret_cast<void*>(&projectionDetour), g_projection);
    patch(kRenderShadowsSet, reinterpret_cast<void*>(&shadowsSetDetour), g_shadowsSet);
    g_tracksUpdate = reinterpret_cast<TracksUpdateFn>(inlineHook(kClipTracksUpdate, reinterpret_cast<void*>(&tracksUpdateDetour)));
    g_updateCams = reinterpret_cast<UpdateCamsFn>(inlineHook(kClipViewerCams, reinterpret_cast<void*>(&updateCamsDetour)));
    g_doShoot = reinterpret_cast<DoShootFn>(inlineHook(kVideoShootSite, reinterpret_cast<void*>(&doShootDetour)));
    g_waterPlane = reinterpret_cast<WaterPlaneFn>(inlineHook(kWaterPlaneSite, reinterpret_cast<void*>(&waterPlaneDetour)));
    g_tileHeight = reinterpret_cast<TileHeightFn>(inlineHook(kWaterTileHeight, reinterpret_cast<void*>(&tileHeightDetour)));
    g_raceReset = reinterpret_cast<RaceResetFn>(inlineHook(kRaceResetSite, reinterpret_cast<void*>(&raceResetDetour)));
    g_respawn = reinterpret_cast<RespawnFn>(inlineHook(kRespawnSite, reinterpret_cast<void*>(&respawnDetour)));
    g_loadDecoration = reinterpret_cast<LoadDecorationFn>(inlineHook(kLoadDecorationSite, reinterpret_cast<void*>(&loadDecorationDetour)));
    g_setChallenge = reinterpret_cast<SetChallengeFn>(inlineHook(kSetChallengeSite, reinterpret_cast<void*>(&setChallengeDetour)));
    g_freeCamSet = reinterpret_cast<FreeCamFn>(inlineHook(kFreeCamSite, reinterpret_cast<void*>(&freeCamDetour)));
    g_zclip = reinterpret_cast<ZClipFn>(inlineHook(kZClipSite, reinterpret_cast<void*>(&zclipDetour)));
    g_drawShader = reinterpret_cast<DrawShaderFn>(inlineHook(kDrawShaderSite, reinterpret_cast<void*>(&drawShaderDetour)));
    g_drawPass = reinterpret_cast<DrawPassFn>(inlineHook(kDrawPassSite, reinterpret_cast<void*>(&drawPassDetour)));
    g_lensFlare = reinterpret_cast<LensFlareFn>(inlineHook(kLensFlareSite, reinterpret_cast<void*>(&lensFlareDetour)));
    g_readInput = reinterpret_cast<ReadInputFn>(inlineHook(kReadInputSite, reinterpret_cast<void*>(&readInputDetour)));
    g_interfaceSet = reinterpret_cast<InterfaceSetFn>(inlineHook(kInterfaceSetSite, reinterpret_cast<void*>(&interfaceSetDetour)));
    g_loadScene = reinterpret_cast<LoadSceneFn>(inlineHook(kLoadSceneSite, reinterpret_cast<void*>(&loadSceneDetour)));
    g_clipTriggers = reinterpret_cast<ClipTriggersFn>(inlineHook(kClipTriggersSite, reinterpret_cast<void*>(&clipTriggersDetour)));
    g_jumpTime = reinterpret_cast<JumpTimeFn>(inlineHook(kJumpTimeSite, reinterpret_cast<void*>(&jumpTimeDetour)));
    g_raceTriggers = reinterpret_cast<RaceTriggersFn>(inlineHook(kRaceTriggersSite, reinterpret_cast<void*>(&raceTriggersDetour)));
    patchFx(0, reinterpret_cast<void*>(&bloomCanActiveDetour));
    patchFx(1, reinterpret_cast<void*>(&colorsCanActiveDetour));
    g_active = true;
    TMVS_LOG("engine: TrackMania %s, CVisionViewportDx9 hooks installed (module base %p)", g_build->name, GetModuleHandleW(nullptr));
    return true;
}

bool active() {
    return g_active;
}

void setNightAtLoad(bool (*decide)(float dayTime)) {
    g_wantNight = decide;
}

float mapDayTime() {
    return g_mapDayTime;
}

bool mapLitAtNight() {
    return g_nightForced;
}

bool relightNight(bool night) {
    if (night == g_nightForced) return true;
    if (!g_jumpTime || !g_weather || g_weatherTime < 0.0f || g_mapDayTime < 0.0f || !readable(g_weather, 0x30)) return false;
    g_nightForced = night;
    TMVS_LOG("engine: real night %s during the map: weather %p to time of day %.3f", night ? "on" : "off", g_weather,
             night ? 0.0f : g_weatherTime);
    __try {
        g_jumpTime(g_weather, nullptr, night ? 0.0f : g_weatherTime);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_weather = nullptr;
        return false;
    }
    return true;
}

const CameraInfo& currentCamera() {
    return g_camera;
}

bool stadium() {
    if (g_environment[0]) return strcmp(g_environment, "Stadium") == 0;
    return g_build == &kBuilds[0]; // Nations Forever has nothing else
}

bool waterHeights(float& blockY, float& seaY) {
    if (!g_mapLoaded) return false;
    // Water blocks only exist in Stadium; elsewhere a surface at their height is no water.
    blockY = seaY = stadium() ? kBlockWaterY : -1e30f; // no sea: both the same
    // The sea level alone is no proof of water: on Rally it is the level of the flat ground
    // (28 m), and every road there became a mirror. Only a water plane the game is drawing
    // the reflection of right now counts.
    float y = 0.0f;
    const bool plane = g_lastWater != 0 && GetTickCount() - g_lastWater < 2000 && levelPlaneHeight(g_waterPlaneEq, y);
    if (plane) seaY = y;
    // Once per map: the first water plane (the owner's log tells which maps have one).
    static long s_loggedMap = -1;
    if (plane && s_loggedMap != g_mapLoads) {
        s_loggedMap = g_mapLoads;
        TMVS_LOG("engine: water plane at %.2f (sea level %.2f)", y, g_seaLevel);
    }
    return true;
}

int mapLoads() {
    return static_cast<int>(g_mapLoads);
}

std::string mapComments() {
    return g_mapComments;
}

bool carLocation(float position[3]) {
    // Only the player of a race that runs now (the hook calls come every frame); the build's
    // own GetInterpolatedLocation in the slot, or nothing is called.
    if (!g_build || !g_build->interpolatedLocation || !g_racePlayer || GetTickCount() - g_racePlayerTick > 300) return false;
    __try {
        const BYTE* gameMobil = *reinterpret_cast<BYTE* const*>(static_cast<const BYTE*>(g_racePlayer) + kPlayerMobil);
        if (!gameMobil || *reinterpret_cast<const int*>(gameMobil + kGameMobilSceneId) == -1) return false;
        void* mobil = *reinterpret_cast<void* const*>(gameMobil + kGameMobilScene);
        if (!mobil) return false;
        void* const* table = *reinterpret_cast<void* const* const*>(mobil);
        if (reinterpret_cast<uintptr_t>(table[kInterpolatedLocationSlot]) != rebase(g_build->interpolatedLocation)) return false;
        using LocationFn = const float*(__fastcall*)(void*, void*, void*);
        const float* iso = reinterpret_cast<LocationFn>(table[kInterpolatedLocationSlot])(mobil, nullptr, nullptr);
        if (!iso) return false;
        // GmIso4: a row-major 3x3 (p' = M p + t: its columns are the car's axes), then t.
        for (int i = 0; i < 3; i++) {
            position[i] = iso[9 + i];
            if (!(fabsf(position[i]) < 1e5f)) return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_racePlayer = nullptr;
        return false;
    }
}

int mapCloses() {
    return static_cast<int>(g_mapCloses);
}

bool mapOpen() {
    return currentChallenge() != nullptr;
}

bool currentMapComments(std::string& comments) {
    static wchar_t text[16384];
    void* challenge = currentChallenge();
    if (!challenge || !commentsInMemory(challenge, text, 16384)) return false;
    comments = toUtf8(text, static_cast<int>(wcslen(text)));
    return true;
}

bool setCurrentMapComments(const std::string& comments) {
    void* challenge = currentChallenge();
    if (!challenge || !g_setChallenge) return false;
    const int n = MultiByteToWideChar(CP_UTF8, 0, comments.c_str(), static_cast<int>(comments.size()), nullptr, 0);
    std::wstring wide(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, comments.c_str(), static_cast<int>(comments.size()), &wide[0], n);
    if (!writeComments(challenge, wide.c_str(), static_cast<uint32_t>(wide.size()))) return false;
    TMVS_LOG("engine: comments of the open map set (%u characters)", static_cast<unsigned>(wide.size()));
    return true;
}

int raceResets() {
    return static_cast<int>(g_raceResets);
}

int respawns() {
    return static_cast<int>(g_respawns);
}

void setProjectionJitter(float x, float y) {
    g_jitter[0] = x;
    g_jitter[1] = y;
}

void setProjectionTile(float scaleX, float scaleY, float offsetX, float offsetY) {
    g_tile[0] = scaleX;
    g_tile[1] = scaleY;
    g_tile[2] = offsetX;
    g_tile[3] = offsetY;
}

void setMainCamera(const void* camera) {
    g_mainCamera = camera;
}

void setProjectionZoom(float zoom) {
    g_zoom = zoom < 1.0f ? 1.0f : zoom;
}

void hideOverlay(bool hide) {
    if (hide == g_hideOverlay) return;
    g_hideOverlay = hide;
    applyInterfaceVisibility();
}

void swallowEscape() {
    g_swallowEscape = true;
}

bool escapeSwallowed() {
    return g_swallowEscape;
}

void hideGameSky(bool hide) {
    g_hideGameSky = hide;
}

void hideSunFlare(bool hide) {
    g_hideSunFlare = hide;
}

void shaderCensus(bool on) {
    g_shaderCensus = on;
}

const void* currentShader() {
    return g_currentShader;
}

Material currentMaterial() {
    return materialOf(g_currentShader);
}

void fullViewDistance(bool full) {
    g_fullViewDistance = full;
}

void skipGameFx(bool skip) {
    if (skip != g_skipGameFx && (g_fxCanActive[0] || g_fxCanActive[1])) TMVS_LOG("engine: game FX post-processing %s", skip ? "skipped" : "back on");
    g_skipGameFx = skip;
}

bool freeCamActive() {
    // The flag itself, not only the last call: the camera may be gone with its replay.
    const BYTE* camera = static_cast<const BYTE*>(g_freeCam);
    return camera && readable(camera + kFreeCamActive, sizeof(int)) && *reinterpret_cast<const int*>(camera + kFreeCamActive) == 1;
}

bool cinematicActive() {
    // Without the hooks there is no way to tell: treat everything as cinematic, so the
    // settings behave as before.
    if (!g_tracksUpdate && !g_updateCams && !g_doShoot) return true;
    return g_lastCinematic != 0 && GetTickCount() - g_lastCinematic < 500;
}

unsigned cinematicSources() {
    const unsigned s = g_cinematicSources;
    g_cinematicSources = 0;
    return s;
}

} // namespace engine
} // namespace tmshaders
