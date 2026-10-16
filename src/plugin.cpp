#include "plugin.h"
#include "audio.h"
#include "capture.h"
#include "casters.h"
#include "config.h"
#include "depth.h"
#include "engine.h"
#include "framecap.h"
#include "gameshaders.h"
#include "gfx.h"
#include "hook.h"
#include "log.h"
#include "lut.h"
#include "overlay.h"
#include "pipeline.h"
#include "autoquality.h"
#include "photo.h"
#include "thumbnails.h"
#include "tracer.h"
#include "tm_shaders_version.h"
#include <d3d9.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace tmshaders {
namespace plugin {
namespace {

HMODULE g_module = nullptr;
std::wstring g_moduleDir;
IDirect3DDevice9* g_device = nullptr;
unsigned g_frame = 0;
int g_cameraIndex = 0;
LARGE_INTEGER g_startTime{};
LARGE_INTEGER g_frequency{};

// Development automation, read from <Documents>\TrackMania\TMVS\dev.ini [dev].
// Times are seconds since the plugin started.
struct DevOptions {
    float traceAt = 0.0f;        // write a frame trace (0 = off)
    float captureAt = 0.0f;      // start writing .tmcap captures (0 = off)
    float captureInterval = 2.0f;
    int captureCount = 1;
    float exitAt = 0.0f;         // terminate the game (0 = off)
    float hardExitAt = 0.0f;     // terminate even if no gameplay camera appeared (absolute time)
    // "Keys=4:13,7:a13,9:min,12:restore": post VK 13 at 4 s, Alt+Enter at 7 s, minimize / restore.
    // Photo mode: "photomode" (like F6), "photo" / "photohires" (like F12 / Shift+F12 in it).
    std::vector<std::pair<float, std::wstring>> keys;
    std::vector<std::pair<float, std::wstring>> sceneKeys; // "SceneKeys=": the same, timed from the first gameplay frame
    std::wstring shaderDir;      // load shaders from here (hot reload with F9)
} g_dev;
float g_nextCaptureAt = 0.0f;
float g_sceneStart = -1.0f; // first frame with a gameplay camera (near plane < 1 m)
bool g_screenshotRequested = false;
size_t g_nextKey = 0;
size_t g_nextSceneKey = 0;

// Main-camera state for the frame being rendered.
struct SceneState {
    bool inMainCamera = false;
    bool haveView = false;
    bool haveProjection = false;
    float view[16] = {};
    float projection[16] = {};
    float lightDirection[3] = {};
    float lightColor[3] = {1.0f, 1.0f, 1.0f};
    bool haveLight = false;
    // Replay camera blends render two main cameras per frame (one into a texture that is
    // blended over the other). Both get the shaders, or the blend shows a "ghost" of the
    // unprocessed image.
    int processedCount = 0;                  // main cameras processed this frame
    IDirect3DSurface9* processedTarget = nullptr; // identity only, not referenced
    int lastFrameCount = 0;
    bool loggedMultiCamera = false;
} g_scene;

bool g_captureRequested = false;
// F12 with the material masks on: the next main camera tags every depth-writing draw with
// its material's own id (1..255) instead of its class, and that frame is captured with the
// ids (capture_NNN.mat): which material drew each pixel, by name.
bool g_idFrame = false;
struct IdMaterial {
    const void* shader;
    int draws;
    bool depthWrite; // tagged in the stencil (else: drawn without depth writes, no pixels)
    bool additive;   // one-one blending (glows, headlight pools)
};
std::vector<IdMaterial> g_idMaterials; // id = index + 1
int g_capturesWritten = 0;
Pipeline g_pipeline;
AutoQuality g_autoQuality;
bool g_shadedThisFrame = false;
bool g_gameplayThisFrame = false; // a gameplay camera (not a menu background) was shaded
float g_jitterUV[2] = {};         // TAA jitter of this frame (uv shift of the image)
UINT g_frameWidth = 0, g_frameHeight = 0;
bool g_pipelineFailed = false; // don't retry a failed compile every frame; F9 retries
gfx::Target g_sceneCopy;
// A camera that draws into part of the screen (the MediaTracker editor's 3D view between its
// panels): its depth and the shaded image, the size of that part.
gfx::Target g_partDepth;
gfx::Target g_partOut;
// The game's colour clears of this frame that reached the top and the bottom row of their
// target (only compared, never used): rows it cleared itself are no leftovers (see fillBars).
struct ClearedRows {
    IDirect3DSurface9* target = nullptr;
    unsigned frame = ~0u;
};
ClearedRows g_clearedTop, g_clearedBottom;
// GameHdr: the main camera draws into a float target instead of the game's 8-bit one, so the
// light the game computes above 1.0 isn't cut (captures for working out the tone curve). At
// the camera's end it is resolved into the game's target, cut to 0..1 (the 8-bit copy the
// guesses, the HUD and the photo split use); the lighting pass takes the float image itself.
gfx::Target g_hdrScene;
IDirect3DSurface9* g_hdrGame = nullptr;     // the game's target while the float one stands in (AddRef'd)
IDirect3DSurface9* g_hdrLastGame = nullptr; // the same after the camera (not held): handed back if the game binds the float one
bool g_hdrValid = false;                    // g_hdrScene holds this main camera's image
int g_hdrSupported = -1;                    // float render target with blending: -1 not checked yet
// The light the game adds on top: its one-one blended draws (TAdd glows over whole blocks, the
// lamps' halos, headlight pools) go into a float target of their own during the camera, and it
// is added back into g_hdrScene at the end (addition doesn't care about order: the image stays
// the same). What remains is pixel-exact: where a glow's quad adds nothing, it is 0 (the stencil
// bit marked the whole quad). The lighting pass can take light sources from it instead of
// guessing them in the image.
gfx::Target g_hdrAdded;
bool g_hdrAddedActive = false; // cleared at this camera's start, takes the additive draws
bool g_hdrAddedBound = false;  // the render target during the current (additive) draw
bool g_hdrAddedValid = false;  // holds the last main camera's added light
bool g_castersOn = false;      // CasterShadows this frame: the main camera's meshes are recorded (casters.cpp)

float seconds() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<float>(static_cast<double>(now.QuadPart - g_startTime.QuadPart) / static_cast<double>(g_frequency.QuadPart));
}

// The game's video export (engine::videoExport), with VideoExportBest on; looked at once per
// frame. Its clock runs at a fixed step: a frame of the video may take seconds to render, and
// on the real clock the exposure adapted, the focus pulled, rain fell and the clouds drifted
// that much faster in the video (and unevenly: as fast as each frame happened to render).
// While it steps, the effects run on the video's clock: one step per rendered frame. Without
// an export it is the real clock; after one it goes on from where the video ended (no jump).
bool g_exporting = false;
engine::VideoExport g_export;
double g_videoClock = -1.0;  // >= 0: the export's clock (s), while it runs at a fixed step
float g_clockOffset = 0.0f;  // added to the real clock after an export (0 until the first one)
struct ExportStats {
    unsigned frames = 0;      // rendered frames of the export
    unsigned stepped = 0;     // of them on the video's clock
    double clockStart = 0.0;  // the effects' clock at its start
    double gameStart = 0.0;   // the game's clock at its start
    int samplesSeen = 0;      // most main cameras shaded in one frame
} g_exportStats;

// The time the effects see (s).
float effectTime() {
    return g_videoClock >= 0.0 ? static_cast<float>(g_videoClock) : seconds() + g_clockOffset;
}

// CPU time the mod's own rendering takes (D3D calls of the pipeline, not the GPU work):
// logged now and then for the first frames, to see whether the CPU side is worth working on.
struct CpuStats {
    double total = 0.0, worst = 0.0;
    unsigned frames = 0;
} g_cpuStats;

// Hangs: a frame the mod held up for long (logged with the time, so a sound glitch or stutter the
// player noticed can be matched to it), and long gaps between two shaded frames in a race (the
// game or the driver stood still: loading is no race).
void logHangs(double ms, double captureMs, bool racing) {
    static int s_logged = 0;
    static float s_last = -1.0f;
    static int s_maps = -1;
    const float now = seconds();
    // A map loaded or closed in between: that wait is the loading screen.
    const int maps = engine::mapLoads() + engine::mapCloses();
    if (maps != s_maps) s_last = -1.0f;
    s_maps = maps;
    if (s_logged < 60) {
        if (ms > 100.0) {
            s_logged++;
            TMVS_LOG("cpu: one frame of the mod took %.0f ms (%.0f ms of it writing the F12 capture)", ms, captureMs);
        }
        if (racing && s_last >= 0.0f && now - s_last > 0.25f) {
            s_logged++;
            TMVS_LOG("frame: %.0f ms since the last shaded frame in a race", (now - s_last) * 1000.0f);
        }
    }
    s_last = racing ? now : -1.0f;
}

void countPipelineCpu(double ms) {
    if (g_cpuStats.frames >= 20000) return;
    g_cpuStats.total += ms;
    g_cpuStats.worst = ms > g_cpuStats.worst ? ms : g_cpuStats.worst;
    if (++g_cpuStats.frames % 2000 == 0) {
        TMVS_LOG("cpu: the mod's rendering takes %.2f ms per shaded frame on the CPU (worst %.1f ms, %u frames)", g_cpuStats.total / g_cpuStats.frames,
                 g_cpuStats.worst, g_cpuStats.frames);
        g_cpuStats.worst = 0.0;
    }
}

float iniFloat(const wchar_t* ini, const wchar_t* key, float fallback) {
    wchar_t value[64] = {};
    if (!GetPrivateProfileStringW(L"dev", key, L"", value, 64, ini)) return fallback;
    return static_cast<float>(_wtof(value));
}

void loadDevOptions() {
    std::wstring path = log::dataDir() + L"\\dev.ini";
    const wchar_t* ini = path.c_str();
    g_dev.traceAt = iniFloat(ini, L"TraceAt", 0.0f);
    g_dev.captureAt = iniFloat(ini, L"CaptureAt", 0.0f);
    g_dev.captureInterval = iniFloat(ini, L"CaptureInterval", 2.0f);
    g_dev.captureCount = GetPrivateProfileIntW(L"dev", L"CaptureCount", 1, ini);
    g_dev.exitAt = iniFloat(ini, L"ExitAt", 0.0f);
    g_dev.hardExitAt = iniFloat(ini, L"HardExitAt", 0.0f);
    wchar_t buffer[MAX_PATH] = {};
    GetPrivateProfileStringW(L"dev", L"ShaderDir", L"", buffer, MAX_PATH, ini);
    g_dev.shaderDir = buffer;
    const auto readKeys = [&](const wchar_t* name, std::vector<std::pair<float, std::wstring>>& keys) {
        GetPrivateProfileStringW(L"dev", name, L"", buffer, MAX_PATH, ini);
        for (wchar_t* token = wcstok(buffer, L","); token; token = wcstok(nullptr, L",")) {
            float at = 0.0f;
            wchar_t action[32] = {};
            if (swscanf(token, L"%f:%31ls", &at, action) == 2) keys.emplace_back(at, action);
        }
    };
    readKeys(L"Keys", g_dev.keys);
    readKeys(L"SceneKeys", g_dev.sceneKeys);
    g_nextCaptureAt = g_dev.captureAt;
    engine::forceShadows(GetPrivateProfileIntW(L"dev", L"ForceShadows", -1, ini));
    engine::shaderCensus(GetPrivateProfileIntW(L"dev", L"LogShaders", 0, ini) != 0);
}

void runDevAutomation(IDirect3DDevice9* device) {
    // Trace/capture times count from the first gameplay frame, so loading and menus don't matter.
    const float now = seconds();
    const float sceneNow = g_sceneStart >= 0.0f ? now - g_sceneStart : -1.0f;
    if (g_dev.traceAt > 0.0f && sceneNow >= g_dev.traceAt) {
        g_dev.traceAt = 0.0f;
        tracer::arm();
    }
    if (g_dev.captureAt > 0.0f && sceneNow >= g_nextCaptureAt && g_capturesWritten < g_dev.captureCount) {
        g_nextCaptureAt = sceneNow + g_dev.captureInterval;
        g_captureRequested = true;
    }
    const std::wstring* action = nullptr;
    if (g_nextKey < g_dev.keys.size() && now >= g_dev.keys[g_nextKey].first) {
        action = &g_dev.keys[g_nextKey++].second;
    } else if (g_nextSceneKey < g_dev.sceneKeys.size() && sceneNow >= g_dev.sceneKeys[g_nextSceneKey].first) {
        action = &g_dev.sceneKeys[g_nextSceneKey++].second;
    }
    if (action) {
        D3DDEVICE_CREATION_PARAMETERS cp{};
        device->GetCreationParameters(&cp);
        HWND window = cp.hFocusWindow;
        if (*action == L"photomode") {
            photo::toggle();
        } else if (*action == L"photo" || *action == L"photohires") {
            photo::request(*action == L"photohires");
        } else if (*action == L"min") {
            ShowWindow(window, SW_MINIMIZE);
        } else if (*action == L"restore") {
            ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
        } else if ((*action)[0] == L'a') {
            const WPARAM vk = static_cast<WPARAM>(_wtoi(action->c_str() + 1));
            PostMessageW(window, WM_SYSKEYDOWN, vk, 1 | (1 << 29));
            PostMessageW(window, WM_SYSKEYUP, vk, 0xE0000001);
        } else {
            const WPARAM vk = static_cast<WPARAM>(_wtoi(action->c_str()));
            PostMessageW(window, WM_KEYDOWN, vk, 1);
            PostMessageW(window, WM_KEYUP, vk, 0xC0000001);
        }
        TMVS_LOG("dev: action %ls", action->c_str());
    }
    if ((g_dev.exitAt > 0.0f && sceneNow >= g_dev.exitAt) || (g_dev.hardExitAt > 0.0f && now >= g_dev.hardExitAt)) {
        TMVS_LOG("dev: ExitAt reached, terminating");
        TerminateProcess(GetCurrentProcess(), 0);
    }
}

void traceMatrix(const char* label, const float* m) {
    tracer::event("    %s [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]", label, m[0], m[1],
                  m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
}

// The main 3D camera renders at full back-buffer size with the shadowed depth buffer bound.
// In races that target is the back buffer; with the engine's own camera effects (menus,
// replays) it is an offscreen texture that is composited afterwards.
bool isMainTarget(IDirect3DDevice9* device, IDirect3DSurface9* rt) {
    if (!rt) return false;
    IDirect3DSurface9* ds = nullptr;
    device->GetDepthStencilSurface(&ds);
    IDirect3DTexture9* readable = depth::textureFor(ds);
    D3DSURFACE_DESC rtDesc{}, depthDesc{};
    if (ds) {
        ds->GetDesc(&depthDesc);
        ds->Release();
    }
    if (!readable) return false;
    rt->GetDesc(&rtDesc);
    return rtDesc.Width == depthDesc.Width && rtDesc.Height == depthDesc.Height && rtDesc.Width >= 320;
}

// World-space direction towards the sun.
bool sunDirection(float out[3]) {
    const Settings& s = Config::get().settings;
    if (s.sunElevationOverride >= 0.0f) {
        const float el = s.sunElevationOverride * 3.14159265f / 180.0f;
        const float az = s.sunAzimuthOverride * 3.14159265f / 180.0f;
        out[0] = cosf(el) * cosf(az);
        out[1] = sinf(el);
        out[2] = cosf(el) * sinf(az);
        return true;
    }
    if (!g_scene.haveLight) return false;
    // D3D light direction is the direction the light travels.
    out[0] = -g_scene.lightDirection[0];
    out[1] = -g_scene.lightDirection[1];
    out[2] = -g_scene.lightDirection[2];
    return true;
}

bool materialsWanted(); // the game's material masks this frame (below, with the draw tagging)

// The colour look-up table picked in the menu: loaded when the choice changes, a broken
// file is reported once and left off. Returns how much of it to use (0 = none).
float updateLut() {
    static std::string loaded;
    const Config& config = Config::get();
    if (config.lutFile != loaded) {
        loaded = config.lutFile;
        lut::Table table;
        std::string error;
        if (!loaded.empty() && !lut::loadCube(lut::path(loaded), table, error)) {
            TMVS_LOG("lut: %s: %s", loaded.c_str(), error.c_str());
        } else if (!loaded.empty()) {
            TMVS_LOG("lut: %s (%d^3)", loaded.c_str(), table.size);
        }
        g_pipeline.setLut(table.texels, table.size);
    }
    return loaded.empty() ? 0.0f : config.lutAmount;
}

float halfToFloat(uint16_t h) {
    const unsigned sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
    float value;
    if (exponent == 0) value = ldexpf(static_cast<float>(mantissa), -24);
    else if (exponent == 31) value = 65504.0f; // inf / nan: as bright as it gets
    else value = ldexpf(static_cast<float>(mantissa | 1024), static_cast<int>(exponent) - 25);
    return sign ? -value : value;
}

// How much of the game's image lies above 1.0, i.e. what its own 8-bit target cut away.
// `keep` receives the half floats (RGBA, rows top-down) when given.
void logHdrStats(IDirect3DDevice9* device, const char* when, std::vector<uint16_t>* keep) {
    std::vector<uint16_t> half;
    if (!g_pipeline.readHalfColor(device, g_hdrScene.surface, half)) {
        TMVS_LOG("hdr: readback failed");
        return;
    }
    const size_t count = half.size() / 4;
    size_t above1 = 0, above125 = 0, above2 = 0, negative = 0;
    float brightest = 0.0f;
    std::vector<size_t> bins(129, 0); // brightest channel in steps of 1/16 up to 8
    for (size_t i = 0; i < count; i++) {
        const float r = halfToFloat(half[i * 4]), g = halfToFloat(half[i * 4 + 1]), b = halfToFloat(half[i * 4 + 2]);
        const float m = std::max(r, std::max(g, b));
        above1 += m > 1.0f + 1.0f / 255.0f;
        above125 += m > 1.25f;
        above2 += m > 2.0f;
        negative += std::min(r, std::min(g, b)) < 0.0f;
        brightest = std::max(brightest, m);
        bins[static_cast<size_t>(std::min(std::max(m, 0.0f), 8.0f) * 16.0f)]++;
    }
    size_t seen = 0, p999 = 0;
    for (size_t i = 0; i < bins.size(); i++) {
        seen += bins[i];
        if (seen >= count - count / 1000) {
            p999 = i;
            break;
        }
    }
    const double n = count ? static_cast<double>(count) / 100.0 : 1.0;
    TMVS_LOG("hdr (%s): %.2f%% of the image above 1.0, %.2f%% above 1.25, %.2f%% above 2.0; brightest %.2f, 99.9%% at most %.2f; %.2f%% negative",
             when, above1 / n, above125 / n, above2 / n, brightest, (p999 + 1) / 16.0, negative / n);
    if (keep) keep->swap(half);
}

// Float targets with blending: the game blends glows and particles into the scene.
bool hdrSupported(IDirect3DDevice9* device) {
    if (g_hdrSupported < 0) {
        g_hdrSupported = 0;
        IDirect3D9* d3d = nullptr;
        D3DDEVICE_CREATION_PARAMETERS params{};
        D3DDISPLAYMODE mode{};
        if (SUCCEEDED(device->GetDirect3D(&d3d)) && d3d) {
            if (SUCCEEDED(device->GetCreationParameters(&params)) && SUCCEEDED(device->GetDisplayMode(0, &mode))) {
                g_hdrSupported = SUCCEEDED(d3d->CheckDeviceFormat(params.AdapterOrdinal, params.DeviceType, mode.Format,
                                                                  D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING,
                                                                  D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F))
                                     ? 1
                                     : 0;
            }
            d3d->Release();
        }
        TMVS_LOG("hdr: float render target with blending %s", g_hdrSupported ? "supported" : "not supported (GameHdr stays off)");
    }
    return g_hdrSupported == 1;
}

// At the main camera's start: the float target stands in for the game's.
void beginHdr(IDirect3DDevice9* device, IDirect3DSurface9* rt) {
    g_hdrValid = false;
    g_hdrAddedValid = false;
    g_hdrAddedActive = false;
    g_hdrAddedBound = false;
    const Settings& s = Config::get().settings;
    if (!s.enabled || !s.gameHdr || g_hdrGame || !rt || !g_pipeline.ready() || !hdrSupported(device)) return;
    D3DSURFACE_DESC desc{};
    rt->GetDesc(&desc);
    if (!g_hdrScene.create(device, desc.Width, desc.Height, D3DFMT_A16B16G16R16F)) return;
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        TMVS_LOG("hdr: the main camera draws into a float target (%ux%u)", desc.Width, desc.Height);
    }
    hook::InternalScope internal;
    // SetRenderTarget resets the viewport and the scissor rect: the game's stay.
    D3DVIEWPORT9 viewport{};
    RECT scissor{};
    device->GetViewport(&viewport);
    device->GetScissorRect(&scissor);
    // The added light starts at 0 (the whole target: no scissor test for the Clear).
    if (g_hdrAdded.create(device, desc.Width, desc.Height, D3DFMT_A16B16G16R16F) && SUCCEEDED(device->SetRenderTarget(0, g_hdrAdded.surface))) {
        DWORD scissorTest = FALSE;
        device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorTest);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        g_hdrAddedActive = SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0));
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, scissorTest);
    }
    if (FAILED(device->SetRenderTarget(0, g_hdrScene.surface))) {
        device->SetRenderTarget(0, rt);
        device->SetViewport(&viewport);
        device->SetScissorRect(&scissor);
        g_hdrAddedActive = false;
        return;
    }
    device->SetViewport(&viewport);
    device->SetScissorRect(&scissor);
    rt->AddRef();
    g_hdrGame = rt;
    g_hdrLastGame = rt;
}

