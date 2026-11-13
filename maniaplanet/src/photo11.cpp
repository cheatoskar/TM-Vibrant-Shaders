#include "photo11.h"
#include "config.h"
#include "hooks.h"
#include "image.h"
#include "keys.h"
#include "log.h"
#include "options.h"
#include "settings.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace tmvs_mp {
namespace photo {
namespace {

using tmshaders::Config;

// F6 and Esc come from different threads (the render thread polls F6, the window procedure sees
// Esc and the mouse): atomics.
std::atomic<bool> g_active{false};
std::atomic<bool> g_requested{false};
std::atomic<float> g_focusU{-1.0f}, g_focusV{-1.0f};
std::atomic<float> g_zoom{1.0f}; // Shift+wheel, 1..8

// A hi-res photo (TMNF's): tiles of the view, each the screen's size with a margin cropped off
// (screen effects need room at the edges), rendered by the game's camera zoomed in on that part.
// Render thread only (request, cameraWant, afterRender); the window procedure only reads g_job.
enum Job : int { kNone = 0, kTiles = 1 };
std::atomic<int> g_job{kNone};
int g_tiles = 2;                   // per side
int g_tile = 0;                    // the tile being taken (row by row, top left first)
int g_settle = 0;                  // frames rendered with this tile's projection
int g_waited = 0;                  // frames waiting for the engine to take this tile's projection
UINT g_width = 0, g_height = 0;    // the screen
UINT g_marginX = 0, g_marginY = 0; // cropped off every tile
UINT g_innerW = 0, g_innerH = 0;
std::vector<unsigned char> g_rgb;  // the whole photo
gamecamera::Want g_tileWant;       // what the last cameraWant asked for this tile
// TAA and the bounce light build up over a few frames after the projection changed.
constexpr int kSettleFrames = 12;
// The engine takes a FovRect a frame or two later; this long without it the photo is given up.
constexpr int kWaitFrames = 120;

std::mutex g_statusLock;
std::string g_status;
ULONGLONG g_statusUntil = 0; // 0 = until replaced
char g_statusCopy[256] = {};

void setStatus(const std::string& text, DWORD seconds) {
    std::lock_guard<std::mutex> lock(g_statusLock);
    g_status = text;
    g_statusUntil = seconds ? GetTickCount64() + seconds * 1000ull : 0;
}

struct SaveJob {
    std::wstring path;
    std::vector<unsigned char> rgb;
    int width;
    int height;
    // The compare split's names (SplitPhotoLabels, as in TMNF), "" = none.
    std::string left, right;
    float splitAt = 0.5f;
};
void splitLabels(SaveJob& job); // below, with the split's state

DWORD WINAPI saveThread(LPVOID param) {
    SaveJob* job = static_cast<SaveJob*>(param);
    // On this thread too: GDI draws the names in a few ms.
    if (!job->left.empty() || !job->right.empty())
        tmshaders::image::drawSplitLabels(job->rgb.data(), job->width, job->height, 3, static_cast<size_t>(job->width) * 3,
                                          job->splitAt, job->left, job->right);
    const bool ok = tmshaders::image::writePng(job->path, job->rgb.data(), job->width, job->height);
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
    splitLabels(*job);
    wchar_t name[64];
    for (int n = 0;; n++) {
        swprintf(name, 64, L"\\photo_%03d.png", n);
        if (GetFileAttributesW((tmshaders::log::dataDir() + name).c_str()) == INVALID_FILE_ATTRIBUTES) break;
    }
    job->path = tmshaders::log::dataDir() + name;
    setStatus("Saving photo...", 0);
    // A PNG of 2560x1440 takes a while: not on the game's render thread.
    HANDLE thread = CreateThread(nullptr, 0, saveThread, job, 0, nullptr);
    if (thread) CloseHandle(thread);
    else saveThread(job);
}

// The frame's size (false: not a texture).
bool frameSize(ID3D11Resource* frame, UINT& w, UINT& h) {
    ID3D11Texture2D* tex = nullptr;
    if (!frame || FAILED(frame->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) return false;
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    tex->Release();
    w = d.Width;
    h = d.Height;
    return true;
}

// `rect` of the frame as RGB rows into dst, `pitch` bytes apart (8-bit formats: the finished image
// is display values already).
bool readRect(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Resource* frame, const RECT& rect, unsigned char* dst, size_t pitch) {
    ID3D11Texture2D* tex = nullptr;
    if (!frame || FAILED(frame->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) return false;
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                      d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8X8_UNORM ||
                      d.Format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8X8_TYPELESS;
    const bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                      d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const bool inside = rect.left >= 0 && rect.top >= 0 && rect.right <= static_cast<LONG>(d.Width) &&
                        rect.bottom <= static_cast<LONG>(d.Height) && rect.right > rect.left && rect.bottom > rect.top;
    bool ok = false;
    if ((bgra || rgba) && d.SampleDesc.Count == 1 && inside) {
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        ID3D11Texture2D* staging = nullptr;
        if (SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &staging))) {
            ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, nullptr);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
                const int r = bgra ? 2 : 0, b = bgra ? 0 : 2;
                const LONG w = rect.right - rect.left;
                for (LONG y = rect.top; y < rect.bottom; y++) {
                    const auto* src = static_cast<const unsigned char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch +
                                      static_cast<size_t>(rect.left) * 4;
                    unsigned char* out = dst + static_cast<size_t>(y - rect.top) * pitch;
                    for (LONG x = 0; x < w; x++) {
                        out[x * 3 + 0] = src[x * 4 + r];
                        out[x * 3 + 1] = src[x * 4 + 1];
                        out[x * 3 + 2] = src[x * 4 + b];
                    }
                }
                ctx->Unmap(staging, 0);
                ok = true;
            }
            staging->Release();
        }
    } else {
        TMVS_LOG("photo: can't read a frame of format %d (%u samples)", d.Format, d.SampleDesc.Count);
    }
    tex->Release();
    return ok;
}

