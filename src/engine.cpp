#include "engine.h"
#include "log.h"
#include <windows.h>
#include <cstring>

namespace tmshaders {
namespace engine {
namespace {

// Link-time addresses from TmForever.map (preferred base 0x00400000).
constexpr uintptr_t kPreferredBase = 0x00400000;
constexpr uintptr_t kViewportVTable = 0x00bd10bc; // ??_7CVisionViewportDx9@@6B@

struct Slot {
    int index;
    uintptr_t expected; // link-time address of the original function
    const char* name;
};

constexpr Slot kRenderFrameBegin = {93, 0x009a0990, "RenderFrameBegin"};
constexpr Slot kRenderCameraBegin = {95, 0x0099d750, "RenderCameraBegin"};
constexpr Slot kRenderCameraEnd = {100, 0x0099fe80, "RenderCameraEnd"};
constexpr Slot kRenderOverlayZones = {101, 0x009a2390, "RenderOverlayZones"};
constexpr Slot kRenderFrameEnd = {102, 0x009a4070, "RenderFrameEnd"};
constexpr Slot kComputeDriverViewMatrix = {103, 0x0095ab40, "ComputeDriverViewMatrix"};
constexpr Slot kComputeDriverProjection = {104, 0x0095d530, "ComputeDriverProjection"};
constexpr Slot kRenderShadowsSet = {76, 0x0095acc0, "RenderShadowsSet"};
constexpr uintptr_t kShaderLevel = 0x00d123ba; // word: GPU shader path (PC0..PC3)

// Non-virtual functions, hooked inline. The prologue bytes are checked before patching and
// must be whole, position-independent instructions (they run again in the trampoline).
struct InlineSite {
    uintptr_t address;
    unsigned char prologue[8];
    size_t length;
    const char* name;
};
constexpr InlineSite kClipTracksUpdate = {0x00693e20, {0xd9, 0xe8, 0x83, 0xec, 0x08}, 5, "CGameCtnMediaClipPlayer::TracksUpdate"};
constexpr InlineSite kClipViewerCams = {0x00673e50, {0x6a, 0xff, 0x68, 0xe8, 0xf4, 0xaa, 0x00}, 7, "CGameCtnMediaClipViewer::UpdateCams"};
constexpr InlineSite kVideoShoot = {0x006f4510, {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8}, 6, "CGameCtnMediaVideoShooter::DoShoot"};

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
TracksUpdateFn g_tracksUpdate = nullptr;
UpdateCamsFn g_updateCams = nullptr;
DoShootFn g_doShoot = nullptr;
float g_jitter[2] = {};          // NDC offset for perspective projections (TAA)
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

void** vtable() {
    return reinterpret_cast<void**>(rebase(kViewportVTable));
}

bool verify(const Slot& slot) {
    void** table = vtable();
    if (!readable(&table[slot.index], sizeof(void*))) return false;
    return reinterpret_cast<uintptr_t>(table[slot.index]) == rebase(slot.expected);
}

template <typename Fn>
void patch(const Slot& slot, void* detour, Fn& original) {
    void** entry = &vtable()[slot.index];
    original = reinterpret_cast<Fn>(*entry);
    DWORD old = 0;
    VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old);
    *entry = detour;
    VirtualProtect(entry, sizeof(void*), old, &old);
}

// Jumps from the function's start to the detour; the trampoline runs the copied prologue
// and jumps back behind it. Returns the trampoline (the "original" to call), or null.
void* inlineHook(const InlineSite& site, void* detour) {
    BYTE* target = reinterpret_cast<BYTE*>(rebase(site.address));
    if (!readable(target, site.length) || memcmp(target, site.prologue, site.length) != 0) {
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

int g_loggedShadowMode = -2;

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
                 static_cast<unsigned>(*reinterpret_cast<const unsigned short*>(rebase(kShaderLevel))));
    }
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
    for (const Slot& slot : slots) {
        if (!verify(slot)) {
            TMVS_LOG("engine: slot %d (%s) does not match this executable, engine hooks disabled", slot.index, slot.name);
            return false;
        }
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
    g_doShoot = reinterpret_cast<DoShootFn>(inlineHook(kVideoShoot, reinterpret_cast<void*>(&doShootDetour)));
    g_active = true;
    TMVS_LOG("engine: CVisionViewportDx9 hooks installed (module base %p)", GetModuleHandleW(nullptr));
    return true;
}

bool active() {
    return g_active;
}

const CameraInfo& currentCamera() {
    return g_camera;
}

void setProjectionJitter(float x, float y) {
    g_jitter[0] = x;
    g_jitter[1] = y;
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