// At the main camera's end, before the pipeline: the image goes into the game's target
// (cut to 0..1, as the game would have drawn it), which is bound again.
void endHdr(IDirect3DDevice9* device) {
    if (!g_hdrGame) return;
    hook::InternalScope internal;
    D3DVIEWPORT9 viewport{};
    RECT scissor{};
    device->GetViewport(&viewport);
    device->GetScissorRect(&scissor);
    const RECT area = {static_cast<LONG>(viewport.X), static_cast<LONG>(viewport.Y), static_cast<LONG>(viewport.X + viewport.Width),
                       static_cast<LONG>(viewport.Y + viewport.Height)};
    g_pipeline.beginStateSave(device);
    // The added light back into the scene (the image is the game's again), then the scene
    // into the game's target.
    g_hdrAddedValid = g_hdrAddedActive && g_pipeline.addScene(device, g_hdrAdded.texture, g_hdrScene.surface, area);
    g_hdrAddedActive = false;
    g_hdrValid = g_pipeline.resolveScene(device, g_hdrScene.texture, g_hdrGame, area);
    g_pipeline.endStateSave(device);
    device->SetRenderTarget(0, g_hdrGame);
    device->SetViewport(&viewport);
    device->SetScissorRect(&scissor);
    g_hdrGame->Release();
    g_hdrGame = nullptr;
    // Once per map, a few seconds into the driving: how much light the float target kept.
    static int s_map = -1;
    static DWORD s_since = 0;
    if (g_hdrValid && g_scene.projection[14] > -1.0f && engine::mapLoads() != s_map) {
        if (!s_since) {
            s_since = GetTickCount();
        } else if (GetTickCount() - s_since > 8000) {
            s_map = engine::mapLoads();
            s_since = 0;
            logHdrStats(device, "map", nullptr);
        }
    }
}

