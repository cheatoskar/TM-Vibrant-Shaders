#include "photo11.h"
#include "config.h"
#include "hooks.h"
#include "image.h"
#include "log.h"
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
};

DWORD WINAPI saveThread(LPVOID param) {
    SaveJob* job = static_cast<SaveJob*>(param);
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
    wchar_t name[64];
    for (int n = 0;; n++) {
        swprintf(name, 64, L"\\photo_%03d.png", n);
        if (GetFileAttributesW((tmshaders::log::dataDir() + name).c_str()) == INVALID_FILE_ATTRIBUTES) break;
    }
    job->path = tmshaders::log::dataDir() + name;
    setStatus("Saving the photo ...", 0);
    // A PNG of 2560x1440 takes a while: not on the game's render thread.
    HANDLE thread = CreateThread(nullptr, 0, saveThread, job, 0, nullptr);
    if (thread) CloseHandle(thread);
    else saveThread(job);
}

// The frame as RGB rows (8-bit formats: the finished image is display values already).
bool readFrame(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Resource* frame, std::vector<unsigned char>& rgb, UINT& w, UINT& h) {
    ID3D11Texture2D* tex = nullptr;
    if (!frame || FAILED(frame->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) return false;
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                      d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8X8_UNORM ||
                      d.Format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8X8_TYPELESS;
    const bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                      d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    bool ok = false;
    if ((bgra || rgba) && d.SampleDesc.Count == 1) {
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
                w = d.Width;
                h = d.Height;
                rgb.resize(static_cast<size_t>(w) * h * 3);
                const int r = bgra ? 2 : 0, b = bgra ? 0 : 2;
                for (UINT y = 0; y < h; y++) {
                    const auto* src = static_cast<const unsigned char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
                    unsigned char* dst = &rgb[static_cast<size_t>(y) * w * 3];
                    for (UINT x = 0; x < w; x++) {
                        dst[x * 3 + 0] = src[x * 4 + r];
                        dst[x * 3 + 1] = src[x * 4 + 1];
                        dst[x * 3 + 2] = src[x * 4 + b];
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

} // namespace

bool active() {
    return g_active;
}

namespace {
bool g_splitOn = false;
float g_splitPosition = 0.5f;
bool g_splitSwapped = false;
} // namespace

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
    setStatus(g_splitOn ? "Compare split: the game's image on one side (Ctrl+Shift+wheel moves the line)" : "Compare split off", 3);
}

bool splitShown() {
    return g_active && g_splitOn;
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
        setStatus("Photo mode   F12 photo   middle click: focus   Ctrl+middle click: no focus   Ctrl+wheel: aperture   F6/Esc: off", 6);
    } else {
        g_requested = false;
        g_focusU = g_focusV = -1.0f;
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
    if (msg == WM_MBUTTONDOWN) {
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
    return false;
}

void request() {
    if (g_active) g_requested = true;
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

void afterRender(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Resource* frame) {
    if (!g_requested.exchange(false) || !g_active) return;
    std::vector<unsigned char> rgb;
    UINT w = 0, h = 0;
    if (readFrame(device, ctx, frame, rgb, w, h)) save(std::move(rgb), static_cast<int>(w), static_cast<int>(h));
    else setStatus("Could not read the frame", 4);
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
        char line[96];
        snprintf(line, sizeof(line), "Photo mode   %s   Focus %s", aperture, g_focusU >= 0.0f ? "point" : "auto");
        text = line;
        if (!g_status.empty()) text += "\n" + g_status;
    } else {
        text = g_status;
    }
    strncpy_s(g_statusCopy, text.c_str(), _TRUNCATE);
    return g_statusCopy;
}

} // namespace photo
} // namespace tmvs_mp
