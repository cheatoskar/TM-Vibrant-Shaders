#include "photo.h"
#include "config.h"
#include "engine.h"
#include "image.h"
#include "log.h"
#include "settings.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tmshaders {
namespace photo {
namespace {

int g_manual = -1;          // F6: -1 follow the free camera, 0 off, 1 on
bool g_wasFreeCam = false;
bool g_active = false;
float g_focus[2] = {-1.0f, -1.0f};

enum class Job { None, Single, Tiles };
Job g_job = Job::None;
int g_tiles = 2;            // per side
int g_tile = 0;             // the tile being taken (row by row, top left first)
int g_settle = 0;           // frames rendered with this tile's projection
UINT g_width = 0, g_height = 0;   // the screen
UINT g_marginX = 0, g_marginY = 0; // cropped off every tile (screen effects need room)
UINT g_innerW = 0, g_innerH = 0;
std::vector<unsigned char> g_rgb;  // the whole photo
// TAA, GI and the bounce light build up over a few frames after the projection changed.
constexpr int kSettleFrames = 12;

SRWLOCK g_statusLock = SRWLOCK_INIT;
std::string g_status;
DWORD g_statusUntil = 0;   // tick count when it goes, 0 = until replaced
char g_statusCopy[256] = {};

void setStatus(const std::string& text, DWORD seconds) {
    AcquireSRWLockExclusive(&g_statusLock);
    g_status = text;
    g_statusUntil = seconds ? GetTickCount() + seconds * 1000 : 0;
    ReleaseSRWLockExclusive(&g_statusLock);
}

std::string hint() {
    char text[160];
    const Config& config = Config::get();
    const float factor = static_cast<float>(config.photoTiles) * 0.84f;
    snprintf(text, sizeof(text), "Photo mode   F12 photo   Shift+F12 %.1fx photo   middle click: focus   Ctrl+wheel: blur   F6: off", factor);
    return text;
}

struct SaveJob {
    std::wstring path;
    std::vector<unsigned char> rgb;
    int width;
    int height;
};

DWORD WINAPI saveThread(LPVOID param) {
    SaveJob* job = static_cast<SaveJob*>(param);
    const bool ok = image::writePng(job->path, job->rgb.data(), job->width, job->height);
    const size_t slash = job->path.find_last_of(L"\\/");
    const std::wstring file = slash == std::wstring::npos ? job->path : job->path.substr(slash + 1);
    TMVS_LOG("photo: %ls (%dx%d) %s", file.c_str(), job->width, job->height, ok ? "saved" : "failed");
    char text[200];
    snprintf(text, sizeof(text), ok ? "Saved %ls (%dx%d)" : "Could not save %ls", file.c_str(), job->width, job->height);
    setStatus(text, 4);
    delete job;
    return 0;
}

void save(std::vector<unsigned char>&& rgb, int width, int height) {
    SaveJob* job = new SaveJob{L"", std::move(rgb), width, height};
    wchar_t name[64];
    for (int n = 0;; n++) {
        swprintf(name, 64, L"\\photo_%03d.png", n);
        if (GetFileAttributesW((log::dataDir() + name).c_str()) == INVALID_FILE_ATTRIBUTES) break;
    }
    job->path = log::dataDir() + name;
    setStatus("Saving the photo ...", 0);
    // PNG of a large photo takes seconds: not on the game's thread.
    HANDLE thread = CreateThread(nullptr, 0, saveThread, job, 0, nullptr);
    if (thread) CloseHandle(thread);
    else saveThread(job);
}

// Copies `rect` of the frame as RGB to dst (pitch in bytes).
bool readFrame(IDirect3DDevice9* device, IDirect3DSurface9* frame, const RECT& rect, unsigned char* dst, size_t pitch) {
    D3DSURFACE_DESC desc{};
    frame->GetDesc(&desc);
    IDirect3DSurface9* copy = nullptr;
    IDirect3DSurface9* readback = nullptr;
    bool ok = SUCCEEDED(device->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &copy, nullptr)) &&
              SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr)) &&
              SUCCEEDED(device->StretchRect(frame, nullptr, copy, nullptr, D3DTEXF_POINT)) &&
              SUCCEEDED(device->GetRenderTargetData(copy, readback));
    if (ok) {
        D3DLOCKED_RECT locked{};
        ok = SUCCEEDED(readback->LockRect(&locked, &rect, D3DLOCK_READONLY));
        if (ok) {
            const LONG w = rect.right - rect.left, h = rect.bottom - rect.top;
            for (LONG y = 0; y < h; y++) {
                const unsigned char* src = static_cast<const unsigned char*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch;
                unsigned char* out = dst + static_cast<size_t>(y) * pitch;
                for (LONG x = 0; x < w; x++) {
                    out[x * 3 + 0] = src[x * 4 + 2];
                    out[x * 3 + 1] = src[x * 4 + 1];
                    out[x * 3 + 2] = src[x * 4 + 0];
                }
            }
            readback->UnlockRect();
        }
    }
    if (copy) copy->Release();
    if (readback) readback->Release();
    return ok;
}