IDirect3DSurface9* onSetRenderTarget(DWORD index, IDirect3DSurface9* surface) {
    if (index != 0 || !surface) return surface;
    // The game binds its target again during the camera: the float one stays.
    if (g_hdrGame && surface == g_hdrGame) return g_hdrScene.surface;
    // It asked for the target during the camera (GetRenderTarget gave it the float one) and
    // binds that later, for the HUD: its own target.
    if (!g_hdrGame && surface == g_hdrScene.surface && g_hdrLastGame) return g_hdrLastGame;
    return surface;
}

// The float image next to the capture: "TMH1", width, height, then RGBA half floats, rows
// top-down (the previewer's --hdr).
void writeHalfFile(const std::wstring& path, UINT width, UINT height, const std::vector<uint16_t>& half) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    const uint32_t header[3] = {0x31484D54u, width, height}; // "TMH1"
    fwrite(header, sizeof(header), 1, f);
    fwrite(half.data(), sizeof(uint16_t), half.size(), f);
    fclose(f);
}

void writeHdrCapture(IDirect3DDevice9* device, const std::wstring& path) {
    std::vector<uint16_t> half;
    logHdrStats(device, "capture", &half);
    if (half.empty()) return;
    writeHalfFile(path, g_hdrScene.width, g_hdrScene.height, half);
}

// The light the game added on top, in the same format (capture_NNN.add16).
void writeAddedCapture(IDirect3DDevice9* device, const std::wstring& path) {
    std::vector<uint16_t> half;
    if (!g_pipeline.readHalfColor(device, g_hdrAdded.surface, half)) {
        TMVS_LOG("hdr: added light not read");
        return;
    }
    const size_t count = half.size() / 4;
    size_t lit = 0, strong = 0;
    float brightest = 0.0f;
    for (size_t i = 0; i < count; i++) {
        const float m = std::max(halfToFloat(half[i * 4]), std::max(halfToFloat(half[i * 4 + 1]), halfToFloat(half[i * 4 + 2])));
        lit += m > 1.0f / 255.0f;
        strong += m > 0.25f;
        brightest = std::max(brightest, m);
    }
    const double n = count ? static_cast<double>(count) / 100.0 : 1.0;
    TMVS_LOG("hdr (capture): added light on %.2f%% of the image, above 0.25 on %.2f%%, brightest %.2f", lit / n, strong / n, brightest);
    writeHalfFile(path, g_hdrAdded.width, g_hdrAdded.height, half);
}

// One-one blending with the colour added (glows, halos, headlight pools) and no depth writes.
bool addsLight(IDirect3DDevice9* device) {
    DWORD blend = FALSE, dest = D3DBLEND_ZERO, op = D3DBLENDOP_ADD, zwrite = TRUE;
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    if (!blend) return false;
    device->GetRenderState(D3DRS_DESTBLEND, &dest);
    device->GetRenderState(D3DRS_BLENDOP, &op);
    device->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
    return dest == D3DBLEND_ONE && op == D3DBLENDOP_ADD && !zwrite;
}

// GameHdr: an additive draw of the main camera goes into g_hdrAdded, for that draw only (it is
// bound again before the game's next call, so the game never finds it: a copy of the scene for
// the water's refraction takes the float scene). SetRenderTarget resets the viewport and the
// scissor rect: the game's stay.
D3DVIEWPORT9 g_addedViewport{};
RECT g_addedScissor{};

void onDrawAdded(IDirect3DDevice9* device) {
    if (!g_hdrAddedActive || !g_hdrGame || !g_scene.inMainCamera || !addsLight(device)) return;
    hook::InternalScope internal;
    IDirect3DSurface9* rt = nullptr;
    device->GetRenderTarget(0, &rt);
    const bool onScene = rt == g_hdrScene.surface;
    if (rt) rt->Release();
    if (!onScene) return;
    device->GetViewport(&g_addedViewport);
    device->GetScissorRect(&g_addedScissor);
    if (FAILED(device->SetRenderTarget(0, g_hdrAdded.surface))) return;
    device->SetViewport(&g_addedViewport);
    device->SetScissorRect(&g_addedScissor);
    g_hdrAddedBound = true;
}

void onDrawDone(IDirect3DDevice9* device) {
    if (!g_hdrAddedBound) return;
    hook::InternalScope internal;
    device->SetRenderTarget(0, g_hdrScene.surface);
    device->SetViewport(&g_addedViewport);
    device->SetScissorRect(&g_addedScissor);
    g_hdrAddedBound = false;
}

// The material ids next to the capture: "TMM1", width, height, count, then per id (1..count)
// class (engine::Material), flags (1 depth writes, 2 one-one blending), draws, the length
// and UTF-8 bytes of its shader file; then one id byte per pixel, rows top-down (0: none).
void writeMaterialCapture(IDirect3DDevice9* device, const std::wstring& path) {
    std::vector<uint8_t> ids;
    if (!g_pipeline.readMaterialIds(device, ids) || ids.size() != static_cast<size_t>(g_frameWidth) * g_frameHeight) {
        TMVS_LOG("capture: material ids not read");
        return;
    }
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    const uint32_t header[4] = {0x314D4D54u, g_frameWidth, g_frameHeight, static_cast<uint32_t>(g_idMaterials.size())}; // "TMM1"
    fwrite(header, sizeof(header), 1, f);
    int lights = 0;
    for (const IdMaterial& m : g_idMaterials) {
        wchar_t file[512] = {};
        char utf8[1024] = {};
        if (!engine::materialFile(m.shader, file, 512)) swprintf(file, 512, m.shader ? L"(unnamed %p)" : L"(none)", m.shader);
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, file, -1, utf8, sizeof(utf8), nullptr, nullptr);
        const uint8_t kind[2] = {static_cast<uint8_t>(engine::materialClass(m.shader)),
                                 static_cast<uint8_t>((m.depthWrite ? 1 : 0) | (m.additive ? 2 : 0))};
        const uint32_t draws = static_cast<uint32_t>(m.draws);
        const uint16_t length = static_cast<uint16_t>(bytes > 0 ? bytes - 1 : 0);
        fwrite(kind, sizeof(kind), 1, f);
        fwrite(&draws, sizeof(draws), 1, f);
        fwrite(&length, sizeof(length), 1, f);
        fwrite(utf8, 1, length, f);
        if (kind[0] == static_cast<uint8_t>(engine::Material::Light)) lights++;
    }
    fwrite(ids.data(), 1, ids.size(), f);
    fclose(f);
    TMVS_LOG("capture: material ids of %d materials (%d lights)", static_cast<int>(g_idMaterials.size()), lights);
}