// Render thread.
void cancel(const char* why) {
    if (g_job == kNone) return;
    TMVS_LOG("photo: hi-res photo cancelled (%s)", why);
    g_job = kNone;
    g_rgb.clear();
    g_rgb.shrink_to_fit();
    setStatus(std::string("Photo cancelled: ") + why, 4);
}

std::string hint() {
    namespace keys = tmshaders::keys;
    const std::string cap = keys::name(keys::get(keys::Action::Capture));
    char factor[32];
    snprintf(factor, sizeof(factor), "%.1fx", static_cast<float>(Config::get().photoTiles) * 0.84f);
    return "Photo mode   " + cap + " photo   Shift+" + cap + " " + factor +
           " photo   middle click: focus   Ctrl+middle click: no focus   Ctrl+wheel: aperture   Shift+wheel: zoom   " +
           keys::name(keys::get(keys::Action::Photo)) + "/Esc: off";
}

} // namespace

bool active() {
    return g_active;
}

bool hideCars() {
    return g_active && Config::get().photoHideCar;
}

float zoom() {
    return g_zoom;
}

bool tiling() {
    return g_active && g_job == kTiles;
}

gamecamera::Want cameraWant(bool gameCamera) {
    gamecamera::Want w;
    if (!g_active) {
        cancel("photo mode off");
        return w;
    }
    if (g_job == kTiles && !gameCamera) cancel("Game camera is off");
    const float z = gameCamera ? static_cast<float>(g_zoom) : 1.0f;
    w.scale[0] = w.scale[1] = z;
    // The screen size comes with the first frame after the request.
    if (g_job == kTiles && g_width > 0 && g_height > 0) {
        // TMNF's tile on top of the zoom: NDC' = s (zoom NDC) + o, the tile's inner part (the
        // screen without its margins) showing 1 / n of the view.
        const int n = g_tiles;
        const int i = g_tile % n, j = g_tile / n;
        const float sx = static_cast<float>(n) * g_innerW / g_width;
        const float sy = static_cast<float>(n) * g_innerH / g_height;
        const float cx = -1.0f + (2.0f * i + 1.0f) / n;
        const float cy = 1.0f - (2.0f * j + 1.0f) / n;
        w.scale[0] = z * sx;
        w.scale[1] = z * sy;
        w.offset[0] = -sx * cx;
        w.offset[1] = -sy * cy;
    }
    g_tileWant = w;
    return w;
}

