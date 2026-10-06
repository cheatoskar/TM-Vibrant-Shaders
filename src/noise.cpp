#include "noise.h"
#include <cmath>

namespace tmshaders {
namespace noise {
namespace {

uint32_t hash(int x, int y, int z, int seed) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + static_cast<uint32_t>(z) * 2147483647u +
                 static_cast<uint32_t>(seed) * 144665u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

float unit(uint32_t h) { return static_cast<float>(h & 0xFFFFFF) / 16777216.0f; }

int wrap(int i, int period) { return ((i % period) + period) % period; }

// Distance to the nearest feature point, 0..1 (1 = one cell away). Tiles with `cells`.
float worley(float x, float y, float z, int cells, int seed) {
    const float px = x * cells, py = y * cells, pz = z * cells;
    const int cx = static_cast<int>(std::floor(px)), cy = static_cast<int>(std::floor(py)), cz = static_cast<int>(std::floor(pz));
    float best = 9.0f;
    for (int k = -1; k <= 1; k++) {
        for (int j = -1; j <= 1; j++) {
            for (int i = -1; i <= 1; i++) {
                const int gx = cx + i, gy = cy + j, gz = cz + k;
                const uint32_t h = hash(wrap(gx, cells), wrap(gy, cells), wrap(gz, cells), seed);
                const float fx = gx + unit(h) - px;
                const float fy = gy + unit(h * 747796405u + 1u) - py;
                const float fz = gz + unit(h * 2891336453u + 7u) - pz;
                const float d = fx * fx + fy * fy + fz * fz;
                if (d < best) best = d;
            }
        }
    }
    const float d = std::sqrt(best);
    return d > 1.0f ? 1.0f : d;
}

float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

float gradient(int x, int y, int z, int period, float dx, float dy, float dz) {
    const uint32_t h = hash(wrap(x, period), wrap(y, period), wrap(z, period), 99) % 12u;
    static const float g[12][3] = {{1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1},
                                   {1, 0, -1}, {-1, 0, -1}, {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}};
    return g[h][0] * dx + g[h][1] * dy + g[h][2] * dz;
}

// Tiling gradient noise, about -1..1.
float perlin(float x, float y, float z, int period) {
    const float px = x * period, py = y * period, pz = z * period;
    const int ix = static_cast<int>(std::floor(px)), iy = static_cast<int>(std::floor(py)), iz = static_cast<int>(std::floor(pz));
    const float fx = px - ix, fy = py - iy, fz = pz - iz;
    const float u = fade(fx), v = fade(fy), w = fade(fz);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float x00 = lerp(gradient(ix, iy, iz, period, fx, fy, fz), gradient(ix + 1, iy, iz, period, fx - 1, fy, fz), u);
    const float x10 = lerp(gradient(ix, iy + 1, iz, period, fx, fy - 1, fz), gradient(ix + 1, iy + 1, iz, period, fx - 1, fy - 1, fz), u);
    const float x01 = lerp(gradient(ix, iy, iz + 1, period, fx, fy, fz - 1), gradient(ix + 1, iy, iz + 1, period, fx - 1, fy, fz - 1), u);
    const float x11 =
        lerp(gradient(ix, iy + 1, iz + 1, period, fx, fy - 1, fz - 1), gradient(ix + 1, iy + 1, iz + 1, period, fx - 1, fy - 1, fz - 1), u);
    return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w);
}

float worleyFbm(float x, float y, float z, int cells, int seed) {
    return (1.0f - worley(x, y, z, cells, seed)) * 0.625f + (1.0f - worley(x, y, z, cells * 2, seed + 1)) * 0.25f +
           (1.0f - worley(x, y, z, cells * 4, seed + 2)) * 0.125f;
}

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

uint32_t byte(float v) { return static_cast<uint32_t>(clamp01(v) * 255.0f + 0.5f); }

} // namespace

const std::vector<uint32_t>& cloudVolume(int size) {
    static std::vector<uint32_t> cache;
    static int cachedSize = 0;
    if (cachedSize == size) return cache;
    cache.assign(static_cast<size_t>(size) * size * size, 0);
    for (int z = 0; z < size; z++) {
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                const float fx = (x + 0.5f) / size, fy = (y + 0.5f) / size, fz = (z + 0.5f) / size;
                // Perlin fBm, remapped by Worley: billowy cells with soft edges.
                const float p = clamp01(0.5f + 0.5f * (perlin(fx, fy, fz, 4) + 0.5f * perlin(fx, fy, fz, 8) + 0.25f * perlin(fx, fy, fz, 16)));
                const float w = worleyFbm(fx, fy, fz, 4, 1);
                const float pw = clamp01((p - (w - 1.0f)) / (1.0f - (w - 1.0f)) * 1.0f - 0.25f) / 0.75f;
                const float g = worleyFbm(fx, fy, fz, 4, 11);
                const float b = worleyFbm(fx, fy, fz, 8, 21);
                const float a = worleyFbm(fx, fy, fz, 16, 31);
                cache[(static_cast<size_t>(z) * size + y) * size + x] = (byte(a) << 24) | (byte(pw) << 16) | (byte(g) << 8) | byte(b);
            }
        }
    }
    cachedSize = size;
    return cache;
}

} // namespace noise
} // namespace tmshaders
