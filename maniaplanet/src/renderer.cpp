#include "renderer.h"
#include "hooks.h"
#include "log.h"
#include "lut.h"
#include "options.h"
#include "settings.h"
#include "shaders/PS_BloomDown.h"
#include "shaders/PS_BloomUp.h"
#include "shaders/PS_Grade.h"
#include "shaders/PS_ToneMap.h"
#include "shaders/VS_Full.h"
#include <algorithm>
#include <cstring>

namespace tmvs_mp {
namespace {

template <typename T> void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// hdr.hlsl ToneParams (b0)
struct ToneConstants {
    float tone0[4]; // has bloom, input already exposed
};

// mp.hlsl GradeParams (b0)
struct GradeConstants {
    float p3[4];             // texel (xy), sRGB view, view
    float p4[4];             // split, has depth, HDR stage ran
    float invViewProj[4][4]; // rows
    float camera[4];         // eye xyz, has camera
    float has[4];            // normal, motion, albedo textures present
};

// look.hlsli LookParams (b3)
struct LookConstants {
    float l0[4];  // exposure (EV), contrast, saturation, vibrance
    float l1[4];  // temperature, tint, split toning, tone knee
    float l2[4];  // lift, gamma, gain, vignette
    float l3[4];  // film grain, chromatic aberration, sharpen, LUT amount (0 = no LUT)
    float sun[4]; // sun colour, tone curve
    float sky[4]; // sky colour, bloom
    float l6[4];  // frame, LUT size, split, bloom radius
};

// The copy of the game's image is made typeless, so it can be read with the game's view format
// (an sRGB view of a UNORM back buffer, for example).
DXGI_FORMAT typelessOf(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default: return f;
    }
}

bool isSrgb(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

void sizeOfView(ID3D11View* view, UINT& w, UINT& h) {
    w = h = 0;
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        w = d.Width;
        h = d.Height;
        tex->Release();
    }
    if (res) res->Release();
}

} // namespace

void StateBackup::save(ID3D11DeviceContext* ctx) {
    scissorCount = viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetScissorRects(&scissorCount, scissors);
    ctx->RSGetViewports(&viewportCount, viewports);
    ctx->RSGetState(&rs);
    ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
    ctx->OMGetDepthStencilState(&dss, &stencilRef);
    ctx->PSGetShaderResources(0, kSrvs, psSrv);
    ctx->PSGetSamplers(0, kSamplers, psSamplers);
    ctx->PSGetConstantBuffers(0, kCbs, psCb);
    ctx->VSGetConstantBuffers(0, kCbs, vsCb);
    ctx->VSGetShaderResources(0, kSrvs, vsSrv);
    psInstCount = vsInstCount = gsInstCount = hsInstCount = dsInstCount = 256;
    ctx->PSGetShader(&ps, psInst, &psInstCount);
    ctx->VSGetShader(&vs, vsInst, &vsInstCount);
    ctx->GSGetShader(&gs, gsInst, &gsInstCount);
    ctx->HSGetShader(&hs, hsInst, &hsInstCount);
    ctx->DSGetShader(&ds, dsInst, &dsInstCount);
    ctx->IAGetPrimitiveTopology(&topology);
    ctx->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
    ctx->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
    ctx->IAGetInputLayout(&layout);
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
}

