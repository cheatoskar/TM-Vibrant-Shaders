#include "clips/grabber.h"
#include "clips/recorder.h"
#include "clips_shaders.h"
#include <cstdio>

namespace tmshaders {
namespace clips {
namespace {

void (*g_begin)() = nullptr;
void (*g_end)() = nullptr;

struct Internal {
    Internal() {
        if (g_begin) g_begin();
    }
    ~Internal() {
        if (g_end) g_end();
    }
};

// The device as it was: the state block (shaders, constants, states, viewport, textures, streams)
// and what it doesn't hold (the render target and the depth buffer). One block is kept and captured
// again each frame (made once, released before a reset: releaseDevice); making one every frame
// cost the game's thread.
struct SavedState {
    IDirect3DDevice9* device;
    IDirect3DStateBlock9* block = nullptr;
    IDirect3DSurface9* target = nullptr;
    IDirect3DSurface9* depth = nullptr;
    SavedState(IDirect3DDevice9* d, IDirect3DStateBlock9*& kept) : device(d) {
        if (kept) {
            if (SUCCEEDED(kept->Capture())) block = kept;
        } else if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &kept)) && kept) {
            block = kept; // holds the state as it is now
        }
        device->GetRenderTarget(0, &target);
        device->GetDepthStencilSurface(&depth);
    }
    ~SavedState() {
        if (target) device->SetRenderTarget(0, target);
        device->SetDepthStencilSurface(depth);
        if (block) block->Apply(); // after the target: setting one resets the viewport
        gfx::release(target);
        gfx::release(depth);
    }
};

double msSince(int64_t start) {
    return static_cast<double>(now100ns() - start) / 1e4;
}

} // namespace

void Grabber::setInternalCalls(void (*begin)(), void (*end)()) {
    g_begin = begin;
    g_end = end;
}

bool Grabber::ensure(IDirect3DDevice9* device, int width, int height, D3DFORMAT format) {
    // After a failure (video memory full, say) not again for 5 s: every frame would log the same.
    const int64_t now = now100ns();
    if (m_retryAt && now < m_retryAt) return false;
    bool ok = true;
    if (!m_psLuma && FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(g_PS_ClipLuma), &m_psLuma))) ok = false;
    if (ok && !m_psChroma && FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(g_PS_ClipChroma), &m_psChroma))) ok = false;
    if (ok && !m_quad.init(device)) ok = false;
    const D3DFORMAT scaledFormat = format == D3DFMT_X8R8G8B8 || format == D3DFMT_A8R8G8B8 ? format : D3DFMT_A8R8G8B8;
    if (ok && !m_scaled.create(device, static_cast<UINT>(width), static_cast<UINT>(height), scaledFormat)) ok = false;
    if (ok && !m_luma.create(device, static_cast<UINT>(width / 4), static_cast<UINT>(height), D3DFMT_A8R8G8B8)) ok = false;
    if (ok && !m_chroma.create(device, static_cast<UINT>(width / 4), static_cast<UINT>(height / 2), D3DFMT_A8R8G8B8)) ok = false;
    m_retryAt = ok ? 0 : now + 50000000;
    return ok;
}

bool Grabber::scale(IDirect3DDevice9* device, IDirect3DSurface9* source) {
    if (SUCCEEDED(device->StretchRect(source, nullptr, m_scaled.surface, nullptr, D3DTEXF_LINEAR))) return true;
    // A multisampled back buffer can't be stretched on every card: resolved at its own size first.
    D3DSURFACE_DESC desc{};
    source->GetDesc(&desc);
    if (!m_resolve || m_resolveW != desc.Width || m_resolveH != desc.Height || m_resolveFormat != desc.Format) {
        gfx::release(m_resolve);
        if (FAILED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &m_resolve, nullptr))) return false;
        m_resolveW = desc.Width;
        m_resolveH = desc.Height;
        m_resolveFormat = desc.Format;
    }
    return SUCCEEDED(device->StretchRect(source, nullptr, m_resolve, nullptr, D3DTEXF_NONE)) &&
           SUCCEEDED(device->StretchRect(m_resolve, nullptr, m_scaled.surface, nullptr, D3DTEXF_LINEAR));
}

