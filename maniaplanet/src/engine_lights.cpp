#include "engine_lights.h"
#include "log.h"
#include <cstring>

namespace tmvs_mp {
namespace lights {
namespace {

template <typename T> void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

struct State {
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    UINT w = 0, h = 0;
    ID3D11ShaderResourceView* black = nullptr; // 1x1, all zero: the material's light texture, off
    ID3D11Resource* hdr = nullptr;              // this frame's HDR image (identity only)
    bool written = false;                       // a light was drawn this frame
    unsigned counts[4] = {};
    int framesLogged = 0;
    // Blend states by (enable, alpha to coverage, source, destination, operation).
    struct Blend {
        uint32_t key;
        ID3D11BlendState* state;
    };
    Blend blends[24] = {};
    int blendCount = 0;
    // The redraws' depth-stencil states, by the engine's state they come from (its description).
    struct Depth {
        D3D11_DEPTH_STENCIL_DESC desc;
        ID3D11DepthStencilState* state;
    };
    Depth depths[16] = {};
    int depthCount = 0;
};
State g;

bool isFloat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R32G32B32A32_FLOAT;
}

// The sky's own draws (stars, moon, sky dome, clouds) light no surface: the lighting pass leaves
// the sky to the sky effects.
bool isSky(const char* name) {
    return name && (!strncmp(name, "Clouds/", 7) || strstr(name, "Sky") || strstr(name, "Stars") || strstr(name, "Moon"));
}

// A source factor that scales the source by the image under it: the share it leaves of the light
// already there is its own business (glass tints the lamp behind it).
bool scalesByDest(D3D11_BLEND b) {
    return b == D3D11_BLEND_DEST_COLOR || b == D3D11_BLEND_DEST_ALPHA;
}

bool ensureTarget(ID3D11Device* device, UINT w, UINT h) {
    if (g.tex && g.w == w && g.h == h) return true;
    safeRelease(g.rtv);
    safeRelease(g.tex);
    g.w = g.h = 0;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &g.tex)) || FAILED(device->CreateRenderTargetView(g.tex, nullptr, &g.rtv))) {
        TMVS_LOG("lights: a %ux%u float target failed", w, h);
        safeRelease(g.rtv);
        safeRelease(g.tex);
        return false;
    }
    if (!g.black) {
        D3D11_TEXTURE2D_DESC b = d;
        b.Width = b.Height = 1;
        b.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        b.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const uint32_t zero = 0;
        D3D11_SUBRESOURCE_DATA init = {&zero, 4, 0};
        ID3D11Texture2D* t = nullptr;
        if (SUCCEEDED(device->CreateTexture2D(&b, &init, &t))) {
            device->CreateShaderResourceView(t, nullptr, &g.black);
            t->Release();
        }
    }
    g.w = w;
    g.h = h;
    TMVS_LOG("lights: the engine's light draws go into a %ux%u float target", w, h);
    return true;
}

ID3D11BlendState* blendState(ID3D11Device* device, bool enable, BOOL a2c, D3D11_BLEND src, D3D11_BLEND dest, D3D11_BLEND_OP op) {
    const uint32_t key = (enable ? 1u : 0u) | (a2c ? 2u : 0u) | (static_cast<uint32_t>(src) << 2) | (static_cast<uint32_t>(dest) << 8) |
                         (static_cast<uint32_t>(op) << 14);
    for (int i = 0; i < g.blendCount; i++)
        if (g.blends[i].key == key) return g.blends[i].state;
    if (g.blendCount == 24) return nullptr;
    D3D11_BLEND_DESC d = {};
    d.AlphaToCoverageEnable = a2c;
    D3D11_RENDER_TARGET_BLEND_DESC& rt = d.RenderTarget[0];
    rt.BlendEnable = enable;
    rt.SrcBlend = src;
    rt.DestBlend = dest;
    rt.BlendOp = op;
    // The alpha channel stays as cleared: only the light's colour is kept.
    rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
    rt.DestBlendAlpha = D3D11_BLEND_ONE;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    ID3D11BlendState* state = nullptr;
    if (FAILED(device->CreateBlendState(&d, &state))) return nullptr;
    g.blends[g.blendCount++] = {key, state};
    return state;
}

