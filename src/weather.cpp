#include "weather.h"
#include <cmath>

namespace tmshaders {
namespace weather {
namespace {

unsigned hash(unsigned x) {
    x *= 2654435761u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return x;
}

} // namespace

bool lightningStrike(int slot, float amount, Strike& strike) {
    if (amount <= 0.0f) return false;
    const unsigned h = hash(static_cast<unsigned>(slot));
    // About one strike every 15 s at full strength.
    if ((h & 0xFFFF) / 65535.0f > amount * 0.07f) return false;
    const unsigned g = hash(h ^ 0x9E3779B9u);
    strike.slot = slot;
    strike.start = static_cast<float>(slot) + ((h >> 16) & 0xFF) / 255.0f;
    strike.distance = (g & 0xFF) / 255.0f;
    strike.side = ((g >> 8) & 0xFF) / 255.0f - 0.5f;
    return true;
}

bool currentStrike(float time, float amount, Strike& strike) {
    for (int k = 0; k < 2; k++) {
        Strike s;
        if (!lightningStrike(static_cast<int>(floorf(time)) - k, amount, s)) continue;
        const float t = time - s.start;
        if (t < 0.0f || t > 1.5f) continue;
        strike = s;
        return true;
    }
    return false;
}

float lightningFlash(float time, float amount) {
    if (amount <= 0.0f) return 0.0f;
    float flash = 0.0f;
    for (int k = 0; k < 2; k++) {
        Strike s;
        if (!lightningStrike(static_cast<int>(floorf(time)) - k, amount, s)) continue;
        const float t = time - s.start;
        if (t < 0.0f || t > 1.5f) continue;
        // A bright flash with a couple of flickers (the return strokes). Far strikes light
        // the scene less.
        const float strength = 1.0f - 0.5f * s.distance;
        flash += strength * (expf(-t * 8.0f) + 0.7f * expf(-fabsf(t - 0.22f) * 30.0f) + 0.5f * expf(-fabsf(t - 0.5f) * 25.0f));
    }
    return fminf(flash, 1.6f) * fminf(amount * 1.5f, 1.0f);
}

} // namespace weather
} // namespace tmshaders
