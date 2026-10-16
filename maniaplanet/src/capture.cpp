#include "capture.h"
#include "hooks.h"
#include "log.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace tmvs_mp {
namespace capture {
namespace {

std::atomic<bool> g_pending{false};
bool g_active = false;
std::wstring g_folder;
std::wstring g_lastFolder;
FILE* g_trace = nullptr;
unsigned g_lines = 0;
constexpr unsigned kMaxLines = 30000;

std::string narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

UINT bytesPerPixel(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return 8;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_SNORM: return 4;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_TYPELESS: return 2;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_A8_UNORM: return 1;
    default: return 0;
    }
}

// DDS readers want a typed format.
DXGI_FORMAT typed(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
    default: return f;
    }
}

bool isDepthFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_R32G8X24_TYPELESS ||
           f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_D16_UNORM;
}

void writeDds(const std::wstring& path, const D3D11_TEXTURE2D_DESC& d, const D3D11_MAPPED_SUBRESOURCE& m, UINT bpp) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    uint32_t header[32] = {};
    header[0] = 0x20534444; // "DDS "
    header[1] = 124;
    header[2] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000; // caps, height, width, pitch, pixel format
    header[3] = d.Height;
    header[4] = d.Width;
    header[5] = d.Width * bpp;
    header[7] = 1;
    header[19] = 32;          // pixel format size
    header[20] = 0x4;         // FOURCC
    header[21] = 0x30315844;  // "DX10"
    header[27] = 0x1000;      // texture
    fwrite(header, 4, 32, f);
    const uint32_t dx10[5] = {static_cast<uint32_t>(typed(d.Format)), 3, 0, 1, 0};
    fwrite(dx10, 4, 5, f);
    for (UINT y = 0; y < d.Height; y++) fwrite(static_cast<const char*>(m.pData) + y * m.RowPitch, 1, d.Width * bpp, f);
    fclose(f);
}

void writeBmp(const std::wstring& path, const D3D11_TEXTURE2D_DESC& d, const D3D11_MAPPED_SUBRESOURCE& m) {
    const DXGI_FORMAT t = typed(d.Format);
    const bool rgba = t == DXGI_FORMAT_R8G8B8A8_UNORM || t == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const bool bgra = t == DXGI_FORMAT_B8G8R8A8_UNORM || t == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || t == DXGI_FORMAT_B8G8R8X8_UNORM;
    const bool rgb10 = t == DXGI_FORMAT_R10G10B10A2_UNORM;
    if (!rgba && !bgra && !rgb10) return;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    const uint32_t row = (d.Width * 3 + 3) & ~3u;
    const uint32_t size = 54 + row * d.Height;
    unsigned char h[54] = {'B', 'M'};
    memcpy(h + 2, &size, 4);
    const uint32_t offset = 54, infoSize = 40, planesBits = 1 | (24 << 16), imageSize = row * d.Height;
    const int32_t w = static_cast<int32_t>(d.Width), hgt = static_cast<int32_t>(d.Height);
    memcpy(h + 10, &offset, 4);
    memcpy(h + 14, &infoSize, 4);
    memcpy(h + 18, &w, 4);
    memcpy(h + 22, &hgt, 4);
    memcpy(h + 26, &planesBits, 4);
    memcpy(h + 34, &imageSize, 4);
    fwrite(h, 1, 54, f);
    std::vector<unsigned char> line(row, 0);
    for (int y = static_cast<int>(d.Height) - 1; y >= 0; y--) {
        const auto* src = reinterpret_cast<const uint32_t*>(static_cast<const char*>(m.pData) + y * m.RowPitch);
        for (UINT x = 0; x < d.Width; x++) {
            const uint32_t p = src[x];
            unsigned char r, g, b;
            if (rgb10) {
                r = static_cast<unsigned char>((p & 0x3ff) >> 2);
                g = static_cast<unsigned char>(((p >> 10) & 0x3ff) >> 2);
                b = static_cast<unsigned char>(((p >> 20) & 0x3ff) >> 2);
            } else if (rgba) {
                r = p & 0xff;
                g = (p >> 8) & 0xff;
                b = (p >> 16) & 0xff;
            } else {
                b = p & 0xff;
                g = (p >> 8) & 0xff;
                r = (p >> 16) & 0xff;
            }
            line[x * 3] = b;
            line[x * 3 + 1] = g;
            line[x * 3 + 2] = r;
        }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
}

// Copies a texture to the CPU and writes it as name.dds (and name.bmp for 8-bit colour).
void save(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Resource* res, const wchar_t* name) {
    if (!res || g_folder.empty()) return;
    hooks::Inside inside;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) return;
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    const UINT bpp = bytesPerPixel(d.Format);
    ID3D11Texture2D* source = tex;
    ID3D11Texture2D* resolved = nullptr;
    if (bpp && d.SampleDesc.Count > 1 && !isDepthFormat(d.Format)) {
        D3D11_TEXTURE2D_DESC rd = d;
        rd.Format = typed(d.Format);
        rd.SampleDesc = {1, 0};
        rd.BindFlags = 0;
        rd.MiscFlags = 0;
        rd.MipLevels = 1;
        rd.ArraySize = 1;
        if (SUCCEEDED(device->CreateTexture2D(&rd, nullptr, &resolved))) {
            ctx->ResolveSubresource(resolved, 0, tex, 0, rd.Format);
            source = resolved;
            d = rd;
        }
    }
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    ID3D11Texture2D* staging = nullptr;
    if (bpp && sd.SampleDesc.Count == 1 && SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &staging))) {
        ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, source, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            writeDds(g_folder + L"\\" + name + L".dds", sd, m, bpp);
            writeBmp(g_folder + L"\\" + name + L".bmp", sd, m);
            ctx->Unmap(staging, 0);
        }
        staging->Release();
    } else {
        TMVS_LOG("capture: %ls not saved (%ux%u format %d, %u samples)", name, d.Width, d.Height, d.Format, d.SampleDesc.Count);
    }
    if (resolved) resolved->Release();
    tex->Release();
}

