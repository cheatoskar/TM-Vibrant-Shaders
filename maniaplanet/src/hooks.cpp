// D3D11/DXGI vtable hooks and the frame logic: which draw is the engine's tone map, where its post
// effects end and its interface begins, and the scene's depth buffer.
//
// The engine's pixel shaders are known by their DXBC checksum (shaderdb), so a frame reads like:
//   scene draws (G-buffer, lights, particles; the main depth buffer bound)
//   -> tone map (HDR -> 8-bit) -> post effects (FXAA, colour grading, bloom ...)
//   -> the first other draw on that image: the interface starts here, the mod's pass goes in front.
// When the interface start can't be found, the pass runs at Present (interface graded too).
#include "hooks.h"
#include "autoquality11.h"
#include "capture.h"
#include "engine_frame.h"
#include "log.h"
#include "overlay.h"
#include "renderer.h"
#include "options.h"
#include "config.h"
#include "thumbnails11.h"
#include "shaderdb.h"
#include <windows.h>
#include <dxgi1_2.h>
#include <mutex>
#include <shared_mutex>
#include <algorithm>
#include <unordered_map>

namespace tmvs_mp {
namespace hooks {
namespace {

// vtable slots (d3d11.h / dxgi.h, checked against the 10.0.26100 SDK headers)
enum DeviceSlot { kCreatePixelShader = 15, kCreateDeferredContext = 27 };
enum ContextSlot {
    kPSSetShader = 9,
    kDrawIndexed = 12,
    kDraw = 13,
    kMap = 14,
    kUnmap = 15,
    kDrawIndexedInstanced = 20,
    kDrawInstanced = 21,
    kOMSetRenderTargets = 33,
    kOMSetRenderTargetsAndUAVs = 34,
    kUpdateSubresource = 48,
    kExecuteCommandList = 58
};
enum SwapSlot { kPresent = 8, kResizeBuffers = 13, kPresent1 = 22 };
enum FactorySlot { kCreateSwapChain = 10, kCreateSwapChainForHwnd = 15 };

thread_local int t_inside = 0;

// One hooked vtable slot. Objects of different classes (a debug-layer device, another runtime's
// swap chain) have their own vtables, so the original is kept per vtable.
//
// The D3D11 runtime keeps a device's and a context's vtable inside the object and rewrites all of
// it when the game calls ID3D10Multithread::SetMultithreadProtected (the entries become locking
// wrappers). ManiaPlanet does that after creating the device, which silently removed every hook.
// install() is therefore repeated at every Present (refresh()): a slot that no longer holds the
// detour takes the runtime's new function as its original and gets the detour again.
struct Hook {
    void** vtables[4] = {};
    void* originals[4] = {};
    int count = 0;

    // True when the slot had to be (re)patched.
    bool install(void* object, int slot, void* detour) {
        void** vtable = *reinterpret_cast<void***>(object);
        int index = -1;
        for (int i = 0; i < count; i++)
            if (vtables[i] == vtable) index = i;
        if (vtable[slot] == detour) return false;
        if (index < 0) {
            if (count == 4) return false;
            index = count;
        }
        DWORD old = 0;
        if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
        originals[index] = vtable[slot];
        vtable[slot] = detour;
        VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
        if (index == count) vtables[count++] = vtable;
        return true;
    }

    template <typename F> F get(const void* object) const {
        void** vtable = *reinterpret_cast<void** const*>(object);
        for (int i = 1; i < count; i++)
            if (vtables[i] == vtable) return reinterpret_cast<F>(originals[i]);
        return reinterpret_cast<F>(originals[0]);
    }
};

void repairDraws(ID3D11DeviceContext* ctx);

using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                                         ID3D11PixelShader**);
using CreateDeferredContextFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);
using PSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                                      ID3D11DepthStencilView*);
using OMSetRenderTargetsAndUAVsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                                             ID3D11DepthStencilView*, UINT, UINT,
                                                             ID3D11UnorderedAccessView* const*, const UINT*);
using ExecuteCommandListFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                          D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using UpdateSubresourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*,
                                                     UINT, UINT);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);

Hook g_createPixelShader;
Hook g_createDeferredContext;
Hook g_psSetShader;
Hook g_drawIndexed;
Hook g_draw;
Hook g_drawIndexedInstanced;
Hook g_drawInstanced;
Hook g_omSetRenderTargets;
Hook g_omSetRenderTargetsAndUAVs;
Hook g_executeCommandList;
Hook g_map;
Hook g_unmap;
Hook g_updateSubresource;
Hook g_present;
Hook g_present1;
Hook g_resizeBuffers;
Hook g_createSwapChain;
Hook g_createSwapChainForHwnd;

// Pixel shader -> what it is. Written by the game's loading threads, read on the render thread.
std::shared_mutex g_shaderLock;
std::unordered_map<ID3D11PixelShader*, ShaderInfo> g_shaders;
int g_unknownLogged = 0;

Stats g_stats;

// The device whose swap chain presents; its immediate context is the one we follow.
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
Renderer g_renderer;
bool g_rendererReady = false;
ID3D11RenderTargetView* g_backBufferRtv = nullptr;
ID3D11Resource* g_backBuffer = nullptr; // identity only

