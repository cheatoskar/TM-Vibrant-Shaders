#include "weather.h"
#include <cmath>

namespace tmshaders {
namespace weather {

bool lightningStrike(int slot, float amount, float& start) {
    if (amount <= 0.0f) return false;
    unsigned h = static_cast<unsigned>(slot) * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    // About one strike every 15 s at full strength.
    if ((h & 0xFFFF) / 65535.0f > amount * 0.07f) return false;
    start = static_cast<float>(slot) + ((h >> 16) & 0xFF) / 255.0f;
    return true;
}

float lightningFlash(float time, float amount) {
    if (amount <= 0.0f) return 0.0f;
    float flash = 0.0f;
    for (int k = 0; k < 2; k++) {
        float start = 0.0f;
        if (!lightningStrike(static_cast<int>(floorf(time)) - k, amount, start)) continue;
        const float t = time - start;
        if (t < 0.0f || t > 1.5f) continue;
        // A bright flash with a couple of flickers.
        flash += expf(-t * 8.0f) + 0.7f * expf(-fabsf(t - 0.22f) * 30.0f) + 0.5f * expf(-fabsf(t - 0.5f) * 25.0f);
    }
    return fminf(flash, 1.6f) * fminf(amount * 1.5f, 1.0f);
}

} // namespace weather
} // namespace tmshaders