void describe(ID3D11View* view, char* out, size_t size) {
    out[0] = 0;
    if (!view) return;
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        snprintf(out, size, "%p %ux%u f%d%s", static_cast<void*>(res), d.Width, d.Height, d.Format,
                 d.SampleDesc.Count > 1 ? " ms" : "");
        tex->Release();
    } else if (res) {
        snprintf(out, size, "%p (not 2D)", static_cast<void*>(res));
    }
    if (res) res->Release();
}

} // namespace

void request() {
    g_pending = true;
}

bool active() {
    return g_active;
}

const std::wstring& lastFolder() {
    return g_lastFolder;
}

void traceDraw(ID3D11DeviceContext* ctx, const char* shader, Role role, unsigned index) {
    if (!g_trace || g_lines >= kMaxLines) return;
    g_lines++;
    ID3D11RenderTargetView* rtv[2] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(2, rtv, &dsv);
    ID3D11ShaderResourceView* srv[4] = {};
    ctx->PSGetShaderResources(0, 4, srv);
    char rt0[96], rt1[96], ds[96], t[4][96];
    describe(rtv[0], rt0, sizeof(rt0));
    describe(rtv[1], rt1, sizeof(rt1));
    describe(dsv, ds, sizeof(ds));
    for (int i = 0; i < 4; i++) describe(srv[i], t[i], sizeof(t[i]));
    // rt0's blending (D3D11_BLEND / D3D11_BLEND_OP numbers): which passes add light (one, one, add).
    char blend[48] = "off";
    ID3D11BlendState* bs = nullptr;
    float factor[4];
    UINT mask = 0;
    ctx->OMGetBlendState(&bs, factor, &mask);
    if (bs) {
        D3D11_BLEND_DESC d;
        bs->GetDesc(&d);
        const D3D11_RENDER_TARGET_BLEND_DESC& b = d.RenderTarget[0];
        if (b.BlendEnable) snprintf(blend, sizeof(blend), "s%d d%d op%d", b.SrcBlend, b.DestBlend, b.BlendOp);
        bs->Release();
    }
    // The state that decides whether a draw lands (a light mask that stayed frozen while its
    // passes drew every frame): viewport, scissor, depth-stencil and rasterizer.
    char vp[64] = "none", sc[64] = "none", dss[96] = "default", rs[64] = "default";
    UINT count = 1;
    D3D11_VIEWPORT port = {};
    ctx->RSGetViewports(&count, &port);
    if (count)
        snprintf(vp, sizeof(vp), "%.0f %.0f %.0fx%.0f z%.1f-%.1f", port.TopLeftX, port.TopLeftY, port.Width, port.Height,
                 port.MinDepth, port.MaxDepth);
    count = 1;
    D3D11_RECT r = {};
    ctx->RSGetScissorRects(&count, &r);
    if (count) snprintf(sc, sizeof(sc), "%ld %ld %ld %ld", r.left, r.top, r.right, r.bottom);
    ID3D11DepthStencilState* ds_ = nullptr;
    UINT ref = 0;
    ctx->OMGetDepthStencilState(&ds_, &ref);
    if (ds_) {
        D3D11_DEPTH_STENCIL_DESC d;
        ds_->GetDesc(&d);
        // Depth: on, write mask, func; stencil: on, read/write masks, front func and pass op, ref.
        snprintf(dss, sizeof(dss), "z%d w%d f%d s%d %02x/%02x f%d p%d ref%u", d.DepthEnable, d.DepthWriteMask, d.DepthFunc,
                 d.StencilEnable, d.StencilReadMask, d.StencilWriteMask, d.FrontFace.StencilFunc, d.FrontFace.StencilPassOp, ref);
        ds_->Release();
    } else {
        snprintf(dss, sizeof(dss), "default ref%u", ref);
    }
    ID3D11RasterizerState* rs_ = nullptr;
    ctx->RSGetState(&rs_);
    if (rs_) {
        D3D11_RASTERIZER_DESC d;
        rs_->GetDesc(&d);
        // Fill, cull, front counter-clockwise, depth clip, scissor on, depth bias.
        snprintf(rs, sizeof(rs), "fill%d cull%d ccw%d clip%d sc%d bias%d", d.FillMode, d.CullMode, d.FrontCounterClockwise,
                 d.DepthClipEnable, d.ScissorEnable, d.DepthBias);
        rs_->Release();
    }
    fprintf(g_trace, "%5u %-7s %-48s rt0[%s] rt1[%s] ds[%s] blend[%s] t0[%s] t1[%s] t2[%s] t3[%s] vp[%s] sc[%s] dss[%s] rs[%s]\n",
            index, shaderdb::roleName(role), shader ? shader : "?", rt0, rt1, ds, blend, t[0], t[1], t[2], t[3], vp, sc, dss, rs);
    for (auto* v : rtv)
        if (v) v->Release();
    if (dsv) dsv->Release();
    for (auto* v : srv)
        if (v) v->Release();
}