bool Grabber::copy(IDirect3DDevice9* device, IDirect3DSurface9* source, int width, int height) {
    if (!device || !source) return false;
    Internal internal;
    D3DSURFACE_DESC desc{};
    source->GetDesc(&desc);
    if (!ensure(device, width, height, desc.Format) || !scale(device, source)) return false;
    m_fresh = true;
    return true;
}

bool Grabber::grab(IDirect3DDevice9* device, IDirect3DSurface9* source, int width, int height, int64_t time) {
    const int64_t t0 = now100ns();
    if (!device || width < 8 || height < 2 || (width & 3) || (height & 1)) return false;
    Internal internal;
    if (!m_readback.canQueue()) {
        lastGrabMs = msSince(t0);
        return false; // every slot on its way back: this frame is left out
    }
    bool ok = true;
    {
        SavedState saved(device, m_state);
        if (source) {
            D3DSURFACE_DESC desc{};
            source->GetDesc(&desc);
            ok = ensure(device, width, height, desc.Format) && scale(device, source);
        } else {
            ok = m_fresh && m_scaled.valid() && m_luma.valid() && m_chroma.valid();
        }
        m_fresh = false;
        if (ok && SUCCEEDED(device->BeginScene())) {
            const float size[4] = {1.0f / width, 1.0f / height, 0.0f, 0.0f};
            device->SetDepthStencilSurface(nullptr);
            device->SetRenderState(D3DRS_ZENABLE, FALSE);
            device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
            device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
            device->SetRenderState(D3DRS_FOGENABLE, FALSE);
            device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
            device->SetTexture(0, m_scaled.texture);
            device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
            device->SetPixelShaderConstantF(0, size, 1);
            device->SetRenderTarget(0, m_luma.surface);
            device->SetPixelShader(m_psLuma);
            m_quad.draw(device, m_luma.width, m_luma.height);
            device->SetRenderTarget(0, m_chroma.surface);
            device->SetPixelShader(m_psChroma);
            m_quad.draw(device, m_chroma.width, m_chroma.height);
            device->SetTexture(0, nullptr);
            device->EndScene();
        } else {
            ok = false;
        }
    }
    if (ok) ok = m_readback.queue(device, m_luma, m_chroma, nullptr, false);
    if (ok) m_times.push_back(time);
    if (!ok && !m_failedOnce) {
        m_failedOnce = true;
        lastError = "the picture could not be copied (StretchRect or the passes failed)";
    }
    lastGrabMs = msSince(t0);
    return ok;
}

void Grabber::collect(const std::function<void(const PlaneReadback::Frame&, int64_t)>& use, bool all) {
    Internal internal;
    while (m_readback.pending() > 0 && (all || m_readback.oldestDone())) {
        const int before = m_readback.pending();
        const int64_t time = m_times.empty() ? 0 : m_times.front();
        m_readback.pop([&](const PlaneReadback::Frame& f) { use(f, time); });
        if (m_readback.pending() == before) break; // nothing came off
        if (!m_times.empty()) m_times.pop_front();
    }
}

void Grabber::releaseQueries() {
    m_readback.releaseQueries();
}

void Grabber::releaseDevice() {
    m_readback.release();
    m_times.clear();
    m_scaled.destroy();
    m_luma.destroy();
    m_chroma.destroy();
    gfx::release(m_resolve);
    gfx::release(m_state); // state blocks go before a reset
    m_fresh = false;
    m_retryAt = 0;
    // The shaders and the quad live through a reset (managed by the device).
}

void Grabber::abandonDevice() {
    // The device may be gone: its objects are forgotten, not released (a few MB left behind, once).
    m_readback = PlaneReadback();
    m_times.clear();
    m_scaled = gfx::Target();
    m_luma = gfx::Target();
    m_chroma = gfx::Target();
    m_resolve = nullptr;
    m_resolveW = m_resolveH = 0;
    m_resolveFormat = D3DFMT_UNKNOWN;
    m_psLuma = nullptr;
    m_psChroma = nullptr;
    m_quad = gfx::Quad();
    m_state = nullptr;
    m_fresh = false;
    m_retryAt = 0;
}

} // namespace clips
} // namespace tmshaders