// The engine's depth-stencil state for its draw's redraws. The engine's draw has just written its
// depth (and its stencil): a strict test (less, greater) would fail the very same draw at the very
// same depth, and its stencil operations would apply two or three times. So the redraws test with
// equal depths let through and write neither depth nor stencil. ManiaPlanet's light draws test
// greater-or-equal or equal (capture 034, reversed depth): those stay as they are but for the
// writes. Null: the engine's state can't be had, the redraws keep it.
ID3D11DepthStencilState* redrawDepth(ID3D11Device* device, ID3D11DepthStencilState* engine) {
    D3D11_DEPTH_STENCIL_DESC d = {};
    if (engine) {
        engine->GetDesc(&d);
    } else {
        // D3D11's default state: depth on, written, less; no stencil.
        d.DepthEnable = TRUE;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        d.DepthFunc = D3D11_COMPARISON_LESS;
        d.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
        d.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
        const D3D11_DEPTH_STENCILOP_DESC keep = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                                 D3D11_COMPARISON_ALWAYS};
        d.FrontFace = d.BackFace = keep;
    }
    // Field by field: the description has padding after the stencil masks.
    auto sameOp = [](const D3D11_DEPTH_STENCILOP_DESC& a, const D3D11_DEPTH_STENCILOP_DESC& b) {
        return a.StencilFailOp == b.StencilFailOp && a.StencilDepthFailOp == b.StencilDepthFailOp &&
               a.StencilPassOp == b.StencilPassOp && a.StencilFunc == b.StencilFunc;
    };
    for (int i = 0; i < g.depthCount; i++) {
        const D3D11_DEPTH_STENCIL_DESC& c = g.depths[i].desc;
        if (c.DepthEnable == d.DepthEnable && c.DepthWriteMask == d.DepthWriteMask && c.DepthFunc == d.DepthFunc &&
            c.StencilEnable == d.StencilEnable && c.StencilReadMask == d.StencilReadMask && c.StencilWriteMask == d.StencilWriteMask &&
            sameOp(c.FrontFace, d.FrontFace) && sameOp(c.BackFace, d.BackFace))
            return g.depths[i].state;
    }
    if (g.depthCount == 16) return nullptr;
    D3D11_DEPTH_STENCIL_DESC r = d;
    if (r.DepthFunc == D3D11_COMPARISON_LESS) r.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (r.DepthFunc == D3D11_COMPARISON_GREATER) r.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    r.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    r.StencilWriteMask = 0;
    ID3D11DepthStencilState* state = nullptr;
    if (FAILED(device->CreateDepthStencilState(&r, &state))) return nullptr;
    g.depths[g.depthCount++] = {d, state};
    TMVS_LOG("lights: redraws of depth test %d (write %d, stencil %d) test %d, write nothing", d.DepthFunc, d.DepthWriteMask,
             d.StencilEnable, r.DepthFunc);
    return state;
}

} // namespace