void onToneMap(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    ID3D11ShaderResourceView* srv[4] = {};
    ctx->PSGetShaderResources(0, 4, srv);
    for (int i = 0; i < 4; i++) {
        if (!srv[i]) continue;
        ID3D11Resource* res = nullptr;
        srv[i]->GetResource(&res);
        wchar_t name[16];
        swprintf(name, 16, L"hdr_t%d", i);
        save(device, ctx, res, name);
        if (res) res->Release();
        srv[i]->Release();
    }
    // Its constants (curve and where the exposure is), for tools/preview.cpp.
    ID3D11Buffer* cb = nullptr;
    ctx->PSGetConstantBuffers(0, 1, &cb);
    if (cb) {
        hooks::Inside inside;
        D3D11_BUFFER_DESC d;
        cb->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        ID3D11Buffer* staging = nullptr;
        if (SUCCEEDED(device->CreateBuffer(&d, nullptr, &staging))) {
            ctx->CopyResource(staging, cb);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
                if (FILE* f = _wfopen((g_folder + L"/tonemap_cb.bin").c_str(), L"wb")) {
                    fwrite(m.pData, 1, d.ByteWidth, f);
                    fclose(f);
                }
                ctx->Unmap(staging, 0);
            }
            staging->Release();
        }
        cb->Release();
    }
    if (g_trace) fprintf(g_trace, "      ^ tone map: inputs saved as hdr_t*.dds\n");
}

void writeCamera(const Camera& c) {
    FILE* f = _wfopen((g_folder + L"/camera.txt").c_str(), L"w");
    if (f) {
        fprintf(f, "eye %.4f %.4f %.4f\n", c.eye[0], c.eye[1], c.eye[2]);
        fprintf(f, "sun %.4f %.4f %.4f\n", c.sunColour[0], c.sunColour[1], c.sunColour[2]);
        fprintf(f, "minmaxz %.6f %.6f %.6f %.6f\n", c.minMaxZ[0], c.minMaxZ[1], c.minMaxZ[2], c.minMaxZ[3]);
        fprintf(f, "worldprcamera_stored_in %s\n", c.transposed ? "columns" : "rows");
        for (int r = 0; r < 4; r++)
            fprintf(f, "viewproj %.6f %.6f %.6f %.6f\n", c.viewProj[r * 4], c.viewProj[r * 4 + 1], c.viewProj[r * 4 + 2],
                    c.viewProj[r * 4 + 3]);
        for (int r = 0; r < 3; r++)
            fprintf(f, "worldtocamera %.6f %.6f %.6f %.6f\n", c.worldToCamera[r * 4], c.worldToCamera[r * 4 + 1],
                    c.worldToCamera[r * 4 + 2], c.worldToCamera[r * 4 + 3]);
        fclose(f);
    }
    f = _wfopen((g_folder + L"/scenev.bin").c_str(), L"wb");
    if (f) {
        fwrite(c.raw, 1, sizeof(c.raw), f);
        fclose(f);
    }
    // The lighting pass's constants (sun direction, ambient, fog).
    if (c.hasSceneP && (f = _wfopen((g_folder + L"/scenep.bin").c_str(), L"wb")) != nullptr) {
        fwrite(c.rawP, 1, sizeof(c.rawP), f);
        fclose(f);
    }
}

