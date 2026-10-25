#include "water11.h"
#include "engine_frame.h"
#include "hooks.h"
#include "log.h"
#include "renderer.h"
#include "waterfx.h"
#include "shaders/PS_WaterCompose.h"
#include "shaders/VS_WaterFull.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace tmvs_mp {
namespace water {
namespace {

template <typename T> void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

struct State {
    ID3D11Device* device = nullptr;
    bool tried = false, ready = false;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11BlendState* accumulate = nullptr; // the fog draws into the buffer: premultiplied over, alpha too
    ID3D11BlendState* dual = nullptr;       // the compose: image * src1 + src0
    ID3D11DepthStencilState* noDepth = nullptr;
    ID3D11RasterizerState* rs = nullptr;
    ID3D11SamplerState* wrap = nullptr;
    ID3D11Buffer* params = nullptr;
    ID3D11Texture2D* pictures = nullptr;
    ID3D11ShaderResourceView* picturesSrv = nullptr;

    // The fog buffer (RGBA16F, the size of the engine's HDR image).
    ID3D11Texture2D* fogTex = nullptr;
    ID3D11RenderTargetView* fogRtv = nullptr;
    ID3D11ShaderResourceView* fogSrv = nullptr;
    UINT fogW = 0, fogH = 0;

    // Copies of the fog draw's constants (made on the GPU at its draw).
    ID3D11Buffer* fogP = nullptr;
    UINT fogPSize = 0;
    ID3D11Buffer* sceneP = nullptr;
    UINT scenePSize = 0;
    ID3D11Buffer* planes[kMaxPlanes] = {};
    UINT planeSize[kMaxPlanes] = {};

    // Views of the G-buffer textures (made when they change).
    struct View {
        ID3D11Resource* res = nullptr; // identity only
        ID3D11ShaderResourceView* srv = nullptr;
    } views[3];

    // This render.
    bool started = false;      // the first fog draw cleared the buffer
    bool pending = false;      // drawn into the buffer, not put into the image yet
    bool seen = false;         // this frame has water (for the effects)
    int planeCount = 0;
    int fogDraws = 0;
    float clarity = 0.0f;
    double time = 0.0;
    ID3D11RenderTargetView* hdrRtv = nullptr; // the engine's target of the fog draws (held)
    ID3D11ShaderResourceView* depth = nullptr; // its TMapDepth (held)

    // Around one fog draw: what the engine had bound.
    ID3D11RenderTargetView* savedRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* savedDsv = nullptr;
    ID3D11BlendState* savedBlend = nullptr;
    FLOAT savedFactor[4] = {};
    UINT savedMask = 0xffffffff;

    char status[160] = "none seen yet";
    int logs = 0;
};
State g;

bool ensureShaders(ID3D11Device* device) {
    if (g.ready && g.device == device) return true;
    if (g.tried && g.device == device) return false;
    g.tried = true;
    g.device = device;
    hooks::Inside inside; // the mod's own shaders aren't the game's
    bool ok = SUCCEEDED(device->CreateVertexShader(g_VS_WaterFull, sizeof(g_VS_WaterFull), nullptr, &g.vs));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_WaterCompose, sizeof(g_PS_WaterCompose), nullptr, &g.ps));
    D3D11_BLEND_DESC bd = {};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok &= SUCCEEDED(device->CreateBlendState(&bd, &g.accumulate));
    rt.DestBlend = D3D11_BLEND_SRC1_COLOR;
    rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
    rt.DestBlendAlpha = D3D11_BLEND_ONE;
    ok &= SUCCEEDED(device->CreateBlendState(&bd, &g.dual));
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ok &= SUCCEEDED(device->CreateDepthStencilState(&dd, &g.noDepth));
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ok &= SUCCEEDED(device->CreateRasterizerState(&rd, &g.rs));
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ok &= SUCCEEDED(device->CreateSamplerState(&sd, &g.wrap));
    D3D11_BUFFER_DESC cb = {};
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.ByteWidth = 16;
    ok &= SUCCEEDED(device->CreateBuffer(&cb, nullptr, &g.params));
    // The water pictures (foam), the same as TMNF's (TMVS\textures\water\ or built in).
    std::string report;
    const std::vector<uint32_t> texels = tmshaders::waterfx::buildAtlas(tmshaders::log::dataDir() + L"\\textures\\water", &report);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = tmshaders::waterfx::kAtlasSize;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // a << 24 | r << 16 | g << 8 | b: B G R A in memory
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {texels.data(), tmshaders::waterfx::kAtlasSize * 4u, 0};
    ok &= SUCCEEDED(device->CreateTexture2D(&td, &init, &g.pictures)) &&
          SUCCEEDED(device->CreateShaderResourceView(g.pictures, nullptr, &g.picturesSrv));
    g.ready = ok;
    TMVS_LOG(ok ? "water: ready (pictures: %s)" : "water: creating shaders or states failed (%s)", report.c_str());
    return ok;
}

