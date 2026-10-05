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

} // namespace engine
} // namespace tmshaders
