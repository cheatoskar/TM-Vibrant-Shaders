#include "planeread.h"
#include <algorithm>
#include <cstring>

namespace tmshaders {
namespace {

double nowMs() {
    static LARGE_INTEGER frequency{};
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

// A system memory surface of the target's size and format (made again when either changed).
bool fit(IDirect3DDevice9* device, IDirect3DSurface9*& surface, UINT width, UINT height, D3DFORMAT format) {
    if (surface) {
        D3DSURFACE_DESC desc{};
        surface->GetDesc(&desc);
        if (desc.Width == width && desc.Height == height && desc.Format == format) return true;
        gfx::release(surface);
    }
    return SUCCEEDED(device->CreateOffscreenPlainSurface(width, height, format, D3DPOOL_SYSTEMMEM, &surface, nullptr)) && surface;
}

} // namespace

bool PlaneReadback::used(int slot) const {
    return std::find(m_order.begin(), m_order.end(), slot) != m_order.end();
}

bool PlaneReadback::canQueue() const {
    for (int i = 0; i < kSlots; i++) {
        if (!used(i) && i != m_last) return true;
    }
    return false;
}

bool PlaneReadback::queue(IDirect3DDevice9* device, const gfx::Target& luma, const gfx::Target& chroma, const gfx::Target* focus, bool tenBit) {
    if (!device || !luma.valid() || !chroma.valid()) return false;
    // The slot after the one queued last: the one queued last stays intact for queueAgain.
    int slot = -1;
    for (int k = 1; k <= kSlots && slot < 0; k++) {
        const int i = (m_last + k + kSlots) % kSlots;
        if (!used(i) && i != m_last) slot = i;
    }
    if (slot < 0) return false;
    Slot& s = m_slots[slot];
    if (!fit(device, s.luma, luma.width, luma.height, luma.format) || !fit(device, s.chroma, chroma.width, chroma.height, chroma.format)) return false;
    if (FAILED(device->GetRenderTargetData(luma.surface, s.luma)) || FAILED(device->GetRenderTargetData(chroma.surface, s.chroma))) return false;
    s.hasFocus = focus && focus->valid() && fit(device, s.focus, 1, 1, focus->format) && SUCCEEDED(device->GetRenderTargetData(focus->surface, s.focus));
    if (!s.done) device->CreateQuery(D3DQUERYTYPE_EVENT, &s.done);
    if (s.done) s.done->Issue(D3DISSUE_END);
    s.tenBit = tenBit;
    m_order.push_back(slot);
    m_last = slot;
    pendingPeak = std::max(pendingPeak, pending());
    return true;
}

bool PlaneReadback::queueAgain() {
    if (m_last < 0 || !m_slots[m_last].luma || !m_slots[m_last].chroma) return false;
    m_order.push_back(m_last);
    pendingPeak = std::max(pendingPeak, pending());
    return true;
}

bool PlaneReadback::slotDone(const Slot& s) const {
    // Lost device or no query (after a reset): the lock tells.
    // Without D3DGETDATA_FLUSH: with it the call itself waited for the GPU on my card (23 ms of a
    // 25 ms frame in tools/preview.cpp --export), the very stall this is here to avoid. The game's
    // Present sends the commands on every frame.
    return !s.done || s.done->GetData(nullptr, 0, 0) != S_FALSE;
}

bool PlaneReadback::oldestDone() const {
    return !m_order.empty() && slotDone(m_slots[m_order.front()]);
}

bool PlaneReadback::pop(const std::function<void(const Frame&)>& use) {
    if (m_order.empty()) return false;
    Slot& s = m_slots[m_order.front()];
    m_order.pop_front();
    if (!slotDone(s)) waited++;
    const double start = nowMs();
    D3DLOCKED_RECT y{}, uv{}, f{};
    if (!s.luma || !s.chroma || FAILED(s.luma->LockRect(&y, nullptr, D3DLOCK_READONLY))) return false;
    if (FAILED(s.chroma->LockRect(&uv, nullptr, D3DLOCK_READONLY))) {
        s.luma->UnlockRect();
        return false;
    }
    Frame frame;
    if (s.hasFocus && s.focus && SUCCEEDED(s.focus->LockRect(&f, nullptr, D3DLOCK_READONLY))) {
        frame.focus = *static_cast<const float*>(f.pBits);
        s.focus->UnlockRect();
        if (!(frame.focus > 0.0f && frame.focus < 1e6f)) frame.focus = 0.0f;
    }
    lockMs += nowMs() - start;
    D3DSURFACE_DESC desc{};
    s.luma->GetDesc(&desc);
    frame.luma = static_cast<const uint8_t*>(y.pBits);
    frame.chroma = static_cast<const uint8_t*>(uv.pBits);
    frame.lumaPitch = y.Pitch;
    frame.chromaPitch = uv.Pitch;
    frame.width = desc.Width;
    frame.height = desc.Height;
    frame.tenBit = s.tenBit;
    use(frame);
    s.chroma->UnlockRect();
    s.luma->UnlockRect();
    return true;
}

void PlaneReadback::releaseQueries() {
    for (Slot& s : m_slots) gfx::release(s.done);
}

void PlaneReadback::release() {
    for (Slot& s : m_slots) {
        gfx::release(s.luma);
        gfx::release(s.chroma);
        gfx::release(s.focus);
        gfx::release(s.done);
        s.hasFocus = false;
    }
    m_order.clear();
    m_last = -1;
}

size_t PlaneReadback::packedBytes(const Frame& frame) {
    const size_t row = static_cast<size_t>(frame.width) * 4 * (frame.tenBit ? 2 : 1);
    return row * frame.height + row * (frame.height / 2);
}

void PlaneReadback::pack(const Frame& frame, uint8_t* out) {
    const size_t row = static_cast<size_t>(frame.width) * 4 * (frame.tenBit ? 2 : 1);
    for (UINT r = 0; r < frame.height; r++) memcpy(out + row * r, frame.luma + static_cast<size_t>(frame.lumaPitch) * r, row);
    uint8_t* chroma = out + row * frame.height;
    for (UINT r = 0; r < frame.height / 2; r++) memcpy(chroma + row * r, frame.chroma + static_cast<size_t>(frame.chromaPitch) * r, row);
}

} // namespace tmshaders