enum class Phase { Scene, Post, Done };

// Render-thread state of the current frame.
struct Frame {
    ShaderInfo ps;
    ID3D11RenderTargetView* rtv = nullptr; // render target 0 as bound (not owned)
    ID3D11Resource* rt = nullptr;          // its resource (identity only)
    UINT rtW = 0, rtH = 0;
    ID3D11Resource* ds = nullptr;          // bound depth buffer (identity only)
    UINT dsW = 0, dsH = 0;
    Phase phase = Phase::Scene;
    ID3D11Resource* postImage = nullptr;   // where the tone map / post effects wrote last
    ID3D11RenderTargetView* postRtv = nullptr; // the view they wrote it through (not owned)
    bool toneMapped = false;               // the mod's HDR stage replaced the engine's tone map
    bool toneMapSeen = false;
    bool injected = false;
    bool texturesTaken = false;
    Placed placed = Placed::None;
    const char* hudShader = nullptr;
    uint32_t draws = 0;
    uint32_t commandLists = 0;
};
Frame g_frame;
// The look of the frame being drawn: your settings, or the preset the menu points at (prepareLook).
tmshaders::Settings g_lookSettings;
Look g_look;
AutoQuality g_autoQuality;
unsigned g_frameCounter = 0;
tmshaders::Mood g_seenMood = tmshaders::Mood::Unknown;
int g_moodFrames = 0;
void prepareLook();

// Seconds since the mod started (animated effects).
double nowSeconds() {
    static LARGE_INTEGER frequency = {}, start = {};
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!frequency.QuadPart) {
        QueryPerformanceFrequency(&frequency);
        start = now;
    }
    return static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);
}

// The engine's textures of this frame (owned references, kept until replaced) and its camera.
ID3D11Resource* g_tex[kTexCount] = {};
Camera g_camera;
bool g_cameraThisFrame = false;

void setTexture(int index, ID3D11Resource* res) {
    if (res == g_tex[index]) return;
    if (g_tex[index]) g_tex[index]->Release();
    g_tex[index] = res;
    if (res) res->AddRef();
}

void setSceneDepth(ID3D11Resource* depth) {
    setTexture(kTexDepth, depth);
}

void releaseTextures() {
    for (int i = 0; i < kTexCount; i++) setTexture(i, nullptr);
}

// Copies of the engine's SceneV (vertex) and SceneP (pixel lighting) constant buffers, taken
// while it uploads them (Map/Unmap or UpdateSubresource), so the camera and the sun can be read
// without a GPU read-back.
struct SceneVCopy {
    ID3D11Resource* buffer = nullptr; // identity only
    void* mapped = nullptr;
    UINT size = 0;
    unsigned char data[kSceneVSize] = {};
    bool filled = false;
};
SceneVCopy g_sceneV[16];
std::unordered_map<ID3D11Resource*, UINT> g_isSceneV;

// The size of the engine's per-camera constant buffer this is (SceneV 704, SceneP 672), else 0.
UINT sceneBufferSize(ID3D11Resource* res) {
    auto it = g_isSceneV.find(res);
    if (it != g_isSceneV.end()) return it->second;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    UINT size = 0;
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d;
        static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
        if ((d.ByteWidth == kSceneVSize || d.ByteWidth == kScenePSize) && (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) size = d.ByteWidth;
    }
    if (g_isSceneV.size() > 4096) g_isSceneV.clear();
    g_isSceneV[res] = size;
    return size;
}

SceneVCopy* sceneVCopy(ID3D11Resource* res, bool create) {
    for (auto& c : g_sceneV)
        if (c.buffer == res) return &c;
    if (!create) return nullptr;
    for (auto& c : g_sceneV) {
        if (!c.buffer) {
            c.buffer = res;
            return &c;
        }
    }
    g_sceneV[0] = SceneVCopy{};
    g_sceneV[0].buffer = res;
    return &g_sceneV[0];
}

