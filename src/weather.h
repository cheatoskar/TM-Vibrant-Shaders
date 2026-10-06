#pragma once

namespace tmshaders {
namespace weather {

// Lightning strikes are deterministic in time, so the flash (every camera, every pass)
// and the thunder agree without sharing state.
//   Does a strike start in the one-second slot `slot` at this lightning amount? start = its time.
bool lightningStrike(int slot, float amount, float& start);
// Brightness of the flash at `time` (0 = none, up to ~1.6).
float lightningFlash(float time, float amount);

} // namespace weather
} // namespace tmshaders