// The sun shadow pass (volumetric light): its constants (read back from our GPU copy), its raster
// scale and its comparison sampler, for tools/preview.cpp.
void writeShadow(ID3D11Device* device, ID3D11DeviceContext* ctx, const EngineShadow& s) {
    if (!s.constants) return;
    hooks::Inside inside;
    D3D11_BUFFER_DESC d;
    s.constants->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Buffer* staging = nullptr;
    if (FAILED(device->CreateBuffer(&d, nullptr, &staging))) return;
    ctx->CopyResource(staging, s.constants);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        if (FILE* f = _wfopen((g_folder + L"/shadow_cb.bin").c_str(), L"wb")) {
            fwrite(m.pData, 1, d.ByteWidth, f);
            fclose(f);
        }
        ctx->Unmap(staging, 0);
    }
    staging->Release();
    if (FILE* f = _wfopen((g_folder + L"/shadow.txt").c_str(), L"w")) {
        D3D11_SAMPLER_DESC sd = {};
        if (s.sampler) s.sampler->GetDesc(&sd);
        fprintf(f, "raster %.9g %.9g %.9g %.9g\nfilter %d\naddress %d %d %d\ncompare %d\n", s.raster[0], s.raster[1], s.raster[2],
                s.raster[3], static_cast<int>(sd.Filter), static_cast<int>(sd.AddressU), static_cast<int>(sd.AddressV),
                static_cast<int>(sd.AddressW), static_cast<int>(sd.ComparisonFunc));
        fclose(f);
    }
}

void onGraded(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* input, DXGI_FORMAT viewFormat,
              const EngineView& engine) {
    save(device, ctx, input, L"input");
    if (FILE* f = _wfopen((g_folder + L"/view.txt").c_str(), L"w")) {
        fprintf(f, "%d", static_cast<int>(viewFormat));
        fclose(f);
    }
    for (int i = 0; engine.tex && i < kTexCount; i++) {
        wchar_t name[32];
        swprintf(name, 32, L"engine_%hs", engineTextureName(i));
        save(device, ctx, engine.tex[i], name);
    }
    if (engine.camera) writeCamera(*engine.camera);
    if (engine.shadow) writeShadow(device, ctx, *engine.shadow);
    if (g_trace) fprintf(g_trace, "      ^ the mod graded here (input.dds = before, engine_*.dds)\n");
}

void onPresent(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Resource* backBuffer) {
    save(device, ctx, backBuffer, L"output");
}

void writeText(const wchar_t* name, const std::string& text) {
    if (!g_active || g_folder.empty()) return;
    if (FILE* f = _wfopen((g_folder + L"\\" + name).c_str(), L"w")) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
}

void endFrame() {
    if (g_active) {
        if (g_trace) {
            const hooks::Stats& s = hooks::stats();
            fprintf(g_trace, "\nshaders created %u, known %u; deferred contexts %u; command lists %u\n", s.shadersCreated,
                    s.shadersKnown, s.deferredContexts, s.commandLists);
            fclose(g_trace);
            g_trace = nullptr;
        }
        TMVS_LOG("capture: written to %s", narrow(g_folder).c_str());
        g_lastFolder = g_folder;
        g_active = false;
        g_folder.clear();
    }
    if (!g_pending.exchange(false)) return;
    const std::wstring root = tmshaders::log::dataDir() + L"\\captures";
    CreateDirectoryW(root.c_str(), nullptr);
    for (int n = 1; n < 10000; n++) {
        wchar_t dir[32];
        swprintf(dir, 32, L"\\capture_%03d", n);
        const std::wstring path = root + dir;
        if (CreateDirectoryW(path.c_str(), nullptr)) {
            g_folder = path;
            break;
        }
    }
    if (g_folder.empty()) return;
    g_trace = _wfopen((g_folder + L"\\trace.txt").c_str(), L"w");
    g_lines = 0;
    g_active = true;
}

} // namespace capture
} // namespace tmvs_mp