bool ensureFogTarget(UINT w, UINT h) {
    if (g.fogTex && g.fogW == w && g.fogH == h) return true;
    safeRelease(g.fogSrv);
    safeRelease(g.fogRtv);
    safeRelease(g.fogTex);
    g.fogW = g.fogH = 0;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g.device->CreateTexture2D(&d, nullptr, &g.fogTex)) || FAILED(g.device->CreateRenderTargetView(g.fogTex, nullptr, &g.fogRtv)) ||
        FAILED(g.device->CreateShaderResourceView(g.fogTex, nullptr, &g.fogSrv))) {
        safeRelease(g.fogSrv);
        safeRelease(g.fogRtv);
        safeRelease(g.fogTex);
        TMVS_LOG("water: the fog buffer %ux%u failed", w, h);
        return false;
    }
    g.fogW = w;
    g.fogH = h;
    TMVS_LOG("water: fog buffer %ux%u", w, h);
    return true;
}

// A copy of an engine constant buffer (same size, made on the GPU: nothing waits for it).
bool copyBuffer(ID3D11DeviceContext* ctx, ID3D11Buffer* source, ID3D11Buffer*& copy, UINT& size) {
    if (!source) return false;
    D3D11_BUFFER_DESC d;
    source->GetDesc(&d);
    if (!copy || size != d.ByteWidth) {
        safeRelease(copy);
        D3D11_BUFFER_DESC c = {};
        c.ByteWidth = d.ByteWidth;
        c.Usage = D3D11_USAGE_DEFAULT;
        c.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(g.device->CreateBuffer(&c, nullptr, &copy))) copy = nullptr;
        size = d.ByteWidth;
    }
    if (!copy) return false;
    ctx->CopyResource(copy, source);
    return true;
}

ID3D11ShaderResourceView* viewOf(int index, ID3D11Resource* res) {
    State::View& v = g.views[index];
    if (v.res == res) return v.srv;
    safeRelease(v.srv);
    v.res = res;
    if (!res) return nullptr;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) return nullptr;
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    const DXGI_FORMAT read = readableFormat(d.Format);
    if ((d.BindFlags & D3D11_BIND_SHADER_RESOURCE) && read != DXGI_FORMAT_UNKNOWN && d.SampleDesc.Count == 1) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = read;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(g.device->CreateShaderResourceView(tex, &sv, &v.srv))) v.srv = nullptr;
    }
    tex->Release();
    return v.srv;
}

void releaseRender() {
    safeRelease(g.hdrRtv);
    safeRelease(g.depth);
    g.started = false;
    g.pending = false;
}

void logOnce(int bit, const char* fmt, ...) {
    if (g.logs & (1 << bit)) return;
    g.logs |= 1 << bit;
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    TMVS_LOG("%s", line);
}

} // namespace

bool isFogShader(const char* name) {
    return name && !strcmp(name, "Tech3/DeferredWaterFog_p");
}

