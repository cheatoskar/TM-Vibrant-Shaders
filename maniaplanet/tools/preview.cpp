// Offline previewer for the ManiaPlanet build: renders a capture (F12 in the game) through the
// same renderer as the game, without the game.
//
//   mp_preview <capture folder> <out.bmp> [--preset Name] [--view N] [--set Key=Value ...]
//              [--hdr] [--tonecb file]
//
// --frames N runs the HDR stage N times 1/60 s apart (TAA, motion blur and auto focus need a
// previous frame; the camera stays, the engine's motion vectors are the captured ones), --photo
// runs it in photo mode, --focus u,v sets the photo mode's focus point.
//
// --hdr runs the HDR stage first (the mod's tone map and bloom on engine_hdr.dds with
// engine_exposure.dds), with the engine's tone-map constants from tonemap_cb.bin in the capture
// (captures from before 0.2 lack it: pass one with --tonecb).
//
// The capture holds the image the mod graded (input.dds and the view format it was drawn with),
// the engine's textures (engine_*.dds) and its camera (scenev.bin). --view takes the menu's views:
// 0 normal, 1 split, 2 depth, 3 normals, 4 motion, 5 light buffer, 6 world grid.
#include "effects_trail.h"
#include "engine_frame.h"
#include "log.h"
#include "renderer.h"
#include "config.h"
#include "options.h"
#include "settings.h"
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace tmvs_mp;

namespace {

struct Dds {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width = 0, height = 0, pitch = 0;
    std::vector<char> data;
};

bool readDds(const std::string& path, Dds& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint32_t header[32] = {}, dx10[5] = {};
    const bool ok = fread(header, 4, 32, f) == 32 && header[0] == 0x20534444 && header[21] == 0x30315844 &&
                    fread(dx10, 4, 5, f) == 5;
    if (ok) {
        out.format = static_cast<DXGI_FORMAT>(dx10[0]);
        out.height = header[3];
        out.width = header[4];
        out.pitch = header[5];
        out.data.resize(static_cast<size_t>(out.pitch) * out.height);
        if (fread(out.data.data(), 1, out.data.size(), f) != out.data.size()) out.data.clear();
    }
    fclose(f);
    return ok && !out.data.empty();
}

// The resource format for a captured (typed) format, so the views the game used can be made again.
DXGI_FORMAT resourceFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    default: return f;
    }
}

ID3D11Texture2D* upload(ID3D11Device* device, const Dds& dds, UINT bind) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = dds.width;
    d.Height = dds.height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = resourceFormat(dds.format);
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init = {dds.data.data(), dds.pitch, 0};
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(device->CreateTexture2D(&d, &init, &tex))) {
        printf("cannot create a %ux%u texture of format %d\n", dds.width, dds.height, d.Format);
        return nullptr;
    }
    return tex;
}

bool writeBmp(const std::string& path, const D3D11_MAPPED_SUBRESOURCE& m, UINT w, UINT h, bool bgra) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
    unsigned char hd[54] = {'B', 'M'};
    const uint32_t offset = 54, info = 40, planesBits = 1 | (24 << 16), image = row * h;
    const int32_t iw = static_cast<int32_t>(w), ih = static_cast<int32_t>(h);
    memcpy(hd + 2, &size, 4);
    memcpy(hd + 10, &offset, 4);
    memcpy(hd + 14, &info, 4);
    memcpy(hd + 18, &iw, 4);
    memcpy(hd + 22, &ih, 4);
    memcpy(hd + 26, &planesBits, 4);
    memcpy(hd + 34, &image, 4);
    fwrite(hd, 1, 54, f);
    std::vector<unsigned char> line(row, 0);
    for (int y = static_cast<int>(h) - 1; y >= 0; y--) {
        const auto* src = static_cast<const unsigned char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
        for (UINT x = 0; x < w; x++) {
            const unsigned char* p = src + x * 4;
            line[x * 3] = bgra ? p[0] : p[2];
            line[x * 3 + 1] = p[1];
            line[x * 3 + 2] = bgra ? p[2] : p[0];
        }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
    return true;
}


