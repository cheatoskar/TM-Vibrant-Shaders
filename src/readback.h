#pragma once
#include <d3d9.h>
#include <functional>

namespace tmshaders {

// A render target read back without stalling the game's thread. GetRenderTargetData followed by
// LockRect in the same frame makes the CPU wait until the GPU has worked through everything queued
// before the copy (the whole frame so far); the GPU then sits idle while the game prepares its next
// frame. Here the copy into system memory is queued right after the target was drawn, with an event
// query behind it, and the memory is locked frames later, once the query says the copy is done
// (asked without D3DGETDATA_FLUSH: with it the call itself waited for the GPU on my card; the game's
// Present sends the commands every frame). See planeread.h for the same scheme with a ring.
//
// One copy in flight at a time; the system memory surface is kept for the next copy (made again
// when the size or format changes). It lives through a device reset, the query doesn't
// (releaseQueries also drops a copy in flight). Everything on the device's thread.
class AsyncReadback {
public:
    struct Data {
        const void* bits = nullptr;
        int pitch = 0;
        UINT width = 0, height = 0;
        D3DFORMAT format = D3DFMT_UNKNOWN;
    };

    // Queues the copy of `source` (a render target, not multisampled). Replaces a copy in flight.
    // False: no copy (nothing pending then).
    bool queue(IDirect3DDevice9* device, IDirect3DSurface9* source);
    bool pending() const { return m_pending; }
    // The copy in flight is done (asked without waiting). Without a query (CreateQuery failed): true
    // (read() then waits, counted as blocked).
    bool ready() const;
    // Locks the copy (waiting for it if it isn't ready: counted as blocked time, perfcpu) and hands
    // it to `use`; then nothing is pending. False: nothing pending or the lock failed.
    bool read(const std::function<void(const Data&)>& use);
    // Drops the copy in flight without reading it.
    void cancel() { m_pending = false; }

    void releaseQueries(); // before a device reset (drops the copy in flight)
    void release();        // everything

    // For tests and the log: reads that found the copy unfinished.
    unsigned waited = 0;

private:
    IDirect3DSurface9* m_memory = nullptr;
    IDirect3DQuery9* m_done = nullptr;
    bool m_pending = false;
};

} // namespace tmshaders
