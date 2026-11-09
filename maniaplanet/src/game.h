#pragma once
// ManiaPlanet's own objects (ManiaPlanet.exe x64, the last build: 31,314,016 bytes), read through
// the places the engine's member reflection names (class id, member, offset). Every object's
// class is checked through its vtable's GetMwClassId before anything is read. Used by maplook
// (Author's Shaders), the replay state and the game camera (gamecamera.cpp).
#include <cstddef>
#include <cstdint>

namespace tmvs_mp {
namespace game {

// The module is ManiaPlanet.exe (the first call looks; false in the offline tools).
bool attached();
uintptr_t base();
// Inside the exe's code section.
bool inCode(uintptr_t address, size_t bytes);
// The code at this RVA starts with these bytes (another build has something else there).
bool codeIs(uintptr_t rva, const unsigned char* bytes, size_t count);

// The class id an engine object reports (GetMwClassId, vtable slot 3), 0 when it isn't one or
// can't be read.
uint32_t classOf(uintptr_t object);
// A pointer read from the game's memory, 0 when it can't be read.
uintptr_t readPointer(uintptr_t at);
bool readBytes(uintptr_t at, void* out, size_t bytes);
// A write into the game's memory (only the game camera's FovRect, gamecamera.cpp).
bool writeBytes(uintptr_t at, const void* in, size_t bytes);

// The app (CTrackMania <- CGameManiaPlanet <- CGameCtnApp), 0 when not found.
uintptr_t app();

// CGameCtnApp members (reflection: CGameCtnApp 0x030D3000).
constexpr size_t kAppGameScene = 0x2D8;
constexpr size_t kAppGameCamera = 0x2E8;
constexpr size_t kAppRootMap = 0x378;
constexpr size_t kAppCurrentPlayground = 0x3F8;
constexpr size_t kAppEditor = 0x7C8;

// The replay editor (CGameCtnMediaTracker) is open: watching or editing a replay, or exporting
// one as a video. Read once a frame (tick).
bool replayEditor();
// Once a frame on the render thread: the replay state; its changes go to the log with the classes
// of the app's editor and playground (to learn what the game has open).
void tick();

} // namespace game
} // namespace tmvs_mp
