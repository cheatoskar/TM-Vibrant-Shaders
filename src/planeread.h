#pragma once
#include "gfx.h"
#include <d3d9.h>
#include <cstdint>
#include <deque>
#include <functional>

namespace tmshaders {

// The video export's planes on their way to the encoder (VideoOwnFrames). Reading a frame back
// right after it was drawn (GetRenderTargetData, then LockRect) made the CPU wait until the GPU
// had finished the whole frame (20 ms of my 39 ms frames at 1440p with 2 x 2 samples), and the
// GPU then sat idle while the game worked out its next frame. GetRenderTargetData into system
// memory returns at once on my GTX 1080 Ti: the copy runs when the GPU gets to it, LockRect is
// where the CPU waits (measured with tools/preview.cpp, see docs/engine-video-export.md). So each
// frame's planes go into a slot of their own when the game writes the frame, and the slot is
// locked a write or two later, once an event query says its copy is done (or when every slot is
// taken). Frames come out in the order they went in, repeats included.
//
// System memory surfaces live through a device reset, the queries don't (releaseQueries): a slot
// without one counts as done. Everything on the device's thread.
class PlaneReadback {
public:
    // Three slots: one being drawn, one on its way back, one spare. At 4K in P010 a slot is 24 MB
    // of the game's 2 GB address space.
    static constexpr int kSlots = 3;

    struct Frame {
        const uint8_t* luma = nullptr;   // the luma target's texels (4 pixels each), `lumaPitch` bytes a row
        const uint8_t* chroma = nullptr; // the chroma target's (2 pixel pairs each), half the rows
        int lumaPitch = 0, chromaPitch = 0;
        UINT width = 0, height = 0;      // the luma target's size (the video is 4 x width by height)
        bool tenBit = false;             // P010 (else NV12)
        float focus = 0.0f;              // the auto focus (m) copied with it, 0 = none
    };

    // A slot is free for queue().
    bool canQueue() const;
    // The planes (and the 1x1 auto focus target, may be null) into a free slot: GetRenderTargetData
    // and an event query behind it. False: no free slot, or a copy failed (nothing queued).
    bool queue(IDirect3DDevice9* device, const gfx::Target& luma, const gfx::Target& chroma, const gfx::Target* focus, bool tenBit);
    // The frame queued last once more (the game wrote again before a new one was finished).
    bool queueAgain();
    int pending() const { return static_cast<int>(m_order.size()); }
    // The oldest pending frame's copy is done (asked without waiting).
    bool oldestDone() const;
    // Locks the oldest pending frame (waiting for its copy if need be), hands it to `use` and
    // takes it off. False: nothing pending, or the lock failed (that frame is gone).
    bool pop(const std::function<void(const Frame&)>& use);

    void releaseQueries(); // device reset
    void release();        // everything, the pending frames too

    // The planes tightly packed, Y then interleaved UV (the encoder's NV12 / P010 layout).
    static void pack(const Frame& frame, uint8_t* out);
    static size_t packedBytes(const Frame& frame);

    // For the log: time spent locking (the wait for the copy, ms), pops that found the copy
    // unfinished, most frames pending at once.
    double lockMs = 0.0;
    unsigned waited = 0;
    int pendingPeak = 0;

private:
    struct Slot {
        IDirect3DSurface9* luma = nullptr;
        IDirect3DSurface9* chroma = nullptr;
        IDirect3DSurface9* focus = nullptr;
        IDirect3DQuery9* done = nullptr;
        bool hasFocus = false;
        bool tenBit = false;
    };
    bool used(int slot) const;
    bool slotDone(const Slot& s) const;

    Slot m_slots[kSlots];
    std::deque<int> m_order; // pending slots, oldest first (a slot twice: a repeated frame)
    int m_last = -1;         // the slot queued last
};

} // namespace tmshaders