// --trailtest: the trail's recording on a car driving straight along x at 144 frames a second
// (frame times +-20 %, 3 % of the frames without the car): how far its drawn end lags behind the
// car, and how evenly the fade-in behind the car (1.6 m from that end) moves from frame to frame.
// "old" is the recording before 2026-10-16 (the live end only from 30 cm on, none without the car).
int trailTest() {
    struct Old { // the old recording, for comparison
        float kept = 0, clock = 0, liveX = 0;
        bool keptValid = false;
        float newest = 0;
        void step(float dt, bool seen, float x) {
            clock += dt;
            if (!seen) { newest = kept; return; }
            const float moved = keptValid ? std::fabs(x - kept) : 0.0f;
            bool live = moved >= 0.3f || !keptValid;
            if ((clock >= 1.0f / 60.0f && moved >= 0.3f) || moved >= 1.0f || !keptValid) {
                clock = moved >= 1.0f ? 0.0f : std::fmod(clock, 1.0f / 60.0f);
                kept = x;
                keptValid = true;
                live = false;
            }
            newest = live ? x : kept;
        }
    };
    unsigned seed = 12345;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
    for (float speed : {8.0f, 25.0f, 60.0f}) {
        TrailStage trail;
        trail.startRecording();
        Old old;
        float x = 0.0f, lastFadeNew = 0.0f, lastFadeOld = 0.0f;
        double lagNew = 0, lagOld = 0, jerkNew = 0, jerkOld = 0;
        int backNew = 0, backOld = 0, frames = 0;
        for (int f = 0; f < 2000; f++) {
            const float dt = (1.0f / 144.0f) * (0.8f + 0.4f * rnd());
            x += speed * dt;
            const bool seen = rnd() > 0.03f;
            const float car[3] = {x, 0.0f, 0.0f};
            trail.tick(dt);
            const bool live = trail.record(dt, seen ? car : nullptr);
            const float newest = trail.point(live ? trail.head() : trail.head() + 16384 - 1)[0];
            old.step(dt, seen, x);
            const float fadeNew = newest - 1.6f, fadeOld = old.newest - 1.6f;
            if (f > 10) {
                frames++;
                lagNew += x - newest;
                lagOld += x - old.newest;
                // How far the fade-in's move this frame differs from the car's.
                jerkNew += std::fabs((fadeNew - lastFadeNew) - speed * dt);
                jerkOld += std::fabs((fadeOld - lastFadeOld) - speed * dt);
                backNew += fadeNew < lastFadeNew - 1e-4f;
                backOld += fadeOld < lastFadeOld - 1e-4f;
            }
            lastFadeNew = fadeNew;
            lastFadeOld = fadeOld;
        }
        printf("trail test %4.0f m/s, 144 fps: end behind the car  new %.3f m, old %.3f m (mean); fade-in step off the car's  "
               "new %.4f m, old %.4f m (mean); frames it went back  new %d, old %d of %d\n",
               speed, lagNew / frames, lagOld / frames, jerkNew / frames, jerkOld / frames, backNew, backOld, frames);
    }
    return 0;
}

