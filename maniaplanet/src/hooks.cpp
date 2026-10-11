// D3D11/DXGI vtable hooks and the frame logic: which draw is the engine's tone map, where its post
// effects end and its interface begins, and the scene's depth buffer.
//
// The engine's pixel shaders are known by their DXBC checksum (shaderdb), so a frame reads like:
//   scene draws (G-buffer, lights, particles; the main depth buffer bound)
//   -> tone map (HDR -> 8-bit) -> post effects (FXAA, colour grading, bloom ...)
//   -> the first other draw on that image: the interface starts here, the mod's pass goes in front.
// When the interface start can't be found, the pass runs at Present (interface graded too).
#include "hooks.h"
#include "capture.h"
#include "log.h"
#include "overlay.h"
#include "renderer.h"
#include "settings.h"
#include "shaderdb.h"
#include <windows.h>
#include <dxgi1_2.h>
#include <mutex>
#include <shared_mutex>
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
    kDrawIndexedInstanced = 20,
    kDrawInstanced = 21,
    kOMSetRenderTargets = 33,
    kOMSetRenderTargetsAndUAVs = 34,
    kExecuteCommandList = 58
};
enum SwapSlot { kPresent = 8, kResizeBuffers = 13, kPresent1 = 22 };
enum FactorySlot { kCreateSwapChain = 10, kCreateSwapChainForHwnd = 15 };

thread_local int t_inside = 0;

// One hooked vtable slot. Objects of different classes (a debug-layer device, another runtime's
// swap chain) have their own vtables, so the original is kept per vtable.
struct Hook {
    void** vtables[4] = {};
    void* originals[4] = {};
    int count = 0;

    void install(void* object, int slot, void* detour) {
        void** vtable = *reinterpret_cast<void***>(object);
        for (int i = 0; i < count; i++)
            if (vtables[i] == vtable) return;
        if (count == 4 || vtable[slot] == detour) return;
        DWORD old = 0;
        if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
        originals[count] = vtable[slot];
        vtable[slot] = detour;
        VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
        vtables[count++] = vtable;
    }

    template <typename F> F get(const void* object) const {
        void** vtable = *reinterpret_cast<void** const*>(object);
        for (int i = 1; i < count; i++)
            if (vtables[i] == vtable) return reinterpret_cast<F>(originals[i]);
        return reinterpret_cast<F>(originals[0]);
    }
};

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
    bool toneMapSeen = false;
    bool injected = false;
    Placed placed = Placed::None;
    const char* hudShader = nullptr;
    uint32_t draws = 0;
    uint32_t commandLists = 0;
};
Frame g_frame;
ID3D11Resource* g_sceneDepth = nullptr; // owned reference

void setSceneDepth(ID3D11Resource* depth) {
    if (depth == g_sceneDepth) return;
    if (g_sceneDepth) g_sceneDepth->Release();
    g_sceneDepth = depth;
    if (g_sceneDepth) g_sceneDepth->AddRef();
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
    const Settings& s = settings::current();
    if (!s.enabled || !g_rendererReady) return;
    if (g_renderer.apply(ctx, target, g_sceneDepth, s)) {
        g_frame.placed = where;
        if (capture::active()) capture::onGraded(g_device, ctx, g_renderer.lastInput(), g_sceneDepth);
    }
}

void onDraw(ID3D11DeviceContext* ctx) {
    Frame& f = g_frame;
    f.draws++;
    if (capture::active()) capture::traceDraw(ctx, f.ps.name, f.ps.role, f.draws);
    const Role role = f.ps.role;

    if (role == Role::ToneMap && f.rt && screenSized(f.rtW, f.rtH)) {
        if (capture::active() && !f.toneMapSeen) capture::onToneMap(g_device, ctx);
        f.phase = Phase::Post;
        f.postImage = f.rt;
        f.toneMapSeen = true;
        return;
    }
    if (f.phase == Phase::Scene) {
        // The scene's depth: the last screen-shaped depth buffer a scene draw used.
        if (role == Role::Scene && f.ds && screenSized(f.dsW, f.dsH)) setSceneDepth(f.ds);
        return;
    }
    if (f.phase != Phase::Post) return;
    if (role == Role::Post || role == Role::ToneMap) {
        if (f.rt && screenSized(f.rtW, f.rtH)) f.postImage = f.rt;
        return;
    }
    if (!f.rt || !screenSized(f.rtW, f.rtH)) return; // interface work off screen (atlases, minimap)
    if (f.rt != f.postImage) {
        // A draw that reads the post image into another target (an upscale or copy to the back
        // buffer) carries it on.
        if (readsResource(ctx, f.postImage)) f.postImage = f.rt;
        return;
    }
    // The first draw of something else on the finished image: the interface starts here.
    f.phase = Phase::Done;
    f.hudShader = f.ps.name ? f.ps.name : "(unknown shader)";
    if (settings::current().placement == Placement::BeforeHud && !f.injected) {
        f.injected = true;
        inject(ctx, f.rtv, Placed::BeforeHud);
    }
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
    if (isMain(ctx)) trackTargets(count, rtvs, dsv);
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
    if (isMain(ctx)) onDraw(ctx);
    g_drawIndexed.get<DrawIndexedFn>(ctx)(ctx, a, b, c);
}