// The main camera: the SceneV bound to the vertex shader of the lighting-read pass.
void takeCamera(ID3D11DeviceContext* ctx) {
    ID3D11Buffer* cb = nullptr;
    ctx->VSGetConstantBuffers(13, 1, &cb);
    if (!cb) return;
    const SceneVCopy* copy = sceneVCopy(cb, false);
    if (!copy || !copy->filled || copy->size != kSceneVSize) {
        // Not an upload we saw: log what the engine binds instead (once), to learn its scheme.
        static int logged = 0;
        if (logged++ < 3) {
            D3D11_BUFFER_DESC d;
            cb->GetDesc(&d);
            TMVS_LOG("hooks: vertex constants 13 are %u bytes, usage %d, cpu 0x%x (no SceneV copy)", d.ByteWidth, d.Usage,
                     d.CPUAccessFlags);
        }
        cb->Release();
        return;
    }
    cb->Release();
    memcpy(g_camera.raw, copy->data, kSceneVSize);
    const bool was = g_camera.valid;
    decodeCamera(g_camera);
    g_cameraThisFrame = g_camera.valid;
    // The lighting pass's own constants (pixel slot 13): the sun exactly as the engine lights with it.
    ID3D11Buffer* pcb = nullptr;
    ctx->PSGetConstantBuffers(13, 1, &pcb);
    if (pcb) {
        const SceneVCopy* p = sceneVCopy(pcb, false);
        if (p && p->filled && p->size == kScenePSize) {
            memcpy(g_camera.rawP, p->data, kScenePSize);
            g_camera.hasSceneP = true;
            const bool had = g_camera.sunFromSceneP;
            if (decodeSceneP(g_camera) && !had)
                TMVS_LOG("hooks: sun from the lighting constants (towards the sun %.3f %.3f %.3f, colour %.2f %.2f %.2f)",
                         g_camera.sunDirection[0], g_camera.sunDirection[1], g_camera.sunDirection[2], g_camera.sunColour[0],
                         g_camera.sunColour[1], g_camera.sunColour[2]);
        }
        pcb->Release();
    }
    static int logged = 0;
    if (g_camera.valid != was && logged++ < 5) {
        TMVS_LOG("hooks: camera %s (eye %.1f %.1f %.1f, matrix %s)", g_camera.valid ? "found" : "lost", g_camera.eye[0],
                 g_camera.eye[1], g_camera.eye[2], g_camera.transposed ? "in columns" : "in rows");
    }
}

// The textures a scene shader reads by name (normals, light buffers, motion).
void takeSceneTextures(ID3D11DeviceContext* ctx, const ShaderInfo& ps) {
    static const struct {
        Slot slot;
        int tex;
    } kWanted[] = {{kSlotNormal, kTexNormal},       {kSlotDiffuse, kTexDiffuse}, {kSlotSpecular, kTexSpecular},
                {kSlotLightMask, kTexLightMask}, {kSlotMotion, kTexMotion}};
    for (const auto& m : kWanted) {
        if (!ps.reads(m.slot)) continue;
        ID3D11ShaderResourceView* srv = nullptr;
        ctx->PSGetShaderResources(ps.slots[m.slot], 1, &srv);
        if (!srv) continue;
        ID3D11Resource* res = nullptr;
        srv->GetResource(&res);
        srv->Release();
        if (res) {
            setTexture(m.tex, res);
            res->Release();
        }
    }
}

void sizeOf(ID3D11Resource* res, UINT& w, UINT& h) {
    w = h = 0;
    ID3D11Texture2D* tex = nullptr;
    if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        w = d.Width;
        h = d.Height;
        tex->Release();
    }
}

ID3D11Resource* resourceOf(ID3D11View* view) {
    if (!view) return nullptr;
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    if (res) res->Release(); // the view keeps it alive; we only compare the pointer
    return res;
}

bool isMain(ID3D11DeviceContext* ctx) {
    return ctx == g_ctx && !t_inside;
}

// A render target big enough and with the screen's shape to be the game's view (not a shadow map,
// a reflection or a thumbnail). Lower render scales are fine.
bool screenSized(UINT w, UINT h) {
    if (!g_stats.width || !w || !h) return false;
    if (w * 2 < g_stats.width || h * 2 < g_stats.height) return false;
    const float a = static_cast<float>(w) / h, b = static_cast<float>(g_stats.width) / g_stats.height;
    return a > b * 0.97f && a < b * 1.03f;
}

bool readsResource(ID3D11DeviceContext* ctx, ID3D11Resource* res) {
    if (!res) return false;
    ID3D11ShaderResourceView* srvs[8] = {};
    ctx->PSGetShaderResources(0, 8, srvs);
    bool found = false;
    for (auto* s : srvs) {
        if (!s) continue;
        if (resourceOf(s) == res) found = true;
        s->Release();
    }
    return found;
}

void inject(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, Placed where) {
    if (!g_lookSettings.enabled || !g_rendererReady) return;
    const EngineView view{g_tex, g_cameraThisFrame ? &g_camera : nullptr};
    if (!g_renderer.apply(ctx, target, view, g_look, options::current(), g_frame.toneMapped)) return;
    g_frame.placed = where;
    if (capture::active()) capture::onGraded(g_device, ctx, g_renderer.lastInput(), g_renderer.lastViewFormat(), view);
    // A picture for a preset you saved: the finished frame without the interface.
    tmshaders::Config& config = tmshaders::Config::get();
    std::string picture;
    if (where == Placed::BeforeHud && config.takePictureRequest(picture)) {
        thumbnails::savePicture(g_device, ctx, resourceOf(target), config.presetPicture(picture));
        config.pictureTaken();
    }
}

// Runs the mod's HDR stage in place of the engine's tone-map draw (same inputs, same target).
// True: the engine's draw is to be skipped.
bool replaceToneMap(ID3D11DeviceContext* ctx) {
    if (!g_lookSettings.enabled || !options::current().hdrStage || !g_rendererReady || !g_frame.rtv) return false;
    ID3D11ShaderResourceView* in[2] = {};
    ctx->PSGetShaderResources(0, 2, in);
    ID3D11Buffer* constants = nullptr;
    ctx->PSGetConstantBuffers(0, 1, &constants);
    EngineView view{g_tex, g_cameraThisFrame ? &g_camera : nullptr};
    view.time = nowSeconds();
    const bool done = g_renderer.toneMap(ctx, g_frame.rtv, in[0], in[1], constants, g_look, view);
    for (auto* v : in)
        if (v) v->Release();
    if (constants) constants->Release();
    return done;
}