void writeCapture(IDirect3DDevice9* device, const Pipeline::Inputs& inputs, bool materialIds) {
    FrameCapture cap;
    cap.width = inputs.width;
    cap.height = inputs.height;
    memcpy(cap.view, inputs.view, sizeof(cap.view));
    memcpy(cap.projection, inputs.projection, sizeof(cap.projection));
    memcpy(cap.sunDirection, inputs.sunDirection, sizeof(float) * 3);
    cap.sunDirection[3] = inputs.sunKnown ? 1.0f : 0.0f;
    memcpy(cap.sunColor, inputs.sunColor, sizeof(float) * 3);
    cap.sunColor[3] = inputs.sunColorKnown ? 1.0f : 0.0f;
    cap.time = inputs.time;
    if (!g_pipeline.readColor(device, g_sceneCopy.surface, cap.color) || !g_pipeline.readDepth(device, inputs.depth, cap.depth)) {
        TMVS_LOG("capture: readback failed");
        return;
    }
    wchar_t name[64];
    // The next free number: captures of earlier sessions are kept.
    for (;;) {
        swprintf(name, 64, L"\\capture_%03d.tmcap", g_capturesWritten);
        if (GetFileAttributesW((log::dataDir() + name).c_str()) == INVALID_FILE_ATTRIBUTES) break;
        g_capturesWritten++;
    }
    g_capturesWritten++;
    cap.save(log::dataDir() + name);
    TMVS_LOG("capture: wrote %ls", name + 1);
    // GameHdr: the image before the game's target cut it, next to it (capture_NNN.hdr16).
    if (g_hdrValid && g_hdrScene.width == inputs.width && g_hdrScene.height == inputs.height) {
        std::wstring hdr = log::dataDir() + name;
        hdr.replace(hdr.size() - 6, 6, L".hdr16");
        writeHdrCapture(device, hdr);
    }
    // ... and the light the game added on top (capture_NNN.add16).
    if (g_hdrAddedValid && g_hdrAdded.width == inputs.width && g_hdrAdded.height == inputs.height) {
        std::wstring added = log::dataDir() + name;
        added.replace(added.size() - 6, 6, L".add16");
        writeAddedCapture(device, added);
    }
    // Which material drew each pixel (capture_NNN.mat).
    if (materialIds) {
        std::wstring mat = log::dataDir() + name;
        mat.replace(mat.size() - 6, 6, L".mat");
        writeMaterialCapture(device, mat);
    }
    g_screenshotRequested = true;
}

// Switching presets (or a map bringing its own look) glides over about a second instead of
// jumping: every number and colour eases toward its new value. Switches (sky, on/off) change
// at once. Angles take the short way round.
Settings g_shown;
float g_shownTime = -1.0f;

void smoothLook(Settings& s, float now) {
    const float dt = now - g_shownTime;
    if (g_shownTime < 0.0f || dt > 0.5f) {
        g_shown = s; // first frame, or after a pause (loading): no glide
        g_shownTime = now;
        return;
    }
    if (dt > 0.0f) { // several cameras in one frame share one step
        g_shownTime = now;
        const float k = 1.0f - expf(-dt / 0.3f);
        char* shown = reinterpret_cast<char*>(&g_shown);
        const char* target = reinterpret_cast<const char*>(&s);
        for (const Field& f : fields()) {
            float* a = reinterpret_cast<float*>(shown + f.offset);
            const float* b = reinterpret_cast<const float*>(target + f.offset);
            if (f.kind == Field::Float || f.kind == Field::Color) {
                const bool angle = strstr(f.key, "Azimuth") || !strcmp(f.key, "SkyRotation");
                for (int i = 0; i < (f.kind == Field::Color ? 3 : 1); i++) {
                    float d = b[i] - a[i];
                    if (angle) d = fmodf(d + 540.0f, 360.0f) - 180.0f;
                    a[i] = fabsf(d) < 1e-4f * (fabsf(b[i]) + 1.0f) ? b[i] : a[i] + d * k;
                    if (angle) a[i] = fmodf(a[i] + 360.0f, 360.0f);
                }
            } else {
                memcpy(shown + f.offset, target + f.offset, f.kind == Field::Bool ? sizeof(bool) : sizeof(int));
            }
        }
    }
    const bool enabled = s.enabled;
    const int debugView = s.debugView;
    s = g_shown;
    s.enabled = enabled;
    s.debugView = debugView;
}

// The replay menu at the end of a race: the replay shrinks into a full-width band between the
// menu's half-transparent bars (2560x1364 at 0,38 down to 2560x1080 at 0,180 within half a
// second) and the game draws nothing above and below it any more. Under the bars stays what the
// shrinking band left there, a strip of each frame: in capture 075 the image jumps exactly at
// rows 38, 56, 72, 88, ... 180 and 1402, 1384, ... 1260, the band's edges frame by frame. A
// stepped, blocky copy, each strip from another frame, scaled differently and shaded on its own
// (the pipeline's targets were new every frame: no history). The shaded band, mirrored at its
// edges, fills those rows instead. Not where the game cleared them itself this frame (bars it
// wants black), not for parts that leave columns free (the MediaTracker editor's panels).
void fillMenuBars(IDirect3DDevice9* device, IDirect3DSurface9* target, const D3DSURFACE_DESC& desc, const RECT& area, bool wanted) {
    if (!wanted || area.left != 0 || area.right != static_cast<LONG>(desc.Width)) return;
    const bool top = area.top > 0 && !(g_clearedTop.target == target && g_clearedTop.frame == g_frame);
    const bool bottom = area.bottom < static_cast<LONG>(desc.Height) && !(g_clearedBottom.target == target && g_clearedBottom.frame == g_frame);
    static unsigned s_logged = 0; // each outcome once: 0 left alone (cleared by the game), 1 filled, 2 failed
    int state = 0;
    if (top || bottom) state = g_pipeline.fillBars(device, g_partOut.texture, target, area, top, bottom) ? 1 : 2;
    if (!(s_logged & (1u << state))) {
        s_logged |= 1u << state;
        if (state == 1) {
            TMVS_LOG("scene: rows outside the camera's band (%ld above, %ld below) hold earlier frames: filled with the shaded band, mirrored",
                     top ? area.top : 0L, bottom ? static_cast<LONG>(desc.Height) - area.bottom : 0L);
        } else if (state == 2) {
            TMVS_LOG("scene: filling the rows outside the camera's band failed: left as the game had them");
        } else {
            TMVS_LOG("scene: the game cleared the rows outside the camera's band itself: left alone");
        }
    }
}