void STDMETHODCALLTYPE drawDetour(ID3D11DeviceContext* ctx, UINT a, UINT b) {
    if (isMain(ctx)) onDraw(ctx);
    g_draw.get<DrawFn>(ctx)(ctx, a, b);
}

void STDMETHODCALLTYPE drawIndexedInstancedDetour(ID3D11DeviceContext* ctx, UINT a, UINT b, UINT c, INT d, UINT e) {
    if (isMain(ctx)) onDraw(ctx);
    g_drawIndexedInstanced.get<DrawIndexedInstancedFn>(ctx)(ctx, a, b, c, d, e);
}

void STDMETHODCALLTYPE drawInstancedDetour(ID3D11DeviceContext* ctx, UINT a, UINT b, UINT c, UINT d) {
    if (isMain(ctx)) onDraw(ctx);
    g_drawInstanced.get<DrawInstancedFn>(ctx)(ctx, a, b, c, d);
}

void STDMETHODCALLTYPE executeCommandListDetour(ID3D11DeviceContext* ctx, ID3D11CommandList* list, BOOL restore) {
    if (isMain(ctx)) g_frame.commandLists++;
    g_executeCommandList.get<ExecuteCommandListFn>(ctx)(ctx, list, restore);
}

// ---- swap chain ----

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
        overlay::init(desc.OutputWindow, device, g_ctx);
    }
    TMVS_LOG("hooks: following swap chain %p (window %p, %ux%u), renderer %s", static_cast<void*>(sc),
             static_cast<void*>(desc.OutputWindow), desc.BufferDesc.Width, desc.BufferDesc.Height,
             g_rendererReady ? "ready" : "FAILED");
}

void onPresent(IDXGISwapChain* sc) {
    if (!g_swapChain) beginWith(sc);
    if (sc != g_swapChain || !g_ctx) return;
    ensureBackBuffer(sc);

    Frame& f = g_frame;
    const Settings& s = settings::current();
    if (!f.injected && s.enabled && g_backBufferRtv) {
        Placed where = Placed::None;
        if (s.placement == Placement::EndOfFrame) where = Placed::EndOfFrame;
        else if (s.fallbackAtEnd && f.toneMapSeen) where = Placed::Fallback;
        if (where != Placed::None) {
            f.injected = true;
            inject(g_ctx, g_backBufferRtv, where);
        }
    }
    if (capture::active()) capture::onPresent(g_device, g_ctx, g_backBuffer);

    g_stats.toneMapSeen = f.toneMapSeen;
    g_stats.placed = f.placed;
    g_stats.hudShader = f.hudShader;
    g_stats.drawsLastFrame = f.draws;
    g_stats.commandLists = f.commandLists;
    g_stats.depthFound = g_renderer.hasDepthView();
    g_stats.depthFormat = g_renderer.depthFormat();

    {
        Inside guard;
        overlay::frame(g_backBufferRtv);
    }
    capture::endFrame();

    // Next frame. The bound targets stay as the game left them.
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

HRESULT STDMETHODCALLTYPE presentDetour(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) onPresent(sc);
    return g_present.get<PresentFn>(sc)(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE present1Detour(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (!(flags & DXGI_PRESENT_TEST)) onPresent(sc);
    return g_present1.get<Present1Fn>(sc)(sc, sync, flags, p);
}

HRESULT STDMETHODCALLTYPE resizeBuffersDetour(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags) {
    if (sc == g_swapChain) {
        releaseBackBuffer();
        g_renderer.releaseTargets();
        setSceneDepth(nullptr);
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

void onDevice(ID3D11Device* device) {
    if (!device) return;
    g_createPixelShader.install(device, kCreatePixelShader, reinterpret_cast<void*>(&createPixelShaderDetour));
    g_createDeferredContext.install(device, kCreateDeferredContext, reinterpret_cast<void*>(&createDeferredContextDetour));
    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (ctx) {
        g_psSetShader.install(ctx, kPSSetShader, reinterpret_cast<void*>(&psSetShaderDetour));
        g_drawIndexed.install(ctx, kDrawIndexed, reinterpret_cast<void*>(&drawIndexedDetour));
        g_draw.install(ctx, kDraw, reinterpret_cast<void*>(&drawDetour));
        g_drawIndexedInstanced.install(ctx, kDrawIndexedInstanced, reinterpret_cast<void*>(&drawIndexedInstancedDetour));
        g_drawInstanced.install(ctx, kDrawInstanced, reinterpret_cast<void*>(&drawInstancedDetour));
        g_omSetRenderTargets.install(ctx, kOMSetRenderTargets, reinterpret_cast<void*>(&omSetRenderTargetsDetour));
        g_omSetRenderTargetsAndUAVs.install(ctx, kOMSetRenderTargetsAndUAVs, reinterpret_cast<void*>(&omSetRenderTargetsAndUAVsDetour));
        g_executeCommandList.install(ctx, kExecuteCommandList, reinterpret_cast<void*>(&executeCommandListDetour));
        ctx->Release();
    }
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