void StateBackup::restore(ID3D11DeviceContext* ctx) {
    ctx->RSSetScissorRects(scissorCount, scissors);
    ctx->RSSetViewports(viewportCount, viewports);
    ctx->RSSetState(rs);
    safeRelease(rs);
    ctx->OMSetBlendState(blend, blendFactor, sampleMask);
    safeRelease(blend);
    ctx->OMSetDepthStencilState(dss, stencilRef);
    safeRelease(dss);
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
    for (auto& r : rtvs) safeRelease(r);
    safeRelease(dsv);
    ctx->PSSetShaderResources(0, kSrvs, psSrv);
    for (auto& s : psSrv) safeRelease(s);
    ctx->PSSetSamplers(0, kSamplers, psSamplers);
    for (auto& s : psSamplers) safeRelease(s);
    ctx->PSSetConstantBuffers(0, kCbs, psCb);
    for (auto& b : psCb) safeRelease(b);
    ctx->VSSetConstantBuffers(0, kCbs, vsCb);
    for (auto& b : vsCb) safeRelease(b);
    ctx->VSSetShaderResources(0, kSrvs, vsSrv);
    for (auto& s : vsSrv) safeRelease(s);
    ctx->PSSetShader(ps, psInst, psInstCount);
    safeRelease(ps);
    for (UINT i = 0; i < psInstCount; i++) safeRelease(psInst[i]);
    ctx->VSSetShader(vs, vsInst, vsInstCount);
    safeRelease(vs);
    for (UINT i = 0; i < vsInstCount; i++) safeRelease(vsInst[i]);
    ctx->GSSetShader(gs, gsInst, gsInstCount);
    safeRelease(gs);
    for (UINT i = 0; i < gsInstCount; i++) safeRelease(gsInst[i]);
    ctx->HSSetShader(hs, hsInst, hsInstCount);
    safeRelease(hs);
    for (UINT i = 0; i < hsInstCount; i++) safeRelease(hsInst[i]);
    ctx->DSSetShader(ds, dsInst, dsInstCount);
    safeRelease(ds);
    for (UINT i = 0; i < dsInstCount; i++) safeRelease(dsInst[i]);
    ctx->IASetPrimitiveTopology(topology);
    ctx->IASetIndexBuffer(ib, ibFormat, ibOffset);
    safeRelease(ib);
    ctx->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
    safeRelease(vb);
    ctx->IASetInputLayout(layout);
    safeRelease(layout);
}

bool Renderer::init(ID3D11Device* device) {
    m_device = device;
    bool ok = SUCCEEDED(device->CreateVertexShader(g_VS_Full, sizeof(g_VS_Full), nullptr, &m_vs));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_Grade, sizeof(g_PS_Grade), nullptr, &m_grade));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_BloomDown, sizeof(g_PS_BloomDown), nullptr, &m_bloomDown));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_BloomUp, sizeof(g_PS_BloomUp), nullptr, &m_bloomUp));
    ok &= SUCCEEDED(device->CreatePixelShader(g_PS_ToneMap, sizeof(g_PS_ToneMap), nullptr, &m_toneMap));

    D3D11_BUFFER_DESC cb = {};
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.ByteWidth = sizeof(GradeConstants);
    ok &= SUCCEEDED(device->CreateBuffer(&cb, nullptr, &m_gradeCb));
    cb.ByteWidth = 16;
    ok &= SUCCEEDED(device->CreateBuffer(&cb, nullptr, &m_passCb));
    cb.ByteWidth = sizeof(ToneConstants);
    ok &= SUCCEEDED(device->CreateBuffer(&cb, nullptr, &m_toneCb));
    cb.ByteWidth = sizeof(LookConstants);
    ok &= SUCCEEDED(device->CreateBuffer(&cb, nullptr, &m_lookCb));

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ok &= SUCCEEDED(device->CreateSamplerState(&sd, &m_linear));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ok &= SUCCEEDED(device->CreateSamplerState(&sd, &m_point));

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ok &= SUCCEEDED(device->CreateRasterizerState(&rd, &m_rs));
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ok &= SUCCEEDED(device->CreateDepthStencilState(&dd, &m_dss));
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok &= SUCCEEDED(device->CreateBlendState(&bd, &m_blend));
    if (!ok) TMVS_LOG("renderer: creating shaders or states failed");
    // Shader model 5 effects: feature level 11 (else the look and the tone map only).
    if (device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0) m_effectsReady = m_effects.init(device);
    else TMVS_LOG("renderer: feature level 0x%x, the scene effects need 11.0", device->GetFeatureLevel());
    tmshaders::lut::writeBuiltInLooks();
    return ok;
}

void Renderer::releaseTargets() {
    safeRelease(m_copySrv);
    safeRelease(m_copy);
    m_copyDesc = {};
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < kBloomLevels; i++) {
            safeRelease(m_bloomSrv[k][i]);
            safeRelease(m_bloomRtv[k][i]);
            safeRelease(m_bloomTex[k][i]);
        }
    }
    m_bloomBaseW = m_bloomBaseH = 0;
    m_effects.releaseTargets();
    for (int i = 0; i < kTexCount; i++) {
        for (auto& v : m_tex[i]) {
            safeRelease(v.srv);
            v = TexView{};
        }
        m_texCurrent[i] = -1;
    }
}