// Runs the post pipeline over the main camera's image, before the HUD is drawn.
void processScene(IDirect3DDevice9* device) {
    Settings& settings = Config::get().settings;
    if (!g_scene.haveView || !g_scene.haveProjection) return;
    if (!settings.enabled && !photo::active() && !g_captureRequested) {
        return;
    }
    if (g_sceneStart < 0.0f && g_scene.projection[14] > -1.0f) {
        g_sceneStart = seconds();
        TMVS_LOG("scene: first gameplay camera");
    }
    // A new map: its own look (from its comments), before the mood picks a preset.
    {
        static int s_mapLoads = 0;
        if (engine::mapLoads() != s_mapLoads) {
            s_mapLoads = engine::mapLoads();
            Config::get().onMapComments(engine::mapComments());
        }
    }
    // Map mood, only in gameplay: menu backgrounds have their own light. From the map's time of
    // day when the engine told it (a map lit at midnight keeps its own mood; Rally at noon has
    // a sun warm enough for the colour test to call it sunset), else from the sun's colour.
    if (g_scene.haveLight && g_scene.projection[14] > -1.0f) {
        const float dayTime = engine::mapDayTime();
        Config::get().onMoodDetected(dayTime >= 0.0f ? moodOfDayTime(dayTime) : classifyMood(g_scene.lightColor));
    }

    hook::InternalScope internal;
    IDirect3DSurface9* target = nullptr;
    IDirect3DSurface9* savedDepth = nullptr;
    if (FAILED(device->GetRenderTarget(0, &target)) || !target) return;
    // The video export's supersampling (its dialog's "high quality" without the sub-frames of
    // its motion blur) renders the camera n x n times a frame into one target, each with a
    // sub-pixel offset, and averages them. Each is shaded: with only the first, the video got
    // 1 / (n n) of the effects. The rest repeat the first (its motion blur, focus, exposure).
    const bool samples = g_exporting && g_export.samplesPerFrame > 1;
    const bool repeat = samples && g_scene.processedCount > 0;
    // The same target twice in a frame would be shaded twice.
    if (g_scene.processedCount > 0 && target == g_scene.processedTarget && !repeat) {
        target->Release();
        return;
    }
    // Only the first camera of a single-camera frame continues the temporal history (and the
    // first of the export's samples: every frame has them).
    const bool temporal = g_scene.processedCount == 0 && (g_scene.lastFrameCount <= 1 || samples);
    if (g_scene.processedCount == 1 && !g_scene.loggedMultiCamera && !repeat) {
        g_scene.loggedMultiCamera = true;
        TMVS_LOG("scene: several main cameras in one frame (camera blend) - shading each, temporal effects paused");
    }
    g_scene.processedCount++;
    g_scene.processedTarget = target;
    device->GetDepthStencilSurface(&savedDepth);
    IDirect3DTexture9* sceneDepth = depth::textureFor(savedDepth);
    if (!sceneDepth) {
        target->Release();
        if (savedDepth) savedDepth->Release();
        return;
    }

    D3DSURFACE_DESC desc{};
    target->GetDesc(&desc);
    // The MediaTracker editor draws its 3D view into part of the screen, its panels around it
    // (drawn later, half transparent). Only that part is shaded: shading the whole screen
    // turned the panels' background into sky, and the effects missed the view (the camera's
    // projection belongs to its part of the screen).
    D3DVIEWPORT9 viewport{};
    device->GetViewport(&viewport);
    // Real panels only: replay cameras sometimes leave a few rows out (2560x1433 at 0,3).
    const bool part = viewport.Width >= 320 && viewport.Height >= 200 && viewport.X + viewport.Width <= desc.Width &&
                      viewport.Y + viewport.Height <= desc.Height &&
                      (viewport.Width < desc.Width * 0.95f || viewport.Height < desc.Height * 0.95f);
    const RECT area = {static_cast<LONG>(viewport.X), static_cast<LONG>(viewport.Y), static_cast<LONG>(viewport.X + viewport.Width),
                       static_cast<LONG>(viewport.Y + viewport.Height)};
    const UINT width = part ? viewport.Width : desc.Width, height = part ? viewport.Height : desc.Height;
    {
        static bool s_loggedPart = false;
        if (part && !s_loggedPart) {
            s_loggedPart = true;
            TMVS_LOG("scene: the camera draws into part of the screen (%ux%u at %u,%u): shading that part", width, height, viewport.X, viewport.Y);
        }
    }
    if (!g_pipeline.ready() && (g_pipelineFailed || !g_pipeline.init(device, g_dev.shaderDir))) {
        g_pipelineFailed = true;
        target->Release();
        if (savedDepth) savedDepth->Release();
        return;
    }
    if (!g_sceneCopy.create(device, width, height, D3DFMT_A8R8G8B8) || (part && !g_partOut.create(device, width, height, D3DFMT_A8R8G8B8))) {
        target->Release();
        if (savedDepth) savedDepth->Release();
        return;
    }

    g_pipeline.beginStateSave(device);
    device->StretchRect(target, part ? &area : nullptr, g_sceneCopy.surface, nullptr, D3DTEXF_NONE);
    device->SetDepthStencilSurface(nullptr);
    if (part) {
        if (!g_pipeline.copyDepthArea(device, sceneDepth, area, g_partDepth)) {
            device->SetRenderTarget(0, target);
            device->SetDepthStencilSurface(savedDepth);
            g_pipeline.endStateSave(device);
            target->Release();
            if (savedDepth) savedDepth->Release();
            return;
        }
        sceneDepth = g_partDepth.texture;
    }
    // Where the pipeline writes: the screen, or the part's own image (copied into place below).
    IDirect3DSurface9* shaded = part ? g_partOut.surface : target;

    Pipeline::Inputs inputs;
    inputs.color = g_sceneCopy.texture;
    if (g_hdrValid && !part && g_hdrScene.width == width && g_hdrScene.height == height) {
        inputs.hdr = g_hdrScene.texture;
        if (g_hdrAddedValid) inputs.added = g_hdrAdded.texture;
    }
    inputs.depth = sceneDepth;
    inputs.width = width;
    inputs.height = height;
    memcpy(inputs.view, g_scene.view, sizeof(inputs.view));
    memcpy(inputs.projection, g_scene.projection, sizeof(inputs.projection));
    inputs.sunKnown = sunDirection(inputs.sunDirection);
    inputs.sunColorKnown = g_scene.haveLight;
    inputs.moonReplaced = engine::moonReplaced();
    memcpy(inputs.sunColor, g_scene.lightColor, sizeof(inputs.sunColor));
    inputs.time = effectTime();
    inputs.temporal = temporal;
    inputs.video = g_exporting;
    inputs.repeat = repeat;
    {
        float lo = 0.0f, hi = 0.0f;
        if (engine::waterHeights(lo, hi)) {
            // -1e30 = none (TMUF Island reports no sea: 0, the pipeline measures it from the
            // sea's material). Stadium: 2, its water blocks (only their height is known).
            const bool known = lo > -1e29f || hi > -1e29f;
            inputs.water[0] = lo;
            inputs.water[1] = hi;
            inputs.water[2] = !known ? 0.0f : (engine::stadium() ? 2.0f : 1.0f);
        }
    }
    // The export counts as cinematic for as long as it runs (the hooks' 500 ms ran out within
    // one of its frames: a 4K frame with supersampling lost its blur halfway through).
    const bool cinematic = engine::cinematicActive() || g_exporting;
    inputs.driving = !cinematic;
    inputs.carKnown = engine::carLocation(inputs.carPosition);
    {
        static DWORD s_carSeen = 0;
        if (inputs.carKnown) s_carSeen = GetTickCount();
        inputs.carFromEngine = s_carSeen && GetTickCount() - s_carSeen < 5000;
    }
    {
        static int s_carSource = -1; // the log says once which way the trail finds the car
        const int source = inputs.carKnown ? 1 : 0;
        if (inputs.driving && source != s_carSource && (source == 1 || s_carSource < 0)) {
            s_carSource = source;
            TMVS_LOG("pipeline: neon trail follows the car %s", source ? "from the engine" : "in the depth buffer");
        }
    }
    {
        // Neon trail: a restart or a new map starts it over, a respawn starts a new line.
        static int s_resets = -1, s_respawns = -1;
        const int resets = engine::raceResets(), respawns = engine::respawns();
        static int s_maps = -1;
        if (engine::mapLoads() != s_maps) {
            s_maps = engine::mapLoads();
            g_pipeline.resetSeaLevel();
        }
        if (resets != s_resets) {
            if (s_resets >= 0) TMVS_LOG("engine: race reset (trail cleared)");
            s_resets = resets;
            g_pipeline.resetTrail();
            g_pipeline.resetHeights();
            g_pipeline.resetCeilings();
        }
        if (respawns != s_respawns) {
            if (s_respawns >= 0) TMVS_LOG("engine: respawn");
            s_respawns = respawns;
            g_pipeline.breakTrail();
            g_pipeline.resetHeights();
        }
    }
    inputs.jitter[0] = g_jitterUV[0];
    inputs.jitter[1] = g_jitterUV[1];
    inputs.photo = photo::active();
    photo::focusPoint(inputs.focusPoint);
    inputs.freeze = photo::tiling();
    inputs.lut = updateLut();
    inputs.materials = materialsWanted();
    if (g_castersOn) inputs.casterMap = casters::update;
    g_frameWidth = width;
    g_frameHeight = height;

    // F12: with the material masks on, the capture is the next frame whose draws carried their
    // material ids (the mask is built from them); not of a camera into part of the screen.
    const bool idFrame = g_idFrame && inputs.materials && !part;
    unsigned char idClasses[256] = {};
    if (idFrame) {
        for (size_t i = 0; i < g_idMaterials.size(); i++) idClasses[i + 1] = static_cast<unsigned char>(engine::materialClass(g_idMaterials[i].shader));
        inputs.materialClasses = idClasses;
    }
    if (g_idFrame && !idFrame) inputs.materials = false; // ids in the stencil, but no capture of them
    g_idFrame = false;
    const bool capture = g_captureRequested && (idFrame || !inputs.materials || part);
    if (capture) g_captureRequested = false;
    if (capture && !settings.enabled && !inputs.photo) writeCapture(device, inputs, false);
    if (settings.enabled || inputs.photo) {
        // Auto quality renders a reduced copy; your settings stay untouched.
        Settings effective = settings;
        // The menu points at a preset: show it (it glides in like a chosen one).
        if (Config::get().previewSettings(effective)) effective.debugView = settings.debugView;
        smoothLook(effective, effectTime());
        // Photo mode and the video export: full quality, whatever the frame rate.
        if (!inputs.photo && !inputs.video) g_autoQuality.apply(effective);
        if (inputs.video) {
            effective.quality = 2;
            // The game's supersampling is the anti-aliasing. TAA would run on the first of its
            // samples in a frame only (the others repeat it), and over its sub-frames it smears.
            if (g_export.samplesPerFrame > 1 || g_export.subFrames) effective.taa = false;
        }
        // Motion blur and depth of field are for watching, not for driving (the auto focus
        // would blur the track ahead of you).
        if (effective.cinematicOnlyInReplays && !cinematic && !inputs.photo) {
            effective.motionBlur = 0.0f;
            effective.depthOfField = 0.0f;
        }
        photo::adjust(effective);
        LARGE_INTEGER renderStart, renderEnd;
        QueryPerformanceCounter(&renderStart);
        g_pipeline.render(device, inputs, effective, shaded);
        LARGE_INTEGER captureStart, captureEnd;
        QueryPerformanceCounter(&captureStart);
        if (capture) writeCapture(device, inputs, idFrame);
        QueryPerformanceCounter(&captureEnd);
        // A preset was just saved: this frame (without HUD and menu) becomes its picture, never
        // with the compare split.
        std::string picture;
        if (temporal && Config::get().takePictureRequest(picture)) {
            thumbnails::savePicture(device, shaded, Config::get().presetPicture(picture));
            Config::get().pictureTaken();
        }
        // Photo mode's compare split: the game's image (copied before the pipeline) on one side.
        photo::applySplit(device, g_sceneCopy.surface, shaded);
        if (part) {
            device->StretchRect(g_partOut.surface, nullptr, target, &area, D3DTEXF_NONE);
            fillMenuBars(device, target, desc, area, settings.shadeMenuBars);
        }
        QueryPerformanceCounter(&renderEnd);
        const double toMs = 1000.0 / static_cast<double>(g_frequency.QuadPart);
        countPipelineCpu(static_cast<double>(renderEnd.QuadPart - renderStart.QuadPart) * toMs);
        logHangs(static_cast<double>(renderEnd.QuadPart - renderStart.QuadPart) * toMs,
                 static_cast<double>(captureEnd.QuadPart - captureStart.QuadPart) * toMs,
                 inputs.driving && !inputs.photo && g_scene.projection[14] > -1.0f); // not a menu background
        g_shadedThisFrame = true;
        // The shaded frame before the HUD: a photo or a hi-res tile.
        if (temporal) photo::afterRender(device, shaded);
        g_gameplayThisFrame = g_gameplayThisFrame || g_scene.projection[14] > -1.0f; // not a menu background
    }

    device->SetRenderTarget(0, target);
    device->SetDepthStencilSurface(savedDepth);
    g_pipeline.endStateSave(device);

    target->Release();
    if (savedDepth) savedDepth->Release();
}