namespace {
bool g_splitOn = false;
float g_splitPosition = 0.5f;
bool g_splitSwapped = false;
std::string g_splitPreset; // the other side: "" the game's image, else this preset's look

// The photo has the compare split (the line and the other side are in the frame): which side
// shows what, as the labels at the line name them on the screen (never in photos).
void splitLabels(SaveJob& job) {
    const Config& config = Config::get();
    if (!splitShown() || !config.splitPhotoLabels) return;
    job.splitAt = g_splitPosition < 0.01f ? 0.01f : (g_splitPosition > 0.99f ? 0.99f : g_splitPosition);
    job.left = splitName(false);
    job.right = splitName(true);
}
} // namespace

std::string& splitPreset() {
    return g_splitPreset;
}

std::string splitName(bool right) {
    const Config& config = Config::get();
    const std::string mine = !config.settings.enabled ? std::string("Shaders off") : (config.preset.empty() ? std::string("Your look") : config.preset);
    const std::string other = g_splitPreset.empty() ? std::string("Game") : g_splitPreset;
    // The other side is on the left unless swapped.
    return right != g_splitSwapped ? mine : other;
}

bool& splitOn() {
    return g_splitOn;
}

float& splitPosition() {
    return g_splitPosition;
}

bool& splitSwapped() {
    return g_splitSwapped;
}

void toggleSplit() {
    g_splitOn = !g_splitOn;
    if (g_splitOn && !g_active) toggle();
    const std::string other = g_splitPreset.empty() ? std::string("the game's image") : g_splitPreset;
    setStatus(g_splitOn ? "Compare split: " + other + " on one side (Ctrl+Shift+wheel moves the line)" : std::string("Compare split off"), 3);
}

bool splitShown() {
    // Not in hi-res photos: the line would be in every tile.
    return g_active && g_splitOn && g_job == kNone;
}

float splitForLook() {
    if (!splitShown()) return 0.0f;
    const float p = g_splitPosition < 0.01f ? 0.01f : (g_splitPosition > 0.99f ? 0.99f : g_splitPosition);
    return g_splitSwapped ? -p : p;
}

void toggle() {
    const bool on = !g_active;
    g_active = on;
    TMVS_LOG("photo: mode %s", on ? "on" : "off");
    if (on) {
        setStatus(hint(), 6);
    } else {
        // A hi-res photo under way is cancelled by the render thread (cameraWant).
        g_requested = false;
        g_focusU = g_focusV = -1.0f;
        g_zoom = 1.0f;
        setStatus("", 0);
    }
}

bool hideInterface() {
    return g_active && Config::get().photoHideHud;
}

float fNumber(float dof) {
    return 1.4f * powf(16.0f / 1.4f, 1.0f - (dof < 0.0f ? 0.0f : (dof > 1.0f ? 1.0f : dof)));
}

bool handleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!g_active) return false;
    if (msg == WM_KEYDOWN && wparam == VK_ESCAPE) {
        toggle();
        return true;
    }
    // While a hi-res photo renders the view must stay as it is.
    if (g_job != kNone && (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP || msg == WM_MOUSEWHEEL)) return true;
    if (msg == WM_MBUTTONDOWN) {
        if (wparam & MK_SHIFT) {
            g_zoom = 1.0f;
            setStatus("Zoom off", 2);
            return true;
        }
        if (wparam & MK_CONTROL) {
            g_focusU = g_focusV = -1.0f;
            setStatus("Focus off", 2);
            return true;
        }
        RECT client{};
        GetClientRect(hwnd, &client);
        if (client.right <= 0 || client.bottom <= 0) return false;
        g_focusU = static_cast<float>(static_cast<short>(LOWORD(lparam))) / client.right;
        g_focusV = static_cast<float>(static_cast<short>(HIWORD(lparam))) / client.bottom;
        Config& config = Config::get();
        if (config.photoDof <= 0.0f) {
            config.photoDof = 0.5f;
            config.markDirty();
        }
        setStatus("Focus set", 2);
        return true;
    }
    if (msg == WM_MBUTTONUP) return true;
    if (msg == WM_MOUSEWHEEL && g_splitOn && (GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL) && (GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT)) {
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        const float v = g_splitPosition + steps * 0.03f;
        g_splitPosition = v < 0.01f ? 0.01f : (v > 0.99f ? 0.99f : v);
        return true;
    }
    if (msg == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL)) {
        Config& config = Config::get();
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        const float v = config.photoDof + steps * 0.05f;
        config.photoDof = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        config.markDirty();
        char text[64];
        snprintf(text, sizeof(text), "Aperture f/%.1f", fNumber(config.photoDof));
        setStatus(text, 2);
        return true;
    }
    // Zoom: 10 % per wheel step, up to 8x (TMNF's), through the game's camera.
    if (msg == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT)) {
        if (!options::current().gameCamera) {
            setStatus("Zoom needs Game camera (Settings page, ManiaPlanet)", 3);
            return true;
        }
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        const float z = g_zoom * powf(1.1f, steps);
        g_zoom = z < 1.0f ? 1.0f : (z > 8.0f ? 8.0f : z);
        char text[64];
        snprintf(text, sizeof(text), "Zoom %.1fx", static_cast<float>(g_zoom));
        setStatus(text, 2);
        return true;
    }
    return false;
}