// The trail shader's line near the car, on the CPU (trail.hlsl VS_Trail: the smoothing, the line's
// direction, the way back to the car that places the fade-in). `ramp`: the shader since
// 2026-10-17 (a pair of neighbours joins a point's smoothing as the one ahead moves 0 to 3 m away
// from the car); without it the shader before.
struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float dot3(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float len3(V3 a) { return std::sqrt(dot3(a, a)); }
V3 cross3(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

struct TrailShape {
    const TrailStage& t;
    float count, y; // g_trail.x (points), g_trail.y (next index + ring)
    bool ramp;
    static constexpr float kRing = 16384.0f;
    float w(float i) const { return t.point(static_cast<UINT>(std::fmod(i + 2.0f * kRing, kRing)))[3]; }
    V3 p(float i) const {
        const float* q = t.point(static_cast<UINT>(std::fmod(i + 2.0f * kRing, kRing)));
        return {q[0], q[1], q[2]};
    }
    // Metres from the car back to point i (100: far), as the shader's loop.
    float along(float i) const {
        const float newest = y - 1.0f;
        if (newest - i >= 28.0f || i > newest + 0.5f) return 100.0f;
        float path = 0.0f;
        V3 q = p(newest);
        for (int k = 1; k <= 28 && newest - k >= i - 0.5f; k++) {
            const V3 r = p(newest - k);
            path += len3(q - r);
            q = r;
        }
        return path;
    }
    V3 smooth(float i) const {
        const float first = y - count, last = y - 1.0f;
        const V3 c = p(i);
        V3 back[3], ahead[3];
        float nb = 0, nf = 0;
        bool open = w(i) > 0.0f;
        for (int k = 1; k <= 3; k++) {
            back[k - 1] = p(i - k);
            open = open && i - k >= first && w(i - k) != 0.0f && len3(back[k - 1] - c) < 12.0f;
            nb += open ? 1.0f : 0.0f;
            open = open && w(i - k) > 0.0f;
        }
        open = true;
        for (int k = 1; k <= 3; k++) {
            ahead[k - 1] = p(i + k);
            open = open && i + k <= last && w(i + k) > 0.0f && len3(ahead[k - 1] - c) < 12.0f;
            nf += open ? 1.0f : 0.0f;
        }
        const float n = std::min(nb, nf);
        V3 sum = c * 4.0f;
        float weight = 4.0f;
        for (int k = 1; k <= 3; k++) {
            if (k > n) continue;
            const float pair = (4.0f - k) * (ramp ? std::clamp((along(i + k) - 2.6f) / 3.0f, 0.0f, 1.0f) : 1.0f);
            sum = sum + (back[k - 1] + ahead[k - 1]) * pair;
            weight += 2.0f * pair;
        }
        return sum * (1.0f / weight);
    }
    V3 tangent(float j, V3 s) const {
        const float first = y - count, last = y - 1.0f;
        const V3 pj = p(j), pm = p(j - 1.0f), pp = p(j + 1.0f);
        const bool hasPrev = w(j) > 0.0f && j - 1.0f >= first && w(j - 1.0f) != 0.0f && len3(pm - pj) < 25.0f;
        const bool hasNext = j + 1.0f <= last && w(j + 1.0f) > 0.0f && len3(pp - pj) < 25.0f;
        return (hasNext ? smooth(j + 1.0f) : s) - (hasPrev ? smooth(j - 1.0f) : s);
    }
    // Point j of the line `offset` metres to the side (the tyre lines: 0.75), metres from the car.
    V3 vertex(float j, float offset, float& fromCar) const {
        const V3 s = smooth(j);
        const V3 d = tangent(j, s);
        fromCar = along(j);
        const float l = len3(d);
        if (l < 1e-3f) return s;
        const V3 right = cross3({0, 1, 0}, d * (1.0f / l));
        const float rl = len3(right);
        return s + (rl > 1e-3f ? right * (1.0f / rl) : V3{1, 0, 0}) * offset;
    }
};

// A chase camera rigidly behind the car (6 m back, 3.3 m up, looking 9 degrees down, 2560x1440 at
// the game's 938 px focal length, as in the owner's captures): where a point is on the screen.
struct ChaseCamera {
    V3 eye, right, up, fwd;
    ChaseCamera(V3 car, V3 heading) {
        eye = car - heading * 6.0f + V3{0, 3.3f, 0};
        const float pitch = 9.2f * 3.14159265f / 180.0f;
        fwd = heading * std::cos(pitch) - V3{0, std::sin(pitch), 0};
        right = cross3({0, 1, 0}, fwd);
        right = right * (1.0f / len3(right));
        up = cross3(fwd, right);
    }
    void screen(V3 p, float& sx, float& sy) const {
        const V3 d = p - eye;
        const float z = dot3(d, fwd);
        sx = 1280.0f + 938.0f * dot3(d, right) / z;
        sy = 720.0f - 938.0f * dot3(d, up) / z;
    }
};

// --trailtest, the line's start: a car on a straight and on a 60 m curve at 125 frames a second
// (frame times +-20 %), its height 2.5 cm up and down around the road (capture 030: -1.9 to +3.1
// cm). Per frame, as the chase camera sees it: how far the fade-in (where it is half on) and the
// right tyre line 2 and 4 m behind the car move on the screen (in a perfect line they stand
// still), and the lowest the first 4 m get under the road (on capture 030 the road hides the line
// from 4.2 cm under it: mp_preview with --car at heights around the road's 25.372).
void trailShapeTest() {
    for (int curve = 0; curve < 2; curve++) {
        for (float speed : {8.0f, 30.0f, 60.0f, 120.0f}) {
            for (int version = 0; version < 2; version++) {
                const bool ramp = version == 1;
                const float lift = ramp ? 0.1f : 0.0f; // effects_trail.h kLift
                const float radius = 60.0f;
                TrailStage trail;
                trail.startRecording();
                unsigned seed = 777;
                auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
                float s = 0.0f, time = 0.0f;
                double fadeMove = 0, fadeMax = 0, tyre2 = 0, tyre4 = 0;
                int frames = 0;
                float lowest = 1.0f;
                float last[3][2] = {};
                bool haveLast = false;
                for (int f = 0; f < 1500; f++) {
                    const float dt = (1.0f / 125.0f) * (0.8f + 0.4f * rnd());
                    s += speed * dt;
                    time += dt;
                    const float height = 0.006f + 0.025f * std::sin(time * 2.0f * 3.14159265f / 0.6f);
                    V3 car, heading;
                    if (curve) {
                        const float a = s / radius;
                        car = {radius * std::sin(a), height, radius * (1.0f - std::cos(a))};
                        heading = {std::cos(a), 0, std::sin(a)};
                    } else {
                        car = {s, height, 0};
                        heading = {1, 0, 0};
                    }
                    const float recorded[3] = {car.x, car.y + lift, car.z};
                    trail.tick(dt);
                    const bool live = trail.record(dt, recorded);
                    const UINT points = live ? std::min(trail.count() + 1, 16384u) : trail.count();
                    const UINT next = live ? (trail.head() + 1) % 16384 : trail.head();
                    if (points < 40 || f < 100) continue;
                    const TrailShape shape{trail, static_cast<float>(points), static_cast<float>(next) + TrailShape::kRing, ramp};
                    const ChaseCamera camera(car, heading);
                    // The right tyre line from the car back: its points and metres from the car.
                    V3 line[30];
                    float from[30];
                    for (int k = 0; k < 30; k++) line[k] = shape.vertex(shape.y - 1.0f - k, 0.75f, from[k]);
                    // Where the fade-in is half on (2.1 m from the car along the line).
                    float now[3][2] = {};
                    for (int k = 0; k + 1 < 30; k++) {
                        if (from[k] <= 2.1f && from[k + 1] > 2.1f) {
                            const float u = (2.1f - from[k]) / (from[k + 1] - from[k]);
                            camera.screen(line[k] + (line[k + 1] - line[k]) * u, now[0][0], now[0][1]);
                            break;
                        }
                    }
                    // The tyre line 2 and 4 m behind the car (on the true path), as the camera sees it.
                    for (int m = 0; m < 2; m++) {
                        const float back = m == 0 ? 2.0f : 4.0f;
                        for (int k = 0; k + 1 < 30; k++) {
                            const float da = dot3(car - line[k], heading), db = dot3(car - line[k + 1], heading);
                            if (curve) {
                                // On the curve: the angle around its centre.
                                const V3 centre{0, 0, radius};
                                const float a = s / radius - back / radius;
                                const V3 tangentDir{std::cos(a), 0, std::sin(a)};
                                const float ta = dot3(line[k] - centre, tangentDir), tb = dot3(line[k + 1] - centre, tangentDir);
                                if (ta >= 0.0f && tb < 0.0f) {
                                    const V3 hit = line[k] + (line[k + 1] - line[k]) * (ta / (ta - tb));
                                    camera.screen(hit, now[1 + m][0], now[1 + m][1]);
                                    break;
                                }
                            } else if (da <= back && db > back) {
                                const V3 hit = line[k] + (line[k + 1] - line[k]) * ((back - da) / (db - da));
                                camera.screen(hit, now[1 + m][0], now[1 + m][1]);
                                break;
                            }
                        }
                    }
                    // The road is at height 0.
                    for (int k = 0; k < 30 && from[k] < 4.0f; k++) lowest = std::min(lowest, line[k].y);
                    if (haveLast) {
                        const float fm = std::fabs(now[0][1] - last[0][1]);
                        fadeMove += fm;
                        fadeMax = std::max<double>(fadeMax, fm);
                        tyre2 += std::hypot(now[1][0] - last[1][0], now[1][1] - last[1][1]);
                        tyre4 += std::hypot(now[2][0] - last[2][0], now[2][1] - last[2][1]);
                        frames++;
                    }
                    memcpy(last, now, sizeof(last));
                    haveLast = true;
                }
                printf("trail start %-8s %4.0f m/s %s: fade-in moves %5.2f px (max %5.1f), tyre line at 2 m %5.2f px, at 4 m %5.2f px "
                       "(mean per frame); first 4 m at least %+5.1f cm over the road\n",
                       curve ? "curve" : "straight", speed, ramp ? "new" : "old", fadeMove / frames, fadeMax, tyre2 / frames, tyre4 / frames,
                       lowest * 100.0f);
            }
        }
    }
}

// --cartest: which car the trail follows (CarTracker, engine_frame.h) against the pick before
// 2026-10-17 (the car that continues last frame's within 30 m, else the nearest; a frame without
// any car forgot it). The player drives 100 m/s at 125 frames a second, a ghost 90 m/s 0.4 m to
// the side starts 40 m ahead (the player drives into it and through it). Scenes: the chase camera
// all along; 0.5 s chase, 1.5 s cockpit (its car drawn or not), chase again; the same with the
// ghost only 10 m ahead as the cockpit starts; a respawn 50 m back. Exit code 1: the new pick was
// on the ghost in a chase camera after the first scene's start.
int carTest() {
    struct OldPick {
        bool lastValid = false;
        float last[3] = {};
        int pick(const CarCandidate* cars, int count, const V3& eye) {
            float best = lastValid ? 30.0f * 30.0f : 1e30f;
            const float ref[3] = {lastValid ? last[0] : eye.x, lastValid ? last[1] : eye.y, lastValid ? last[2] : eye.z};
            int found = -1;
            for (int c = 0; c < count; c++) {
                const float dx = cars[c].world[0] - ref[0], dy = cars[c].world[1] - ref[1], dz = cars[c].world[2] - ref[2];
                const float d = dx * dx + dy * dy + dz * dz;
                if (d < best) {
                    best = d;
                    found = c;
                }
            }
            lastValid = found >= 0;
            if (found >= 0) memcpy(last, cars[found].world, sizeof(last));
            return found;
        }
    };
    const char* scenes[] = {"chase camera", "cockpit, car not drawn", "cockpit, car drawn", "respawn 50 m back", "cockpit, ghost close"};
    int failures = 0;
    for (int scene = 0; scene < 5; scene++) {
        CarTracker tracker;
        OldPick old;
        unsigned seed = 99;
        auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
        double now = 100.0;
        float player = 0.0f, ghost = scene == 4 ? 15.0f : 40.0f;
        int ghostNew = 0, ghostOld = 0, noneNew = 0, noneOld = 0, ghostNewAfter = 0, ghostOldAfter = 0, tracks = 0;
        unsigned lastTrack = 0;
        for (int f = 0; f < 500; f++) {
            const float dt = (1.0f / 125.0f) * (0.9f + 0.2f * rnd());
            now += dt;
            const float t = f / 125.0f;
            player += 100.0f * dt;
            ghost += 90.0f * dt;
            if (scene == 3 && f == 200) player -= 50.0f;
            const bool cockpit = (scene == 1 || scene == 2 || scene == 4) && t >= 0.5f && t < 2.0f;
            const V3 p{player, 0, 0}, g{ghost, 0, -0.4f};
            // The camera: chase (6 m back, 3.3 m up, aimed at the car) or cockpit (at the driver's head).
            V3 eye, fwd;
            if (cockpit) {
                eye = p + V3{0.2f, 1.0f, 0};
                fwd = {1, 0, 0};
            } else {
                eye = p - V3{6.0f, -3.3f, 0};
                fwd = p - eye;
                fwd = fwd * (1.0f / len3(fwd));
            }
            V3 right = cross3({0, 1, 0}, fwd);
            right = right * (1.0f / len3(right));
            const V3 up = cross3(fwd, right);
            CarCandidate cars[2];
            int count = 0, playerIndex = -1;
            auto add = [&](const V3& w) {
                const V3 d = w - eye;
                CarCandidate& c = cars[count++];
                c.view[0] = dot3(d, right);
                c.view[1] = dot3(d, up);
                c.view[2] = dot3(d, fwd);
                c.world[0] = w.x;
                c.world[1] = w.y;
                c.world[2] = w.z;
            };
            // The ghost first (the engine's order is not known), the player's car unless the cockpit hides it.
            if (len3(g - eye) < 400.0f) add(g);
            if (!((scene == 1 || scene == 4) && cockpit)) {
                playerIndex = count;
                add(p);
            }
            const int n = tracker.pick(cars, count, now);
            const int o = old.pick(cars, count, eye);
            const bool after = scene == 3 ? f >= 210 : (scene == 0 ? f >= 0 : (scene == 4 ? f >= 250 + 6 : t >= 2.1f));
            ghostNew += n >= 0 && n != playerIndex;
            ghostOld += o >= 0 && o != playerIndex;
            noneNew += n < 0;
            noneOld += o < 0;
            if (after) {
                ghostNewAfter += n >= 0 && n != playerIndex;
                ghostOldAfter += o >= 0 && o != playerIndex;
            }
            if (n >= 0 && tracker.track() != lastTrack) {
                tracks++;
                lastTrack = tracker.track();
                if (getenv("CARTEST_VERBOSE"))
                    printf("  frame %d (%.2f s): takes up the %s\n", f, t, n == playerIndex ? "player" : "ghost");
            }
        }
        printf("car test %-24s on the ghost  new %3d, old %3d of 500 frames (in the chase camera afterwards  new %3d, old %3d); "
               "no car  new %3d, old %3d; cars taken up %d\n",
               scenes[scene], ghostNew, ghostOld, ghostNewAfter, ghostOldAfter, noneNew, noneOld, tracks);
        failures += ghostNewAfter > 0;
    }
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--trailtest")) {
        trailShapeTest();
        return trailTest();
    }
    if (argc == 2 && !strcmp(argv[1], "--cartest")) return carTest();
    if (argc < 3) {
        printf("usage: mp_preview <capture folder> <out.bmp> [--preset Name] [--view N] [--set Key=Value ...]\n");
        return 2;
    }
    tmshaders::log::setFileName(L"tmvs-mp-preview.log"); // never truncate the game's log
    const std::string folder = argv[1];
    bool hdrStage = false;
    double time = 10.0;
    int frames = 1;       // --frames: the HDR stage this many times, 1/60 s apart (TAA, motion blur, auto focus)
    bool photo = false;   // --photo: photo mode (full quality, full-res depth of field)
    float trailCar[3] = {}; // --car x,y,z: a car driving to there over the frames (the neon trail)
    bool haveTrailCar = false;
    float trailFrom[3] = {}; // --carfrom x,y,z: where it starts (default: from the camera's side, at --car's height)
    bool haveTrailFrom = false;
    float focus[2] = {-1.0f, -1.0f}; // --focus u,v: the photo mode's focus point
    std::string toneCbPath = folder + "/tonemap_cb.bin";

    // The look: a built-in or your preset (default Vibrant), then --set values on top.
    tmshaders::Config& config = tmshaders::Config::get();
    config.selectPreset("Vibrant");
    Options& opts = options::current();
    for (int i = 3; i < argc; i++) {
        const std::string opt = argv[i];
        if (opt == "--hdr") {
            hdrStage = true;
            continue;
        }
        if (opt == "--photo") {
            photo = true;
            continue;
        }
        if (i + 1 >= argc) {
            printf("%s needs a value\n", opt.c_str());
            return 2;
        }
        const std::string value = argv[++i];
        if (opt == "--preset") {
            if (!config.selectPreset(value)) {
                printf("unknown preset %s\n", value.c_str());
                return 2;
            }
        }
        else if (opt == "--tonecb") toneCbPath = value;
        else if (opt == "--time") time = atof(value.c_str());
        else if (opt == "--frames") frames = std::max(1, atoi(value.c_str()));
        else if (opt == "--car") haveTrailCar = sscanf(value.c_str(), "%f,%f,%f", &trailCar[0], &trailCar[1], &trailCar[2]) == 3;
        else if (opt == "--carfrom") haveTrailFrom = sscanf(value.c_str(), "%f,%f,%f", &trailFrom[0], &trailFrom[1], &trailFrom[2]) == 3;
        else if (opt == "--focus") {
            if (sscanf(value.c_str(), "%f,%f", &focus[0], &focus[1]) != 2) {
                printf("--focus wants u,v\n");
                return 2;
            }
        }
        else if (opt == "--debug") config.settings.debugView = atoi(value.c_str());
        else if (opt == "--view") opts.view = static_cast<View>(atoi(value.c_str()));
        else if (opt == "--set") {
            const size_t eq = value.find('=');
            if (eq == std::string::npos || !setValue(value.substr(0, eq), value.substr(eq + 1))) {
                printf("unknown setting %s\n", value.c_str());
                return 2;
            }
        } else {
            printf("unknown option %s\n", opt.c_str());
            return 2;
        }
    }

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                                 &ctx))) {
        printf("no D3D11 device\n");
        return 2;
    }

    Dds input;
    if (!readDds(folder + "/input.dds", input)) {
        printf("%s/input.dds missing (a capture taken while the mod graded)\n", folder.c_str());
        return 2;
    }
    DXGI_FORMAT viewFormat = input.format;
    if (FILE* f = fopen((folder + "/view.txt").c_str(), "r")) {
        int v = 0;
        if (fscanf(f, "%d", &v) == 1 && v > 0) viewFormat = static_cast<DXGI_FORMAT>(v);
        fclose(f);
    }
    ID3D11Texture2D* image = upload(device, input, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    if (!image) return 2;
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = viewFormat;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(device->CreateRenderTargetView(image, &rd, &rtv))) {
        printf("cannot make a view of format %d\n", viewFormat);
        return 2;
    }

    ID3D11Resource* tex[kTexCount] = {};
    for (int i = 0; i < kTexCount; i++) {
        Dds dds;
        if (!readDds(folder + "/engine_" + engineTextureName(i) + ".dds", dds)) continue;
        tex[i] = upload(device, dds, D3D11_BIND_SHADER_RESOURCE);
        if (tex[i]) printf("  engine %s %ux%u format %d\n", engineTextureName(i), dds.width, dds.height, dds.format);
    }
    Camera camera;
    bool haveCamera = false;
    if (FILE* f = fopen((folder + "/scenev.bin").c_str(), "rb")) {
        haveCamera = fread(camera.raw, 1, sizeof(camera.raw), f) == sizeof(camera.raw) && decodeCamera(camera);
        fclose(f);
    }
    if (FILE* f = fopen((folder + "/scenep.bin").c_str(), "rb")) {
        camera.hasSceneP = fread(camera.rawP, 1, sizeof(camera.rawP), f) == sizeof(camera.rawP);
        fclose(f);
        if (haveCamera && decodeSceneP(camera)) printf("  sun from the lighting constants\n");
    }
    printf("  camera %s\n", haveCamera ? "found" : "missing");

    // The sun shadow pass (volumetric light): constants, raster scale, comparison sampler.
    EngineShadow shadow;
    {
        std::vector<char> cb;
        if (FILE* f = fopen((folder + "/shadow_cb.bin").c_str(), "rb")) {
            fseek(f, 0, SEEK_END);
            cb.resize(static_cast<size_t>(ftell(f)));
            fseek(f, 0, SEEK_SET);
            if (cb.empty() || fread(cb.data(), 1, cb.size(), f) != cb.size()) cb.clear();
            fclose(f);
        }
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        bool haveRaster = false;
        if (FILE* f = fopen((folder + "/shadow.txt").c_str(), "r")) {
            int filter = 0, au = 0, av = 0, aw = 0, cmp = 0;
            haveRaster = fscanf(f, "raster %f %f %f %f\nfilter %d\naddress %d %d %d\ncompare %d", &shadow.raster[0], &shadow.raster[1],
                                &shadow.raster[2], &shadow.raster[3], &filter, &au, &av, &aw, &cmp) == 9;
            if (haveRaster) {
                sd.Filter = static_cast<D3D11_FILTER>(filter);
                sd.AddressU = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(au);
                sd.AddressV = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(av);
                sd.AddressW = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(aw);
                sd.ComparisonFunc = static_cast<D3D11_COMPARISON_FUNC>(cmp);
            }
            fclose(f);
        }
        if (!cb.empty() && haveRaster) {
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = static_cast<UINT>(cb.size());
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA init = {cb.data(), 0, 0};
            device->CreateBuffer(&bd, &init, &shadow.constants);
            device->CreateSamplerState(&sd, &shadow.sampler);
            shadow.constantsSize = bd.ByteWidth;
            shadow.valid = shadow.constants && shadow.sampler && tex[kTexShadowMap0] && tex[kTexShadowMap1];
        }
        printf("  sun shadow pass %s\n", shadow.valid ? "found (volumetric light)" : "missing");
    }

    Look look;
    look.settings = &config.settings;
    look.lut = config.lutFile;
    look.lutAmount = config.lutAmount;
    if (haveCamera) {
        const float peak = std::max(camera.sunColour[0], std::max(camera.sunColour[1], camera.sunColour[2]));
        if (peak > 1e-3f)
            for (int i = 0; i < 3; i++) look.sun[i] = camera.sunColour[i] / peak;
    }
    look.split = opts.view == View::Split ? opts.split : 0.0f;

    Renderer renderer;
    if (!renderer.init(device)) return 2;
    if (hdrStage) {
        std::vector<char> cb;
        if (FILE* f = fopen(toneCbPath.c_str(), "rb")) {
            cb.resize(48);
            if (fread(cb.data(), 1, cb.size(), f) != cb.size()) cb.clear();
            fclose(f);
        }
        if (cb.empty() || !tex[kTexHdr] || !tex[kTexExposure]) {
            printf("--hdr needs engine_hdr.dds, engine_exposure.dds and the tone-map constants (%s)\n", toneCbPath.c_str());
            return 2;
        }
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = static_cast<UINT>(cb.size());
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init = {cb.data(), 0, 0};
        ID3D11Buffer* engineConstants = nullptr;
        device->CreateBuffer(&bd, &init, &engineConstants);
        ID3D11ShaderResourceView* in[2] = {};
        for (int i = 0; i < 2; i++) {
            ID3D11Texture2D* t = static_cast<ID3D11Texture2D*>(tex[i == 0 ? kTexHdr : kTexExposure]);
            D3D11_TEXTURE2D_DESC td;
            t->GetDesc(&td);
            D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
            sv.Format = readableFormat(td.Format);
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(t, &sv, &in[i]);
        }
        EngineView engine{tex, haveCamera ? &camera : nullptr};
        engine.shadow = shadow.valid ? &shadow : nullptr;
        engine.photo = photo;
        engine.focus[0] = focus[0];
        engine.focus[1] = focus[1];
        for (int f = 0; f < frames; f++) {
            engine.time = time + (f - frames + 1) / 60.0;
            // The car: from the camera's side (or --carfrom) towards --car, arriving in the last frame.
            EngineCar car;
            if (haveTrailCar && haveCamera) {
                const float k = frames > 1 ? static_cast<float>(f) / (frames - 1) : 1.0f;
                for (int i = 0; i < 3; i++) car.position[i] = camera.eye[i] + (trailCar[i] - camera.eye[i]) * (0.3f + 0.7f * k);
                car.position[1] = trailCar[1];
                if (haveTrailFrom)
                    for (int i = 0; i < 3; i++) car.position[i] = trailFrom[i] + (trailCar[i] - trailFrom[i]) * k;
                car.valid = true;
                car.up[1] = 1.0f;
                engine.car = &car;
            }
            if (!renderer.toneMap(ctx, rtv, in[0], in[1], engineConstants, look, engine)) {
                printf("the HDR stage failed\n");
                return 1;
            }
        }
        printf("  HDR stage: preset %s, curve %d, bloom %.2f, scene effects %s\n", config.preset.c_str(),
               config.settings.toneCurve, config.settings.bloom, renderer.effectsRan() ? "ran" : "off");
    }
    const EngineView view{tex, haveCamera ? &camera : nullptr};
    if (!renderer.apply(ctx, rtv, view, look, opts, hdrStage)) {
        printf("the renderer refused the image\n");
        return 1;
    }

    D3D11_TEXTURE2D_DESC sd;
    image->GetDesc(&sd);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    device->CreateTexture2D(&sd, nullptr, &staging);
    ctx->CopyResource(staging, image);
    D3D11_MAPPED_SUBRESOURCE m;
    if (!staging || FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) return 1;
    const bool bgra = sd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    const bool ok = writeBmp(argv[2], m, sd.Width, sd.Height, bgra);
    ctx->Unmap(staging, 0);
    printf("%s %s\n", ok ? "wrote" : "cannot write", argv[2]);
    return ok ? 0 : 1;
}