bool beginFog(ID3D11Device* device, ID3D11DeviceContext* ctx, const ShaderInfo& ps, const Params& params) {
    if (params.clarity <= 0.0f && !params.splash) return false;
    if (!device || !ensureShaders(device)) return false;
    hooks::Inside inside; // our own binds are not the engine's (the hooks would track them)
    // The engine's blend must be premultiplied over (one, inverse source alpha): the buffer adds up
    // its draws the same way and the compose does what the blend did.
    ID3D11BlendState* blend = nullptr;
    FLOAT factor[4] = {};
    UINT mask = 0xffffffff;
    ctx->OMGetBlendState(&blend, factor, &mask);
    bool over = false;
    if (blend) {
        D3D11_BLEND_DESC d;
        blend->GetDesc(&d);
        const auto& rt = d.RenderTarget[0];
        over = rt.BlendEnable && rt.SrcBlend == D3D11_BLEND_ONE && rt.DestBlend == D3D11_BLEND_INV_SRC_ALPHA && rt.BlendOp == D3D11_BLEND_OP_ADD;
        if (!over) logOnce(0, "water: the engine's water fog blends %d / %d / %d, not premultiplied over: left alone", rt.SrcBlend, rt.DestBlend, rt.BlendOp);
    } else {
        logOnce(1, "water: the engine's water fog draws without blending: left alone");
    }
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    auto releaseBound = [&] {
        for (auto*& r : rtvs) safeRelease(r);
        safeRelease(dsv);
        safeRelease(blend);
    };
    if (!over || !rtvs[0]) {
        releaseBound();
        return false;
    }
    ID3D11Resource* res = nullptr;
    rtvs[0]->GetResource(&res);
    D3D11_TEXTURE2D_DESC td = {};
    ID3D11Texture2D* tex = nullptr;
    if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
        tex->GetDesc(&td);
        tex->Release();
    }
    if (res) res->Release();
    if (!td.Width || td.SampleDesc.Count != 1) {
        logOnce(2, "water: the water fog draws into a %ux%u target with %u samples: left alone", td.Width, td.Height, td.SampleDesc.Count);
        releaseBound();
        return false;
    }
    // A second render of the same frame (the video export's) or a new target: start over.
    if (g.started && (g.hdrRtv != rtvs[0] || g.fogW != td.Width || g.fogH != td.Height)) releaseRender();
    if (!g.started) {
        if (!ensureFogTarget(td.Width, td.Height)) {
            releaseBound();
            return false;
        }
        const float zero[4] = {};
        ctx->ClearRenderTargetView(g.fogRtv, zero);
        ID3D11Buffer* cb0 = nullptr;
        ID3D11Buffer* cb13 = nullptr;
        ctx->PSGetConstantBuffers(0, 1, &cb0);
        ctx->PSGetConstantBuffers(13, 1, &cb13);
        const bool constants = copyBuffer(ctx, cb0, g.fogP, g.fogPSize) && copyBuffer(ctx, cb13, g.sceneP, g.scenePSize) &&
                               g.fogPSize >= 48 && g.scenePSize >= 176;
        safeRelease(cb0);
        safeRelease(cb13);
        if (ps.depthSlot != kNoSlot) ctx->PSGetShaderResources(ps.depthSlot, 1, &g.depth);
        g.hdrRtv = rtvs[0];
        g.hdrRtv->AddRef();
        g.planeCount = constants && g.depth ? 0 : -1; // -1: no detail this render (constants or depth missing)
        g.fogDraws = 0;
        g.started = true;
        g.seen = true;
        g.clarity = params.clarity;
        g.time = params.time;
        logOnce(3, "water: the engine's water fog drawn into the mod's buffer (%ux%u, ShaderP %u bytes, SceneP %u bytes, depth %s)", td.Width,
                td.Height, g.fogPSize, g.scenePSize, g.depth ? "found" : "missing");
    }
    // This draw's surface (ObjectP: WorldToWaterDepth).
    if (g.planeCount >= 0 && g.planeCount < kMaxPlanes) {
        ID3D11Buffer* cb1 = nullptr;
        ctx->PSGetConstantBuffers(1, 1, &cb1);
        if (cb1) {
            D3D11_BUFFER_DESC d;
            cb1->GetDesc(&d);
            logOnce(4, "water: a water volume's constants: %u bytes, usage %d", d.ByteWidth, d.Usage);
            if (copyBuffer(ctx, cb1, g.planes[g.planeCount], g.planeSize[g.planeCount])) g.planeCount++;
            cb1->Release();
        }
    }
    g.fogDraws++;
    // Into the buffer, with the engine's depth test (its depth buffer stays bound).
    memcpy(g.savedRtvs, rtvs, sizeof(rtvs));
    g.savedDsv = dsv;
    g.savedBlend = blend;
    memcpy(g.savedFactor, factor, sizeof(factor));
    g.savedMask = mask;
    ctx->OMSetRenderTargets(1, &g.fogRtv, dsv);
    ctx->OMSetBlendState(g.accumulate, nullptr, 0xffffffff);
    g.pending = true;
    return true;
}

void endFog(ID3D11DeviceContext* ctx) {
    hooks::Inside inside;
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, g.savedRtvs, g.savedDsv);
    ctx->OMSetBlendState(g.savedBlend, g.savedFactor, g.savedMask);
    for (auto*& r : g.savedRtvs) safeRelease(r);
    safeRelease(g.savedDsv);
    safeRelease(g.savedBlend);
}