Draw classify(ID3D11Device* device, ID3D11DeviceContext* ctx, const ShaderInfo& ps, ID3D11RenderTargetView* rtv,
              ID3D11Resource* rt, bool screenSized) {
    Draw d;
    if (!rt || !rtv) return d;
    if (!g.hdr) {
        // The lighting pass: it reads the light mask, into the HDR image (CarDetails_DefRead,
        // Block_DefReadP1 ...). The shadow and light-mask passes before it don't write a float image.
        if (!ps.reads(kSlotLightMask) || !screenSized) return d;
        D3D11_RENDER_TARGET_VIEW_DESC vd;
        rtv->GetDesc(&vd);
        if (!isFloat(vd.Format) || vd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D) return d;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(rt->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) return d;
        D3D11_TEXTURE2D_DESC td;
        tex->GetDesc(&td);
        tex->Release();
        if (td.SampleDesc.Count > 1 || !ensureTarget(device, td.Width, td.Height)) return d;
        const float zero[4] = {};
        ctx->ClearRenderTargetView(g.rtv, zero);
        g.hdr = rt;
        g.written = false;
    }
    if (rt != g.hdr || isSky(ps.name)) return d;

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11BlendState* bs = nullptr;
    FLOAT factor[4];
    UINT mask = 0;
    ctx->OMGetBlendState(&bs, factor, &mask);
    if (bs) {
        bs->GetDesc(&bd);
        bs->Release();
    }
    const D3D11_RENDER_TARGET_BLEND_DESC& b = bd.RenderTarget[0];
    if (!(b.RenderTargetWriteMask & 7)) return d; // depth only
    d.blend = b.BlendEnable != FALSE;
    d.src = b.SrcBlend;
    d.dest = b.DestBlend;
    d.alphaToCoverage = bd.AlphaToCoverageEnable;
    d.lightSlot = ps.lightSlot;
    const bool add = !d.blend || b.BlendOp == D3D11_BLEND_OP_ADD;
    const bool additive = d.blend && b.BlendOp == D3D11_BLEND_OP_ADD && b.DestBlend == D3D11_BLEND_ONE &&
                          (b.SrcBlend == D3D11_BLEND_ONE || b.SrcBlend == D3D11_BLEND_SRC_ALPHA);
    // An additive draw adds light, whatever it reads: once is enough (Tech3_Block_TAdd has a
    // TMapSelfIllum too; its second, black draw added nothing: captures 031/032).
    if (additive) {
        d.kind = Kind::Added;
    } else if (ps.lightSlot != kNoSlot && add && !scalesByDest(d.src)) {
        d.kind = Kind::Light;
    } else if (!g.written || !add) {
        return d;
    } else if (d.blend) {
        d.kind = Kind::Cover;
    } else {
        // Opaque: it covers what is under it when it writes its depth. The lighting pass's draws
        // (depth equal to the G-buffer's) can't be in front of each other: left out.
        ID3D11DepthStencilState* ds = nullptr;
        UINT ref = 0;
        ctx->OMGetDepthStencilState(&ds, &ref);
        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_LESS;
        if (ds) {
            ds->GetDesc(&dd);
            ds->Release();
        }
        if (!dd.DepthEnable || dd.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ALL || dd.DepthFunc == D3D11_COMPARISON_EQUAL) return d;
        d.kind = Kind::Cover;
    }
    if (d.kind != Kind::Cover) g.written = true;
    g.counts[static_cast<int>(d.kind)]++;
    return d;
}

void save(ID3D11DeviceContext* ctx, Saved& s) {
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtvs, &s.dsv);
    s.rtvCount = 0;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++)
        if (s.rtvs[i]) s.rtvCount = i + 1;
    ctx->OMGetBlendState(&s.blend, s.factor, &s.sampleMask);
    ctx->OMGetDepthStencilState(&s.depth, &s.stencilRef);
}

