#pragma once

namespace tmshaders {
namespace weather {

// Lightning strikes are deterministic in time, so the flash (every camera, every pass), the
// bolt in the sky and the thunder agree without sharing state.
struct Strike {
    int slot = 0;          // one-second slot the strike starts in
    float start = 0.0f;    // time of the strike (s)
    float distance = 0.0f; // 0 = close (~1 km) .. 1 = far (~4.5 km)
    float side = 0.0f;     // -0.5 .. 0.5: where it strikes relative to the view
};

// Does a strike start in the one-second slot `slot` at this lightning amount?
bool lightningStrike(int slot, float amount, Strike& strike);
// The most recent strike that is still visible at `time` (up to 1.5 s after it started).
bool currentStrike(float time, float amount, Strike& strike);
// Brightness of the flash at `time` (0 = none, up to ~1.6).
float lightningFlash(float time, float amount);

} // namespace weather
} // namespace tmshaders