bool pending() {
    return g.pending;
}

void compose(ID3D11DeviceContext* ctx, ID3D11Resource* albedo, ID3D11Resource* normal, ID3D11Resource* lightMask) {
    if (!g.pending) return;
    g.pending = false;
    if (!g.ready || !g.hdrRtv || !g.fogSrv) return;
    hooks::Inside inside;
    StateBackup backup;
    backup.save(ctx);
    ID3D11Buffer* savedCbs[7] = {}; // b5 - b11 (the backup keeps b0 - b4)
    ctx->PSGetConstantBuffers(5, 7, savedCbs);

    const int planes = std::max(g.planeCount, 0);
    ID3D11ShaderResourceView* gAlbedo = viewOf(0, albedo);
    ID3D11ShaderResourceView* gNormal = viewOf(1, normal);
    ID3D11ShaderResourceView* gMask = viewOf(2, lightMask);
    const bool detail = planes > 0 && g.depth && gAlbedo && gNormal && gMask;
    const float params[4] = {g.clarity, static_cast<float>(std::fmod(g.time, 1000.0)), static_cast<float>(planes), detail ? 1.0f : 0.0f};
    ctx->UpdateSubresource(g.params, 0, nullptr, params, 0, 0);

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g.vs, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->RSSetState(g.rs);
    D3D11_VIEWPORT vp = {0, 0, static_cast<float>(g.fogW), static_cast<float>(g.fogH), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &g.hdrRtv, nullptr);
    ctx->OMSetBlendState(g.dual, nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(g.noDepth, 0);
    ID3D11ShaderResourceView* srvs[7] = {g.fogSrv, detail ? g.depth : nullptr, gAlbedo, gNormal, gMask, nullptr, g.picturesSrv};
    ctx->PSSetShaderResources(0, 7, srvs);
    ctx->PSSetSamplers(0, 1, &g.wrap);
    ID3D11Buffer* cbs[12] = {g.fogP, nullptr, g.params, g.sceneP};
    for (int i = 0; i < kMaxPlanes; i++) cbs[4 + i] = i < planes ? g.planes[i] : nullptr;
    ctx->PSSetConstantBuffers(0, 12, cbs);
    ctx->PSSetShader(g.ps, nullptr, 0);
    ctx->Draw(3, 0);

    ctx->PSSetConstantBuffers(5, 7, savedCbs);
    for (auto*& b : savedCbs) safeRelease(b);
    backup.restore(ctx);
    snprintf(g.status, sizeof(g.status), "%d volume%s, %s", g.fogDraws, g.fogDraws == 1 ? "" : "s",
             g.clarity > 0.0f ? (detail ? "clear, with caustics and foam" : "clear") : "the game's murk");
    logOnce(5, "water: put into the image (%d fog draws, %d planes, clarity %.2f, caustics and foam %s)", g.fogDraws, planes,
            g.clarity, detail ? "on" : "off");
}

void endRender() {
    releaseRender();
}

void endFrame() {
    releaseRender();
    g.seen = false;
}

void release() {
    releaseRender();
    endFrame();
    safeRelease(g.vs);
    safeRelease(g.ps);
    safeRelease(g.accumulate);
    safeRelease(g.dual);
    safeRelease(g.noDepth);
    safeRelease(g.rs);
    safeRelease(g.wrap);
    safeRelease(g.params);
    safeRelease(g.picturesSrv);
    safeRelease(g.pictures);
    safeRelease(g.fogSrv);
    safeRelease(g.fogRtv);
    safeRelease(g.fogTex);
    g.fogW = g.fogH = 0;
    safeRelease(g.fogP);
    safeRelease(g.sceneP);
    for (auto& p : g.planes) safeRelease(p);
    for (auto& v : g.views) {
        safeRelease(v.srv);
        v.res = nullptr;
    }
    g.ready = g.tried = false;
    g.device = nullptr;
}

ID3D11ShaderResourceView* fogView() {
    return g.seen ? g.fogSrv : nullptr;
}

ID3D11Buffer* const* planeBuffers() {
    return g.planes;
}

int planeCount() {
    return g.seen ? std::max(g.planeCount, 0) : 0;
}

const char* status() {
    return g.status;
}

} // namespace water
} // namespace tmvs_mp