void cancel(const char* why) {
    if (g_job == Job::None) return;
    TMVS_LOG("photo: cancelled (%s)", why);
    g_job = Job::None;
    g_rgb.clear();
    g_rgb.shrink_to_fit();
    engine::setProjectionTile(1.0f, 1.0f, 0.0f, 0.0f);
    setStatus(std::string("Photo cancelled: ") + why, 4);
}

} // namespace

bool active() {
    return g_active;
}

void toggle() {
    g_manual = g_active ? 0 : 1;
}

void beginFrame() {
    const bool freeCam = engine::freeCamActive();
    if (freeCam != g_wasFreeCam) {
        g_wasFreeCam = freeCam;
        g_manual = -1; // entering or leaving cam 7 hands it back to the automatic
        TMVS_LOG("photo: free camera %s", freeCam ? "on" : "off");
    }
    const Config& config = Config::get();
    const bool wanted = config.settings.enabled && (g_manual >= 0 ? g_manual == 1 : (config.photoAuto && freeCam));
    if (wanted != g_active) {
        g_active = wanted;
        TMVS_LOG("photo: mode %s", wanted ? "on" : "off");
        if (wanted) {
            setStatus(hint(), 6); // the keys, for a few seconds
        } else {
            cancel("photo mode off");
            g_focus[0] = g_focus[1] = -1.0f;
            setStatus("", 0);
        }
    }
    engine::hideOverlay(g_active);

    if (g_job == Job::Tiles) {
        const int n = g_tiles;
        const int i = g_tile % n, j = g_tile / n;
        const float sx = static_cast<float>(n) * g_innerW / g_width;
        const float sy = static_cast<float>(n) * g_innerH / g_height;
        const float cx = -1.0f + (2.0f * i + 1.0f) / n;
        const float cy = 1.0f - (2.0f * j + 1.0f) / n;
        engine::setProjectionTile(sx, sy, -sx * cx, -sy * cy);
    } else {
        engine::setProjectionTile(1.0f, 1.0f, 0.0f, 0.0f);
    }
}

bool handleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!g_active || g_job != Job::None) return false;
    if (msg == WM_MBUTTONDOWN) {
        if (wparam & MK_CONTROL) {
            g_focus[0] = g_focus[1] = -1.0f;
            setStatus("Focus off", 2);
            return true;
        }
        RECT client{};
        GetClientRect(hwnd, &client);
        if (client.right <= 0 || client.bottom <= 0) return false;
        g_focus[0] = static_cast<float>(static_cast<short>(LOWORD(lparam))) / client.right;
        g_focus[1] = static_cast<float>(static_cast<short>(HIWORD(lparam))) / client.bottom;
        if (Config::get().photoDof <= 0.0f) {
            Config::get().photoDof = 0.5f;
            Config::get().markDirty();
        }
        setStatus("Focus set", 2);
        return true;
    }
    if (msg == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL)) {
        Config& config = Config::get();
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        float v = config.photoDof + steps * 0.05f;
        config.photoDof = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        config.markDirty();
        char text[64];
        snprintf(text, sizeof(text), "Depth of field %.0f %%", config.photoDof * 100.0f);
        setStatus(text, 2);
        return true;
    }
    if (msg == WM_MBUTTONUP) return true;
    return false;
}

void request(bool hiRes) {
    if (!g_active || g_job != Job::None) return;
    if (hiRes) {
        const int n = Config::get().photoTiles;
        g_tiles = n < 2 ? 2 : (n > 4 ? 4 : n);
        g_tile = 0;
        g_settle = 0;
        g_width = 0; // known with the first frame
        g_job = Job::Tiles;
        TMVS_LOG("photo: hi-res, %dx%d tiles", g_tiles, g_tiles);
    } else {
        g_job = Job::Single;
    }
}

