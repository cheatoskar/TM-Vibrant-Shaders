#pragma once
#include "settings.h"
#include <windows.h>

namespace tmshaders {
namespace audio {

// Weather sound: rain and thunder (recordings, see sounds/CREDITS.md) and the wind of a snow
// storm (synthesised), played through winmm on a thread of its own.
//   Call once per presented frame. active = the 3D scene is on screen (not a menu);
//   window = the game window (sound fades out while it isn't in front).
void update(const Settings& settings, float time, bool active, HWND window);
void shutdown();

// Previewer: renders 28 s of rain with a close, a middle and a far thunder to a WAV file,
// through the same synthesis the game hears.
// wind > 0: the snow storm wind (snow * (0.4 + wind)) instead of the thunders.
bool renderWav(const wchar_t* path, float rain, float volume, float wind = 0.0f);
// Previewer: decodes an MP3 to a 44.1 kHz stereo WAV (as the game would load it).
bool decodeToWav(const wchar_t* mp3Path, const wchar_t* wavPath);
// Previewer: play as if the game were in front (the console window belongs to another process).
void setAlwaysInFront(bool on);

} // namespace audio
} // namespace tmshaders
