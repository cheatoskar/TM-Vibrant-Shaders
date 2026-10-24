#include "engine.h"
#include "blockorder.h"
#include "gbx.h"
#include "hook.h"
#include "log.h"
#include <windows.h>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <string>
#include <vector>
#include <cwchar>
#include <cstdio>
#include <intrin.h>

namespace tmshaders {
namespace engine {
namespace {

// TmForever.exe comes in two builds: Nations Forever and United Forever. Same code, shifted
// addresses. Link-time addresses (preferred base 0x00400000): TMNF from TmForever.map, TMUF
// found by matching TMNF's code (absolute addresses and call targets masked) and checked
// over 256 bytes.
constexpr uintptr_t kPreferredBase = 0x00400000;

enum SlotId { kFrameBegin, kCameraBegin, kCameraEnd, kOverlayZones, kFrameEnd, kViewMatrix, kProjection, kShadowsSet, kSlotCount };
enum SiteId { kClipTracks, kClipCams, kVideoShoot, kTileHeight, kRaceReset, kRespawn, kWaterPlane, kLoadDecoration, kSetChallenge, kFreeCam, kZClip, kDrawShader, kDrawPass, kLensFlare, kReadInput, kInterfaceSet, kLoadScene, kClipTriggers, kJumpTime, kRaceTriggers, kCacheUpToDate, kCacheSave, kAviOpen, kAviVideoOpen, kAviVideoWrite, kAviAudioOpen, kAviAudioWrite, kAviDestroy, kRemoveBlock, kBaseRgb, kSiteCount };

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
    uintptr_t coreCmdBuffer;       // CMwCmdBufferCore* CMwCmdBufferCore::TheCoreCmdBuffer (the game's clock)
    // CMotionManagerWeathers::UpdateAsync, the moon's branch: its calls of GmMat3::SetDOV (the
    // moon's direction) and GxLight::SetBaseRGB (its colour), and those two functions.
    uintptr_t moonDirectionCall, setDov, moonColorCall, setBaseRgb;
    uintptr_t shaderApplyVTable;   // ??_7CPlugShaderApply@@6B@ (the materials whose textures are read)
    // RenderFrameEnd's phase of its spread-out video read-back (a global, see kViewportVideoAsync);
    // RenderFrameEnd + kReadPhaseCheck writes it (mov [phase], ebp), checked before use.
    uintptr_t videoReadPhase;
    // CSceneVehicleCar's WheelGetCount, WheelGetAsyncGroundContactPos and VehicleAsyncWorldSpeedGet
    // (vtable slots 98, 101, 78): the car's mobil is checked against them before its wheels are
    // read (see kVehicleWheels).
    uintptr_t wheelCount, wheelContact, asyncSpeed;
};

// Same in both builds: CGameCtnApp holds the current map (a reference), the map its comments.
constexpr size_t kAppChallenge = 0x198;      // CGameCtnChallenge* (CGameCtnApp::GetChallenge)
constexpr size_t kChallengeComments = 0x19c; // CFastStringInt (SHeaderThumbnail::FillHeaderUserData)

constexpr GameBuild kBuilds[] = {
    {"Nations Forever", 0x00bd10bc,
     {0x009a0990, 0x0099d750, 0x0099fe80, 0x009a2390, 0x009a4070, 0x0095ab40, 0x0095d530, 0x0095acc0},
     {0x00693e20, 0x00673e50, 0x006f4510, 0x0054e100, 0x004bedd0, 0x0047c0d0, 0x00991e30, 0x005a4f60, 0x005f6ed0, 0x00690d10, 0x0053fc80, 0x00971b10, 0x0096fd20, 0x00959260, 0x008fb190, 0x005e1d00, 0x007158e0, 0x00673cd0, 0x00574170, 0x005dee70, 0x005425a0, 0x00542eb0,
      0x0095a150, 0x00959d80, 0x0095a0f0, 0x00959fb0, 0x00959c40, 0x00959d50, 0x005a5ff0, 0x00834ec0},
     {0, 0x00aaf4e8, 0, 0, 0, 0, 0, 0x00a9cc20, 0x00aa3da8, 0, 0x00d731e0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00a9cd20}, 0x00d123ba, 0x00935400, 0x0041be40, 0x0042b590, 0x00bbf7dc,
     0x00903280, {0x00ba90e4, 0x00ba1cd4}, {0x00821c10, 0x007ea7d0}, 0x00d54380, 0x006736d0, 0x005de270, 0x007b38f0, 0x00d731e0,
     0x00573324, 0x008e17c0, 0x00573368, 0x00834ec0, 0x00baf1bc, 0x00d77c78, 0x007bf6b0, 0x007bf720, 0x007bff70},
    {"United Forever", 0x00bd109c,
     {0x009a0710, 0x0099d4d0, 0x0099fc00, 0x009a2110, 0x009a3df0, 0x0095aad0, 0x0095d440, 0x0095ac50},
     {0x00693ff0, 0x00673f70, 0x006f44e0, 0x0054e030, 0x004bea30, 0x0047bed0, 0x00991bd0, 0x005a5100, 0x005f7010, 0x00690ee0, 0x0053fab0, 0x009719e0, 0x0096fbf0, 0x009591f0, 0x008fb9c0, 0x005e1da0, 0x00715780, 0x00673df0, 0x005741f0, 0x005deef0, 0x00542450, 0x00542d60,
      0x0095a0e0, 0x00959d10, 0x0095a080, 0x00959f40, 0x00959bd0, 0x00959ce0, 0x005a6190, 0x00834ce0},
     {0, 0x00aaeee8, 0, 0, 0, 0, 0, 0x00a9c620, 0x00aa37a8, 0, 0x00d74840, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00a9c720}, 0x00d1442a, 0x00935290, 0x0041be50, 0x0042b680, 0x00bbf7bc,
     0x00903a90, {0x00ba90c4, 0x00ba1cb4}, {0x00821ab0, 0x007ea3b0}, 0x00d559e0, 0x006737f0, 0x005de340, 0x007b3500, 0x00d74840,
     0x005733a4, 0x008e2010, 0x005733e8, 0x00834ce0, 0x00baf19c, 0x00d792d8, 0x007bf1d0, 0x007bf240, 0x007bfa70},
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
// The export's settings (the same in both builds, from DoShoot): the shooter holds the dialog's
// CGameCtnMediaVideoParams at +0x4c (Fps +0x14, Width +0x18, Height +0x1c, Hq +0x20,
// HqSampleSize +0x24, HqMB +0x28, IsAudioStream +0x34, Stereo3d +0x38, from its parameter table); +0x14 set means
// this run opens no video output. With Hq and a sample size n > 1 the camera gets a
// CSceneFxSuperSample of n x n (n at most 11). With HqMB and n x n >= 9 the samples are spread
// over time ("external loop"): every rendered frame is one sample, the clock advances by
// 1 / (fps n n), and only the last of them is written. Otherwise its FxRender renders the
// camera n x n times within the frame, each with a sub-pixel offset, into the same texture.
constexpr size_t kShooterNoVideo = 0x14;
// The clip's start and end (ms, CMwTimer::SecondsToMwTime of the clip's StartTimeGet / StopTimeGet).
// The frames run from the start; the sound pass sets the clip's time from 0 instead (DoShoot,
// state 0x184: tick - tick at the capture's start), so with a start above 0 its sound begins earlier.
constexpr size_t kShooterClipStart = 0x30;
constexpr size_t kShooterClipStop = 0x38;
constexpr size_t kShooterParams = 0x4c;
constexpr size_t kVideoParamsSize = 0x3c;
// CMwCmdBufferCore::EnableFixedTickTime(enable, ms, fraction): while +0x38 or +0x3c is set,
// each Run() advances the game's clock by +0x38 ms plus +0x3c / 2^31 ms instead of the real
// time (the export sets 1 / (fps x samples per video frame)). The clock (CMwTimer at +0x70)
// keeps its time in ms at +0x78 (CMwTimer::GetTickTime). Checked in both builds.
constexpr size_t kCoreFixedMs = 0x38;
constexpr size_t kCoreFixedFraction = 0x3c;
constexpr size_t kCoreTickTime = 0x78;
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
// The car's axes in that location (a GmIso4, row-major 3x3, p' = M p + t): its columns are the
// car's x, y, z. Forward is +z: CSceneVehicleCar::VehicleFrontSpeedGet (TMNF 0x008148a0, TMUF
// 0x007bcd40) returns the z of CHmsDyna::GetLocalLinearSpeed (the world speed times the
// transposed matrix) as the car's speed in km/h. Up is y (the model's), x the side.
constexpr int kForwardColumn = 2;
// The replay editor, its video export and replays run no race (raceTriggersDetour stays silent:
// my export logs never had the car), only clip players (CGameCtnMediaClipPlayer::TracksUpdate,
// already hooked). The car their cameras show, the same in both builds (Ghidra, TMNF names,
// ported to TMUF over 256 bytes: GameMobilGetFromGhostId, GhostIdToGameMobilId,
// LocalPlayerGameMobilGet, CGameCtnMediaBlockCameraGame::Update, PlayerGameMobilIdSet,
// CGameScene::GameMobilGetFromId):
// - clip player +0x18 its context: +0x20 the CGameScene (its CGameMobils: CFastBuffer at +0x1c,
//   {count, data}; each mobil's id at +0x18), +0x28 the CGamePlayerCameraSet, whose +0x14 is the
//   id of the mobil the game cameras follow (set every frame by a game camera block from its
//   ghost: CameraGame::Update -> PlayerGameMobilIdSet).
// - clip player +0x60 the local player's CGameCtnGhost (ghost id 0; +0x90 its CGameMobil), else
//   +0x5c the local player's mobil id (-1 none); +0x98 the ghost blocks (CFastBuffer of
//   CGameCtnMediaBlockGhost, +0x30 each one's CGameCtnGhost).
constexpr size_t kClipContext = 0x18;
constexpr size_t kClipLocalMobilId = 0x5c;
constexpr size_t kClipLocalGhost = 0x60;
constexpr size_t kClipGhostBlocks = 0x98;
constexpr size_t kContextScene = 0x20;
constexpr size_t kContextCameraSet = 0x28;
constexpr size_t kSceneMobils = 0x1c;
constexpr size_t kCameraSetFollowed = 0x14;
constexpr size_t kGameMobilId = 0x18;
constexpr size_t kBlockGhost = 0x30;
constexpr size_t kGhostMobil = 0x90;
// CSceneVehicleCar (the vehicle of every car, the same layout in both builds, found by pattern):
// its SSimulationWheels are a CFastBuffer at +0x2e8 (0x2fc bytes each), each one's async ground
// contact (world, the physics state: up to a step behind the drawn car) at +0x2bc; the car's
// async world speed at +0x4b4 (m/s). The vtable slots of their getters identify the class.
constexpr size_t kVehicleWheels = 0x2e8;
constexpr size_t kWheelSize = 0x2fc;
constexpr size_t kWheelContact = 0x2bc;
constexpr size_t kVehicleAsyncSpeed = 0x4b4;
constexpr int kWheelCountSlot = 98;
constexpr int kWheelContactSlot = 101;
constexpr int kAsyncSpeedSlot = 78;
// Every jump of a weather animation to a time of day (LoadScene3d at map load, and a time
// set as a parameter later, e.g. when the race starts): 0 = midnight. Real night forces 0
// here too, or the later jump puts the map's own time back.
constexpr InlineSite kJumpTimeSite = {kJumpTime, {0x51, 0x56, 0x8b, 0xf1, 0xd9, 0x46, 0x28}, 7, -1, "CMotionManagerWeathers::JumpToTimeRemapped"};
constexpr size_t kDecorationMood = 0x60; // CGameCtnDecorationMood*
// The weather sets the scene's directional light (CMotionManagerWeathers::UpdateAsync, at every
// time jump and frame). Where its moon colour is brighter than the sun's (night) it builds the
// light's travel direction itself, a horizontal one from the time of day (A01 lit at midnight:
// (-1, 0, 0)), turns it into the light's location (GmMat3::SetDOV, then the CSceneLight's
// SetLocation) and gives the light the moon's colour (GxLight::SetBaseRGB). The same vector
// makes its specular cube. At night the map's lightmap (CHmsPackLightMap, fid parameter IsNight)
// is baked in colour at map load from that light: ambient dome, the moon with its shadows, the
// lamps; the block shaders take their light from it (PreLightGen) and add no moon of their own.
// So the direction or colour given to these two calls is the moon of the whole map: the bake,
// the glints, the car. Both calls are in the moon's branch only (the sun has its own matrix).
// The lightmap cache (ProgramData/TrackMania/Cache/*.LightMap.zip) is keyed by map, decoration
// and IsNight, not by the light: a night bake saved there would come back as it is.
// The lightmap cache of a map (CHmsPackLightMap): whether the saved one still fits, and saving
// a new one. With the moon changed the cache is neither used nor written: a saved bake of the
// game's moon would come back without the change, and a changed one would stay when it is off.
constexpr InlineSite kCacheUpToDateSite = {kCacheUpToDate, {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8}, 6, -1, "CHmsPackLightMap::CacheIsUpToDate"};
constexpr InlineSite kCacheSaveSite = {kCacheSave, {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8}, 6, -1, "CHmsPackLightMap::CacheSaveLightMap"};
// The video export's file (CVisionVideoAvi, VfW; the same code in both builds, 0x70 lower in
// TMUF). DoShoot creates it and OpenToWrite makes the .avi (Documents\TrackMania\Video). The
// viewport's RenderFrameEnd opens the video stream at the first frame (VideoStreamOpen: the
// size rounded up to 8 and written back, AVISaveOptions - the codec dialog -, a BI_RGB header
// at +0x0c of its data [this+4], the image size at +0x20 for GetImageByteSize, the frame
// counter at +0x40) and writes every finished frame (VideoStreamWrite: bottom-up BGR, 3 bytes
// a pixel, with supersampling only the averaged frame). The frame is the one DoShoot flagged
// (CHmsViewport::VideoAddCurrentFrame, +0x54c) before this render: GetRenderTargetData of the
// viewport's colour surface, then the write, then EndScene and Present - and while the stream
// is open only flagged frames are presented: the sub-frames between them never reach Present
// (docs/engine-video-export.md). DoShoot opens the sound
// (AudioStreamOpen, an SWaveFormat = WAVEFORMATEX without cbSize) and writes it in pieces
// between the frames (AudioStreamWrite, a count of blocks). Close and the destructor end it.
constexpr InlineSite kAviOpenSite = {kAviOpen, {0x56, 0x8b, 0xf1, 0x8b, 0x46, 0x04}, 6, -1, "CVisionVideoAvi::OpenToWrite"};
constexpr InlineSite kAviVideoOpenSite = {kAviVideoOpen, {0x81, 0xec, 0x0c, 0x01, 0x00, 0x00}, 6, -1, "CVisionVideoAvi::VideoStreamOpen"};
constexpr InlineSite kAviVideoWriteSite = {kAviVideoWrite, {0x8b, 0x41, 0x04, 0x83, 0x78, 0x08, 0x00}, 7, -1, "CVisionVideoAvi::VideoStreamWrite"};
constexpr InlineSite kAviAudioOpenSite = {kAviAudioOpen, {0x81, 0xec, 0xd0, 0x00, 0x00, 0x00}, 6, -1, "CVisionVideoAvi::AudioStreamOpen"};
constexpr InlineSite kAviAudioWriteSite = {kAviAudioWrite, {0x57, 0x8b, 0xf9, 0x8b, 0x47, 0x04}, 6, -1, "CVisionVideoAvi::AudioStreamWrite"};
constexpr InlineSite kAviDestroySite = {kAviDestroy, {0x56, 0x8b, 0xf1, 0x8b, 0x46, 0x04}, 6, -1, "CVisionVideoAvi::~CVisionVideoAvi"};
// The editor takes a block out of the map (every way of removing one ends here: the block and
// puzzle editors, undo, replacing a block; only the editor and the map's clip update call it).
// CasterShadows keeps the meshes it saw: a removed block would keep casting its shadow.
constexpr InlineSite kRemoveBlockSite = {kRemoveBlock, {0x6a, 0xff, 0x68, 0, 0, 0, 0}, 7, 3, "CGameCtnChallenge::RemoveBlock"};
// GxLight::SetBaseRGB(rgb), every writer of a light's colour (21 call sites in each build: the
// weather's sun and moon, CFuncLightColor, CSceneFxSuperSample::FxRender, CSceneLight::Create*,
// CPlugLight::Make*, ...). Hooked for the log only (sun diag): which one lights the end-of-race
// replay under Real night. A leaf function; its first two instructions (mov eax,[esp+4];
// fld [eax]) run unchanged in the trampoline. GxLight: +0x14 flags, +0x18 base rgb, +0x24 and
// +0x28 the intensity factors, +0x2c rgb * both (what the shaders get).
constexpr InlineSite kBaseRgbSite = {kBaseRgb, {0x8b, 0x44, 0x24, 0x04, 0xd9, 0x00}, 6, -1, "GxLight::SetBaseRGB"};
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
using RemoveBlockFn = int(__fastcall*)(void*, void*, void*);
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
RemoveBlockFn g_removeBlock = nullptr;
volatile LONG g_raceResets = 0; // counters: the plugin compares them every frame
volatile LONG g_respawns = 0;
volatile LONG g_blocksRemoved = 0;
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
// Accumulation depth of field: the eye of the main camera moves by a lens offset (metres, view
// space) and the projection is sheared so the plane at g_lensFocus stays put. One offset per
// render, from a table: by the game's own supersampling sample when the camera has it (the
// video export: CSceneFxSuperSample::FxRender writes the sample index into the CHmsCamera before
// each render, also for the sub-frames of its motion blur), else one entry per main camera
// projection in turn. count 0 = off.
constexpr int kMaxLensSamples = 128; // the game takes at most 11 x 11 samples
float g_lensXY[kMaxLensSamples][2] = {};
int g_lensCount = 0;
float g_lensFocus = 0.0f;
unsigned g_lensRenders = 0;   // main camera projections since the table was set (without game samples)
int g_lensUsed = -1;          // the entry the last main camera projection got (-1 none)
int g_gameSample[2] = {-1, 0}; // the game's sample index and count of that projection (-1: none)
bool g_loggedLens = false;
// CHmsCamera, while CSceneFxSuperSample::FxRender renders it (CHmsCamera::ZClipCompute shifts the
// render frustum by a sub-pixel from them, in both builds): +0x180 on, +0x184 / +0x188 the
// sample's column and row, +0x18c / +0x190 the columns and rows (n, n).
constexpr size_t kCameraSuperSample = 0x180;
// The matrices the engine computed last (ComputeDriverViewMatrix / ComputeDriverProjection), as
// it hands them to SetTransform, and those it had computed when a camera began. A camera drawn
// into a texture (TexRender_RenderTexture: TransformStackCameraPush, then RenderCameraBegin; the
// video export's supersampling renders every sample that way) gets its view and projection set
// on the device BEFORE its RenderCameraBegin, with the dirty bits cleared, so the plugin's
// SetTransform hook sees none within the camera. A camera drawn to the viewport
// (RenderCameraNormal) gets them lazily at its first draw (UpdateCameraAndRect), within it.
Matrix4 g_lastView{}, g_lastProjection{};
bool g_lastViewSet = false, g_lastProjectionSet = false;
Matrix4 g_beginView{}, g_beginProjection{};
bool g_beginMatrices = false;
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
bool (*g_moonAtLoad)(float dayTime, bool litAtNight, MoonLight& moon) = nullptr; // the plugin: what the moon does on this map
using SetDovFn = void*(__fastcall*)(void*, void*, float*, int);
using SetBaseRgbFn = void*(__fastcall*)(void*, void*, const float*);
SetDovFn g_setDov = nullptr;
SetBaseRgbFn g_setBaseRgb = nullptr;
int g_moonMode = 0;                // this map: 0 the game's moon, 1 along g_moonTravel, 2 off
float g_moonTravel[3] = {};        // the light's travel direction (away from the black hole)
bool g_moonApplied = false;        // the weather reached the moon's code since the load
bool g_moonBaked = false;          // this load decided a changed moon: its lightmap is never saved
bool g_moonLogged = false;
float g_moonGameTravel[3] = {};    // the direction the game wanted (for the log)
using CacheFn = int(__fastcall*)(void*, void*);
CacheFn g_cacheUpToDate = nullptr;
CacheFn g_cacheSave = nullptr;
using AviOpenFn = int(__fastcall*)(void*, void*, const void*);
using AviVideoOpenFn = int(__fastcall*)(void*, void*, DWORD*, DWORD*, float, int);
using AviVideoWriteFn = int(__fastcall*)(void*, void*, void*);
using AviAudioOpenFn = int(__fastcall*)(void*, void*, const void*);
using AviAudioWriteFn = int(__fastcall*)(void*, void*, void*, DWORD);
using AviDestroyFn = void(__fastcall*)(void*, void*);
AviOpenFn g_aviOpen = nullptr;
AviVideoOpenFn g_aviVideoOpen = nullptr;
AviVideoWriteFn g_aviVideoWrite = nullptr;
AviAudioOpenFn g_aviAudioOpen = nullptr;
AviAudioWriteFn g_aviAudioWrite = nullptr;
AviDestroyFn g_aviDestroy = nullptr;
VideoFileHooks g_videoFile;
bool g_cacheLogged = false;
using JumpTimeFn = void(__fastcall*)(void*, void*, float);
JumpTimeFn g_jumpTime = nullptr;
int g_jumpsLogged = 0;
using ClipTriggersFn = void(__fastcall*)(void*, void*);
ClipTriggersFn g_clipTriggers = nullptr;
using RaceTriggersFn = void(__fastcall*)(void*, void*, void*, void*);
RaceTriggersFn g_raceTriggers = nullptr;
void* g_racePlayer = nullptr;                  // the player the race checked last (its car)
DWORD g_racePlayerTick = 0;                    // when (a player of a race that ended is gone)
void* g_clipPlayer = nullptr;                  // the clip player whose tracks ran last (replay editor, export)
unsigned g_clipPlayerFrame = 0;                // in which frame (g_engineFrames)
float g_mapDayTime = -1.0f;                   // the loaded map's own time of day (-1 unknown)
bool g_nightForced = false;                   // the game lights it at midnight instead
void* g_weather = nullptr;                    // this map's weather (the last to jump in time)
float g_weatherTime = -1.0f;                  // the time of day the game asked it for last
// The game loads a map (CGameCtnApp::InitChallengeDataWithLoadScreen: LoadDecorationAndCollection
// -> LoadScene3d, the weather's jumps) before SetChallenge makes it the current one
// (StartChallengeMinimal, Ghidra). The map whose decoration load set the state above, so that
// its own SetChallenge keeps it: clearing it there left every map without its time of day and
// its forced midnight (no live relight, no keepNight, the mood from the light's colour).
const void* g_loadingChallenge = nullptr;     // inside its LoadDecorationAndCollection
const void* g_sceneChallenge = nullptr;       // the map the state above belongs to
// Real night kept (keepNight): the weather's own clock (a CMotionTimerLoop at +0x1c: +0x14 mode,
// +0x18 / +0x1c its two times in ms, +0x20 the length of a day in ms, +0x28 speed) as the forced
// midnight left it, and the weather's vtable then (the object is checked before it is used again).
constexpr size_t kWeatherTimer = 0x1c;
float g_nightClock = -1.0f;
float g_nightClockAsync = -1.0f; // its other time then (each clock is compared with its own)
const void* g_weatherVtable = nullptr;
int g_nightLogged = 0;
int g_nightJumps = 0;            // keepNight's jumps back to midnight on this map (logged at the next)
DWORD g_nightJumpTick = 0;       // the last one (at most one a second)
volatile LONG g_flares[3] = {}; // lens flares since the last read: other lights, the sun, the sun hidden
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
DWORD g_lastNotClip = 0;         // ... of the clip viewer's cameras or the video export (not the clip player)
unsigned g_engineFrames = 0;    // RenderFrameBegin calls
void* g_shooter = nullptr;      // the video shooter whose DoShoot ran last
unsigned g_shootFrame = 0;      // and in which frame (g_engineFrames)

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
// The function at the site starts with the expected bytes (this executable has it there).
bool prologueMatches(const InlineSite& site) {
    const BYTE* target = reinterpret_cast<const BYTE*>(rebase(g_build->sites[site.id]));
    unsigned char expected[12];
    memcpy(expected, site.prologue, sizeof(expected));
    if (site.handlerAt >= 0) {
        const uint32_t handler = static_cast<uint32_t>(rebase(g_build->handlers[site.id]));
        memcpy(expected + site.handlerAt, &handler, sizeof(handler));
    }
    return readable(target, site.length) && memcmp(target, expected, site.length) == 0;
}

void* inlineHook(const InlineSite& site, void* detour) {
    BYTE* target = reinterpret_cast<BYTE*>(rebase(g_build->sites[site.id]));
    if (!prologueMatches(site)) {
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

// Points one call instruction (E8 rel32) inside a function at the detour, after checking that it
// calls the function the map names there. Returns that function (the original), or null.
void* patchCall(uintptr_t site, uintptr_t callee, void* detour, const char* name) {
    BYTE* call = reinterpret_cast<BYTE*>(rebase(site));
    const uintptr_t expected = rebase(callee);
    if (!site || !readable(call, 5) || call[0] != 0xE8 ||
        reinterpret_cast<uintptr_t>(call + 5) + static_cast<uintptr_t>(*reinterpret_cast<const int32_t*>(call + 1)) != expected) {
        TMVS_LOG("engine: %s does not match this executable, not hooked", name);
        return nullptr;
    }
    DWORD old = 0;
    VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old);
    *reinterpret_cast<int32_t*>(call + 1) = static_cast<int32_t>(static_cast<BYTE*>(detour) - (call + 5));
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    return reinterpret_cast<void*>(expected);
}

// --- Sun diag (log only) ----------------------------------------------------
// Under Real night the end-of-race replay of A01 is lit by a warm directional light although
// the map's only weather stays at midnight (capture_117/118 against capture_120). Which code
// gives the light the replay draws its colour? Read-only: per "epoch" (a scene load, then every
// start of a cinematic) the sun light the lens flares see is logged next to the weather's own
// GxLight (+0xc4), at the first sun flare and again 2 s later, and the first callers of
// GxLight::SetBaseRGB (de-duplicated by return address) with the light and colour they set.
// When the epoch ends, how often each of them was called. Nothing in the game is written.
bool weatherClock(float& async, float& timed, int& mode, float& speed);
constexpr size_t kWeatherLight = 0xc4;   // GxLight* of the weather's directional light
constexpr int kSunDiagCallers = 8;       // distinct SetBaseRGB callers logged per epoch
constexpr int kSunDiagEpochsPerMap = 12; // cinematic starts logged per scene load
volatile LONG g_sunDiagBusy = 0;         // one writer at a time (the others pass untouched)
LONG g_sunDiagEpoch = 0;
int g_sunDiagEpochsThisMap = 0;
bool g_sunDiagOn = false;                // within the epochs of this map
DWORD g_sunDiagTick = 0;                 // the epoch's start
int g_sunFlareLogged = 0;                // this epoch: 0 none, 1 the first sun flare, 2 also 2 s later
const void* g_sunFlareLight = nullptr;   // the sun's GxLight as the flares saw it last
struct RgbCaller {
    uintptr_t ret;
    unsigned calls;
};
RgbCaller g_rgbCallers[kSunDiagCallers] = {};
int g_rgbCallerCount = 0;
unsigned g_rgbOtherCalls = 0;            // by callers beyond the logged ones
using BaseRgbFn = void(__fastcall*)(void*, void*, const float*);
BaseRgbFn g_baseRgb = nullptr;           // the trampoline

bool sunDiagLock() {
    return InterlockedCompareExchange(&g_sunDiagBusy, 1, 0) == 0;
}

void sunDiagUnlock() {
    InterlockedExchange(&g_sunDiagBusy, 0);
}

// A code address as Ghidra and TmForever.map name it (link time, base 0x00400000), or the
// module and offset outside the game (the mod's own moon colour call).
void describeCode(uintptr_t address, char* out, size_t size) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) || !module) {
        sprintf_s(out, size, "%08x (no module)", static_cast<unsigned>(address));
        return;
    }
    if (module == GetModuleHandleW(nullptr)) {
        sprintf_s(out, size, "TmForever %08x", static_cast<unsigned>(address - reinterpret_cast<uintptr_t>(module) + kPreferredBase));
        return;
    }
    char path[MAX_PATH] = {};
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* name = strrchr(path, '\\');
    sprintf_s(out, size, "%s+%x", name ? name + 1 : path, static_cast<unsigned>(address - reinterpret_cast<uintptr_t>(module)));
}

// A GxLight as text: flags, base rgb, the two intensity factors, rgb * both.
void describeLight(const void* light, char* out, size_t size) {
    __try {
        const BYTE* l = static_cast<const BYTE*>(light);
        if (!l || !readable(l, 0x38)) {
            sprintf_s(out, size, "gx %p (unreadable)", light);
            return;
        }
        const float* f = reinterpret_cast<const float*>(l);
        sprintf_s(out, size, "gx %p flags 0x%x rgb %.3f %.3f %.3f int %.3f x %.3f final %.3f %.3f %.3f", light,
                  *reinterpret_cast<const unsigned*>(l + 0x14), f[0x18 / 4], f[0x1c / 4], f[0x20 / 4], f[0x24 / 4], f[0x28 / 4],
                  f[0x2c / 4], f[0x30 / 4], f[0x34 / 4]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sprintf_s(out, size, "gx %p (fault)", light);
    }
}

const void* weatherLight() {
    __try {
        const BYTE* weather = static_cast<const BYTE*>(g_weather);
        if (!weather || !readable(weather + kWeatherLight, sizeof(void*))) return nullptr;
        return *reinterpret_cast<const void* const*>(weather + kWeatherLight);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// How often the logged callers were called in the epoch that ends (one line).
void reportRgbCallers() {
    if (!g_baseRgb || (g_rgbCallerCount == 0 && g_rgbOtherCalls == 0)) return;
    char line[768] = {};
    size_t used = 0;
    for (int i = 0; i < g_rgbCallerCount && used < sizeof(line) - 64; i++) {
        char where[96];
        describeCode(g_rgbCallers[i].ret, where, sizeof(where));
        const int n = sprintf_s(line + used, sizeof(line) - used, "%s%s x%u", i ? ", " : "", where, g_rgbCallers[i].calls);
        if (n > 0) used += static_cast<size_t>(n);
    }
    TMVS_LOG("engine: sun diag: epoch %ld SetBaseRGB calls: %s; others x%u", g_sunDiagEpoch, line, g_rgbOtherCalls);
}

// A new epoch: a scene load (map: true) or the start of a cinematic.
void sunDiagEpoch(bool map, unsigned source) {
    if (!map && (!g_sunDiagOn || g_sunDiagEpochsThisMap >= kSunDiagEpochsPerMap)) return;
    int tries = 0;
    while (!sunDiagLock()) {
        if (++tries > 100) return; // another thread is stuck in its log line: this epoch goes unlogged
        Sleep(0);
    }
    reportRgbCallers();
    g_sunDiagEpoch++;
    g_sunDiagEpochsThisMap = map ? 0 : g_sunDiagEpochsThisMap + 1;
    g_sunDiagOn = true;
    g_sunDiagTick = GetTickCount();
    g_sunFlareLogged = 0;
    g_rgbCallerCount = 0;
    g_rgbOtherCalls = 0;
    TMVS_LOG("engine: sun diag: epoch %ld: %s", g_sunDiagEpoch, map ? "scene load" : source == 1 ? "cinematic on (clip player)" : source == 2 ? "cinematic on (clip viewer)" : "cinematic on (video export)");
    sunDiagUnlock();
}

// The lens flares' sun light (and the weather's) at the first sun flare of an epoch and 2 s later.
void sunDiagFlare(const void* corpusLight) {
    if (!g_sunDiagOn || g_sunFlareLogged >= 2) return;
    const DWORD age = GetTickCount() - g_sunDiagTick;
    if (g_sunFlareLogged == 1 && age < 2000) return;
    if (!sunDiagLock()) return;
    if (g_sunFlareLogged >= 2) {
        sunDiagUnlock();
        return;
    }
    const char* when = g_sunFlareLogged == 0 ? "first sun flare" : "+2 s";
    g_sunFlareLogged++;
    const void* item = nullptr;
    const void* light = nullptr;
    __try {
        item = *reinterpret_cast<const void* const*>(static_cast<const BYTE*>(corpusLight) + 0x48);
        light = item ? *reinterpret_cast<const void* const*>(static_cast<const BYTE*>(item) + 0x88) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_sunFlareLight = light;
    const void* weatherGx = weatherLight();
    char sun[192], weather[192];
    describeLight(light, sun, sizeof(sun));
    if (weatherGx) describeLight(weatherGx, weather, sizeof(weather));
    else strcpy_s(weather, "gx none");
    float async = -1.0f, timed = -1.0f, speed = 0.0f;
    int mode = 0;
    const bool clock = g_weatherVtable && weatherClock(async, timed, mode, speed);
    TMVS_LOG("engine: sun diag: epoch %ld %s (%u ms, cinematic %d): flare light %p item %p %s", g_sunDiagEpoch, when, age, cinematicActive() ? 1 : 0,
             corpusLight, item, sun);
    TMVS_LOG("engine: sun diag: epoch %ld %s: weather %p %s, clock %.3f/%.3f%s; %s", g_sunDiagEpoch, when, g_weather, weather, timed, async,
             clock ? "" : " (not known)", light && light == weatherGx ? "SAME light" : "DIFFERENT lights");
    sunDiagUnlock();
}

// Who sets a light's colour: the first distinct callers of the epoch, then only counted.
void sunDiagBaseRgb(uintptr_t ret, const void* light, const float* rgb) {
    if (!g_sunDiagOn || !sunDiagLock()) return;
    for (int i = 0; i < g_rgbCallerCount; i++) {
        if (g_rgbCallers[i].ret == ret) {
            g_rgbCallers[i].calls++;
            sunDiagUnlock();
            return;
        }
    }
    if (g_rgbCallerCount >= kSunDiagCallers) {
        g_rgbOtherCalls++;
        sunDiagUnlock();
        return;
    }
    g_rgbCallers[g_rgbCallerCount++] = {ret, 1};
    char where[96];
    describeCode(ret, where, sizeof(where));
    float c[3] = {-1.0f, -1.0f, -1.0f};
    __try {
        if (rgb && readable(rgb, sizeof(c))) memcpy(c, rgb, sizeof(c));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    const void* weatherGx = weatherLight();
    TMVS_LOG("engine: sun diag: epoch %ld SetBaseRGB from %s: gx %p rgb %.3f %.3f %.3f%s%s", g_sunDiagEpoch, where, light, c[0], c[1], c[2],
             light && light == weatherGx ? " (the weather's light)" : "", light && light == g_sunFlareLight ? " (the flares' sun)" : "");
    sunDiagUnlock();
}

void __fastcall baseRgbDetour(void* light, void* edx, const float* rgb) {
    sunDiagBaseRgb(reinterpret_cast<uintptr_t>(_ReturnAddress()), light, rgb);
    g_baseRgb(light, edx, rgb);
}

void markCinematic(unsigned source) {
    const DWORD now = GetTickCount();
    if (g_lastCinematic == 0 || now - g_lastCinematic >= 500) sunDiagEpoch(false, source);
    g_lastCinematic = now;
    g_cinematicSources |= source;
    if (source != 1) g_lastNotClip = g_lastCinematic;
}

void __fastcall tracksUpdateDetour(void* self, void* edx, float time, float delta) {
    markCinematic(1);
    g_clipPlayer = self; // the car its cameras show (carPose)
    g_clipPlayerFrame = g_engineFrames;
    g_tracksUpdate(self, edx, time, delta);
}

void __fastcall updateCamsDetour(void* self, void* edx) {
    markCinematic(2);
    g_updateCams(self, edx);
}

// Runs once per frame while the export shoots (a fiber that is resumed every frame).
void __fastcall doShootDetour(void* self, void* edx, void* fiber) {
    markCinematic(4);
    g_shooter = self;
    g_shootFrame = g_engineFrames;
    g_doShoot(self, edx, fiber);
}

// --- The video export's file ------------------------------------------------

BYTE* aviData(void* self) {
    BYTE* const* data = reinterpret_cast<BYTE* const*>(static_cast<BYTE*>(self) + 4);
    return readable(data, sizeof(void*)) && *data && readable(*data, 0x64) ? *data : nullptr;
}

int __fastcall aviOpenDetour(void* self, void* edx, const void* name) {
    const int ok = g_aviOpen(self, edx, name);
    // CFastStringInt: +0 length, +4 the text.
    const wchar_t* const* text = reinterpret_cast<const wchar_t* const*>(static_cast<const BYTE*>(name) + 4);
    if (g_videoFile.opened && readable(text, sizeof(void*)) && *text && readable(*text, sizeof(wchar_t))) g_videoFile.opened(self, *text, ok != 0);
    return ok;
}

// The mod writes the video itself: the game's stream stays closed (no codec dialog), but its
// data looks as after a successful open (the frame buffer is sized from the image size).
int __fastcall aviVideoOpenDetour(void* self, void* edx, DWORD* width, DWORD* height, float fps, int alpha) {
    BYTE* data = aviData(self);
    if (g_videoFile.videoOpen && data && width && height) {
        const DWORD w = (*width + 7) / 8 * 8, h = (*height + 7) / 8 * 8, bpp = alpha ? 4 : 3;
        if (g_videoFile.videoOpen(self, static_cast<int>(w), static_cast<int>(h), fps, static_cast<int>(bpp))) {
            *width = w;
            *height = h;
            BITMAPINFOHEADER header{};
            header.biSize = sizeof(header);
            header.biWidth = static_cast<LONG>(w);
            header.biHeight = static_cast<LONG>(h);
            header.biPlanes = 1;
            header.biBitCount = static_cast<WORD>(bpp * 8);
            header.biCompression = BI_RGB;
            header.biSizeImage = w * h * bpp;
            memcpy(data + 0x0c, &header, sizeof(header));
            *reinterpret_cast<DWORD*>(data + 0x40) = 0;
            return 1;
        }
    }
    return g_aviVideoOpen(self, edx, width, height, fps, alpha);
}

int __fastcall aviVideoWriteDetour(void* self, void* edx, void* image) {
    if (g_videoFile.videoFrame && image && g_videoFile.videoFrame(self, image)) {
        if (BYTE* data = aviData(self)) ++*reinterpret_cast<DWORD*>(data + 0x40);
        return 1;
    }
    return g_aviVideoWrite(self, edx, image);
}

int __fastcall aviAudioOpenDetour(void* self, void* edx, const void* format) {
    const int ok = g_aviAudioOpen(self, edx, format);
    if (g_videoFile.audioOpen && format && readable(format, 16)) g_videoFile.audioOpen(self, format);
    return ok;
}

int __fastcall aviAudioWriteDetour(void* self, void* edx, void* data, DWORD blocks) {
    const int ok = g_aviAudioWrite(self, edx, data, blocks);
    if (g_videoFile.audioData && data) g_videoFile.audioData(self, data, blocks);
    return ok;
}

void __fastcall aviDestroyDetour(void* self, void* edx) {
    g_aviDestroy(self, edx); // closes the .avi if it is still open
    if (g_videoFile.closed) g_videoFile.closed(self);
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
//   Soil ...                         the terrain's dirt (Stadium: SoilGen21, SoilFix, SoilFixToGen21:
//                                    the dirt ground, platforms and dirt roads)
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
    if (!_wcsnicmp(file, L"Soil", 4)) return Material::Dirt;
    const wchar_t* sea = wcsstr(file, L"Sea");
    if ((sea && sea[3] != L't') || wcsstr(file, L"Water")) return Material::Water; // not "Seat..."
    return Material::Other;
}

// The textures a material draws with. Every block material is its own copy of a shared shader
// file (CPlugMaterial::DuplicateShaderFromMaterialModel: "TDiff_Spec_Nrm ... .Shader.Gbx?113")
// with the material's own bitmaps put in: CPlugShaderApply's texture list (AddTextureApply: +0x94
// the count, +0x98 the CPlugBitmapApply*), each holding its CPlugBitmap at +0x1c
// (CPlugBitmapSampler::SetBitmap). The same offsets in both builds; only a CPlugShaderApply (its
// vtable, from the build's table) is read. Returns the number of bitmaps, -1 for another class.
constexpr size_t kShaderTextureCount = 0x94;
constexpr size_t kShaderTextures = 0x98;
constexpr size_t kSamplerBitmap = 0x1c;
int shaderBitmaps(const void* shader, const void** out, int max) {
    if (!g_build || !g_build->shaderApplyVTable) return -1;
    __try {
        const BYTE* s = static_cast<const BYTE*>(shader);
        if (*reinterpret_cast<const uintptr_t*>(s) != rebase(g_build->shaderApplyVTable)) return -1;
        const uint32_t count = *reinterpret_cast<const uint32_t*>(s + kShaderTextureCount);
        const BYTE* const* list = *reinterpret_cast<const BYTE* const* const*>(s + kShaderTextures);
        if (count > 64 || (count && !list)) return 0;
        int n = 0;
        for (uint32_t i = 0; i < count && n < max; i++) {
            const void* bitmap = list[i] ? *reinterpret_cast<const void* const*>(list[i] + kSamplerBitmap) : nullptr;
            if (bitmap) out[n++] = bitmap;
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The neon strips: plain block material to the game (TDiff_Spec_Nrm, lit like the road, nothing
// added on top: the same class and shader file as the lane paint and the start banner's letters),
// their blue is painted into the textures of two Stadium materials: the road borders' walls
// (StadiumRoadBorderD, the one texture with its own night and sunset versions in Moods\) and the
// curbs (StadiumRoadCircuitBorderD). The material decides, not the colour: a blue floor or a blue
// letter is no strip however bright it is. Within the material its colour still tells the blue
// strip from the grey and orange of the same texture.
bool neonTexture(const wchar_t* path) {
    const wchar_t* file = wcsrchr(path, L'\\');
    const wchar_t* slash = wcsrchr(path, L'/');
    if (slash > file) file = slash;
    file = file ? file + 1 : path;
    return !_wcsnicmp(file, L"StadiumRoadBorder", 17) || !_wcsnicmp(file, L"StadiumRoadCircuitBorder", 24);
}

volatile bool g_textureNames = false; // a material's texture had a file name (neon by material works)

uintptr_t vtableOf(const void* nod) {
    __try {
        return *static_cast<const uintptr_t*>(nod);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Whether a material draws with a neon strip texture. Logs each neon texture once per map
// ("engine: neon material ..."), and once when the textures can't be read.
bool neonMaterial(const void* shader) {
    const void* bitmaps[16] = {};
    const int n = shaderBitmaps(shader, bitmaps, 16);
    static bool s_loggedClass = false;
    if (n < 0 && !s_loggedClass && g_build && g_build->shaderApplyVTable) {
        s_loggedClass = true;
        TMVS_LOG("engine: material %p is no CPlugShaderApply (vtable %08x), its textures are not read", shader, static_cast<unsigned>(vtableOf(shader)));
    }
    static std::vector<std::wstring> s_logged;
    static LONG s_loggedLoads = -1;
    if (s_loggedLoads != g_mapLoads) {
        s_logged.clear();
        s_loggedLoads = g_mapLoads;
    }
    bool neon = false;
    for (int i = 0; i < n; i++) {
        wchar_t path[512] = {};
        if (!nodFilePath(bitmaps[i], path, 512)) continue;
        if (!g_textureNames) {
            g_textureNames = true;
            TMVS_LOG("engine: materials' textures readable (%ls), neon strips by their material", path);
        }
        if (!neonTexture(path)) continue;
        neon = true;
        if (s_logged.size() < 32 && std::find(s_logged.begin(), s_logged.end(), path) == s_logged.end()) {
            s_logged.push_back(path);
            TMVS_LOG("engine: neon material %p: %ls", shader, path);
        }
    }
    return neon;
}

// The file names of a material's textures, "; " between them (F12's capture_NNN.mat).
bool textureFiles(const void* shader, wchar_t* out, size_t size) {
    const void* bitmaps[16] = {};
    const int n = shaderBitmaps(shader, bitmaps, 16);
    size_t used = 0;
    out[0] = L'\0';
    for (int i = 0; i < n; i++) {
        wchar_t path[512] = {};
        if (!nodFilePath(bitmaps[i], path, 512)) continue;
        const size_t length = wcslen(path);
        if (used + length + 3 >= size) break;
        if (used) {
            wcscpy_s(out + used, size - used, L"; ");
            used += 2;
        }
        wcscpy_s(out + used, size - used, path);
        used += length;
    }
    return used > 0;
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
    Material m = named ? classify(path) : Material::Other;
    if (m == Material::Other && neonMaterial(shader)) m = Material::Neon;
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
    // The block this draw belongs to gives the device wrapper its depth bias rank (blockorder.h).
    const int previousRank = blockorder::beginDraw(corpus, false);
    if (!(g_hideGameSky && isPaintedHorizon(shader))) g_drawShader(self, edx, shader, corpus, tree, visual);
    blockorder::endDraw(previousRank);
    g_currentShader = previous;
}

void __fastcall drawPassDetour(void* self, void* edx, void* delayed) {
    const void* previous = g_currentShader;
    g_currentShader = *reinterpret_cast<void* const*>(static_cast<const BYTE*>(delayed) + kDelayedRenderShader);
    if (g_shaderCensus) censusShader(g_currentShader);
    const int previousRank = blockorder::beginDraw(delayed, true);
    if (!(g_hideGameSky && isPaintedHorizon(g_currentShader))) g_drawPass(self, edx, delayed);
    blockorder::endDraw(previousRank);
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
    // Counted for the log (the sun came back in end-of-race replays: is it its flare?).
    const bool sun = isSunLight(light);
    g_flares[sun ? 1 : 0]++;
    if (sun) sunDiagFlare(light);
    if (g_hideSunFlare && testVisible && sun) {
        testVisible = 0;
        g_flares[2]++;
    }
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

int __fastcall removeBlockDetour(void* self, void* edx, void* block) {
    InterlockedIncrement(&g_blocksRemoved);
    blockorder::invalidate();
    return g_removeBlock(self, edx, block);
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

// Once a map: how often keepNight put the weather back (a replay or a clip that keeps setting
// the clock would otherwise jump it unseen once the drift log's 20 lines are used up).
void reportNightJumps() {
    if (g_nightJumps > 0) TMVS_LOG("engine: real night kept: the weather was put back to midnight %d times on the last map", g_nightJumps);
    g_nightJumps = 0;
}

void __fastcall setChallengeDetour(void* self, void* edx, void* challenge) {
    g_app = self;
    // Another map (or none: back to the menu): the objects of the last one go with it. Not the
    // weather, time of day and forced midnight of the map just loaded (its load came first).
    g_freeCam = nullptr;
    g_racePlayer = nullptr;
    g_clipPlayer = nullptr;
    if (!challenge || challenge != g_sceneChallenge) {
        reportNightJumps();
        g_weather = nullptr;
        g_nightForced = false;
        g_mapDayTime = -1.0f;
        g_sceneChallenge = nullptr;
    }
    g_setChallenge(self, edx, challenge);
    blockorder::setChallenge(challenge);
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
    g_loadingChallenge = self;
    g_loadDecoration(self, edx, forcedMods);
    g_loadingChallenge = nullptr;
    readMapComments(self);
    const void* collection = static_cast<const BYTE*>(self) + 0xc8;
    if (!readable(collection, sizeof(uint32_t))) return;
    const char* name = reinterpret_cast<IdGetStringFn>(rebase(g_build->idGetString))(collection, nullptr);
    if (!name || !readable(name, 1)) return;
    if (strncmp(name, g_environment, sizeof(g_environment)) != 0) TMVS_LOG("engine: environment %s", name);
    strncpy_s(g_environment, name, _TRUNCATE);
}

// The weather's clock as a fraction of the day: its two times (async, timed). False when the
// weather isn't the one that jumped last (gone, or another object at its address).
bool weatherClock(float& async, float& timed, int& mode, float& speed) {
    __try {
        const BYTE* weather = static_cast<const BYTE*>(g_weather);
        if (!weather || !readable(weather, kWeatherTimer + 4) || *reinterpret_cast<const void* const*>(weather) != g_weatherVtable) return false;
        const BYTE* timer = *reinterpret_cast<const BYTE* const*>(weather + kWeatherTimer);
        if (!timer || !readable(timer, 0x2c)) return false;
        const int period = *reinterpret_cast<const int*>(timer + 0x20);
        if (period <= 0) return false;
        async = static_cast<float>(*reinterpret_cast<const unsigned*>(timer + 0x18) % static_cast<unsigned>(period)) / static_cast<float>(period);
        timed = static_cast<float>(*reinterpret_cast<const unsigned*>(timer + 0x1c) % static_cast<unsigned>(period)) / static_cast<float>(period);
        mode = *reinterpret_cast<const int*>(timer + 0x14);
        speed = *reinterpret_cast<const float*>(timer + 0x28);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void noteNightClock() {
    __try {
        g_weatherVtable = g_weather && readable(g_weather, 4) ? *reinterpret_cast<const void* const*>(g_weather) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_weatherVtable = nullptr;
    }
    float async = 0.0f, timed = 0.0f, speed = 0.0f;
    int mode = 0;
    const bool known = weatherClock(async, timed, mode, speed);
    g_nightClock = known ? timed : -1.0f;
    g_nightClockAsync = known ? async : -1.0f;
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
    if (g_nightForced) noteNightClock();
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

// The moon's direction, as the game passes it to SetDOV: replaced in place, so the specular cube
// (built from the same vector afterwards) follows.
void* __fastcall moonDirectionDetour(void* matrix, void* edx, float* direction, int flag) {
    if (g_moonMode != 0 && direction) {
        __try {
            memcpy(g_moonGameTravel, direction, sizeof(g_moonGameTravel));
            if (g_moonMode == 1) memcpy(direction, g_moonTravel, sizeof(g_moonTravel));
            g_moonApplied = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return g_setDov(matrix, edx, direction, flag);
}

// The moon's colour: none with the moon off (the light's intensity and flags stay).
void* __fastcall moonColorDetour(void* light, void* edx, const float* rgb) {
    static const float kNone[3] = {};
    if (g_moonMode != 0 && rgb && !g_moonLogged) {
        g_moonLogged = true;
        __try {
            TMVS_LOG("engine: the game's moon (colour %.2f %.2f %.2f, towards %.2f %.2f %.2f) %s", rgb[0], rgb[1], rgb[2], -g_moonGameTravel[0],
                     -g_moonGameTravel[1], -g_moonGameTravel[2], g_moonMode == 1 ? "shines from the black hole" : "is off");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (g_moonMode == 2) {
        g_moonApplied = true;
        return g_setBaseRgb(light, edx, kNone);
    }
    return g_setBaseRgb(light, edx, rgb);
}

int __fastcall cacheUpToDateDetour(void* self, void* edx) {
    if (g_moonMode == 0) return g_cacheUpToDate(self, edx);
    if (!g_cacheLogged) {
        g_cacheLogged = true;
        TMVS_LOG("engine: lightmap cache not used: the game bakes this map's light with the changed moon (and doesn't save it)");
    }
    return 0;
}

int __fastcall cacheSaveDetour(void* self, void* edx) {
    // Not g_moonMode: real night switched off during the map clears it (relightNight), and a save
    // after that would store the changed moon's bake as the map's own.
    return g_moonBaked ? 0 : g_cacheSave(self, edx);
}

// EngineMoon, once per map load, before the scene (and with it the lightmap) is made: the
// lightmap is baked once, so the moon stays as decided here for the whole map.
void decideMoon(float dayTime, bool litAtNight) {
    g_moonMode = 0;
    g_moonApplied = false;
    g_moonBaked = false;
    g_moonLogged = false;
    g_cacheLogged = false;
    MoonLight moon;
    if (!g_moonAtLoad || !g_moonAtLoad(dayTime, litAtNight, moon) || moon.mode < 1 || moon.mode > 2) return;
    if (!g_setDov || !g_setBaseRgb || !g_cacheUpToDate || !g_cacheSave) {
        TMVS_LOG("engine: the moon can't be changed in this build (hooks missing), the game's moon stays");
        return;
    }
    const float len = sqrtf(moon.toward[0] * moon.toward[0] + moon.toward[1] * moon.toward[1] + moon.toward[2] * moon.toward[2]);
    if (!(len > 1e-4f && len < 1e4f)) return;
    for (int i = 0; i < 3; i++) g_moonTravel[i] = -moon.toward[i] / len;
    g_moonMode = moon.mode;
    g_moonBaked = true;
    if (moon.mode == 1)
        TMVS_LOG("engine: this map's moon shines from the black hole (towards %.3f %.3f %.3f)", -g_moonTravel[0], -g_moonTravel[1], -g_moonTravel[2]);
    else
        TMVS_LOG("engine: this map's moon is off (ambient and lamps stay)");
}

void __fastcall loadSceneDetour(void* self, void* edx) {
    const BYTE* decoration = static_cast<const BYTE*>(self);
    void* mood = readable(decoration + kDecorationMood, sizeof(void*)) ? *reinterpret_cast<void* const*>(decoration + kDecorationMood) : nullptr;
    float* time = mood && readable(static_cast<BYTE*>(mood) + kMoodDayTime, sizeof(float)) ? reinterpret_cast<float*>(static_cast<BYTE*>(mood) + kMoodDayTime) : nullptr;
    // A map's load (null: the editor changing the decoration, a menu scene).
    g_sceneChallenge = g_loadingChallenge;
    sunDiagEpoch(true, 0);
    if (!time || !(*time >= 0.0f && *time <= 1.0f)) {
        // Nothing of the last map carries over (its forced midnight, its time of day).
        g_weather = nullptr;
        g_nightForced = false;
        g_mapDayTime = -1.0f;
        decideMoon(-1.0f, false);
        g_loadScene(self, edx);
        return;
    }
    const float original = *time;
    g_mapDayTime = original;
    g_weather = nullptr; // the last map's (set again by the jumps of this load)
    g_nightForced = g_wantNight && g_wantNight(original);
    TMVS_LOG("engine: map time of day %.3f%s", original, g_nightForced ? ", lit at midnight (night sky)" : "");
    if (g_nightForced) *time = 0.0f;
    decideMoon(original, g_nightForced);
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
    g_engineFrames++;
    if (g_callbacks.frameBegin) g_callbacks.frameBegin();
    return g_frameBegin(self, edx);
}

void __fastcall cameraBeginDetour(void* self, void* edx, void* camera, void* clearDesc) {
    g_camera.camera = camera;
    g_camera.hasView = false;
    g_camera.hasProjection = false;
    // This camera's matrices, computed just before its begin (both paths); the next camera
    // computes its own.
    g_beginMatrices = g_lastViewSet && g_lastProjectionSet;
    if (g_beginMatrices) {
        g_beginView = g_lastView;
        g_beginProjection = g_lastProjection;
    }
    g_lastViewSet = g_lastProjectionSet = false;
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

// The game's read-back of a video frame (CVisionViewportDx9::RenderFrameEnd, the same code in both
// builds): when DoShoot flagged the frame (+0x54c) and the stream is open (+0x554) it copies its
// colour surface to system memory (GetRenderTargetData: the GPU's whole frame must be done first,
// 4 bytes a pixel over the bus, 33 MB at 4K), converts it on the CPU into the bottom-up BGR buffer
// (CopySurfaceToImage) and hands that to VideoStreamWrite. With +0x150 set it spreads the three
// steps over three flagged frames by a global phase: 0 read-back, 1 convert, 2 write. When the
// mod hands over its own frame the game's pixels go nowhere: +0x150 and phase 2 for that one call
// skip straight to the write (aviVideoWriteDetour takes the mod's frame), both restored after.
// The .dds path (+0x558) and a frame without the stream are left alone.
constexpr size_t kViewportVideoAsync = 0x150;
constexpr size_t kViewportVideoFile = 0x160;   // CVisionVideoAvi*
constexpr size_t kViewportVideoAdd = 0x54c;    // VideoAddCurrentFrame
constexpr size_t kViewportVideoStream = 0x554; // the stream is open
constexpr size_t kViewportVideoDds = 0x558;
constexpr size_t kReadPhaseCheck = 0x322;      // mov dword ptr [phase], ebp (stream open)
volatile LONG* g_readPhase = nullptr;          // checked at install, null: never skipped
unsigned g_readbacksSkipped = 0;

bool skipGameReadback(void* viewport) {
    if (!g_readPhase || !g_videoFile.ownFrameReady) return false;
    BYTE* v = static_cast<BYTE*>(viewport);
    if (!readable(v + kViewportVideoAsync, kViewportVideoDds + sizeof(int) - kViewportVideoAsync)) return false;
    const auto at = [v](size_t offset) { return reinterpret_cast<int*>(v + offset); };
    if (*at(kViewportVideoAdd) == 0 || *at(kViewportVideoStream) == 0 || *at(kViewportVideoDds) != 0 || *at(kViewportVideoAsync) != 0 ||
        *g_readPhase != 0)
        return false;
    void* file = *reinterpret_cast<void**>(v + kViewportVideoFile);
    if (!file || !g_videoFile.ownFrameReady(file)) return false;
    *at(kViewportVideoAsync) = 1;
    *g_readPhase = 2;
    if (g_readbacksSkipped++ == 0)
        TMVS_LOG("engine: the game's read-back of its video frames skipped (GetRenderTargetData and the CPU conversion): the mod hands over its own");
    return true;
}

void __fastcall frameEndDetour(void* self, void* edx) {
    if (g_callbacks.frameEnd) g_callbacks.frameEnd();
    // After the plugin's frameEnd: its frame for this write is finished by then.
    const bool skipped = skipGameReadback(self);
    g_frameEnd(self, edx);
    if (skipped) {
        *reinterpret_cast<int*>(static_cast<BYTE*>(self) + kViewportVideoAsync) = 0;
        *g_readPhase = 0; // the write wrapped it to 0 already
    }
}

// SHmsCameraLocation: +0x00 GmIso4 camera location, +0x60 GmMat4 driver view matrix.
void __fastcall viewMatrixDetour(void* self, void* edx, void* location) {
    g_viewMatrix(self, edx, location);
    const BYTE* base = static_cast<const BYTE*>(location);
    memcpy(g_camera.location, base, sizeof(g_camera.location));
    memcpy(&g_camera.view, base + 0x60, sizeof(Matrix4));
    g_camera.hasView = true;
    memcpy(&g_lastView, base + 0x60, sizeof(Matrix4)); // what SetTransform(D3DTS_VIEW) gets
    g_lastViewSet = true;
}

// The game's supersampling sample this camera is being rendered for (index, count), false when
// it has none. The projection is computed after CHmsCamera::ZClipCompute and before the render's
// RenderCameraBegin (RenderCameraNormal, TransformStackCameraPush), so the fields are those of
// the render about to start.
bool gameSampleOf(const void* camera, int& index, int& count) {
    const BYTE* c = static_cast<const BYTE*>(camera);
    if (!c || !readable(c + kCameraSuperSample, 5 * sizeof(int))) return false;
    const int* s = reinterpret_cast<const int*>(c + kCameraSuperSample);
    if (s[0] == 0 || s[3] < 1 || s[4] < 1 || s[3] > 11 || s[4] > 11 || s[1] < 0 || s[2] < 0 || s[1] >= s[3] || s[2] >= s[4]) return false;
    index = s[2] * s[3] + s[1];
    count = s[3] * s[4];
    return true;
}

// The lens offset on a perspective projection (after zoom, tiles and jitter, so with their
// scale). D3D row vectors, clip = [x y z 1] P, view space z forward in metres (P23 = 1 makes
// w = z). The eye moved by a and the image re-aimed so the plane z = F stays put means
// x -> x + a.x (z / F - 1), y likewise: row 2 gains (a.x row 0 + a.y row 1) / F, row 3 loses
// a.x row 0 + a.y row 1. Every column, so nothing is assumed about the other terms.
void applyLens(float* m, const void* camera) {
    int index = -1, count = 0;
    g_gameSample[0] = -1;
    g_gameSample[1] = 0;
    const bool game = gameSampleOf(camera, index, count);
    if (game) {
        g_gameSample[0] = index;
        g_gameSample[1] = count;
    }
    g_lensUsed = -1;
    if (g_lensCount <= 0 || !(g_lensFocus > 0.0f)) return;
    const int entry = game ? index % g_lensCount : static_cast<int>(g_lensRenders++ % static_cast<unsigned>(g_lensCount));
    const float ax = g_lensXY[entry][0], ay = g_lensXY[entry][1];
    g_lensUsed = entry;
    if (ax == 0.0f && ay == 0.0f) return;
    if (!g_loggedLens) {
        g_loggedLens = true;
        TMVS_LOG("engine: lens offsets on the main camera's projection (%d of them, focus %.2f m, %s)", g_lensCount, g_lensFocus,
                 game ? "one per sample of the game's supersampling" : "one per render in turn");
    }
    float* p = m + 16;
    for (int c = 0; c < 4; c++) {
        const float shift = ax * p[c] + ay * p[4 + c];
        p[8 + c] += shift / g_lensFocus;
        p[12 + c] -= shift;
        m[c * 4 + 2] = p[8 + c]; // the engine's GmMat4 (the transpose) at m[0]
        m[c * 4 + 3] = p[12 + c];
    }
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
    if (main && m[14] == 1.0f && m[16 + 11] == 1.0f) applyLens(m, camera);
    // The D3D layout, as the header says (m[0] holds the engine's GmMat4, its transpose).
    memcpy(&g_camera.projection, m + 16, sizeof(Matrix4));
    g_camera.hasProjection = true;
    memcpy(&g_lastProjection, m + 16, sizeof(Matrix4)); // the D3D layout SetTransform(D3DTS_PROJECTION) gets
    g_lastProjectionSet = true;
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
    g_removeBlock = reinterpret_cast<RemoveBlockFn>(inlineHook(kRemoveBlockSite, reinterpret_cast<void*>(&removeBlockDetour)));
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
    g_cacheUpToDate = reinterpret_cast<CacheFn>(inlineHook(kCacheUpToDateSite, reinterpret_cast<void*>(&cacheUpToDateDetour)));
    g_cacheSave = reinterpret_cast<CacheFn>(inlineHook(kCacheSaveSite, reinterpret_cast<void*>(&cacheSaveDetour)));
    g_baseRgb = reinterpret_cast<BaseRgbFn>(inlineHook(kBaseRgbSite, reinterpret_cast<void*>(&baseRgbDetour)));
    // The export's file: all six or none (a frame written without the open would go nowhere).
    {
        const struct {
            const InlineSite* site;
            void* detour;
            void** original;
        } avi[] = {{&kAviOpenSite, reinterpret_cast<void*>(&aviOpenDetour), reinterpret_cast<void**>(&g_aviOpen)},
                   {&kAviVideoOpenSite, reinterpret_cast<void*>(&aviVideoOpenDetour), reinterpret_cast<void**>(&g_aviVideoOpen)},
                   {&kAviVideoWriteSite, reinterpret_cast<void*>(&aviVideoWriteDetour), reinterpret_cast<void**>(&g_aviVideoWrite)},
                   {&kAviAudioOpenSite, reinterpret_cast<void*>(&aviAudioOpenDetour), reinterpret_cast<void**>(&g_aviAudioOpen)},
                   {&kAviAudioWriteSite, reinterpret_cast<void*>(&aviAudioWriteDetour), reinterpret_cast<void**>(&g_aviAudioWrite)},
                   {&kAviDestroySite, reinterpret_cast<void*>(&aviDestroyDetour), reinterpret_cast<void**>(&g_aviDestroy)}};
        bool all = true;
        for (const auto& a : avi) all = all && prologueMatches(*a.site);
        if (all) {
            for (const auto& a : avi) *a.original = inlineHook(*a.site, a.detour);
            // The read-back's phase: only where RenderFrameEnd writes exactly this global.
            const BYTE* check = reinterpret_cast<const BYTE*>(rebase(g_build->slots[kFrameEnd]) + kReadPhaseCheck);
            const uint32_t phase = static_cast<uint32_t>(rebase(g_build->videoReadPhase));
            if (readable(check, 6) && check[0] == 0x89 && check[1] == 0x2d && memcmp(check + 2, &phase, 4) == 0)
                g_readPhase = reinterpret_cast<volatile LONG*>(rebase(g_build->videoReadPhase));
            else
                TMVS_LOG("engine: RenderFrameEnd's read-back phase not where expected: the game's read-back stays");
        } else {
            TMVS_LOG("engine: the video export's file writer does not match this executable: MP4 export off");
        }
    }
    g_setDov = reinterpret_cast<SetDovFn>(patchCall(g_build->moonDirectionCall, g_build->setDov, reinterpret_cast<void*>(&moonDirectionDetour),
                                                    "CMotionManagerWeathers::UpdateAsync (moon direction)"));
    g_setBaseRgb = reinterpret_cast<SetBaseRgbFn>(patchCall(g_build->moonColorCall, g_build->setBaseRgb, reinterpret_cast<void*>(&moonColorDetour),
                                                            "CMotionManagerWeathers::UpdateAsync (moon colour)"));
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

void setMoonAtLoad(bool (*decide)(float dayTime, bool litAtNight, MoonLight& moon)) {
    g_moonAtLoad = decide;
}

bool moonReplaced() {
    return g_moonMode != 0 && g_moonApplied;
}

bool relightNight(bool night) {
    if (night == g_nightForced) return true;
    if (!g_jumpTime || !g_weather || g_weatherTime < 0.0f || g_mapDayTime < 0.0f || !readable(g_weather, 0x30)) return false;
    g_nightForced = night;
    // The lightmap stays as it was baked: a moon changed at load ends with the night (back to
    // the game's light by day); one switched on now is the game's own until the next map.
    if (!night && g_moonMode != 0) {
        g_moonMode = 0;
        g_moonApplied = false;
    }
    TMVS_LOG("engine: real night %s during the map: weather %p to time of day %.3f", night ? "on" : "off", g_weather,
             night ? 0.0f : g_weatherTime);
    __try {
        g_jumpTime(g_weather, nullptr, night ? 0.0f : g_weatherTime);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_weather = nullptr;
        return false;
    }
    if (night) noteNightClock();
    else g_nightClock = -1.0f;
    return true;
}

bool keepNight(bool fix) {
    if (!g_nightForced || !g_jumpTime || g_nightClock < 0.0f) return false;
    float async = 0.0f, timed = 0.0f, speed = 0.0f;
    int mode = 0;
    if (!weatherClock(async, timed, mode, speed)) return false;
    // Around the clock: 0.99 and 0.01 are 0.02 apart.
    auto apart = [](float a, float b) {
        const float d = fabsf(a - b);
        return d < 0.5f ? d : 1.0f - d;
    };
    const float movedTimed = apart(timed, g_nightClock), movedAsync = g_nightClockAsync >= 0.0f ? apart(async, g_nightClockAsync) : 0.0f;
    const float moved = movedTimed > movedAsync ? movedTimed : movedAsync;
    if (moved < 0.01f) return false; // a quarter of an hour of the game's day
    // At most one jump a second: a clock that doesn't come back within 0.01 (or a replay that keeps
    // setting it) would otherwise jump the weather every frame.
    const DWORD now = GetTickCount();
    if (fix && g_nightJumps > 0 && now - g_nightJumpTick < 1000) return false;
    if (g_nightLogged < 20) {
        g_nightLogged++;
        TMVS_LOG("engine: the weather's time of day moved away from the forced midnight (%.3f -> timed %.3f, async %.3f; clock mode %d, speed %.3f, "
                 "cinematic %d)%s",
                 g_nightClock, timed, async, mode, speed, cinematicActive() ? 1 : 0, fix ? ": back to midnight" : "");
    }
    if (!fix) return false;
    __try {
        g_jumpTime(g_weather, nullptr, 0.0f); // the original (trampoline): the detour doesn't see it
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_weather = nullptr;
        return false;
    }
    // Where the clocks are now is the midnight to keep (the next frame compares with this).
    noteNightClock();
    g_nightJumps++;
    g_nightJumpTick = now;
    return true;
}

void flareCounts(unsigned& other, unsigned& sun, unsigned& hidden) {
    other = static_cast<unsigned>(InterlockedExchange(&g_flares[0], 0));
    sun = static_cast<unsigned>(InterlockedExchange(&g_flares[1], 0));
    hidden = static_cast<unsigned>(InterlockedExchange(&g_flares[2], 0));
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
    // Once per map: the first water plane (my log tells which maps have one).
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

bool carLocation(float position[3], float up[3]) {
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
        if (up) {
            // The car's up is one column of the 3x3 (the model's y, column 1, if TMNF is like
            // ManiaPlanet). Which one is taken from the car standing level (at the start: one
            // column within 18 degrees of the world's up), its sign with it; until then the world's
            // up. Picked again on every map, and only once the same column and sign held for 30
            // frames: a car on its roof the moment it was first seen made it upside down for the
            // whole session. Logged when it differs from the last pick.
            static int s_axis = -1, s_logged = -1;
            static float s_sign = 1.0f;
            static LONG s_loads = -1;
            static int s_candidate = -1, s_frames = 0;
            if (s_loads != g_mapLoads) {
                s_loads = g_mapLoads;
                s_axis = s_candidate = -1;
                s_frames = 0;
            }
            float column[3][3];
            float length[3];
            for (int c = 0; c < 3; c++) {
                for (int r = 0; r < 3; r++) column[c][r] = iso[r * 3 + c];
                length[c] = sqrtf(column[c][0] * column[c][0] + column[c][1] * column[c][1] + column[c][2] * column[c][2]);
            }
            if (s_axis < 0) {
                int level = -1; // column * 2 + (negated)
                for (int c = 0; c < 3 && level < 0; c++) {
                    if (length[c] > 1e-3f && fabsf(column[c][1]) > 0.95f * length[c]) level = c * 2 + (column[c][1] < 0.0f ? 1 : 0);
                }
                s_frames = level >= 0 && level == s_candidate ? s_frames + 1 : (level >= 0 ? 1 : 0);
                s_candidate = level;
                if (s_frames >= 30) {
                    s_axis = level / 2;
                    s_sign = (level & 1) ? -1.0f : 1.0f;
                    if (level != s_logged) {
                        s_logged = level;
                        const int c = s_axis;
                        TMVS_LOG("engine: the car's up is column %d of its matrix%s (columns %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f)",
                                 c, s_sign < 0.0f ? ", negated" : "", column[0][0], column[0][1], column[0][2], column[1][0], column[1][1],
                                 column[1][2], column[2][0], column[2][1], column[2][2]);
                    }
                }
            }
            up[0] = 0.0f;
            up[1] = 1.0f;
            up[2] = 0.0f;
            if (s_axis >= 0 && length[s_axis] > 1e-3f && length[s_axis] < 1e3f) {
                for (int r = 0; r < 3; r++) up[r] = column[s_axis][r] * s_sign / length[s_axis];
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_racePlayer = nullptr;
        return false;
    }
}

namespace {

// The location a CGameMobil is drawn at (GmIso4: 9 floats, then the position), or null: not in
// the scene, or its vehicle's slot 32 is not this build's GetInterpolatedLocation. vehicle: its
// CSceneMobil. Inside __try (the pointers may be stale).
const float* drawnLocation(const BYTE* gameMobil, void*& vehicle) {
    if (!gameMobil || *reinterpret_cast<const int*>(gameMobil + kGameMobilSceneId) == -1) return nullptr;
    vehicle = *reinterpret_cast<void* const*>(gameMobil + kGameMobilScene);
    if (!vehicle) return nullptr;
    void* const* table = *reinterpret_cast<void* const* const*>(vehicle);
    if (!table || reinterpret_cast<uintptr_t>(table[kInterpolatedLocationSlot]) != rebase(g_build->interpolatedLocation)) return nullptr;
    using LocationFn = const float*(__fastcall*)(void*, void*, void*);
    const float* iso = reinterpret_cast<LocationFn>(table[kInterpolatedLocationSlot])(vehicle, nullptr, nullptr);
    if (!iso) return nullptr;
    for (int i = 0; i < 12; i++) {
        if (!(fabsf(iso[i]) < 1e5f)) return nullptr;
    }
    return iso;
}

// The CGameMobil with this id in the clip's scene, or null.
const BYTE* sceneMobil(const BYTE* scene, DWORD id) {
    if (!scene || id == 0xffffffffu) return nullptr;
    const DWORD count = *reinterpret_cast<const DWORD*>(scene + kSceneMobils);
    const BYTE* const* mobils = *reinterpret_cast<const BYTE* const* const*>(scene + kSceneMobils + 4);
    for (DWORD i = 0; mobils && i < count && i < 4096; i++) {
        if (mobils[i] && *reinterpret_cast<const DWORD*>(mobils[i] + kGameMobilId) == id) return mobils[i];
    }
    return nullptr;
}

// The car a clip player's cameras show: the mobil the game cameras follow, else the local
// player's ghost (ghost id 0), else the first ghost of the clip. Inside __try.
const BYTE* clipCar(const BYTE* clip, int& which) {
    const BYTE* context = *reinterpret_cast<const BYTE* const*>(clip + kClipContext);
    const BYTE* scene = context ? *reinterpret_cast<const BYTE* const*>(context + kContextScene) : nullptr;
    const BYTE* cameras = context ? *reinterpret_cast<const BYTE* const*>(context + kContextCameraSet) : nullptr;
    which = 0;
    if (cameras) {
        if (const BYTE* m = sceneMobil(scene, *reinterpret_cast<const DWORD*>(cameras + kCameraSetFollowed))) return m;
    }
    which = 1;
    if (const BYTE* ghost = *reinterpret_cast<const BYTE* const*>(clip + kClipLocalGhost)) {
        if (const BYTE* m = *reinterpret_cast<const BYTE* const*>(ghost + kGhostMobil)) return m;
    }
    if (const BYTE* m = sceneMobil(scene, *reinterpret_cast<const DWORD*>(clip + kClipLocalMobilId))) return m;
    which = 2;
    const DWORD blocks = *reinterpret_cast<const DWORD*>(clip + kClipGhostBlocks);
    const BYTE* const* block = *reinterpret_cast<const BYTE* const* const*>(clip + kClipGhostBlocks + 4);
    if (blocks && block && block[0]) {
        if (const BYTE* ghost = *reinterpret_cast<const BYTE* const*>(block[0] + kBlockGhost)) {
            return *reinterpret_cast<const BYTE* const*>(ghost + kGhostMobil);
        }
    }
    return nullptr;
}

// The wheels' contact points of a CSceneVehicleCar in the car's axes (right, up, forward; m from
// its drawn position), only while it stands (async speed below 0.5 m/s: the physics state is a
// step behind the drawn car, at speed that would move them). False: not that class in this
// build, not standing, not 4 wheels. Inside __try.
// model: the model's axis for right, up, forward (columns of the car's matrix) and forward's sign.
bool standingWheels(void* vehicle, const float* iso, const float axes[3][3], const int model[3], float forwardSign, float local[4][3]) {
    void* const* table = *reinterpret_cast<void* const* const*>(vehicle);
    if (!g_build->wheelCount || reinterpret_cast<uintptr_t>(table[kWheelCountSlot]) != rebase(g_build->wheelCount) ||
        reinterpret_cast<uintptr_t>(table[kWheelContactSlot]) != rebase(g_build->wheelContact) ||
        reinterpret_cast<uintptr_t>(table[kAsyncSpeedSlot]) != rebase(g_build->asyncSpeed))
        return false;
    const BYTE* car = static_cast<const BYTE*>(vehicle);
    const float* speed = reinterpret_cast<const float*>(car + kVehicleAsyncSpeed);
    if (!(speed[0] * speed[0] + speed[1] * speed[1] + speed[2] * speed[2] < 0.25f)) return false;
    if (*reinterpret_cast<const DWORD*>(car + kVehicleWheels) != 4) return false;
    const BYTE* wheels = *reinterpret_cast<const BYTE* const*>(car + kVehicleWheels + 4);
    if (!wheels) return false;
    // The contact points are in the car's own frame (my test: read as world points they came out
    // as the car's position, 686 / -90 / -170 m, with the track and wheelbase swapped); world
    // points (far from the origin) are still taken as such.
    bool own = true;
    for (int k = 0; k < 4; k++) {
        const float* p = reinterpret_cast<const float*>(wheels + k * kWheelSize + kWheelContact);
        own = own && fabsf(p[0]) < 10.0f && fabsf(p[1]) < 10.0f && fabsf(p[2]) < 10.0f;
    }
    for (int k = 0; k < 4; k++) {
        const float* p = reinterpret_cast<const float*>(wheels + k * kWheelSize + kWheelContact);
        if (own) {
            local[k][0] = p[model[0]];
            local[k][1] = p[model[1]];
            local[k][2] = p[model[2]] * forwardSign;
        } else {
            const float d[3] = {p[0] - iso[9], p[1] - iso[10], p[2] - iso[11]};
            for (int a = 0; a < 3; a++) local[k][a] = d[0] * axes[a][0] + d[1] * axes[a][1] + d[2] * axes[a][2];
        }
    }
    return true;
}

// The Stadium car's tyres (estimates from its model: a long car, the front wheels far ahead of
// the cockpit; the drawn position is about the height of the wheel centres): until the map's own
// car was measured standing at a start.
constexpr float kFrontAxle = 1.7f;     // m ahead of the drawn position
constexpr float kRearAxle = -1.2f;     // m behind it (wheelbase 2.9 m)
constexpr float kHalfTrack = 0.86f;    // half the track (1.72 m)
constexpr float kContactHeight = -0.35f; // the road under the tyres (wheel radius)

} // namespace

bool carPose(CarPose& pose) {
    pose = CarPose();
    if (!g_build || !g_build->interpolatedLocation) return false;
    const bool race = g_racePlayer && GetTickCount() - g_racePlayerTick <= 300;
    // Counted in frames (one frame of a 4K export with supersampling takes seconds).
    const bool clip = !race && g_clipPlayer && g_engineFrames - g_clipPlayerFrame <= 3;
    if (!race && !clip) return false;
    const float* iso = nullptr;
    void* vehicle = nullptr;
    int which = -1;
    __try {
        const BYTE* mobil = race ? *reinterpret_cast<const BYTE* const*>(static_cast<const BYTE*>(g_racePlayer) + kPlayerMobil)
                                 : clipCar(static_cast<const BYTE*>(g_clipPlayer), which);
        iso = drawnLocation(mobil, vehicle);
        if (!iso) return false;
        memcpy(pose.position, iso + 9, sizeof(pose.position));
        float column[3][3], length[3];
        for (int c = 0; c < 3; c++) {
            for (int r = 0; r < 3; r++) column[c][r] = iso[r * 3 + c];
            length[c] = sqrtf(column[c][0] * column[c][0] + column[c][1] * column[c][1] + column[c][2] * column[c][2]);
            if (!(length[c] > 1e-3f && length[c] < 1e3f)) return false;
            for (int r = 0; r < 3; r++) column[c][r] /= length[c];
        }
        // My check of the forward axis from the car's motion: the column the car moves along
        // (within 25 degrees) while it moves 0.3 m or more from one call to the next, counted in
        // races and exports (the replay editor's clock also runs backwards). After 120 such
        // calls, the column with 90 % of them is logged, and taken if it is not +z (my reading
        // of the engine above would be wrong then).
        static float s_last[3] = {};
        static bool s_haveLast = false;
        static int s_votes[6] = {}, s_total = 0;
        static int s_forward = kForwardColumn * 2; // column * 2 + (negated)
        static bool s_decided = false;
        const bool counted = race || g_engineFrames - g_shootFrame <= 3;
        if (s_haveLast && counted && !s_decided) {
            const float d[3] = {pose.position[0] - s_last[0], pose.position[1] - s_last[1], pose.position[2] - s_last[2]};
            const float moved = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (moved > 0.3f && moved < 20.0f) {
                for (int c = 0; c < 3; c++) {
                    const float along = (d[0] * column[c][0] + d[1] * column[c][1] + d[2] * column[c][2]) / moved;
                    if (fabsf(along) > 0.906f) {
                        s_votes[c * 2 + (along < 0.0f ? 1 : 0)]++;
                        s_total++;
                    }
                }
                if (s_total >= 120) {
                    int best = 0;
                    for (int k = 1; k < 6; k++) best = s_votes[k] > s_votes[best] ? k : best;
                    s_decided = true;
                    if (s_votes[best] * 10 >= s_total * 9) s_forward = best;
                    TMVS_LOG("engine: the car's forward from its motion: column %d%s in %d of %d moves (the engine's VehicleFrontSpeedGet: column %d)%s",
                             best / 2, (best & 1) ? " negated" : "", s_votes[best], s_total, kForwardColumn,
                             s_forward == kForwardColumn * 2 ? ", the same: kept"
                                                             : (s_forward == best ? ", NOT the same: the spray takes the measured one"
                                                                                  : ", no clear winner: kept the engine's"));
                }
            }
        }
        memcpy(s_last, pose.position, sizeof(s_last));
        s_haveLast = true;
        const int f = s_forward / 2;
        const float sign = (s_forward & 1) ? -1.0f : 1.0f;
        const int u = f == 1 ? 2 : 1; // the model's y (when it isn't forward)
        const int r = 3 - f - u;
        for (int k = 0; k < 3; k++) {
            pose.forward[k] = column[f][k] * sign;
            pose.up[k] = column[u][k];
            pose.right[k] = column[r][k];
        }
        // The map's car standing (at the start of a race): its tyres' contact points, kept per
        // environment for the session (the export's replays then have them too). Each wheel
        // within 3 cm over 15 calls, and like a car: two ahead, two behind, a pair on each side.
        pose.frontAxle = kFrontAxle;
        pose.rearAxle = kRearAxle;
        pose.halfTrack = kHalfTrack;
        pose.contactHeight = kContactHeight;
        static char s_measuredFor[32] = {};
        static float s_measured[4] = {};
        static float s_sum[4][3] = {}, s_first[4][3] = {};
        static int s_samples = 0;
        static char s_rejectedFor[32] = {};
        if (s_measuredFor[0] && !strcmp(s_measuredFor, g_environment)) {
            memcpy(&pose.frontAxle, s_measured, sizeof(s_measured));
            pose.wheelsMeasured = true;
        } else if (race && g_environment[0]) {
            const float axes[3][3] = {{pose.right[0], pose.right[1], pose.right[2]},
                                      {pose.up[0], pose.up[1], pose.up[2]},
                                      {pose.forward[0], pose.forward[1], pose.forward[2]}};
            float local[4][3];
            const int model[3] = {r, u, f};
            if (standingWheels(vehicle, iso, axes, model, sign, local)) {
                bool steady = true;
                for (int k = 0; k < 4 && s_samples > 0; k++) {
                    for (int a = 0; a < 3; a++) steady = steady && fabsf(local[k][a] - s_first[k][a]) < 0.03f;
                }
                if (!steady || s_samples == 0) {
                    memcpy(s_first, local, sizeof(s_first));
                    memset(s_sum, 0, sizeof(s_sum));
                    s_samples = 0;
                }
                for (int k = 0; k < 4; k++) {
                    for (int a = 0; a < 3; a++) s_sum[k][a] += local[k][a];
                }
                if (++s_samples >= 15) {
                    float m[4][3];
                    for (int k = 0; k < 4; k++) {
                        for (int a = 0; a < 3; a++) m[k][a] = s_sum[k][a] / s_samples;
                    }
                    s_samples = 0;
                    // Front: the two farthest ahead.
                    int order[4] = {0, 1, 2, 3};
                    for (int i = 0; i < 4; i++) {
                        for (int j = i + 1; j < 4; j++) {
                            if (m[order[j]][2] > m[order[i]][2]) {
                                const int t = order[i];
                                order[i] = order[j];
                                order[j] = t;
                            }
                        }
                    }
                    const float* a0 = m[order[0]];
                    const float* a1 = m[order[1]];
                    const float* b0 = m[order[2]];
                    const float* b1 = m[order[3]];
                    const float front = (a0[2] + a1[2]) * 0.5f, rear = (b0[2] + b1[2]) * 0.5f;
                    const float track = (fabsf(a0[0] - a1[0]) + fabsf(b0[0] - b1[0])) * 0.25f;
                    const float height = (a0[1] + a1[1] + b0[1] + b1[1]) * 0.25f;
                    const bool sides = a0[0] * a1[0] < 0.0f && b0[0] * b1[0] < 0.0f;
                    const bool like = sides && front > 0.3f && front < 3.5f && rear < -0.2f && rear > -3.5f && track > 0.4f && track < 1.5f &&
                                      height < 0.2f && height > -1.2f;
                    if (like) {
                        strncpy(s_measuredFor, g_environment, sizeof(s_measuredFor) - 1);
                        s_measured[0] = front;
                        s_measured[1] = rear;
                        s_measured[2] = track;
                        s_measured[3] = height;
                        memcpy(&pose.frontAxle, s_measured, sizeof(s_measured));
                        pose.wheelsMeasured = true;
                    }
                    if (like || strcmp(s_rejectedFor, g_environment)) {
                        if (!like) strncpy(s_rejectedFor, g_environment, sizeof(s_rejectedFor) - 1);
                        TMVS_LOG("engine: the %s car's tyres touch the road (standing, in its axes right/up/forward): %.2f %.2f %.2f, %.2f %.2f %.2f, "
                                 "%.2f %.2f %.2f, %.2f %.2f %.2f -> front axle %+.2f m, rear %+.2f m, half track %.2f m, %.2f m below its "
                                 "position%s",
                                 g_environment, m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2], m[3][0],
                                 m[3][1], m[3][2], front, rear, track, -height,
                                 like ? "" : ": not like a car, the spray keeps the Stadium car's estimates");
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (race) g_racePlayer = nullptr;
        else g_clipPlayer = nullptr;
        return false;
    }
    pose.source = race ? 1 : 2;
    // Once per change: where the pose comes from (the clip's: which car).
    static int s_logged = -1, s_lines = 0;
    const int state = race ? 0 : 1 + which;
    if (state != s_logged && s_lines < 20) {
        s_logged = state;
        s_lines++;
        static const char* const kFrom[] = {"the race's player", "the clip's camera follows (replay editor, export)",
                                            "the clip's local player (no game camera followed one)", "the clip's first ghost"};
        TMVS_LOG("engine: the car's axes come from the car %s; forward %.2f %.2f %.2f, up %.2f %.2f %.2f", kFrom[state], pose.forward[0],
                 pose.forward[1], pose.forward[2], pose.up[0], pose.up[1], pose.up[2]);
    }
    return true;
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

int blocksRemoved() {
    return static_cast<int>(g_blocksRemoved);
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

void setLensOffset(float x, float y, float focusDistance) {
    const float xy[2] = {x, y};
    setLensSamples(x == 0.0f && y == 0.0f ? nullptr : xy, 1, focusDistance);
}

void setLensSamples(const float* xy, int count, float focusDistance) {
    if (!xy || count <= 0 || !(focusDistance > 0.0f) || !std::isfinite(focusDistance)) {
        if (g_lensCount > 0) TMVS_LOG("engine: lens offsets off");
        g_lensCount = 0;
        g_loggedLens = false;
        g_lensUsed = -1;
        return;
    }
    count = (std::min)(count, kMaxLensSamples);
    for (int i = 0; i < count; i++) {
        const bool ok = std::isfinite(xy[2 * i]) && std::isfinite(xy[2 * i + 1]);
        g_lensXY[i][0] = ok ? xy[2 * i] : 0.0f;
        g_lensXY[i][1] = ok ? xy[2 * i + 1] : 0.0f;
    }
    g_lensCount = count;
    // Nearer than about the near plane the shear would throw the image about.
    g_lensFocus = (std::max)(focusDistance, 0.05f);
    g_lensRenders = 0;
}

bool mainCameraSample(int& index, int& count, int& lensEntry) {
    index = g_gameSample[0];
    count = g_gameSample[1];
    lensEntry = g_lensUsed;
    return index >= 0;
}

bool cameraMatrices(float view[16], float projection[16]) {
    if (!g_beginMatrices) return false;
    memcpy(view, &g_beginView, sizeof(g_beginView));
    memcpy(projection, &g_beginProjection, sizeof(g_beginProjection));
    return true;
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

Material materialClass(const void* shader) {
    return materialOf(shader);
}

bool materialFile(const void* shader, wchar_t* out, size_t size) {
    return shader && nodFilePath(shader, out, size);
}

bool materialTextures(const void* shader, wchar_t* out, size_t size) {
    return shader && size > 0 && textureFiles(shader, out, size);
}

bool neonMaterialsKnown() {
    return g_drawShader && g_drawPass && g_textureNames;
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

bool clipInRace() {
    // The map's own clips (in-game triggers) only run the clip player; replays, the end of race
    // replay and the editor also run the clip viewer's cameras (my log: "sources 3"
    // there, "sources 1" for the clips on the track). The race checks its player's trigger zones
    // every frame it runs, also while such a clip plays.
    const DWORD now = GetTickCount();
    if (!cinematicActive() || !g_tracksUpdate || (g_lastNotClip != 0 && now - g_lastNotClip < 500)) return false;
    return g_racePlayer && now - g_racePlayerTick <= 300;
}

unsigned cinematicSources() {
    const unsigned s = g_cinematicSources;
    g_cinematicSources = 0;
    return s;
}

void setVideoFileHooks(const VideoFileHooks& hooks) {
    g_videoFile = hooks;
}

bool videoFileHooked() {
    return g_aviOpen && g_aviVideoOpen && g_aviVideoWrite && g_aviAudioOpen && g_aviAudioWrite && g_aviDestroy;
}

bool videoExport(VideoExport& out) {
    out = VideoExport();
    // Counted in frames, not ms: one frame of a 4K export with supersampling takes seconds.
    if (!g_doShoot || !g_shooter || g_engineFrames - g_shootFrame > 3) return false;
    const BYTE* shooter = static_cast<const BYTE*>(g_shooter);
    __try {
        if (!readable(shooter, kShooterParams + sizeof(void*))) return false; // covers the clip times too
        const BYTE* params = *reinterpret_cast<const BYTE* const*>(shooter + kShooterParams);
        if (!params || !readable(params, kVideoParamsSize)) return false;
        const auto at = [params](size_t offset) { return *reinterpret_cast<const int*>(params + offset); };
        out.fps = static_cast<float>(at(0x14));
        out.width = at(0x18);
        out.height = at(0x1c);
        out.hq = at(0x20) != 0;
        out.hqSamples = out.hq && at(0x24) > 1 ? (at(0x24) > 11 ? 11 : at(0x24)) : 1;
        out.hqMotionBlur = at(0x28) != 0;
        out.stereo = at(0x38);
        out.sound = at(0x34) != 0;
        out.writesVideo = *reinterpret_cast<const int*>(shooter + kShooterNoVideo) == 0;
        out.clipStart = *reinterpret_cast<const DWORD*>(shooter + kShooterClipStart) / 1000.0;
        out.clipStop = *reinterpret_cast<const DWORD*>(shooter + kShooterClipStop) / 1000.0;
        // Not the dialog's numbers (another build, a patched game): no export as far as we know.
        if (out.fps < 1.0f || out.fps > 1000.0f || out.width < 16 || out.height < 16 || out.width > 16384 || out.height > 16384) return false;
        const int samples = out.hqSamples * out.hqSamples;
        out.subFrames = samples > 1 && out.hqMotionBlur && samples >= 9 && out.writesVideo;
        out.samplesPerFrame = samples > 1 && !out.subFrames ? samples : 1;
        out.samplesPerVideoFrame = out.subFrames ? samples : 1;
        const BYTE* const* core = reinterpret_cast<const BYTE* const*>(rebase(g_build->coreCmdBuffer));
        if (readable(core, sizeof(void*)) && *core && readable(*core, kCoreTickTime + sizeof(DWORD))) {
            const DWORD ms = *reinterpret_cast<const DWORD*>(*core + kCoreFixedMs);
            const DWORD fraction = *reinterpret_cast<const DWORD*>(*core + kCoreFixedFraction);
            out.step = (static_cast<double>(ms) + static_cast<double>(fraction) / 2147483648.0) / 1000.0;
            if (out.step > 1.0) out.step = 0.0; // not a frame step: the real clock
            out.gameClock = static_cast<double>(*reinterpret_cast<const DWORD*>(*core + kCoreTickTime)) / 1000.0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = VideoExport();
        return false;
    }
    return true;
}

} // namespace engine
} // namespace tmshaders