// The engine's bloom is added onto its HDR image by FxBloom_Final (blended: one + one). With the
// mod's bloom on and "Turn off the game's FX" (DisableGameFX), that draw is skipped, so nothing
// glows twice. Only when it really adds (checked on its blend state): anything else stays.
bool skipEngineBloom(ID3D11DeviceContext* ctx, const char* name) {
    if (!name || strcmp(name, "Effects/FxBloom_Final") != 0) return false;
    const tmshaders::Settings& s = g_lookSettings;
    if (!s.enabled || !s.disableGameFx || s.bloom <= 0.001f || !options::current().hdrStage) return false;
    ID3D11BlendState* blend = nullptr;
    FLOAT factor[4];
    UINT mask = 0;
    ctx->OMGetBlendState(&blend, factor, &mask);
    bool additive = false;
    if (blend) {
        D3D11_BLEND_DESC d;
        blend->GetDesc(&d);
        const auto& rt = d.RenderTarget[0];
        additive = rt.BlendEnable && rt.BlendOp == D3D11_BLEND_OP_ADD && rt.DestBlend == D3D11_BLEND_ONE;
        blend->Release();
    }
    static int logged = 0;
    if (logged++ < 1)
        TMVS_LOG("hooks: the engine's bloom (FxBloom_Final) %s", additive ? "adds onto the HDR image: skipped while the mod's bloom is on"
                                                                         : "is not additive: left alone");
    return additive;
}

// Before a draw of the game on the main context. True: skip the game's draw.
bool onDraw(ID3D11DeviceContext* ctx) {
    Frame& f = g_frame;
    f.draws++;
    if (capture::active()) capture::traceDraw(ctx, f.ps.name, f.ps.role, f.draws);
    const Role role = f.ps.role;
    if (f.phase == Phase::Scene && role == Role::Post && skipEngineBloom(ctx, f.ps.name)) return true;

    if (role == Role::ToneMap && f.rt && screenSized(f.rtW, f.rtH)) {
        if (capture::active() && !f.toneMapSeen) capture::onToneMap(g_device, ctx);
        // The tone map reads the HDR image (t0) and the exposure (t1).
        ID3D11ShaderResourceView* in[2] = {};
        ctx->PSGetShaderResources(0, 2, in);
        for (int i = 0; i < 2; i++) {
            if (!in[i]) continue;
            ID3D11Resource* res = nullptr;
            in[i]->GetResource(&res);
            in[i]->Release();
            setTexture(i == 0 ? kTexHdr : kTexExposure, res);
            if (res) res->Release();
        }
        f.phase = Phase::Post;
        f.postImage = f.rt;
        f.postRtv = f.rtv;
        f.toneMapSeen = true;
        f.toneMapped = replaceToneMap(ctx);
        return f.toneMapped;
    }
    if (f.phase == Phase::Scene) {
        // The scene's depth: the last screen-shaped depth buffer a scene draw used.
        if (role == Role::Scene && f.ds && screenSized(f.dsW, f.dsH)) setSceneDepth(f.ds);
        // The lighting-read pass (it reads the normal and light buffers) runs with the main camera.
        if (f.ps.reads(kSlotNormal) || f.ps.reads(kSlotDiffuse)) {
            if (!g_cameraThisFrame) takeCamera(ctx);
            if (!f.texturesTaken && f.rt && screenSized(f.rtW, f.rtH)) {
                f.texturesTaken = true;
                takeSceneTextures(ctx, f.ps);
                // Its second target is the motion buffer (R16G16 float) that the motion blur reads.
                ID3D11RenderTargetView* rtv[2] = {};
                ctx->OMGetRenderTargets(2, rtv, nullptr);
                if (rtv[1]) {
                    D3D11_RENDER_TARGET_VIEW_DESC d;
                    rtv[1]->GetDesc(&d);
                    if (d.Format == DXGI_FORMAT_R16G16_FLOAT) setTexture(kTexMotion, resourceOf(rtv[1]));
                }
                for (auto* v : rtv)
                    if (v) v->Release();
            }
        }
        return false;
    }
    if (f.phase != Phase::Post) return false;
    if (role == Role::Post || role == Role::ToneMap) {
        if (f.rt && screenSized(f.rtW, f.rtH)) {
            f.postImage = f.rt;
            f.postRtv = f.rtv;
        }
        return false;
    }
    const bool full = f.rt && screenSized(f.rtW, f.rtH);
    if (full && f.rt != f.postImage) {
        // A draw that reads the post image into another full-size target (an upscale or copy to the
        // back buffer) carries it on.
        if (readsResource(ctx, f.postImage)) {
            f.postImage = f.rt;
            f.postRtv = f.rtv;
        }
        return false;
    }
    // The interface starts here: the first other draw on the finished image, or one that reads it
    // into a small target (the blurred backgrounds behind the interface's panels).
    if (!full && !readsResource(ctx, f.postImage)) return false; // other interface work (atlases, minimap)
    f.phase = Phase::Done;
    f.hudShader = f.ps.name ? f.ps.name : "(unknown shader)";
    if (options::current().placement == Placement::BeforeHud && !f.injected && f.postRtv) {
        f.injected = true;
        inject(ctx, f.postRtv, Placed::BeforeHud);
    }
    return false;
}