// The video export: its start and end in the log, its clock (effectTime), and a clean start
// (the first frame of the video doesn't fade in from the editor's view).
void updateVideoExport() {
    engine::VideoExport v;
    const bool on = Config::get().settings.videoExportBest && engine::videoExport(v);
    ExportStats& stats = g_exportStats;
    if (g_exporting && g_scene.processedCount > stats.samplesSeen) stats.samplesSeen = g_scene.processedCount;
    if (on && !g_exporting) {
        stats = ExportStats();
        stats.clockStart = effectTime();
        stats.gameStart = v.gameClock;
        g_pipeline.resetHistory();
        TMVS_LOG("video export: %dx%d at %.0f fps%s, %s; game step %.3f ms. Full quality, the effects on the video's clock%s", v.width,
                 v.height, v.fps, v.writesVideo ? "" : " (a run without video output)",
                 v.samplesPerFrame > 1 ? "supersampling, every sample shaded"
                 : v.subFrames         ? "supersampling over sub-frames (the game's motion blur)"
                                       : "no supersampling",
                 v.step * 1000.0, v.samplesPerFrame > 1 || v.subFrames ? ", TAA off (the game's samples are the anti-aliasing)" : "");
        if (v.samplesPerFrame > 1 || v.subFrames)
            TMVS_LOG("video export: %d x %d samples (%d each frame, %d per video frame), stereo %d", v.hqSamples, v.hqSamples,
                     v.samplesPerFrame, v.samplesPerVideoFrame, v.stereo);
        if (v.step <= 0.0) TMVS_LOG("video export: the game's clock is not stepped (unexpected): the effects stay on the real clock");
    } else if (!on && g_exporting) {
        TMVS_LOG("video export ended: %u frames (%u on the video's clock), the effects' clock advanced %.2f s, the game's %.2f s; "
                 "at most %d main cameras shaded in a frame (expected %d)",
                 stats.frames, stats.stepped, effectTime() - stats.clockStart, g_export.gameClock - stats.gameStart, stats.samplesSeen,
                 g_export.samplesPerFrame);
    }
    g_exporting = on;
    if (on) {
        g_export = v;
        stats.frames++;
    }
    if (on && v.step > 0.0) {
        g_videoClock = g_videoClock < 0.0 ? static_cast<double>(effectTime()) : g_videoClock + v.step;
        stats.stepped++;
    } else if (g_videoClock >= 0.0) {
        g_clockOffset = static_cast<float>(g_videoClock - static_cast<double>(seconds()));
        g_videoClock = -1.0;
    }
}

// --- Engine callbacks -------------------------------------------------------

// TAA jitter: a Halton (2, 3) sequence of 8 sub-pixel offsets, one per frame. Only while
// TAA runs: without it the image would visibly shake.

float halton(int index, int base) {
    float f = 1.0f, r = 0.0f;
    for (int i = index; i > 0; i /= base) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(i % base);
    }
    return r;
}

void updateJitter() {
    Settings effective = Config::get().settings;
    if (!photo::active() && !g_exporting) g_autoQuality.apply(effective);
    // Hi-res tiles: no jitter (each tile must line up with its neighbours to the pixel). The
    // export's supersampling: no TAA, and the game shifts its samples itself.
    const bool supersampled = g_exporting && (g_export.samplesPerFrame > 1 || g_export.subFrames);
    const bool on = effective.enabled && effective.taa && effective.taaJitter && effective.debugView == 0 && g_frameWidth > 0 &&
                    !photo::tiling() && !supersampled;
    if (!on) {
        engine::setProjectionJitter(0.0f, 0.0f);
        g_jitterUV[0] = g_jitterUV[1] = 0.0f;
        return;
    }
    static int s_index = 0;
    s_index = s_index % 8 + 1;
    const float jx = (halton(s_index, 2) - 0.5f) * 2.0f / static_cast<float>(g_frameWidth);  // NDC, +-half a pixel
    const float jy = (halton(s_index, 3) - 0.5f) * 2.0f / static_cast<float>(g_frameHeight);
    engine::setProjectionJitter(jx, jy);
    g_jitterUV[0] = jx * 0.5f;
    g_jitterUV[1] = -jy * 0.5f;
}

void onFrameBegin() {
    tracer::event("## RenderFrameBegin");
    photo::beginFrame();
    {
        // The game's bloom and colour filter would work on the image the mod already graded.
        const Settings& s = Config::get().settings;
        engine::skipGameFx(s.enabled && s.disableGameFx);
        // A night sky (or the black hole) has no sun: the game's halo and flare go too.
        engine::hideSunFlare(s.enabled && Pipeline::skyReplacesSun(s));
        engine::hideGameSky(s.enabled && Pipeline::skyReplacesSun(s) && !s.gameSkyClouds);
    }
    updateVideoExport();
    updateJitter();
    g_cameraIndex = 0;
    g_scene.lastFrameCount = g_scene.processedCount;
    g_scene.processedCount = 0;
}

void onCameraBegin(void* camera) {
    if (!g_device) return;
    IDirect3DSurface9* rt = nullptr;
    g_device->GetRenderTarget(0, &rt);
    g_scene.inMainCamera = isMainTarget(g_device, rt);
    if (g_scene.inMainCamera) {
        g_scene.haveView = false;
        g_scene.haveProjection = false;
        engine::setMainCamera(camera);
        // Material masks: nothing left from the last frame where nothing is drawn (the sky).
        if (materialsWanted()) g_device->Clear(0, nullptr, D3DCLEAR_STENCIL, 0, 1.0f, 0);
        g_idFrame = g_captureRequested && materialsWanted();
        if (g_idFrame) g_idMaterials.clear();
        beginHdr(g_device, rt);
    } else if (rt) {
        // A screen-sized camera that isn't shaded: say why (a player's shaders stayed off in
        // races while they worked in the editor, and the log showed nothing). Each kind once.
        static std::vector<std::array<UINT, 5>> s_logged;
        D3DSURFACE_DESC rtDesc{}, dsDesc{};
        rt->GetDesc(&rtDesc);
        IDirect3DSurface9* ds = nullptr;
        g_device->GetDepthStencilSurface(&ds);
        if (ds) ds->GetDesc(&dsDesc);
        const bool readable = depth::textureFor(ds) != nullptr;
        if (ds) ds->Release();
        const std::array<UINT, 5> kind = {rtDesc.Width, rtDesc.Height, dsDesc.Width, dsDesc.Height, static_cast<UINT>(dsDesc.Format)};
        if (rtDesc.Width >= 640 && rtDesc.Height >= 400 && s_logged.size() < 8 &&
            std::find(s_logged.begin(), s_logged.end(), kind) == s_logged.end()) {
            s_logged.push_back(kind);
            TMVS_LOG("scene: camera into %ux%u not shaded (depth %ux%u format %u, %s)", rtDesc.Width, rtDesc.Height, dsDesc.Width,
                     dsDesc.Height, static_cast<unsigned>(dsDesc.Format),
                     !ds ? "no depth buffer" : (readable ? "size differs from the depth" : "depth not readable"));
        }
    }
    if (tracer::tracing()) {
        IDirect3DSurface9* ds = nullptr;
        g_device->GetDepthStencilSurface(&ds);
        tracer::event("## RenderCameraBegin camera=%p main=%d rt=%s ds=%s", camera, g_scene.inMainCamera, tracer::describe(rt),
                      tracer::describe(ds));
        if (ds) ds->Release();
    }
    if (rt) rt->Release();
}

// The game's materials: every draw of the main camera writes the class of its material
// (engine::currentMaterial: light, car, glass, grass) into the stencil buffer, and the
// pipeline turns that into a mask (lamps glow for real, white walls don't). Draws that don't
// write depth (glows, headlight cones, decals, particles over whole blocks) leave what is
// behind them: they marked whole blocks as lamps. If the game uses the stencil itself (shadow
// volumes) the masks stay off.
bool g_stencilTagged = false;
bool g_gameStencil = false;

bool materialsWanted() {
    const Settings& s = Config::get().settings;
    return s.enabled && s.gameMaterials && !g_gameStencil;
}

// The stencil states the tagging changes, as the game had them (given back when it stops).
constexpr D3DRENDERSTATETYPE kTagStates[] = {D3DRS_STENCILENABLE, D3DRS_TWOSIDEDSTENCILMODE, D3DRS_STENCILFUNC, D3DRS_STENCILPASS,
                                             D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_STENCILWRITEMASK, D3DRS_STENCILREF};
DWORD g_savedStencil[sizeof(kTagStates) / sizeof(kTagStates[0])] = {};

void untagStencil(IDirect3DDevice9* device) {
    if (!g_stencilTagged) return;
    for (size_t i = 0; i < sizeof(kTagStates) / sizeof(kTagStates[0]); i++) device->SetRenderState(kTagStates[i], g_savedStencil[i]);
    g_stencilTagged = false;
}

void tagStencil(IDirect3DDevice9* device, DWORD value, DWORD mask) {
    if (!g_stencilTagged) {
        for (size_t i = 0; i < sizeof(kTagStates) / sizeof(kTagStates[0]); i++) device->GetRenderState(kTagStates[i], &g_savedStencil[i]);
    }
    device->SetRenderState(D3DRS_STENCILENABLE, TRUE);
    device->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
    device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
    device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
    device->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_STENCILWRITEMASK, mask);
    device->SetRenderState(D3DRS_STENCILREF, value);
    g_stencilTagged = true;
}

// F12's frame: the id of a material (1..254; 255 = the 255th and every one after it).
DWORD materialId(const void* shader, bool depthWrite, bool additive) {
    for (size_t i = 0; i < g_idMaterials.size(); i++) {
        IdMaterial& m = g_idMaterials[i];
        if (m.shader == shader) {
            m.draws++;
            m.depthWrite = m.depthWrite || depthWrite;
            m.additive = m.additive || additive;
            return static_cast<DWORD>(i + 1);
        }
    }
    if (g_idMaterials.size() >= 255) return 255;
    g_idMaterials.push_back({shader, 1, depthWrite, additive});
    return static_cast<DWORD>(g_idMaterials.size());
}

// Every draw call (all four kinds: a draw without its own tag must not write the tag of the
// draw before it, the stencil state stays set).
void onDraw(IDirect3DDevice9* device) {
    onDrawAdded(device);
    if (!g_scene.inMainCamera || !materialsWanted()) {
        untagStencil(device);
        return;
    }
    DWORD stencil = FALSE;
    device->GetRenderState(D3DRS_STENCILENABLE, &stencil);
    if (stencil && !g_stencilTagged) {
        g_gameStencil = true;
        TMVS_LOG("materials: the game uses the stencil buffer itself, material masks off");
        return;
    }
    // Only draws with the depth test on: with it off the stencil is still written wherever
    // they draw (the stencil test passes always), and a screen-wide draw without depth (the
    // sky, effects at the end of the camera) wiped every class to "other".
    DWORD zwrite = TRUE, ztest = TRUE;
    device->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
    device->GetRenderState(D3DRS_ZENABLE, &ztest);
    if (!ztest) {
        untagStencil(device);
        return;
    }
    DWORD blend = FALSE, dest = D3DBLEND_ZERO;
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    device->GetRenderState(D3DRS_DESTBLEND, &dest);
    if (g_idFrame) {
        const DWORD id = materialId(engine::currentShader(), zwrite != FALSE, blend && dest == D3DBLEND_ONE);
        if (zwrite) tagStencil(device, id, 0xFF);
        else untagStencil(device);
        return;
    }
    if (zwrite) {
        // Its class, in the low bits; clears the "added light" bit of what it covers.
        tagStencil(device, static_cast<DWORD>(engine::currentMaterial()), 0xFF);
        return;
    }
    // Light the game adds on top of a surface (headlight pools, glows: one-one blending,
    // no depth writes) only sets bit 3 and keeps the class of the surface below.
    if (blend && dest == D3DBLEND_ONE) tagStencil(device, Pipeline::kAddedLightBit, Pipeline::kAddedLightBit);
    else untagStencil(device);
}

