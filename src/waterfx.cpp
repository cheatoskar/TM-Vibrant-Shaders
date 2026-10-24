#include "waterfx.h"
#include "image.h"
#include <algorithm>
#include <cmath>

namespace tmshaders {
namespace waterfx {
namespace {

constexpr int N = kAtlasSize;

float hash(int x, int y, int period, int seed) {
    x = ((x % period) + period) % period;
    y = ((y % period) + period) % period;
    unsigned h = static_cast<unsigned>(x) * 374761393u + static_cast<unsigned>(y) * 668265263u + static_cast<unsigned>(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFF) / 16777216.0f;
}

// Value noise that repeats every `period` cells, over u, v in 0..1 (one tile).
float valueNoise(float u, float v, int period, int seed) {
    const float x = u * period, y = v * period;
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    const float fx = x - ix, fy = y - iy;
    const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
    const float a = hash(ix, iy, period, seed), b = hash(ix + 1, iy, period, seed);
    const float c = hash(ix, iy + 1, period, seed), d = hash(ix + 1, iy + 1, period, seed);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

float fbm(float u, float v, int seed) {
    return 0.5f * valueNoise(u, v, 4, seed) + 0.3f * valueNoise(u, v, 8, seed + 1) + 0.2f * valueNoise(u, v, 16, seed + 2);
}

float smooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

uint8_t byte(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

// Droplet: a soft round body, a bright spot where the sky mirrors in it, a rim.
float builtinDroplet(float u, float v) {
    const float x = u * 2.0f - 1.0f, y = v * 2.0f - 1.0f;
    const float r = std::sqrt(x * x + y * y);
    const float body = smooth(1.0f, 0.72f, r);
    const float spot = std::exp(-((x + 0.32f) * (x + 0.32f) + (y + 0.34f) * (y + 0.34f)) * 34.0f);
    const float rim = smooth(0.5f, 0.9f, r) * body;
    return std::clamp(body * 0.5f + spot * 0.9f + rim * 0.35f, 0.0f, 1.0f);
}

// Foam: bubbles and the thin walls between them (the distance to the nearest and second nearest
// of a jittered grid of points, wrapped), broken up by a slow noise.
float builtinFoam(float u, float v) {
    constexpr int G = 9;
    const float x = u * G, y = v * G;
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    float f1 = 9.0f, f2 = 9.0f;
    for (int j = -1; j <= 1; j++) {
        for (int i = -1; i <= 1; i++) {
            const float px = ix + i + 0.15f + 0.7f * hash(ix + i, iy + j, G, 11);
            const float py = iy + j + 0.15f + 0.7f * hash(ix + i, iy + j, G, 12);
            const float d = std::sqrt((px - x) * (px - x) + (py - y) * (py - y));
            if (d < f1) {
                f2 = f1;
                f1 = d;
            } else if (d < f2) {
                f2 = d;
            }
        }
    }
    const float walls = smooth(0.32f, 0.0f, f2 - f1) * 0.75f;
    const float bubbles = smooth(0.38f, 0.1f, f1) * 0.55f;
    const float cover = 0.35f + 0.65f * smooth(0.25f, 0.7f, fbm(u, v, 21));
    return std::clamp((walls + bubbles) * cover + 0.18f * cover, 0.0f, 1.0f);
}

// Ripple: the slope of a tiling noise (x, y), flat = 0.5.
void builtinRipple(float u, float v, float& nx, float& ny) {
    const float e = 1.0f / N;
    const float h0 = fbm(u, v, 31);
    const float dx = fbm(u + e, v, 31) - fbm(u - e, v, 31);
    const float dy = fbm(u, v + e, 31) - fbm(u, v - e, 31);
    (void)h0;
    nx = std::clamp(0.5f + dx * 9.0f, 0.0f, 1.0f);
    ny = std::clamp(0.5f + dy * 9.0f, 0.0f, 1.0f);
}

struct Picture {
    std::vector<uint32_t> pixels;
    int width = 0, height = 0;
    bool valid() const { return width > 0 && height > 0; }
    // Bilinear, wrapped, as 0xAARRGGBB channels 0..1 (b, g, r, a).
    void sample(float u, float v, float out[4]) const {
        const float x = u * width - 0.5f, y = v * height - 0.5f;
        const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
        const float fx = x - ix, fy = y - iy;
        for (int c = 0; c < 4; c++) out[c] = 0.0f;
        for (int j = 0; j < 2; j++) {
            for (int i = 0; i < 2; i++) {
                const int px = (((ix + i) % width) + width) % width, py = (((iy + j) % height) + height) % height;
                const uint32_t p = pixels[static_cast<size_t>(py) * width + px];
                const float w = (i ? fx : 1.0f - fx) * (j ? fy : 1.0f - fy);
                out[0] += w * ((p >> 16) & 0xFF) / 255.0f; // r
                out[1] += w * ((p >> 8) & 0xFF) / 255.0f;  // g
                out[2] += w * (p & 0xFF) / 255.0f;         // b
                out[3] += w * ((p >> 24) & 0xFF) / 255.0f; // a
            }
        }
    }
};

bool load(const std::wstring& folder, const wchar_t* name, Picture& out) {
    if (folder.empty()) return false;
    for (const wchar_t* extension : {L".png", L".tga", L".jpg", L".jpeg", L".bmp"}) {
        if (image::readRgba(folder + L"\\" + name + extension, out.pixels, out.width, out.height)) return true;
    }
    return false;
}

} // namespace

std::vector<uint32_t> buildAtlas(const std::wstring& folder, std::string* report) {
    Picture droplet, foam, ripple;
    const bool fileDroplet = load(folder, L"water_droplet", droplet);
    const bool fileFoam = load(folder, L"water_foam", foam);
    const bool fileRipple = load(folder, L"water_ripple", ripple);
    if (report) {
        *report = std::string("droplet ") + (fileDroplet ? "from a file" : "built in") + ", foam " + (fileFoam ? "from a file" : "built in") +
                  ", ripple " + (fileRipple ? "from a file" : "built in");
    }
    std::vector<uint32_t> atlas(static_cast<size_t>(N) * N);
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            const float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
            float d = 0.0f, f = 0.0f, nx = 0.5f, ny = 0.5f;
            float s[4];
            if (fileDroplet) {
                // The picture's brightness times its alpha (a white dot on transparent works as well as grey on black).
                droplet.sample(u, v, s);
                d = (0.2126f * s[0] + 0.7152f * s[1] + 0.0722f * s[2]) * s[3];
            } else {
                d = builtinDroplet(u, v);
            }
            if (fileFoam) {
                foam.sample(u, v, s);
                f = 0.2126f * s[0] + 0.7152f * s[1] + 0.0722f * s[2];
            } else {
                f = builtinFoam(u, v);
            }
            if (fileRipple) {
                ripple.sample(u, v, s);
                nx = s[0];
                ny = s[1];
            } else {
                builtinRipple(u, v, nx, ny);
            }
            // a = y slope, b = x slope (the sampler returns r g b a as stored; the shader reads .rgba).
            atlas[static_cast<size_t>(y) * N + x] = (static_cast<uint32_t>(byte(ny)) << 24) | (static_cast<uint32_t>(byte(d)) << 16) |
                                                     (static_cast<uint32_t>(byte(f)) << 8) | byte(nx);
        }
    }
    return atlas;
}

} // namespace waterfx
} // namespace tmshaders
