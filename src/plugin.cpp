#include "plugin.h"
#include "capture.h"
#include "config.h"
#include "depth.h"
#include "engine.h"
#include "framecap.h"
#include "gfx.h"
#include "hook.h"
#include "log.h"
#include "overlay.h"
#include "pipeline.h"
#include "tracer.h"
#include "tm_shaders_version.h"
#include <d3d9.h>
#include <cmath>
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
    std::vector<std::pair<float, std::wstring>> keys;
    std::wstring shaderDir;      // load shaders from here (hot reload with F9)
} g_dev;
float g_nextCaptureAt = 0.0f;
float g_sceneStart = -1.0f; // first frame with a gameplay camera (near plane < 1 m)
bool g_screenshotRequested = false;
size_t g_nextKey = 0;

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
    bool processedThisFrame = false;
} g_scene;

bool g_captureRequested = false;
int g_capturesWritten = 0;
Pipeline g_pipeline;
bool g_pipelineFailed = false; // don't retry a failed compile every frame; F9 retries
gfx::Target g_sceneCopy;

float seconds() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<float>(static_cast<double>(now.QuadPart - g_startTime.QuadPart) / static_cast<double>(g_frequency.QuadPart));
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
    GetPrivateProfileStringW(L"dev", L"Keys", L"", buffer, MAX_PATH, ini);
    for (wchar_t* token = wcstok(buffer, L","); token; token = wcstok(nullptr, L",")) {
        float at = 0.0f;
        wchar_t action[32] = {};
        if (swscanf(token, L"%f:%31ls", &at, action) == 2) g_dev.keys.emplace_back(at, action);
    }
    g_nextCaptureAt = g_dev.captureAt;
    engine::forceShadows(GetPrivateProfileIntW(L"dev", L"ForceShadows", -1, ini));
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
    if (g_nextKey < g_dev.keys.size() && now >= g_dev.keys[g_nextKey].first) {
        D3DDEVICE_CREATION_PARAMETERS cp{};
        device->GetCreationParameters(&cp);
        const std::wstring& action = g_dev.keys[g_nextKey].second;
        HWND window = cp.hFocusWindow;
        if (action == L"min") {
            ShowWindow(window, SW_MINIMIZE);
        } else if (action == L"restore") {
            ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
        } else if (action[0] == L'a') {
            const WPARAM vk = static_cast<WPARAM>(_wtoi(action.c_str() + 1));
            PostMessageW(window, WM_SYSKEYDOWN, vk, 1 | (1 << 29));
            PostMessageW(window, WM_SYSKEYUP, vk, 0xE0000001);
        } else {
            const WPARAM vk = static_cast<WPARAM>(_wtoi(action.c_str()));
            PostMessageW(window, WM_KEYDOWN, vk, 1);
            PostMessageW(window, WM_KEYUP, vk, 0xC0000001);
        }
        TMVS_LOG("dev: action %ls", action.c_str());
        g_nextKey++;
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

void writeCapture(IDirect3DDevice9* device, const Pipeline::Inputs& inputs) {
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
    swprintf(name, 64, L"\\capture_%03d.tmcap", g_capturesWritten++);
    cap.save(log::dataDir() + name);
    TMVS_LOG("capture: wrote %ls", name + 1);
    g_screenshotRequested = true;
}

// Runs the post pipeline over the main camera's image, before the HUD is drawn.
void processScene(IDirect3DDevice9* device) {
    Settings& settings = Config::get().settings;
    if (g_scene.processedThisFrame) return;
    if (!settings.enabled && !g_captureRequested) return;
    if (!g_scene.haveView || !g_scene.haveProjection) return;
    g_scene.processedThisFrame = true;
    if (g_sceneStart < 0.0f && g_scene.projection[14] > -1.0f) {
        g_sceneStart = seconds();
        TMVS_LOG("scene: first gameplay camera");
    }
    // Map mood from the sun light, only in gameplay: menu backgrounds have their own light.
    if (g_scene.haveLight && g_scene.projection[14] > -1.0f) Config::get().onMoodDetected(classifyMood(g_scene.lightColor));

    hook::InternalScope internal;
    IDirect3DSurface9* target = nullptr;
    IDirect3DSurface9* savedDepth = nullptr;
    if (FAILED(device->GetRenderTarget(0, &target)) || !target) return;
    device->GetDepthStencilSurface(&savedDepth);
    IDirect3DTexture9* sceneDepth = depth::textureFor(savedDepth);
    if (!sceneDepth) {
        target->Release();
        if (savedDepth) savedDepth->Release();
        return;
    }

    D3DSURFACE_DESC desc{};
    target->GetDesc(&desc);
    if (!g_pipeline.ready() && (g_pipelineFailed || !g_pipeline.init(device, g_dev.shaderDir))) {
        g_pipelineFailed = true;
        target->Release();
        if (savedDepth) savedDepth->Release();
        return;
    }
    if (!g_sceneCopy.create(device, desc.Width, desc.Height, D3DFMT_A8R8G8B8)) {
        target->Release();
        if (savedDepth) savedDepth->Release();
        return;
    }

    g_pipeline.beginStateSave(device);
    device->StretchRect(target, nullptr, g_sceneCopy.surface, nullptr, D3DTEXF_NONE);
    device->SetDepthStencilSurface(nullptr);

    Pipeline::Inputs inputs;
    inputs.color = g_sceneCopy.texture;
    inputs.depth = sceneDepth;
    inputs.width = desc.Width;
    inputs.height = desc.Height;
    memcpy(inputs.view, g_scene.view, sizeof(inputs.view));
    memcpy(inputs.projection, g_scene.projection, sizeof(inputs.projection));
    inputs.sunKnown = sunDirection(inputs.sunDirection);
    inputs.sunColorKnown = g_scene.haveLight;
    memcpy(inputs.sunColor, g_scene.lightColor, sizeof(inputs.sunColor));
    inputs.time = seconds();

    if (g_captureRequested) {
        g_captureRequested = false;
        writeCapture(device, inputs);
    }
    if (settings.enabled) g_pipeline.render(device, inputs, settings, target);

    device->SetRenderTarget(0, target);
    device->SetDepthStencilSurface(savedDepth);
    g_pipeline.endStateSave(device);

    target->Release();
    if (savedDepth) savedDepth->Release();
}

// --- Engine callbacks -------------------------------------------------------

void onFrameBegin() {
    tracer::event("## RenderFrameBegin");
    g_cameraIndex = 0;
    g_scene.processedThisFrame = false;
}

void onCameraBegin(void* camera) {
    if (!g_device) return;
    IDirect3DSurface9* rt = nullptr;
    g_device->GetRenderTarget(0, &rt);
    g_scene.inMainCamera = isMainTarget(g_device, rt);
    if (g_scene.inMainCamera) {
        g_scene.haveView = false;
        g_scene.haveProjection = false;
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

void onCameraEnd(void* camera, const engine::CameraInfo&) {
    if (tracer::tracing()) {
        tracer::event("## RenderCameraEnd camera=%p main=%d", camera, g_scene.inMainCamera);
        if (g_scene.inMainCamera) {
            traceMatrix("scene view", g_scene.view);
            traceMatrix("scene proj", g_scene.projection);
        }
    }
    if (g_scene.inMainCamera && g_device) processScene(g_device);
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

void onPresent(IDirect3DDevice9* device) {
    g_device = device;
    g_frame++;
    // Device lost (alt-tab, mode switch): nothing can be drawn until the game resets it.
    if (device->TestCooperativeLevel() != D3D_OK) return;

    // Fallback when the engine hooks are unavailable: process at Present (HUD included).
    if (!engine::active() && g_scene.haveView) processScene(device);

    overlay::Status status;
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
    Config::get().tick();
    if (GetAsyncKeyState(VK_F11) & 1) tracer::arm();
    if (GetAsyncKeyState(VK_F12) & 1) g_captureRequested = true;
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

void bootOnce() {
    QueryPerformanceFrequency(&g_frequency);
    QueryPerformanceCounter(&g_startTime);
    TMVS_LOG("TM Vibrant Shaders %s starting", TM_SHADERS_VERSION_A);
    loadDevOptions();
    Config::get().load();

    engine::Callbacks engineCallbacks;
    engineCallbacks.frameBegin = onFrameBegin;
    engineCallbacks.cameraBegin = onCameraBegin;
    engineCallbacks.cameraEnd = onCameraEnd;
    engineCallbacks.overlayBegin = onOverlayBegin;
    engineCallbacks.overlayEnd = onOverlayEnd;
    engineCallbacks.frameEnd = onFrameEnd;
    engine::install(engineCallbacks);

    hook::DeviceCallbacks deviceCallbacks;
    deviceCallbacks.adjustPresentParams = onAdjustPresentParams;
    deviceCallbacks.deviceCreated = onDeviceCreated;
    deviceCallbacks.present = onPresent;
    deviceCallbacks.preReset = onPreReset;
    deviceCallbacks.postReset = onPostReset;
    deviceCallbacks.setDepthStencil = onSetDepthStencil;
    deviceCallbacks.createDepthStencil = onCreateDepthStencil;
    deviceCallbacks.setTransform = onSetTransform;
    deviceCallbacks.setLight = onSetLight;
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
    hook::remove();
}

} // namespace plugin
} // namespace tmshaders
