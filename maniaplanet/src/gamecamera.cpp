#include "gamecamera.h"
#include "game.h"
#include "log.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace tmvs_mp {
namespace gamecamera {
namespace {

constexpr uint32_t kGameCameraId = 0x03023000;  // CGameCamera
constexpr uint32_t kSceneCameraId = 0x0A00A000; // CSceneCamera
constexpr uint32_t kHmsCameraId = 0x06001000;   // CHmsCamera
constexpr size_t kGameCameraScene = 0xF0;       // CGameCamera::SceneCamera
constexpr size_t kSceneCameraHms = 0x48;        // CSceneCamera's CHmsCamera
constexpr size_t kFovRect = 0x2F0;              // FovRectMin (x, y), FovRectMax (x, y)
constexpr size_t kFovRectEnable = 0x30C;
constexpr size_t kDrawRect = 0x190;             // DrawRectMin, DrawRectMax of render slot 0 (+0x28 a slot)
constexpr size_t kDrawRectSlot = 0x28;

// CHmsCamera::SetFovRect (RVA 0x1CE940: mov rax, [rdx]; mov [rcx+2F0h], rax; mov rax, [rdx+8];
// mov [rcx+2F8h], rax; ret) and ::SetFovRectEnable (0x1CE960: mov [rcx+30Ch], edx; ret): the
// offsets of this build. The frustum's check of FovRectEnable (0x1CEC17: cmp dword [r9+30Ch], 0).
constexpr unsigned char kSetFovRect[] = {0x48, 0x8B, 0x02, 0x48, 0x89, 0x81, 0xF0, 0x02, 0x00, 0x00, 0x48,
                                         0x8B, 0x42, 0x08, 0x48, 0x89, 0x81, 0xF8, 0x02, 0x00, 0x00, 0xC3};
constexpr unsigned char kSetFovRectEnable[] = {0x89, 0x91, 0x0C, 0x03, 0x00, 0x00, 0xC3};
constexpr unsigned char kFrustumCheck[] = {0x41, 0x83, 0xB9, 0x0C, 0x03, 0x00, 0x00, 0x00};

struct Saved {
    uintptr_t camera = 0;
    float rect[4] = {};
    uint32_t enable = 0;
};

bool g_checked = false, g_usable = false;
Saved g_saved;               // the game's own FovRect while the mod's is in (camera 0: none)
float g_written[4] = {};     // what the mod wrote last
bool g_active = false;
float g_sign[2] = {-1.0f, 1.0f}; // FovRect's axes against NDC (x: the camera's x runs left, P00 < 0)
int g_signChecked[2] = {};   // frames that confirmed the direction (0: not yet)
// The last projections asked for (the engine takes them a frame or two later), newest first.
Want g_asked[4];
// Measured: the projection before the mod's FovRect (reference) and the last one.
float g_refScale[2] = {0.0f, 0.0f};
float g_scale[2] = {1.0f, 1.0f}, g_offset[2] = {};
bool g_measured = false;
char g_status[160] = "Off";

bool usable() {
    if (g_checked) return g_usable;
    g_checked = true;
    if (!game::attached()) return false;
    g_usable = game::codeIs(0x1CE940, kSetFovRect, sizeof(kSetFovRect)) &&
               game::codeIs(0x1CE960, kSetFovRectEnable, sizeof(kSetFovRectEnable)) &&
               game::codeIs(0x1CEC17, kFrustumCheck, sizeof(kFrustumCheck));
    TMVS_LOG("gamecamera: %s", g_usable ? "the camera's FovRect found (photo zoom, hi-res photos, TAA jitter)"
                                        : "not the ManiaPlanet build this was made for: no photo zoom, hi-res photos or TAA jitter");
    return g_usable;
}

// The main camera's CHmsCamera, 0 when the chain doesn't hold (no scene, another build).
uintptr_t mainCamera() {
    const uintptr_t a = game::app();
    if (!a) return 0;
    const uintptr_t gameCamera = game::readPointer(a + game::kAppGameCamera);
    if (game::classOf(gameCamera) != kGameCameraId) return 0;
    const uintptr_t scene = game::readPointer(gameCamera + kGameCameraScene);
    if (game::classOf(scene) != kSceneCameraId) return 0;
    const uintptr_t hms = game::readPointer(scene + kSceneCameraHms);
    return game::classOf(hms) == kHmsCameraId ? hms : 0;
}

bool sane(const float* r) {
    for (int i = 0; i < 4; i++)
        if (!std::isfinite(r[i]) || std::fabs(r[i]) > 1e5f) return false;
    return std::fabs(r[2] - r[0]) > 1e-3f && std::fabs(r[3] - r[1]) > 1e-3f;
}

void restore() {
    if (!g_active) return;
    g_active = false;
    const uintptr_t camera = mainCamera();
    if (camera && camera == g_saved.camera) {
        // Always, even when the values there aren't the ones written last (a render between the
        // engine's begin and end moves them by the viewport; it puts back what it found): a
        // camera left zoomed would be worse than a FovRect of the game's own overwritten (the
        // game sets one only around a tiled render of its own, which saves and restores it).
        float now[4] = {};
        const bool ours = game::readBytes(camera + kFovRect, now, sizeof(now)) && !memcmp(now, g_written, sizeof(now));
        game::writeBytes(camera + kFovRectEnable, &g_saved.enable, sizeof(g_saved.enable));
        game::writeBytes(camera + kFovRect, g_saved.rect, sizeof(g_saved.rect));
        TMVS_LOG("gamecamera: the game's own FovRect back (enable %u%s)", g_saved.enable, ours ? "" : "; the camera held other values");
    } else {
        TMVS_LOG("gamecamera: the camera changed meanwhile, nothing to put back");
    }
    g_saved = Saved{};
}

// The FovRect for `want` against DrawRect `d` (min x, min y, max x, max y). The engine maps FovRect
// onto DrawRect (s = D.size / F.size, t = D.c - F.c s per axis) and renders the part A([-1, 1]) of
// the camera's view. Wanted: the part of the view NDC' = S NDC + O shows, centre -O / S and half
// size 1 / S, in the view's own axes (g_sign). So F.half = D.half S and F.c = D.c S + sign O.
void rectFor(const Want& want, const float* d, float* f) {
    for (int a = 0; a < 2; a++) {
        const float dc = 0.5f * (d[a] + d[a + 2]), dh = 0.5f * (d[a + 2] - d[a]);
        const float s = want.scale[a], o = want.offset[a];
        const float fc = dc * s + g_sign[a] * o, fh = dh * s;
        f[a] = fc - fh;
        f[a + 2] = fc + fh;
    }
}

} // namespace

void apply(const Want& want, bool allowed) {
    for (int i = 3; i > 0; i--) g_asked[i] = g_asked[i - 1];
    g_asked[0] = allowed ? want : Want{};
    if (!allowed || !want.any() || !usable()) {
        restore();
        snprintf(g_status, sizeof(g_status), "%s", !allowed ? "Off" : (usable() ? "Ready" : "Not this ManiaPlanet build"));
        return;
    }
    const uintptr_t camera = mainCamera();
    if (!camera) {
        if (g_active) {
            g_active = false; // the camera went (map unloaded): nothing to put back
            g_saved = Saved{};
            TMVS_LOG("gamecamera: the main camera went");
        }
        snprintf(g_status, sizeof(g_status), "No main camera");
        return;
    }
    if (g_active && camera != g_saved.camera) {
        g_active = false;
        g_saved = Saved{};
    }
    if (!g_active) {
        Saved s;
        s.camera = camera;
        if (!game::readBytes(camera + kFovRect, s.rect, sizeof(s.rect)) || !game::readBytes(camera + kFovRectEnable, &s.enable, sizeof(s.enable)))
            return;
        if (s.enable) {
            // The game uses the camera's FovRect itself (a tiled render of its own): left alone.
            static int logged = 0;
            if (logged++ < 3) TMVS_LOG("gamecamera: the game has its own FovRect on: left alone");
            snprintf(g_status, sizeof(g_status), "In use by the game");
            return;
        }
        g_saved = s;
    }
    float d[4] = {}, d1[4] = {};
    if (!game::readBytes(camera + kDrawRect, d, sizeof(d)) || !sane(d)) {
        static int logged = 0;
        if (logged++ < 3) {
            game::readBytes(camera + kDrawRect + kDrawRectSlot, d1, sizeof(d1));
            TMVS_LOG("gamecamera: the main camera's DrawRect makes no sense (%g,%g - %g,%g; second slot %g,%g - %g,%g): left alone", d[0], d[1],
                     d[2], d[3], d1[0], d1[1], d1[2], d1[3]);
        }
        snprintf(g_status, sizeof(g_status), "No draw rectangle");
        return;
    }
    static bool s_drawLogged = false;
    if (!s_drawLogged) {
        s_drawLogged = true;
        game::readBytes(camera + kDrawRect + kDrawRectSlot, d1, sizeof(d1));
        TMVS_LOG("gamecamera: main camera %p, DrawRect %.3f,%.3f - %.3f,%.3f (second slot %.3f,%.3f - %.3f,%.3f), FovRect %.3f,%.3f - %.3f,%.3f",
                 reinterpret_cast<void*>(camera), d[0], d[1], d[2], d[3], d1[0], d1[1], d1[2], d1[3], g_saved.rect[0], g_saved.rect[1],
                 g_saved.rect[2], g_saved.rect[3]);
    }
    float f[4];
    rectFor(want, d, f);
    const uint32_t on = 1;
    if (!game::writeBytes(camera + kFovRect, f, sizeof(f)) || !game::writeBytes(camera + kFovRectEnable, &on, sizeof(on))) return;
    memcpy(g_written, f, sizeof(f));
    if (!g_active) {
        TMVS_LOG("gamecamera: the mod's FovRect in the main camera (scale %.3f %.3f, offset %.4f %.4f%s)", want.scale[0], want.scale[1],
                 want.offset[0], want.offset[1], want.jitter ? ", TAA jitter" : "");
        g_measured = false;
    }
    g_active = true;
    snprintf(g_status, sizeof(g_status), "In use (scale %.2f, offset %.3f %.3f)", want.scale[0], want.offset[0], want.offset[1]);
}

void observe(const float* p) {
    if (!p || !(std::fabs(p[0]) > 1e-4f) || !(std::fabs(p[5]) > 1e-4f)) return;
    // The game's own scale (its field of view changes with the speed): from every frame the mod
    // doesn't scale (nothing, or only TAA jitter, asked for in the last frames).
    bool scaled = false;
    for (const Want& w : g_asked) scaled = scaled || w.scaled();
    if (!g_active || !scaled) {
        g_refScale[0] = p[0];
        g_refScale[1] = p[5];
    }
    if (!g_active) {
        g_scale[0] = g_scale[1] = 1.0f;
        g_offset[0] = g_offset[1] = 0.0f;
        g_measured = false;
        return;
    }
    if (g_refScale[0] == 0.0f) return;
    g_scale[0] = p[0] / g_refScale[0];
    g_scale[1] = p[5] / g_refScale[1];
    g_offset[0] = p[2];
    g_offset[1] = p[6];
    g_measured = true;
    // Which way FovRect's axes run: an offset the engine took with the other sign (against every
    // projection asked for lately) flips that axis, logged. Only from a tile's or a zoom's offsets:
    // TAA's jitter has the same sizes with both signs within a few frames (its own direction
    // doesn't matter, TAA takes the measured one).
    for (int a = 0; a < 2; a++) {
        if (g_signChecked[a] >= 30) continue;
        for (const Want& w : g_asked) {
            const float o = w.offset[a];
            if (std::fabs(o) < 0.02f || w.jitter) continue;
            const float tol = std::fabs(o) * 0.1f + 1e-6f;
            if (std::fabs(g_offset[a] - o) < tol) {
                if (++g_signChecked[a] == 30)
                    TMVS_LOG("gamecamera: the engine's projection follows the FovRect on %s (offset %.5f as asked)", a ? "y" : "x", g_offset[a]);
                break;
            }
            if (std::fabs(g_offset[a] + o) < tol) {
                g_sign[a] = -g_sign[a];
                g_signChecked[a] = 0;
                TMVS_LOG("gamecamera: the FovRect's %s runs the other way (offset %.5f for %.5f): turned", a ? "y" : "x", g_offset[a], o);
                break;
            }
        }
    }
    static int logged = 0;
    if (logged < 6 && (std::fabs(g_scale[0] - 1.0f) > 0.01f || std::fabs(g_offset[0]) > 0.01f)) {
        logged++;
        TMVS_LOG("gamecamera: the engine's projection now: scale %.3f %.3f, offset %.4f %.4f (asked %.3f %.3f, %.4f %.4f)", g_scale[0], g_scale[1],
                 g_offset[0], g_offset[1], g_asked[0].scale[0], g_asked[0].scale[1], g_asked[0].offset[0], g_asked[0].offset[1]);
    }
}

bool matches(const Want& want) {
    if (!g_active || !g_measured) return false;
    for (int a = 0; a < 2; a++) {
        if (std::fabs(g_scale[a] / want.scale[a] - 1.0f) > 0.01f) return false;
        if (std::fabs(g_offset[a] - want.offset[a]) > 0.01f * want.scale[a] + 1e-4f) return false;
    }
    return true;
}

void jitterUv(const float* p, float out[2]) {
    out[0] = out[1] = 0.0f;
    if (!p || !g_active) return;
    // Only while the mod jitters (no tile, no offset of a zoom), and only an offset of a pixel or
    // less (a frame of the camera before or after a tile is no jitter).
    bool jitter = false;
    for (const Want& w : g_asked) jitter = jitter || w.jitter;
    if (!jitter) return;
    if (std::fabs(p[2]) > 0.01f || std::fabs(p[6]) > 0.01f) return;
    // The image's content moves by the NDC offset: x right, y up in NDC, down in uv.
    out[0] = p[2] * 0.5f;
    out[1] = -p[6] * 0.5f;
}

bool active() {
    return g_active;
}

const char* status() {
    return g_status;
}

} // namespace gamecamera
} // namespace tmvs_mp