void request(bool hiRes) {
    if (!g_active || g_job != kNone) return;
    if (!hiRes) {
        g_requested = true;
        return;
    }
    if (!options::current().gameCamera) {
        setStatus("Hi-res photos need Game camera (Settings page, ManiaPlanet)", 4);
        return;
    }
    const int n = Config::get().photoTiles;
    g_tiles = n < 2 ? 2 : (n > 4 ? 4 : n);
    g_tile = 0;
    g_settle = 0;
    g_waited = 0;
    g_width = 0; // known with the first frame
    g_job = kTiles;
    TMVS_LOG("photo: hi-res, %dx%d tiles", g_tiles, g_tiles);
}

void adjust(tmshaders::Settings& s) {
    if (!g_active) return;
    // Stills: the most of every effect, and no smear of a moving camera. Depth of field only
    // on a point clicked (as in TMNF).
    s.quality = 2;
    s.motionBlur = 0.0f;
    const Config& config = Config::get();
    if (config.photoDof > 0.0f) {
        s.depthOfField = config.photoDof;
        s.focusDistance = 0.0f;
    } else {
        s.depthOfField = 0.0f;
    }
    if (g_job == kTiles) {
        // Effects of the whole frame would repeat in every tile (TMNF's).
        s.vignette = 0.0f;
        s.filmGrain = 0.0f;
        s.chromaticAberration = 0.0f;
        s.lensFlare = 0.0f;
        s.godRays = 0.0f;
        s.taaJitter = false;
        // The blur is measured in pixels: the photo has more of them.
        s.bokehSize *= static_cast<float>(g_tiles) * (g_innerW && g_width ? static_cast<float>(g_innerW) / g_width : 0.84f);
    }
}

void vanillaLook(tmshaders::Settings& s) {
    s.aoStrength = 0.0f;
    s.shadowStrength = 0.0f;
    s.globalIllumination = 0.0f;
    s.neonLight = 0.0f;
    s.sunLight = 0.0f;
    s.ambientTint = 0.0f;
    s.fogDensity = 0.0f;
    s.skyMode = 0;
    s.skyEnhance = 0.0f;
    s.volumetricClouds = 0.0f;
    s.godRays = 0.0f;
    s.volumetricLight = 0.0f;
    s.bloom = 0.0f;
    s.lensFlare = 0.0f;
    s.wetness = 0.0f;
    s.rain = 0.0f;
    s.snow = 0.0f;
    s.puddles = 0.0f;
    s.reflections = 0.0f;
    s.lightning = 0.0f;
    s.neonTrail = 0.0f;
    s.exposure = 0.0f;
    s.contrast = 1.0f;
    s.saturation = 1.0f;
    s.vibrance = 0.0f;
    s.temperature = 0.0f;
    s.tint = 0.0f;
    s.shadowTint = 0.0f;
    s.lift = 0.0f;
    s.gamma = 1.0f;
    s.gain = 1.0f;
    s.vignette = 0.0f;
    s.filmGrain = 0.0f;
    s.chromaticAberration = 0.0f;
    s.toneCurve = 0; // the engine's curve
}

void focusPoint(float out[2]) {
    out[0] = g_focusU;
    out[1] = g_focusV;
}

void afterTile(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Resource* frame); // below