// ---- device ----

HRESULT STDMETHODCALLTYPE createPixelShaderDetour(ID3D11Device* device, const void* code, SIZE_T size,
                                                  ID3D11ClassLinkage* linkage, ID3D11PixelShader** out) {
    const HRESULT hr = g_createPixelShader.get<CreatePixelShaderFn>(device)(device, code, size, linkage, out);
    if (SUCCEEDED(hr) && out && *out && !t_inside) {
        const ShaderInfo info = shaderdb::identify(code, size);
        {
            std::unique_lock<std::shared_mutex> lock(g_shaderLock);
            g_shaders[*out] = info;
            g_stats.shadersCreated++;
            if (info.name) g_stats.shadersKnown++;
            if (!info.name && g_unknownLogged < 40 && size >= 20) {
                g_unknownLogged++;
                const auto* b = static_cast<const unsigned char*>(code);
                TMVS_LOG("shader: unknown pixel shader %02x%02x%02x%02x%02x%02x%02x%02x... (%zu bytes)", b[4], b[5], b[6],
                         b[7], b[8], b[9], b[10], b[11], static_cast<size_t>(size));
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE createDeferredContextDetour(ID3D11Device* device, UINT flags, ID3D11DeviceContext** out) {
    const HRESULT hr = g_createDeferredContext.get<CreateDeferredContextFn>(device)(device, flags, out);
    if (SUCCEEDED(hr)) {
        g_stats.deferredContexts++;
        TMVS_LOG("hooks: the game created a deferred context (%u so far)", g_stats.deferredContexts);
    }
    return hr;
}

// ---- context ----

void STDMETHODCALLTYPE psSetShaderDetour(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11ClassInstance* const* inst,
                                         UINT count) {
    if (isMain(ctx)) {
        repairDraws(ctx);
        ShaderInfo info;
        if (ps) {
            std::shared_lock<std::shared_mutex> lock(g_shaderLock);
            auto it = g_shaders.find(ps);
            if (it != g_shaders.end()) info = it->second;
        }
        g_frame.ps = info;
    }
    g_psSetShader.get<PSSetShaderFn>(ctx)(ctx, ps, inst, count);
}

void trackTargets(UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    Frame& f = g_frame;
    f.rtv = count > 0 && rtvs ? rtvs[0] : nullptr;
    f.rt = resourceOf(f.rtv);
    sizeOf(f.rt, f.rtW, f.rtH);
    f.ds = resourceOf(dsv);
    sizeOf(f.ds, f.dsW, f.dsH);
}

void STDMETHODCALLTYPE omSetRenderTargetsDetour(ID3D11DeviceContext* ctx, UINT count, ID3D11RenderTargetView* const* rtvs,
                                                ID3D11DepthStencilView* dsv) {
    if (isMain(ctx)) {
        repairDraws(ctx);
        trackTargets(count, rtvs, dsv);
    }
    g_omSetRenderTargets.get<OMSetRenderTargetsFn>(ctx)(ctx, count, rtvs, dsv);
}

void STDMETHODCALLTYPE omSetRenderTargetsAndUAVsDetour(ID3D11DeviceContext* ctx, UINT count,
                                                       ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
                                                       UINT uavStart, UINT uavCount, ID3D11UnorderedAccessView* const* uavs,
                                                       const UINT* initialCounts) {
    if (isMain(ctx) && count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) trackTargets(count, rtvs, dsv);
    g_omSetRenderTargetsAndUAVs.get<OMSetRenderTargetsAndUAVsFn>(ctx)(ctx, count, rtvs, dsv, uavStart, uavCount, uavs, initialCounts);
}

void STDMETHODCALLTYPE drawIndexedDetour(ID3D11DeviceContext* ctx, UINT a, UINT b, INT c) {
    if (isMain(ctx) && onDraw(ctx)) return;
    g_drawIndexed.get<DrawIndexedFn>(ctx)(ctx, a, b, c);
}

void STDMETHODCALLTYPE drawDetour(ID3D11DeviceContext* ctx, UINT a, UINT b) {
    if (isMain(ctx) && onDraw(ctx)) return;
    g_draw.get<DrawFn>(ctx)(ctx, a, b);
}

void STDMETHODCALLTYPE drawIndexedInstancedDetour(ID3D11DeviceContext* ctx, UINT a, UINT b, UINT c, INT d, UINT e) {
    if (isMain(ctx) && onDraw(ctx)) return;
    g_drawIndexedInstanced.get<DrawIndexedInstancedFn>(ctx)(ctx, a, b, c, d, e);
}

void STDMETHODCALLTYPE drawInstancedDetour(ID3D11DeviceContext* ctx, UINT a, UINT b, UINT c, UINT d) {
    if (isMain(ctx) && onDraw(ctx)) return;
    g_drawInstanced.get<DrawInstancedFn>(ctx)(ctx, a, b, c, d);
}

void STDMETHODCALLTYPE executeCommandListDetour(ID3D11DeviceContext* ctx, ID3D11CommandList* list, BOOL restore) {
    if (isMain(ctx)) g_frame.commandLists++;
    g_executeCommandList.get<ExecuteCommandListFn>(ctx)(ctx, list, restore);
}

HRESULT STDMETHODCALLTYPE mapDetour(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags,
                                    D3D11_MAPPED_SUBRESOURCE* out) {
    const HRESULT hr = g_map.get<MapFn>(ctx)(ctx, res, sub, type, flags, out);
    if (SUCCEEDED(hr) && out && isMain(ctx) && sub == 0 && (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE)) {
        if (const UINT size = sceneBufferSize(res)) {
            SceneVCopy* copy = sceneVCopy(res, true);
            copy->mapped = out->pData;
            copy->size = size;
        }
    }
    return hr;
}

void STDMETHODCALLTYPE unmapDetour(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub) {
    if (isMain(ctx) && sub == 0) {
        SceneVCopy* copy = sceneVCopy(res, false);
        if (copy && copy->mapped) {
            memcpy(copy->data, copy->mapped, copy->size);
            copy->mapped = nullptr;
            copy->filled = true;
        }
    }
    g_unmap.get<UnmapFn>(ctx)(ctx, res, sub);
}

void STDMETHODCALLTYPE updateSubresourceDetour(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub, const D3D11_BOX* box,
                                               const void* data, UINT rowPitch, UINT depthPitch) {
    if (isMain(ctx) && sub == 0 && !box && data) {
        if (const UINT size = sceneBufferSize(res)) {
            SceneVCopy* copy = sceneVCopy(res, true);
            memcpy(copy->data, data, size);
            copy->size = size;
            copy->filled = true;
        }
    }
    g_updateSubresource.get<UpdateSubresourceFn>(ctx)(ctx, res, sub, box, data, rowPitch, depthPitch);
}

// ---- swap chain ----

int installContextHooks(ID3D11DeviceContext* ctx) {
    int patched = 0;
    patched += g_psSetShader.install(ctx, kPSSetShader, reinterpret_cast<void*>(&psSetShaderDetour));
    patched += g_drawIndexed.install(ctx, kDrawIndexed, reinterpret_cast<void*>(&drawIndexedDetour));
    patched += g_draw.install(ctx, kDraw, reinterpret_cast<void*>(&drawDetour));
    patched += g_drawIndexedInstanced.install(ctx, kDrawIndexedInstanced, reinterpret_cast<void*>(&drawIndexedInstancedDetour));
    patched += g_drawInstanced.install(ctx, kDrawInstanced, reinterpret_cast<void*>(&drawInstancedDetour));
    patched += g_omSetRenderTargets.install(ctx, kOMSetRenderTargets, reinterpret_cast<void*>(&omSetRenderTargetsDetour));
    patched += g_omSetRenderTargetsAndUAVs.install(ctx, kOMSetRenderTargetsAndUAVs,
                                                   reinterpret_cast<void*>(&omSetRenderTargetsAndUAVsDetour));
    patched += g_executeCommandList.install(ctx, kExecuteCommandList, reinterpret_cast<void*>(&executeCommandListDetour));
    patched += g_map.install(ctx, kMap, reinterpret_cast<void*>(&mapDetour));
    patched += g_unmap.install(ctx, kUnmap, reinterpret_cast<void*>(&unmapDetour));
    patched += g_updateSubresource.install(ctx, kUpdateSubresource, reinterpret_cast<void*>(&updateSubresourceDetour));
    return patched;
}

// The runtime may rewrite the draw slots between two Presents too: checked before the game sets
// a pixel shader or render targets, which comes before its draws.
void repairDraws(ID3D11DeviceContext* ctx) {
    void** vt = *reinterpret_cast<void***>(ctx);
    if (vt[kDraw] == reinterpret_cast<void*>(&drawDetour) && vt[kDrawIndexed] == reinterpret_cast<void*>(&drawIndexedDetour) &&
        vt[kDrawInstanced] == reinterpret_cast<void*>(&drawInstancedDetour) &&
        vt[kDrawIndexedInstanced] == reinterpret_cast<void*>(&drawIndexedInstancedDetour)) {
        return;
    }
    const int patched = installContextHooks(ctx);
    static int logged = 0;
    if (patched && logged++ < 5) TMVS_LOG("hooks: %d context slots patched again during a frame", patched);
}

// Device and immediate-context hooks; also the repair after the runtime rewrote the vtables.
// Returns the number of slots that had to be patched.
int installDeviceHooks(ID3D11Device* device) {
    int patched = 0;
    patched += g_createPixelShader.install(device, kCreatePixelShader, reinterpret_cast<void*>(&createPixelShaderDetour));
    patched += g_createDeferredContext.install(device, kCreateDeferredContext, reinterpret_cast<void*>(&createDeferredContextDetour));
    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (ctx) {
        patched += installContextHooks(ctx);
        ctx->Release();
    }
    return patched;
}

void releaseBackBuffer() {
    if (g_backBufferRtv) g_backBufferRtv->Release();
    g_backBufferRtv = nullptr;
    g_backBuffer = nullptr;
}

bool ensureBackBuffer(IDXGISwapChain* sc) {
    if (g_backBufferRtv) return true;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return false;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    g_stats.width = d.Width;
    g_stats.height = d.Height;
    g_backBuffer = bb;
    const HRESULT hr = g_device->CreateRenderTargetView(bb, nullptr, &g_backBufferRtv);
    bb->Release();
    TMVS_LOG("hooks: back buffer %ux%u format %d%s", d.Width, d.Height, d.Format, SUCCEEDED(hr) ? "" : " (no view)");
    return SUCCEEDED(hr);
}

void beginWith(IDXGISwapChain* sc) {
    ID3D11Device* device = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) return;
    g_swapChain = sc;
    g_device = device; // keep the reference: the device outlives us in practice
    device->GetImmediateContext(&g_ctx);
    g_ctx->Release(); // the device holds it
    DXGI_SWAP_CHAIN_DESC desc;
    sc->GetDesc(&desc);
    {
        Inside guard; // the mod's own shaders aren't the game's
        g_rendererReady = g_renderer.init(device);
        prepareLook();
        overlay::init(desc.OutputWindow, device, g_ctx);
    }
    TMVS_LOG("hooks: following swap chain %p (window %p, %ux%u), renderer %s", static_cast<void*>(sc),
             static_cast<void*>(desc.OutputWindow), desc.BufferDesc.Width, desc.BufferDesc.Height,
             g_rendererReady ? "ready" : "FAILED");
}

// The look of the next frame: your settings, or the preset the menu points at.
void prepareLook() {
    tmshaders::Config& config = tmshaders::Config::get();
    g_lookSettings = config.settings;
    if (config.showOverlay && config.hoverPreview) config.previewSettings(g_lookSettings);
    g_autoQuality.apply(g_lookSettings);
    g_look.settings = &g_lookSettings;
    g_look.lut = config.lutFile;
    g_look.lutAmount = config.lutAmount;
    if (g_cameraThisFrame) {
        // The engine's light colour, brightness taken out (split toning only wants its hue).
        const float* c = g_camera.sunColour;
        const float peak = std::max(c[0], std::max(c[1], c[2]));
        if (peak > 1e-3f)
            for (int i = 0; i < 3; i++) g_look.sun[i] = c[i] / peak;
    }
    g_look.frame = ++g_frameCounter;
    const Options& o = options::current();
    g_look.split = o.view == View::Split ? o.split : 0.0f;
}

void onPresent(IDXGISwapChain* sc) {
    if (!g_swapChain) beginWith(sc);
    if (sc != g_swapChain || !g_ctx) return;
    if (const int patched = installDeviceHooks(g_device)) {
        static int logged = 0;
        if (logged++ < 10) TMVS_LOG("hooks: %d device/context slots patched again (the runtime rewrote its vtables)", patched);
    }
    ensureBackBuffer(sc);

    Frame& f = g_frame;
    const Options& o = options::current();
    if (!f.injected && g_lookSettings.enabled && g_backBufferRtv) {
        Placed where = Placed::None;
        if (o.placement == Placement::EndOfFrame) where = Placed::EndOfFrame;
        else if (o.fallbackAtEnd && f.toneMapSeen) where = Placed::Fallback;
        if (where != Placed::None) {
            f.injected = true;
            inject(g_ctx, g_backBufferRtv, where);
        }
    }

    g_stats.toneMapSeen = f.toneMapSeen;
    g_stats.placed = f.placed;
    g_stats.hudShader = f.hudShader;
    g_stats.toneMapped = f.toneMapped;
    g_stats.effectsRan = f.toneMapped && g_renderer.effectsRan();
    g_stats.sunFromLighting = g_camera.sunFromSceneP;
    memcpy(g_stats.sun, g_camera.sunDirection, sizeof(g_stats.sun));
    g_stats.drawsLastFrame = f.draws;
    g_stats.commandLists = f.commandLists;
    g_stats.depthFound = g_renderer.hasDepthView();
    g_stats.depthFormat = g_renderer.depthFormat();
    g_stats.cameraFound = g_cameraThisFrame;
    g_stats.textures = 0;
    for (int i = 0; i < kTexCount; i++)
        if (g_tex[i]) g_stats.textures |= 1u << i;
    if (g_cameraThisFrame) memcpy(g_stats.eye, g_camera.eye, sizeof(g_stats.eye));
    if (g_rendererReady)
        g_autoQuality.onFrame(nowSeconds(), g_stats.effectsRan, g_renderer.effects().profiler(), tmshaders::Config::get().settings);

    {
        Inside guard;
        overlay::frame(g_backBufferRtv);
    }
    // The frame as it is shown (with the menu when it's open).
    if (capture::active()) capture::onPresent(g_device, g_ctx, g_backBuffer);
    capture::endFrame();

    // The map's time of day from the engine's light (day, sunset, night presets, as in TMNF):
    // only in frames with a scene, and only once it holds for a second.
    if (g_cameraThisFrame && f.toneMapSeen) {
        const tmshaders::Mood mood = tmshaders::classifyMood(g_camera.sunColour);
        g_moodFrames = mood == g_seenMood ? g_moodFrames + 1 : 0;
        g_seenMood = mood;
        if (g_moodFrames == 60) tmshaders::Config::get().onMoodDetected(mood);
    }
    prepareLook();

    // Next frame. The bound targets stay as the game left them.
    g_cameraThisFrame = false;
    const ShaderInfo ps = f.ps;
    ID3D11RenderTargetView* rtv = f.rtv;
    ID3D11Resource *rt = f.rt, *ds = f.ds;
    const UINT rtW = f.rtW, rtH = f.rtH, dsW = f.dsW, dsH = f.dsH;
    f = Frame{};
    f.ps = ps;
    f.rtv = rtv;
    f.rt = rt;
    f.ds = ds;
    f.rtW = rtW;
    f.rtH = rtH;
    f.dsW = dsW;
    f.dsH = dsH;
}

// The runtime also writes the draw slots back while presenting, so they are patched again after it.
void afterPresent(IDXGISwapChain* sc) {
    if (sc == g_swapChain && g_device) installDeviceHooks(g_device);
}

HRESULT STDMETHODCALLTYPE presentDetour(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) onPresent(sc);
    const HRESULT hr = g_present.get<PresentFn>(sc)(sc, sync, flags);
    afterPresent(sc);
    return hr;
}

HRESULT STDMETHODCALLTYPE present1Detour(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (!(flags & DXGI_PRESENT_TEST)) onPresent(sc);
    const HRESULT hr = g_present1.get<Present1Fn>(sc)(sc, sync, flags, p);
    afterPresent(sc);
    return hr;
}

HRESULT STDMETHODCALLTYPE resizeBuffersDetour(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags) {
    if (sc == g_swapChain) {
        releaseBackBuffer();
        g_renderer.releaseTargets();
        overlay::releaseTargets();
        releaseTextures();
        g_isSceneV.clear();
        g_frame = Frame{};
        TMVS_LOG("hooks: ResizeBuffers %ux%u", w, h);
    }
    const HRESULT hr = g_resizeBuffers.get<ResizeBuffersFn>(sc)(sc, count, w, h, format, flags);
    if (sc == g_swapChain && FAILED(hr)) TMVS_LOG("hooks: ResizeBuffers failed 0x%08lx", static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE createSwapChainDetour(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                                IDXGISwapChain** out) {
    const HRESULT hr = g_createSwapChain.get<CreateSwapChainFn>(factory)(factory, device, desc, out);
    if (SUCCEEDED(hr) && out && *out) onSwapChain(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE createSwapChainForHwndDetour(IDXGIFactory2* factory, IUnknown* device, HWND hwnd,
                                                       const DXGI_SWAP_CHAIN_DESC1* desc,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* output,
                                                       IDXGISwapChain1** out) {
    const HRESULT hr = g_createSwapChainForHwnd.get<CreateSwapChainForHwndFn>(factory)(factory, device, hwnd, desc, fs, output, out);
    if (SUCCEEDED(hr) && out && *out) onSwapChain(*out);
    return hr;
}

void hookFactory(ID3D11Device* device) {
    // The factory the device was made with: the game's own factory is of the same class, so
    // patching this vtable catches its CreateSwapChain too.
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory)))) {
        g_createSwapChain.install(factory, kCreateSwapChain, reinterpret_cast<void*>(&createSwapChainDetour));
        IDXGIFactory2* factory2 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2)))) {
            g_createSwapChainForHwnd.install(factory2, kCreateSwapChainForHwnd, reinterpret_cast<void*>(&createSwapChainForHwndDetour));
            factory2->Release();
        }
    }
    if (factory) factory->Release();
    if (adapter) adapter->Release();
    if (dxgiDevice) dxgiDevice->Release();
}

} // namespace