void Renderer::release() {
    releaseTargets();
    m_effects.release();
    m_effectsReady = false;
    safeRelease(m_lutSrv);
    m_lutName.clear();
    safeRelease(m_vs);
    safeRelease(m_grade);
    safeRelease(m_bloomDown);
    safeRelease(m_bloomUp);
    safeRelease(m_toneMap);
    safeRelease(m_gradeCb);
    safeRelease(m_passCb);
    safeRelease(m_toneCb);
    safeRelease(m_lookCb);
    safeRelease(m_linear);
    safeRelease(m_point);
    safeRelease(m_rs);
    safeRelease(m_dss);
    safeRelease(m_blend);
    m_device = nullptr;
}

bool Renderer::ensureCopy(ID3D11Texture2D* source, const D3D11_TEXTURE2D_DESC& desc, DXGI_FORMAT viewFormat) {
    (void)source;
    const bool msaa = desc.SampleDesc.Count > 1;
    const DXGI_FORMAT copyFormat = typelessOf(desc.Format);
    if (m_copy && m_copyDesc.Width == desc.Width && m_copyDesc.Height == desc.Height && m_copyDesc.Format == copyFormat &&
        m_copyViewFormat == viewFormat) {
        return true;
    }
    safeRelease(m_copySrv);
    safeRelease(m_copy);
    D3D11_TEXTURE2D_DESC cd = {};
    cd.Width = desc.Width;
    cd.Height = desc.Height;
    cd.MipLevels = 1;
    cd.ArraySize = 1;
    cd.Format = copyFormat;
    cd.SampleDesc.Count = 1;
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_device->CreateTexture2D(&cd, nullptr, &m_copy))) {
        TMVS_LOG("renderer: copy texture %ux%u format %d failed", desc.Width, desc.Height, copyFormat);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = viewFormat;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (FAILED(m_device->CreateShaderResourceView(m_copy, &sv, &m_copySrv))) {
        TMVS_LOG("renderer: copy view format %d failed", viewFormat);
        safeRelease(m_copy);
        return false;
    }
    m_copyDesc = cd;
    m_copyViewFormat = viewFormat;
    TMVS_LOG("renderer: image %ux%u, format %d (view %d)%s", desc.Width, desc.Height, desc.Format, viewFormat,
             msaa ? ", multisampled" : "");
    return true;
}

ID3D11ShaderResourceView* Renderer::textureView(int index, ID3D11Resource* res) {
    m_texCurrent[index] = -1;
    if (!res) return nullptr;
    for (int k = 0; k < kViewsPerTexture; k++) {
        if (m_tex[index][k].res == res) {
            m_texCurrent[index] = k;
            return m_tex[index][k].srv;
        }
    }
    const int k = m_texNext[index];
    m_texNext[index] = (k + 1) % kViewsPerTexture;
    TexView& v = m_tex[index][k];
    safeRelease(v.srv);
    v = TexView{};
    v.res = res;
    m_texCurrent[index] = k;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) return nullptr;
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    v.format = d.Format;
    const bool log = m_texLogged[index]++ < 2;
    const DXGI_FORMAT read = readableFormat(d.Format);
    if (!(d.BindFlags & D3D11_BIND_SHADER_RESOURCE) || read == DXGI_FORMAT_UNKNOWN || d.SampleDesc.Count > 1) {
        if (log)
            TMVS_LOG("renderer: engine %s %ux%u format %d bind 0x%x samples %u is not readable", engineTextureName(index), d.Width,
                     d.Height, d.Format, d.BindFlags, d.SampleDesc.Count);
        tex->Release();
        return nullptr;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = read;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (d.ArraySize > 1) {
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sv.Texture2DArray.MipLevels = 1;
        sv.Texture2DArray.ArraySize = 1;
    }
    if (FAILED(m_device->CreateShaderResourceView(tex, &sv, &v.srv))) v.srv = nullptr;
    if (log)
        TMVS_LOG("renderer: engine %s %ux%u format %d -> %s", engineTextureName(index), d.Width, d.Height, d.Format,
                 v.srv ? "readable" : "view failed");
    tex->Release();
    return v.srv;
}