void afterRender(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Resource* frame) {
    if (g_job == kTiles && g_active) {
        afterTile(device, ctx, frame);
        return;
    }
    if (!g_requested.exchange(false) || !g_active) return;
    std::vector<unsigned char> rgb;
    UINT w = 0, h = 0;
    bool ok = frameSize(frame, w, h);
    if (ok) {
        rgb.resize(static_cast<size_t>(w) * h * 3);
        const RECT all{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
        ok = readRect(device, ctx, frame, all, rgb.data(), static_cast<size_t>(w) * 3);
    }
    if (ok) save(std::move(rgb), static_cast<int>(w), static_cast<int>(h));
    else setStatus("Could not read the frame", 4);
}

void afterTile(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Resource* frame) {
    UINT w = 0, h = 0;
    if (!frameSize(frame, w, h)) return;
    if (g_width == 0) {
        g_width = w;
        g_height = h;
        g_marginX = static_cast<UINT>(w * 0.08f);
        g_marginY = static_cast<UINT>(h * 0.08f);
        g_innerW = w - 2 * g_marginX;
        g_innerH = h - 2 * g_marginY;
        try {
            g_rgb.assign(static_cast<size_t>(g_innerW) * g_tiles * g_innerH * g_tiles * 3, 0);
        } catch (...) {
            cancel("not enough memory");
            return;
        }
        g_settle = 0;
        g_waited = 0;
        return; // the tile's projection starts with the next frame
    }
    if (w != g_width || h != g_height) {
        cancel("the window changed size");
        return;
    }
    // The engine takes the camera's new FovRect a frame or two later: only frames with this tile's
    // projection count (measured from its constants, gamecamera.cpp).
    if (!gamecamera::matches(g_tileWant)) {
        g_settle = 0;
        if (++g_waited > kWaitFrames) cancel("the game's camera didn't take the tile");
        return;
    }
    if (++g_settle < kSettleFrames) return;
    const int n = g_tiles;
    const int i = g_tile % n, j = g_tile / n;
    const size_t pitch = static_cast<size_t>(g_innerW) * n * 3;
    unsigned char* dst = g_rgb.data() + static_cast<size_t>(j) * g_innerH * pitch + static_cast<size_t>(i) * g_innerW * 3;
    const RECT inner{static_cast<LONG>(g_marginX), static_cast<LONG>(g_marginY), static_cast<LONG>(g_marginX + g_innerW),
                     static_cast<LONG>(g_marginY + g_innerH)};
    if (!readRect(device, ctx, frame, inner, dst, pitch)) {
        cancel("could not read the frame");
        return;
    }
    g_settle = 0;
    g_waited = 0;
    char text[64];
    snprintf(text, sizeof(text), "Hi-res photo: tile %d of %d (keep still)", g_tile + 1, n * n);
    setStatus(text, 0);
    if (++g_tile < n * n) return;
    TMVS_LOG("photo: hi-res photo of %d tiles put together", n * n);
    // Still a hi-res job while saving: no compare labels (the split isn't in hi-res photos).
    save(std::move(g_rgb), static_cast<int>(g_innerW) * n, static_cast<int>(g_innerH) * n);
    g_rgb.clear();
    g_job = kNone;
}

void notify(const char* text, DWORD seconds) {
    setStatus(text, seconds);
}

const char* status() {
    std::lock_guard<std::mutex> lock(g_statusLock);
    if (g_statusUntil && GetTickCount64() > g_statusUntil) {
        g_status.clear();
        g_statusUntil = 0;
    }
    std::string text;
    if (g_active) {
        // The camera's settings stay on screen like on a camera's display (drawn at Present:
        // never in the photos), messages below them.
        const float dof = Config::get().photoDof;
        char aperture[32] = "Depth of field off";
        if (dof > 0.0f) snprintf(aperture, sizeof(aperture), "f/%.1f", fNumber(dof));
        char zoomText[32] = "";
        if (g_zoom > 1.001f && options::current().gameCamera) snprintf(zoomText, sizeof(zoomText), "   Zoom %.1fx", static_cast<float>(g_zoom));
        char line[128];
        snprintf(line, sizeof(line), "Photo mode   %s%s   Focus %s", aperture, zoomText, g_focusU >= 0.0f ? "point" : "auto");
        text = line;
        if (g_splitOn && !g_splitPreset.empty()) text += "   Compare with " + g_splitPreset;
        if (!g_status.empty()) text += "\n" + g_status;
    } else {
        text = g_status;
    }
    strncpy_s(g_statusCopy, text.c_str(), _TRUNCATE);
    return g_statusCopy;
}

} // namespace photo
} // namespace tmvs_mp
