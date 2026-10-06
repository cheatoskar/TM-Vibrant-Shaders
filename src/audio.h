#pragma once
#include "settings.h"
#include <windows.h>

namespace tmshaders {
namespace audio {

// Weather sound: rain and thunder, synthesised on the fly (no sound files) and played
// through winmm on a thread of its own.
//   Call once per presented frame. active = the 3D scene is on screen (not a menu);
//   window = the game window (sound fades out while it isn't in front).
void update(const Settings& settings, float time, bool active, HWND window);
void shutdown();

} // namespace audio
} // namespace tmshaders