void Renderer::updateLut(const std::string& name) {
    if (name == m_lutName) return;
    m_lutName = name;
    m_lutError.clear();
    safeRelease(m_lutSrv);
    m_lutSize = 0;
    if (name.empty()) return;
    tmshaders::lut::Table table;
    if (!tmshaders::lut::loadCube(tmshaders::lut::path(name), table, m_lutError)) {
        TMVS_LOG("renderer: LUT %s: %s", name.c_str(), m_lutError.c_str());
        return;
    }
    D3D11_TEXTURE3D_DESC d = {};
    d.Width = d.Height = d.Depth = static_cast<UINT>(table.size);
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // the table holds A8R8G8B8 texels: B, G, R, A in memory
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = table.texels.data();
    data.SysMemPitch = table.size * 4;
    data.SysMemSlicePitch = table.size * table.size * 4;
    ID3D11Texture3D* tex = nullptr;
    if (SUCCEEDED(m_device->CreateTexture3D(&d, &data, &tex))) {
        m_device->CreateShaderResourceView(tex, nullptr, &m_lutSrv);
        tex->Release();
    }
    m_lutSize = m_lutSrv ? table.size : 0;
    if (!m_lutSrv) m_lutError = "texture failed";
}

void Renderer::setCommonState(ID3D11DeviceContext* ctx) {
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ID3D11Buffer* noBuffer = nullptr;
    UINT zero = 0;
    ctx->IASetVertexBuffers(0, 1, &noBuffer, &zero, &zero);
    ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    ctx->VSSetShader(m_vs, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->RSSetState(m_rs);
    ctx->RSSetScissorRects(0, nullptr);
    const float blendFactor[4] = {};
    ctx->OMSetBlendState(m_blend, blendFactor, 0xffffffff);
    ctx->OMSetDepthStencilState(m_dss, 0);
    ID3D11SamplerState* samplers[2] = {m_linear, m_point};
    ctx->PSSetSamplers(0, 2, samplers);
}

// The shared look constants (b3) and the LUT (t7) for this frame.
void Renderer::uploadLook(ID3D11DeviceContext* ctx, const Look& look) {
    const tmshaders::Settings& s = *look.settings;
    updateLut(look.lut);
    LookConstants c = {};
    c.l0[0] = s.exposure;
    c.l0[1] = s.contrast;
    c.l0[2] = s.saturation;
    c.l0[3] = s.vibrance;
    c.l1[0] = s.temperature;
    c.l1[1] = s.tint;
    c.l1[2] = s.shadowTint;
    c.l1[3] = s.toneKnee;
    c.l2[0] = s.lift;
    c.l2[1] = s.gamma;
    c.l2[2] = s.gain;
    c.l2[3] = s.vignette;
    c.l3[0] = s.filmGrain;
    c.l3[1] = s.chromaticAberration;
    c.l3[2] = s.sharpen;
    c.l3[3] = m_lutSrv ? look.lutAmount : 0.0f;
    // Split toning's warm side: the engine's light mixed with the preset's sun colour, as in TMNF.
    for (int i = 0; i < 3; i++) {
        c.sun[i] = s.sunColor[i] + (look.sun[i] - s.sunColor[i]) * s.gameSunColor;
        c.sky[i] = s.skyColor[i];
    }
    c.sun[3] = static_cast<float>(s.toneCurve);
    c.sky[3] = s.bloom;
    c.l6[0] = static_cast<float>(look.frame % 4096) * 0.618034f;
    c.l6[1] = static_cast<float>(m_lutSize);
    c.l6[2] = look.split;
    c.l6[3] = s.bloomRadius;
    ctx->UpdateSubresource(m_lookCb, 0, nullptr, &c, 0, 0);
    ctx->PSSetConstantBuffers(3, 1, &m_lookCb);
    ctx->PSSetShaderResources(7, 1, &m_lutSrv);
}

bool Renderer::ensureBloom(UINT width, UINT height) {
    if (m_bloomTex[0][0] && m_bloomBaseW == width && m_bloomBaseH == height) return true;
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < kBloomLevels; i++) {
            safeRelease(m_bloomSrv[k][i]);
            safeRelease(m_bloomRtv[k][i]);
            safeRelease(m_bloomTex[k][i]);
        }
    }
    D3D11_TEXTURE2D_DESC d = {};
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    for (int i = 0; i < kBloomLevels; i++) {
        m_bloomW[i] = std::max(1u, width >> (i + 1));
        m_bloomH[i] = std::max(1u, height >> (i + 1));
        d.Width = m_bloomW[i];
        d.Height = m_bloomH[i];
        for (int k = 0; k < 2; k++) {
            if (k == 1 && i == kBloomLevels - 1) continue; // the smallest level has no up copy
            if (FAILED(m_device->CreateTexture2D(&d, nullptr, &m_bloomTex[k][i])) ||
                FAILED(m_device->CreateRenderTargetView(m_bloomTex[k][i], nullptr, &m_bloomRtv[k][i])) ||
                FAILED(m_device->CreateShaderResourceView(m_bloomTex[k][i], nullptr, &m_bloomSrv[k][i]))) {
                TMVS_LOG("renderer: bloom level %d (%ux%u) failed", i, d.Width, d.Height);
                return false;
            }
        }
    }
    m_bloomBaseW = width;
    m_bloomBaseH = height;
    return true;
}

