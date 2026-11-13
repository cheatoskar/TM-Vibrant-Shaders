#include "readback.h"
#include "gfx.h"
#include "perfcpu.h"

namespace tmshaders {

bool AsyncReadback::queue(IDirect3DDevice9* device, IDirect3DSurface9* source) {
    m_pending = false;
    if (!device || !source) return false;
    D3DSURFACE_DESC desc{};
    if (FAILED(source->GetDesc(&desc))) return false;
    if (m_memory) {
        D3DSURFACE_DESC have{};
        m_memory->GetDesc(&have);
        if (have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format) gfx::release(m_memory);
    }
    if (!m_memory && (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &m_memory, nullptr)) || !m_memory)) {
        m_memory = nullptr;
        return false;
    }
    if (FAILED(device->GetRenderTargetData(source, m_memory))) return false;
    if (!m_done) device->CreateQuery(D3DQUERYTYPE_EVENT, &m_done);
    if (m_done) m_done->Issue(D3DISSUE_END);
    m_pending = true;
    return true;
}

bool AsyncReadback::ready() const {
    if (!m_pending) return false;
    return !m_done || m_done->GetData(nullptr, 0, 0) != S_FALSE;
}

bool AsyncReadback::read(const std::function<void(const Data&)>& use) {
    if (!m_pending || !m_memory) return false;
    const bool wasReady = ready();
    m_pending = false;
    D3DLOCKED_RECT locked{};
    HRESULT hr;
    if (wasReady) {
        hr = m_memory->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    } else {
        waited++;
        perfcpu::BlockedScope blocked;
        hr = m_memory->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    }
    if (FAILED(hr)) return false;
    D3DSURFACE_DESC desc{};
    m_memory->GetDesc(&desc);
    Data data;
    data.bits = locked.pBits;
    data.pitch = locked.Pitch;
    data.width = desc.Width;
    data.height = desc.Height;
    data.format = desc.Format;
    use(data);
    m_memory->UnlockRect();
    return true;
}

void AsyncReadback::releaseQueries() {
    gfx::release(m_done);
    m_pending = false;
}

void AsyncReadback::release() {
    gfx::release(m_done);
    gfx::release(m_memory);
    m_pending = false;
}

} // namespace tmshaders