Inside::Inside() {
    t_inside++;
}

Inside::~Inside() {
    t_inside--;
}

bool inside() {
    return t_inside != 0;
}

const Stats& stats() {
    return g_stats;
}

Effects* effects() {
    return g_rendererReady ? &g_renderer.effects() : nullptr;
}

const char* autoQualityReduced() {
    return g_autoQuality.reducedCount() ? g_autoQuality.summary() : nullptr;
}

void onDevice(ID3D11Device* device) {
    if (!device) return;
    installDeviceHooks(device);
    hookFactory(device);
    TMVS_LOG("hooks: device %p hooked (feature level 0x%x)", static_cast<void*>(device), device->GetFeatureLevel());
}

void onSwapChain(IDXGISwapChain* swapChain) {
    if (!swapChain) return;
    g_present.install(swapChain, kPresent, reinterpret_cast<void*>(&presentDetour));
    g_resizeBuffers.install(swapChain, kResizeBuffers, reinterpret_cast<void*>(&resizeBuffersDetour));
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1)))) {
        g_present1.install(sc1, kPresent1, reinterpret_cast<void*>(&present1Detour));
        sc1->Release();
    }
    TMVS_LOG("hooks: swap chain %p hooked", static_cast<void*>(swapChain));
}

} // namespace hooks
} // namespace tmvs_mp