void Renderer::pass(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv, UINT width, UINT height,
                    ID3D11ShaderResourceView* const* srvs, UINT srvCount, float texelX, float texelY, float z, float w) {
    ID3D11ShaderResourceView* none[7] = {};
    ctx->PSSetShaderResources(0, 7, none); // t7 (the LUT) stays
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = {0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    ctx->RSSetViewports(1, &vp);
    const float texel[4] = {texelX, texelY, z, w};
    ctx->UpdateSubresource(m_passCb, 0, nullptr, texel, 0, 0);
    ctx->PSSetShaderResources(0, srvCount, srvs);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->Draw(3, 0);
}

bool Renderer::toneMap(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, ID3D11ShaderResourceView* hdr,
                       ID3D11ShaderResourceView* exposure, ID3D11Buffer* engineConstants, const Look& look, const EngineView& engine) {
    m_effectsRan = false;
    if (!m_toneMap || !target || !hdr || !exposure || !engineConstants || !look.settings) return false;
    UINT w = 0, h = 0, hw = 0, hh = 0;
    sizeOfView(target, w, h);
    sizeOfView(hdr, hw, hh);
    if (!w || !h || !hw || !hh) return false;

    hooks::Inside inside;
    StateBackup backup;
    backup.save(ctx);
    const tmshaders::Settings& s = *look.settings;
    Profiler& profiler = m_effects.profiler();
    profiler.collect(ctx);
    profiler.begin(ctx);

    // The scene effects first, on the engine's HDR image: their result is what gets tone mapped.
    ID3D11ShaderResourceView* game = hdr;
    if (m_effectsReady && engine.camera) {
        ID3D11ShaderResourceView* views[kTexCount] = {};
        for (int i = 0; i < kTexCount; i++) views[i] = textureView(i, engine.tex ? engine.tex[i] : nullptr);
        SceneFrame frame;
        frame.hdr = hdr;
        frame.exposure = exposure;
        frame.engineTone = engineConstants;
        frame.engine = views;
        frame.camera = engine.camera;
        frame.time = engine.time;
        frame.photo = engine.photo;
        if (m_effects.scene(ctx, frame, s)) {
            hdr = m_effects.result();
            m_effectsRan = true;
        }
    }

    setCommonState(ctx);
    ID3D11Buffer* cbs[3] = {m_toneCb, m_passCb, engineConstants};
    ctx->PSSetConstantBuffers(0, 3, cbs);
    uploadLook(ctx, look);

    const bool bloom = s.bloom > 0.001f && ensureBloom(hw, hh);
    ToneConstants tc = {};
    tc.tone0[0] = bloom ? 1.0f : 0.0f;
    tc.tone0[1] = m_effectsRan ? 1.0f : 0.0f;
    ctx->UpdateSubresource(m_toneCb, 0, nullptr, &tc, 0, 0);

    ID3D11ShaderResourceView* bloomResult = nullptr;
    if (bloom) {
        // Down: the first level exposes and thresholds the HDR image, the rest halve it.
        ID3D11ShaderResourceView* first[2] = {hdr, exposure};
        pass(ctx, m_bloomDown, m_bloomRtv[0][0], m_bloomW[0], m_bloomH[0], first, 2, 1.0f / hw, 1.0f / hh, 1.0f);
        for (int i = 1; i < kBloomLevels; i++) {
            pass(ctx, m_bloomDown, m_bloomRtv[0][i], m_bloomW[i], m_bloomH[i], &m_bloomSrv[0][i - 1], 1, 1.0f / m_bloomW[i - 1],
                 1.0f / m_bloomH[i - 1]);
        }
        // Up: each level adds the blurred smaller one (TMNF's bloom radius scales it).
        const float radius = std::max(0.3f, s.bloomRadius);
        for (int i = kBloomLevels - 2; i >= 0; i--) {
            const int lower = i + 1;
            ID3D11ShaderResourceView* src = lower == kBloomLevels - 1 ? m_bloomSrv[0][lower] : m_bloomSrv[1][lower];
            ID3D11ShaderResourceView* srvs[3] = {src, nullptr, m_bloomSrv[0][i]};
            pass(ctx, m_bloomUp, m_bloomRtv[1][i], m_bloomW[i], m_bloomH[i], srvs, 3, 1.0f / m_bloomW[lower],
                 1.0f / m_bloomH[lower], 0.0f, radius);
        }
        bloomResult = m_bloomSrv[1][0];
        profiler.mark(ctx, kBloomUp);
    }
    ID3D11ShaderResourceView* srvs[4] = {hdr, exposure, bloomResult, game};
    pass(ctx, m_toneMap, target, w, h, srvs, 4, 1.0f / hw, 1.0f / hh);
    profiler.mark(ctx, kFinal);
    profiler.end(ctx);

    ID3D11ShaderResourceView* none[16] = {};
    ctx->PSSetShaderResources(0, 16, none);
    backup.restore(ctx);
    return true;
}

bool Renderer::apply(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, const EngineView& engine, const Look& look,
                     const Options& options, bool hdrDone) {
    if (!m_vs || !target || !look.settings) return false;
    D3D11_RENDER_TARGET_VIEW_DESC rd;
    target->GetDesc(&rd);
    if (rd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D && rd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2DMS) return false;
    if (rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && rd.Texture2D.MipSlice != 0) return false;
    ID3D11Resource* res = nullptr;
    target->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    const bool isTex = res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)));
    if (res) res->Release();
    if (!isTex) return false;
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    if (desc.ArraySize != 1 || !ensureCopy(tex, desc, rd.Format)) {
        static int logged = 0;
        if (logged++ < 5)
            TMVS_LOG("renderer: can't grade a %ux%u image (format %d, view %d, %u layers)", desc.Width, desc.Height, desc.Format,
                     rd.Format, desc.ArraySize);
        tex->Release();
        return false;
    }

    hooks::Inside inside;
    StateBackup backup;
    backup.save(ctx);

    if (desc.SampleDesc.Count > 1) ctx->ResolveSubresource(m_copy, 0, tex, 0, rd.Format);
    else ctx->CopyResource(m_copy, tex);
    tex->Release();

    ID3D11ShaderResourceView* views[kTexCount] = {};
    for (int i = 0; i < kTexCount; i++) views[i] = textureView(i, engine.tex ? engine.tex[i] : nullptr);

    setCommonState(ctx);
    ID3D11Buffer* cbs[2] = {m_gradeCb, m_passCb};
    ctx->PSSetConstantBuffers(0, 2, cbs);
    uploadLook(ctx, look);

    const UINT w = desc.Width, h = desc.Height;
    GradeConstants c = {};
    c.p3[0] = 1.0f / w;
    c.p3[1] = 1.0f / h;
    c.p3[2] = isSrgb(rd.Format) ? 1.0f : 0.0f;
    c.p3[3] = static_cast<float>(static_cast<int>(options.view));
    c.p4[0] = look.split;
    c.p4[1] = views[kTexDepth] ? 1.0f : 0.0f;
    c.p4[2] = hdrDone ? 1.0f : 0.0f;
    if (engine.camera) {
        memcpy(c.invViewProj, engine.camera->invViewProj, sizeof(c.invViewProj));
        memcpy(c.camera, engine.camera->eye, sizeof(float) * 3);
        c.camera[3] = 1.0f;
    }
    c.has[0] = views[kTexNormal] ? 1.0f : 0.0f;
    c.has[1] = views[kTexMotion] ? 1.0f : 0.0f;
    c.has[2] = views[kTexDiffuse] ? 1.0f : 0.0f;
    ctx->UpdateSubresource(m_gradeCb, 0, nullptr, &c, 0, 0);
    ID3D11ShaderResourceView* srvs[7] = {m_copySrv, nullptr, views[kTexDepth], nullptr, views[kTexNormal], views[kTexMotion],
                                         views[kTexDiffuse]};
    pass(ctx, m_grade, target, w, h, srvs, 7, 1.0f / w, 1.0f / h);

    ID3D11ShaderResourceView* none[8] = {};
    ctx->PSSetShaderResources(0, 8, none);
    backup.restore(ctx);
    return true;
}

} // namespace tmvs_mp