void adjust(Settings& s) {
    if (!g_active) return;
    // Stills: the most of every effect, and no smear of a moving camera.
    s.quality = 2;
    s.motionBlur = 0.0f;
    s.spray = 0.0f;
    const Config& config = Config::get();
    if (g_focus[0] >= 0.0f && config.photoDof > 0.0f) {
        s.depthOfField = config.photoDof;
        s.focusDistance = 0.0f;
    } else {
        s.depthOfField = 0.0f;
    }
    if (g_job == Job::Tiles) {
        // Effects of the whole frame would repeat in every tile.
        s.vignette = 0.0f;
        s.filmGrain = 0.0f;
        s.chromaticAberration = 0.0f;
        s.lensFlare = 0.0f;
        s.godRays = 0.0f;
        s.taaJitter = false;
        // The blur is measured in pixels: the photo has more of them.
        s.bokehSize *= static_cast<float>(g_tiles) * (g_innerW ? static_cast<float>(g_innerW) / g_width : 0.84f);
    }
}

bool tiling() {
    return g_job == Job::Tiles;
}

void focusPoint(float out[2]) {
    // Tiles keep the focus measured before (it is frozen): the point is in screen space.
    out[0] = g_focus[0];
    out[1] = g_focus[1];
}

void afterRender(IDirect3DDevice9* device, IDirect3DSurface9* frame) {
    if (g_job == Job::None || !frame) return;
    D3DSURFACE_DESC desc{};
    frame->GetDesc(&desc);
    if (g_job == Job::Single) {
        g_job = Job::None;
        std::vector<unsigned char> rgb(static_cast<size_t>(desc.Width) * desc.Height * 3);
        const RECT all{0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
        if (readFrame(device, frame, all, rgb.data(), static_cast<size_t>(desc.Width) * 3)) save(std::move(rgb), desc.Width, desc.Height);
        else setStatus("Could not read the frame", 4);
        return;
    }
    // Tiles.
    if (g_width == 0) {
        g_width = desc.Width;
        g_height = desc.Height;
        g_marginX = static_cast<UINT>(desc.Width * 0.08f);
        g_marginY = static_cast<UINT>(desc.Height * 0.08f);
        g_innerW = desc.Width - 2 * g_marginX;
        g_innerH = desc.Height - 2 * g_marginY;
        const size_t bytes = static_cast<size_t>(g_innerW) * g_tiles * g_innerH * g_tiles * 3;
        try {
            g_rgb.assign(bytes, 0);
        } catch (...) {
            cancel("not enough memory");
            return;
        }
        g_settle = 0;
        return; // the tile projection starts with the next frame
    }
    if (desc.Width != g_width || desc.Height != g_height) {
        cancel("the window changed size");
        return;
    }
    if (++g_settle < kSettleFrames) return;
    const int n = g_tiles;
    const int i = g_tile % n, j = g_tile / n;
    const size_t pitch = static_cast<size_t>(g_innerW) * n * 3;
    unsigned char* dst = g_rgb.data() + static_cast<size_t>(j) * g_innerH * pitch + static_cast<size_t>(i) * g_innerW * 3;
    const RECT inner{static_cast<LONG>(g_marginX), static_cast<LONG>(g_marginY), static_cast<LONG>(g_marginX + g_innerW),
                     static_cast<LONG>(g_marginY + g_innerH)};
    if (!readFrame(device, frame, inner, dst, pitch)) {
        cancel("could not read the frame");
        return;
    }
    g_settle = 0;
    char text[64];
    snprintf(text, sizeof(text), "Hi-res photo: tile %d of %d (keep still)", g_tile + 1, n * n);
    setStatus(text, 0);
    if (++g_tile < n * n) return;
    g_job = Job::None;
    engine::setProjectionTile(1.0f, 1.0f, 0.0f, 0.0f);
    save(std::move(g_rgb), static_cast<int>(g_innerW) * n, static_cast<int>(g_innerH) * n);
    g_rgb.clear();
}

const char* status() {
    AcquireSRWLockExclusive(&g_statusLock);
    if (g_statusUntil && GetTickCount() > g_statusUntil) {
        g_status.clear();
        g_statusUntil = 0;
    }
    strncpy_s(g_statusCopy, g_status.c_str(), _TRUNCATE);
    ReleaseSRWLockExclusive(&g_statusLock);
    return g_statusCopy;
}

} // namespace photo
} // namespace tmshaders