// CasterShadows: the main camera's opaque draws of static materials go to casters.cpp (which
// keeps the meshes that stay put). Not the car (it moves), glass or water (light passes).
void onDrawIndexed(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start, UINT count) {
    if (!g_castersOn || !g_scene.inMainCamera || !g_scene.haveView || !g_scene.haveProjection) return;
    DWORD z = FALSE, zwrite = FALSE, blend = FALSE, alphaTest = FALSE, colorWrite = 0;
    device->GetRenderState(D3DRS_ZENABLE, &z);
    device->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    device->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
    device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
    // Alpha-tested meshes (fences, leaves) would cast solid shadows: left out for now.
    if (!z || !zwrite || blend || alphaTest || !colorWrite) return;
    const engine::Material material = engine::currentMaterial();
    if (material == engine::Material::Car || material == engine::Material::Glass || material == engine::Material::Water) return;
    casters::recordDraw(device, type, base, minIndex, vertices, start, count, g_scene.view, g_scene.projection);
}

// Once a frame: the kept meshes belong to the map that is open (the game's buffers are held
// while they are kept), and only while the setting is on.
void castersTick() {
    const Settings& s = Config::get().settings;
    const bool on = s.enabled && s.casterShadows && engine::active();
    static int s_loads = -1, s_closes = -1, s_removed = -1;
    if (engine::mapLoads() != s_loads || engine::mapCloses() != s_closes) {
        s_loads = engine::mapLoads();
        s_closes = engine::mapCloses();
        casters::reset("map changed");
    }
    // The editor took blocks out: their shadows go (what is still there comes back once seen).
    if (engine::blocksRemoved() != s_removed) {
        static DWORD s_logged = 0;
        const bool log = GetTickCount() - s_logged > 60000;
        if (log) s_logged = GetTickCount();
        if (s_removed >= 0) casters::reset("blocks removed in the editor", log);
        s_removed = engine::blocksRemoved();
    }
    if (g_castersOn && !on) casters::reset("CasterShadows off");
    g_castersOn = on;
}

void onCameraEnd(void* camera, const engine::CameraInfo&) {
    if (g_device) untagStencil(g_device);
    if (tracer::tracing()) {
        tracer::event("## RenderCameraEnd camera=%p main=%d", camera, g_scene.inMainCamera);
        if (g_scene.inMainCamera) {
            traceMatrix("scene view", g_scene.view);
            traceMatrix("scene proj", g_scene.projection);
        }
    }
    if (g_scene.inMainCamera && g_device) {
        endHdr(g_device);
        processScene(g_device);
    }
    g_scene.inMainCamera = false;
}

void onOverlayBegin() {
    tracer::event("## RenderOverlayZones begin");
}

void onOverlayEnd() {
    tracer::event("## RenderOverlayZones end");
}

void onFrameEnd() {
    tracer::event("## RenderFrameEnd");
}

// --- Device callbacks -------------------------------------------------------

void onAdjustPresentParams(D3DPRESENT_PARAMETERS* params) {
    // The game rebuilds its present parameters from the bound depth buffer, which is our
    // INTZ texture. A back buffer can't have an INTZ auto depth-stencil: Reset would fail
    // (fullscreen <-> window, Alt+Enter, alt-tab) and the game would wait forever.
    if (params->EnableAutoDepthStencil && params->AutoDepthStencilFormat == depth::kINTZ) {
        TMVS_LOG("present: auto depth-stencil INTZ -> D24S8");
        params->AutoDepthStencilFormat = D3DFMT_D24S8;
    }
    if (Config::get().settings.disableGameMSAA && params->MultiSampleType != D3DMULTISAMPLE_NONE) {
        TMVS_LOG("present: disabling game MSAA x%d (depth effects need a single-sample depth buffer)",
                 static_cast<int>(params->MultiSampleType));
        params->MultiSampleType = D3DMULTISAMPLE_NONE;
        params->MultiSampleQuality = 0;
    }
}

void onDeviceCreated(IDirect3DDevice9* device) {
    g_device = device;
    depth::onDeviceReady(device);
}

void onPreReset() {
    g_pipeline.release();
    g_sceneCopy.destroy();
    g_partDepth.destroy();
    g_partOut.destroy();
    if (g_hdrGame) g_hdrGame->Release();
    g_hdrGame = nullptr;
    g_hdrLastGame = nullptr;
    g_hdrValid = false;
    g_hdrScene.destroy();
    g_hdrAddedActive = g_hdrAddedBound = g_hdrAddedValid = false;
    g_hdrAdded.destroy();
    casters::releaseDevice();
    depth::onPreReset();
    overlay::preReset();
}

void onPostReset(IDirect3DDevice9* device) {
    depth::onDeviceReady(device);
    overlay::postReset();
}

bool onCreateDepthStencil(IDirect3DDevice9* device, UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms,
                          IDirect3DSurface9** out, HRESULT* hr) {
    return Config::get().settings.readableGameDepth && depth::createReadable(device, w, h, format, ms, out, hr);
}

IDirect3DSurface9* onSetDepthStencil(IDirect3DSurface9* surface) {
    return depth::substitute(surface);
}

void onSetTransform(D3DTRANSFORMSTATETYPE type, const D3DMATRIX* m) {
    if (!g_scene.inMainCamera) return;
    if (type == D3DTS_VIEW) {
        memcpy(g_scene.view, m, sizeof(g_scene.view));
        g_scene.haveView = true;
    } else if (type == D3DTS_PROJECTION && m->_34 == 1.0f && m->_43 != 0.0f && !g_scene.haveProjection) {
        // The scene projection (perspective with a near plane). The sky is drawn with
        // a variant whose _43 is 0 (pushed to the far plane); ignore that one.
        memcpy(g_scene.projection, m, sizeof(g_scene.projection));
        g_scene.haveProjection = true;
    }
}

// Notes which game clears of this frame reached the top or bottom row of their target (the
// viewport, or the rects inside it). Only for fillBars: those rows are the game's own then.
void onClear(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects, DWORD flags) {
    if (!(flags & D3DCLEAR_TARGET)) return;
    IDirect3DSurface9* target = nullptr;
    if (FAILED(device->GetRenderTarget(0, &target)) || !target) return;
    D3DSURFACE_DESC desc{};
    target->GetDesc(&desc);
    target->Release(); // only the pointer is kept, to compare
    D3DVIEWPORT9 viewport{};
    device->GetViewport(&viewport);
    LONG top = static_cast<LONG>(viewport.Y), bottom = static_cast<LONG>(viewport.Y + viewport.Height);
    if (count > 0 && rects) {
        LONG lo = bottom, hi = top;
        for (DWORD i = 0; i < count; i++) {
            lo = rects[i].y1 < lo ? rects[i].y1 : lo;
            hi = rects[i].y2 > hi ? rects[i].y2 : hi;
        }
        top = lo > top ? lo : top;
        bottom = hi < bottom ? hi : bottom;
    }
    if (top <= 0) g_clearedTop = {target, g_frame};
    if (bottom >= static_cast<LONG>(desc.Height)) g_clearedBottom = {target, g_frame};
}

void onSetLight(DWORD, const D3DLIGHT9* light) {
    if (light->Type != D3DLIGHT_DIRECTIONAL) return;
    g_scene.lightDirection[0] = light->Direction.x;
    g_scene.lightDirection[1] = light->Direction.y;
    g_scene.lightDirection[2] = light->Direction.z;
    g_scene.lightColor[0] = light->Diffuse.r;
    g_scene.lightColor[1] = light->Diffuse.g;
    g_scene.lightColor[2] = light->Diffuse.b;
    g_scene.haveLight = true;
}

void relightOnSwitch(const Settings& s);