bool setup(ID3D11DeviceContext* ctx, const Draw& d, int pass, Saved& s) {
    if (!g.rtv) return false;
    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);
    if (!device) return false;
    ID3D11BlendState* state = nullptr;
    ID3D11DepthStencilState* depth = redrawDepth(device, s.depth);
    bool more = true;
    switch (d.kind) {
    case Kind::Light:
        if (pass == 0) {
            // As the engine draws it, into the buffer.
            state = blendState(device, d.blend, d.alphaToCoverage, d.src, d.dest, D3D11_BLEND_OP_ADD);
        } else if (pass == 1 && g.black && d.lightSlot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
            // Again with its light texture black, subtracted: its lit surface goes, its light stays.
            ctx->PSGetShaderResources(d.lightSlot, 1, &s.light);
            s.lightSlot = d.lightSlot;
            s.lightSwapped = true;
            ctx->PSSetShaderResources(d.lightSlot, 1, &g.black);
            state = blendState(device, true, d.alphaToCoverage, d.blend ? d.src : D3D11_BLEND_ONE, D3D11_BLEND_ONE,
                               D3D11_BLEND_OP_REV_SUBTRACT);
        } else {
            more = false;
        }
        break;
    case Kind::Added:
        if (pass == 0) state = blendState(device, true, d.alphaToCoverage, d.src, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD);
        else more = false;
        break;
    case Kind::Cover:
        // Its own colour adds no light: a black source, the engine's share of what is under it.
        if (pass == 0)
            state = d.blend ? blendState(device, true, d.alphaToCoverage, scalesByDest(d.src) ? d.src : D3D11_BLEND_ZERO, d.dest,
                                         D3D11_BLEND_OP_ADD)
                            : blendState(device, true, d.alphaToCoverage, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD);
        else more = false;
        break;
    default:
        more = false;
    }
    device->Release();
    if (!more || !state) return false;
    ctx->OMSetRenderTargets(1, &g.rtv, s.dsv);
    // A binding the runtime refused (a depth buffer of another size) would leave the engine's
    // image bound: drawn twice into it. Only into the buffer.
    ID3D11RenderTargetView* bound = nullptr;
    ctx->OMGetRenderTargets(1, &bound, nullptr);
    const bool ours = bound == g.rtv;
    safeRelease(bound);
    if (!ours) {
        static int logged = 0;
        if (logged++ < 3) TMVS_LOG("lights: the light buffer could not be bound with the engine's depth buffer: redraw left out");
        return false;
    }
    ctx->OMSetBlendState(state, s.factor, s.sampleMask);
    if (depth) ctx->OMSetDepthStencilState(depth, s.stencilRef);
    return true;
}

void restore(ID3D11DeviceContext* ctx, Saved& s) {
    if (s.lightSwapped) {
        ctx->PSSetShaderResources(s.lightSlot, 1, &s.light);
        safeRelease(s.light);
        s.lightSwapped = false;
    }
    ctx->OMSetRenderTargets(s.rtvCount, s.rtvs, s.dsv);
    ctx->OMSetBlendState(s.blend, s.factor, s.sampleMask);
    ctx->OMSetDepthStencilState(s.depth, s.stencilRef);
    for (auto*& v : s.rtvs) safeRelease(v);
    safeRelease(s.dsv);
    safeRelease(s.blend);
    safeRelease(s.depth);
}

ID3D11Resource* frameTexture(ID3D11Resource* hdr) {
    if (g.hdr && hdr != g.hdr) {
        static int logged = 0;
        if (logged++ < 3) TMVS_LOG("lights: the tone map reads another image than the lighting pass drew into: no light buffer");
    }
    return hdr && hdr == g.hdr ? g.tex : nullptr;
}

const char* kindName(Kind k) {
    switch (k) {
    case Kind::Light: return "light: its own light into the light buffer";
    case Kind::Added: return "light: added light into the light buffer";
    case Kind::Cover: return "light: covers the light buffer";
    default: return "";
    }
}

void endFrame() {
    if (g.hdr && g.framesLogged < 3 && (g.counts[1] || g.counts[2])) {
        g.framesLogged++;
        TMVS_LOG("lights: %u self-illuminated draws, %u additive, %u covering", g.counts[1], g.counts[2], g.counts[3]);
    }
    memset(g.counts, 0, sizeof(g.counts));
    g.hdr = nullptr;
    g.written = false;
}

void release() {
    safeRelease(g.rtv);
    safeRelease(g.tex);
    g.w = g.h = 0;
    g.hdr = nullptr;
    g.written = false;
}

} // namespace lights
} // namespace tmvs_mp