void onPresent(IDirect3DDevice9* device) {
    g_device = device;
    g_frame++;
    // Device lost (alt-tab, mode switch): nothing can be drawn until the game resets it.
    if (device->TestCooperativeLevel() != D3D_OK) return;

    // Fallback when the engine hooks are unavailable: process at Present (HUD included).
    if (!engine::active() && g_scene.haveView) processScene(device);

    // GPU timing costs a few queries per frame: only while it's used.
    const Settings& settings = Config::get().settings;
    g_pipeline.setProfiling(settings.autoQuality || Config::get().showOverlay);
    // Paused in photo mode: full quality there must not count as slow gameplay (the gap in
    // frame times when it ends starts a quiet period).
    // The video export likewise: its frames are slow on purpose.
    if (!photo::active() && !g_exporting) g_autoQuality.onFrame(seconds(), g_shadedThisFrame, g_pipeline, settings);
    D3DDEVICE_CREATION_PARAMETERS cp{};
    device->GetCreationParameters(&cp);
    audio::update(settings, seconds(), g_gameplayThisFrame, cp.hFocusWindow);
    {
        static float s_waterMin = 1e30f, s_waterMax = 1e30f;
        float lo = 0.0f, hi = 0.0f;
        const bool known = engine::waterHeights(lo, hi);
        if (!known) lo = hi = -1e30f;
        if (fabsf(lo - s_waterMin) > 0.01f || fabsf(hi - s_waterMax) > 0.01f) {
            s_waterMin = lo;
            s_waterMax = hi;
            if (known) TMVS_LOG("engine: water heights %.2f .. %.2f", lo, hi);
        }
    }
    // Replay / intro / video export detection, logged when it changes.
    static int s_cinematic = -1;
    const int cinematic = engine::cinematicActive() ? 1 : 0;
    if (cinematic != s_cinematic) {
        s_cinematic = cinematic;
        TMVS_LOG("engine: cinematic %s (sources %u)", cinematic ? "on" : "off", engine::cinematicSources());
    }
    relightOnSwitch(settings);
    castersTick();
    g_shadedThisFrame = false;
    g_gameplayThisFrame = false;

    overlay::Status status;
    status.pipeline = &g_pipeline;
    status.autoQuality = &g_autoQuality;
    status.depthAvailable = depth::texture() != nullptr;
    status.engineHooks = engine::active();
    status.sunKnown = sunDirection(status.sunDirection);
    status.shaderError = g_pipeline.lastError().empty() ? nullptr : g_pipeline.lastError().c_str();
    overlay::setStatus(status);
    overlay::draw(device);
    const bool reload = overlay::consumeReloadRequest() | ((GetAsyncKeyState(VK_F9) & 1) != 0);
    if (reload) {
        g_pipelineFailed = false;
        g_pipeline.reloadShaders(device);
    }

    if (tracer::tracing() || g_screenshotRequested) {
        // Screenshot of the final frame (effects + HUD), for checking results in game.
        hook::InternalScope internal;
        IDirect3DSurface9* backBuffer = nullptr;
        if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) && backBuffer) {
            wchar_t name[64];
            swprintf(name, 64, L"\\screen_%03d.bmp", g_capturesWritten - 1);
            capture::saveSurface(device, backBuffer, log::dataDir() + (g_screenshotRequested ? name : L"\\present.bmp"));
            backBuffer->Release();
        }
        g_screenshotRequested = false;
    }
    runDevAutomation(device);
    {
        // Back in the menu: an Author's Shader ends with its map, your own look is back (else
        // it stayed until the next map). Only when no map is open any more: switching maps
        // lets go of the old one too, and the next map's load decides then. The same map open
        // again without a new load (its decoration kept) gets its look back.
        static int s_closes = 0, s_loadsAtClose = -1;
        if (engine::mapCloses() != s_closes) {
            s_closes = engine::mapCloses();
            if (!engine::mapOpen() && s_loadsAtClose < 0) {
                TMVS_LOG("engine: map closed");
                s_loadsAtClose = engine::mapLoads();
                Config::get().onMapComments(std::string());
            }
        }
        if (s_loadsAtClose >= 0 && engine::mapOpen()) {
            if (engine::mapLoads() == s_loadsAtClose) {
                TMVS_LOG("engine: the same map open again");
                Config::get().onMapComments(engine::mapComments());
            }
            s_loadsAtClose = -1;
        }
    }
    Config::get().tick();
    if (GetAsyncKeyState(VK_F11) & 1) tracer::arm();
    if (GetAsyncKeyState(VK_F12) & 1) {
        // Photo mode: a photo without the HUD (Shift: hi-res) instead of a .tmcap capture.
        if (photo::active()) photo::request(GetAsyncKeyState(VK_SHIFT) < 0);
        else g_captureRequested = true;
    }
}

} // namespace

void setModule(HMODULE module) {
    g_module = module;
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    g_moduleDir = path;
    size_t slash = g_moduleDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) g_moduleDir.resize(slash);
}

const std::wstring& moduleDir() {
    return g_moduleDir;
}

namespace {

bool g_passive = false;
hook::DeviceCallbacks g_deviceCallbacks;

DWORD WINAPI retryHooks(LPVOID) {
    for (int attempt = 2; attempt <= 20; attempt++) {
        Sleep(1000);
        if (hook::install(g_deviceCallbacks)) return 0;
    }
    TMVS_LOG("hook: failed to hook Direct3D 9");
    return 0;
}

// Map load: light a day or sunset map at midnight when its look will have a night sky (the
// sky replaces the sun anyway; the game's own night has no baked sun to take out).
// The map's mood comes from the same time of day (see processScene), so the look decided here
// is the one the map gets.
bool nightFor(float dayTime, bool log) {
    const Config& config = Config::get();
    const Mood mood = moodOfDayTime(dayTime);
    if (mood == Mood::Night) return false; // a night map already
    const bool night = Pipeline::skyReplacesSun(config.lookForMood(mood));
    if (!night && log) TMVS_LOG("config: real night: the %s look has no night sky", moodName(mood));
    return night;
}

bool nightAtLoad(float dayTime) {
    const Settings& s = Config::get().settings;
    if (!s.enabled || !s.gameNight) {
        TMVS_LOG("config: real night off (setting)");
        return false;
    }
    return nightFor(dayTime, true);
}

// EngineMoon, at map load: on a night map (its own night or lit at midnight) whose look has the
// black hole, the game's own moon (its baked light and shadows, glints, the car) shines from the
// black hole instead of from the side, or is off. Decided with the look of the map's mood, as
// real night is; an Author's Shader read later doesn't change it.
bool moonAtLoad(float dayTime, bool litAtNight, engine::MoonLight& moon) {
    const Settings& s = Config::get().settings;
    if (!s.enabled || s.engineMoon == 0 || dayTime < 0.0f) return false;
    const Mood mood = moodOfDayTime(dayTime);
    if (!litAtNight && mood != Mood::Night) return false; // the game's sun lights this map
    const Settings look = Config::get().lookForMood(mood);
    if (!Pipeline::blackHoleLights(look)) {
        TMVS_LOG("config: engine moon: the %s look has no black hole, the game's moon stays", moodName(mood));
        return false;
    }
    moon.mode = s.engineMoon;
    Pipeline::blackHoleDirection(look, moon.toward);
    // Not from below the horizon (BlackHoleElevation goes down to -10): the ground would shade
    // everything and light the undersides. At least 3 degrees up.
    const float minUp = 0.0523f;
    if (moon.toward[1] < minUp) {
        const float flat = sqrtf(moon.toward[0] * moon.toward[0] + moon.toward[2] * moon.toward[2]);
        const float k = flat > 1e-4f ? sqrtf(1.0f - minUp * minUp) / flat : 0.0f;
        moon.toward[0] *= k;
        moon.toward[2] *= k;
        moon.toward[1] = flat > 1e-4f ? minUp : 1.0f;
    }
    return true;
}

// Real night switched in the menu, or a look picked with or without a night sky while a map
// runs: the map is relit now, not at the next one. It follows the look that is on (also an
// Author's Shader, read after the map's light was set up), once that has held for 1.5 s
// (pointing at presets in the menu previews them; transitions glide). Only in gameplay (the
// weather of a map that was left may be gone); F7 (shaders off to compare) doesn't relight.
void relightOnSwitch(const Settings& s) {
    static double s_since = -1.0;
    static bool s_failed = false;
    const float dayTime = engine::mapDayTime();
    if (!g_gameplayThisFrame || dayTime < 0.0f) {
        s_since = -1.0;
        return;
    }
    const bool want = s.gameNight && moodOfDayTime(dayTime) != Mood::Night && Pipeline::skyReplacesSun(s);
    if (want == engine::mapLitAtNight()) {
        s_since = -1.0;
        s_failed = false;
        return;
    }
    if (s_failed) return;
    if (s_since < 0.0) s_since = seconds();
    if (seconds() - s_since < 1.5) return;
    s_since = -1.0;
    if (!engine::relightNight(want)) {
        s_failed = true;
        TMVS_LOG("config: real night: this map can't be relit now (next map)");
    }
}

void bootOnce() {
    QueryPerformanceFrequency(&g_frequency);
    QueryPerformanceCounter(&g_startTime);
    TMVS_LOG("TM Vibrant Shaders %s starting", TM_SHADERS_VERSION_A);
    loadDevOptions();
    Config::get().load();
    lut::writeBuiltInLooks();
    gameshaders::install();

    engine::Callbacks engineCallbacks;
    engineCallbacks.frameBegin = onFrameBegin;
    engineCallbacks.cameraBegin = onCameraBegin;
    engineCallbacks.cameraEnd = onCameraEnd;
    engineCallbacks.overlayBegin = onOverlayBegin;
    engineCallbacks.overlayEnd = onOverlayEnd;
    engineCallbacks.frameEnd = onFrameEnd;
    engine::install(engineCallbacks);
    engine::setNightAtLoad(nightAtLoad);
    engine::setMoonAtLoad(moonAtLoad);

    hook::DeviceCallbacks deviceCallbacks;
    deviceCallbacks.adjustPresentParams = onAdjustPresentParams;
    deviceCallbacks.deviceCreated = onDeviceCreated;
    deviceCallbacks.present = onPresent;
    deviceCallbacks.preReset = onPreReset;
    deviceCallbacks.postReset = onPostReset;
    deviceCallbacks.setDepthStencil = onSetDepthStencil;
    deviceCallbacks.setRenderTarget = onSetRenderTarget;
    deviceCallbacks.createDepthStencil = onCreateDepthStencil;
    deviceCallbacks.setTransform = onSetTransform;
    deviceCallbacks.setLight = onSetLight;
    deviceCallbacks.clear = onClear;
    deviceCallbacks.draw = onDraw;
    deviceCallbacks.drawDone = onDrawDone;
    deviceCallbacks.drawIndexed = onDrawIndexed;
    g_deviceCallbacks = deviceCallbacks;
    // Never block the caller (possibly the game's main thread) on retries.
    if (!hook::install(deviceCallbacks)) {
        HANDLE thread = CreateThread(nullptr, 0, retryHooks, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
}

} // namespace

bool claimInstance() {
    wchar_t name[64];
    swprintf(name, 64, L"Local\\TMVibrantShaders_%lu", GetCurrentProcessId());
    HANDLE mutex = CreateMutexW(nullptr, FALSE, name); // kept open for the life of the process
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        g_passive = true;
    }
    return !g_passive;
}

bool active() {
    return !g_passive;
}

void ensureBooted() {
    static volatile LONG state = 0; // 0 not started, 1 booting, 2 done
    static DWORD bootThread = 0;
    if (g_passive || state == 2) return;
    if (InterlockedCompareExchange(&state, 1, 0) == 0) {
        bootThread = GetCurrentThreadId();
        bootOnce();
        InterlockedExchange(&state, 2);
        return;
    }
    if (bootThread == GetCurrentThreadId()) return; // re-entered: the boot itself creates Direct3D
    for (int waited = 0; state != 2 && waited < 5000; waited++) Sleep(1);
}

void boot() {
    ensureBooted();
}

void shutdown() {
    audio::shutdown();
    hook::remove();
}

} // namespace plugin
} // namespace tmshaders
