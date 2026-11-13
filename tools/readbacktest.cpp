// AsyncReadback test (src/readback.h): a render target read back without stalling, on a plain
// Direct3D 9 device like the game's. Each round fills a target with its own colour, queues the copy,
// keeps drawing and presenting frames until the copy is done, and checks that what comes out is that
// round's colour (not a later one drawn after the queue); also a size and format change, a dropped
// copy, a copy dropped by a device reset (releaseQueries) and a read that has to wait.
//
//   tmvs_readbacktest [rounds]     (default 200; exit code 0 = passed)
#include "gfx.h"
#include "perfcpu.h"
#include "readback.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>

using namespace tmshaders;

namespace {

int g_failed = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        g_failed++;
        printf("FAIL: %s\n", what);
    }
}

D3DCOLOR colourOf(unsigned round) {
    return D3DCOLOR_XRGB(round * 37 & 0xff, round * 91 & 0xff, (round * 13 + 7) & 0xff);
}

// Every texel of the copy is `colour` (A8R8G8B8, alpha ignored).
bool allOf(const AsyncReadback::Data& d, D3DCOLOR colour) {
    if (d.format != D3DFMT_A8R8G8B8 && d.format != D3DFMT_X8R8G8B8) return false;
    for (UINT y = 0; y < d.height; y++) {
        const DWORD* row = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(d.bits) + static_cast<size_t>(d.pitch) * y);
        for (UINT x = 0; x < d.width; x++) {
            if ((row[x] & 0xffffff) != (colour & 0xffffff)) return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? atoi(argv[1]) : 200;
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TMVSReadbackTest";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"readbacktest", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* device = nullptr;
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp,
                                         &device))) {
        fprintf(stderr, "cannot create a Direct3D 9 device\n");
        return 1;
    }
    gfx::Target target, other;
    if (!target.create(device, 256, 128, D3DFMT_A8R8G8B8) || !other.create(device, 32, 16, D3DFMT_A8R8G8B8)) {
        fprintf(stderr, "cannot create the targets\n");
        return 1;
    }

    AsyncReadback read;
    expect(!read.pending() && !read.ready(), "nothing pending at first");
    expect(!read.read([](const AsyncReadback::Data&) {}), "nothing to read at first");

    unsigned right = 0, frames = 0, notReadyAtOnce = 0;
    for (int round = 1; round <= rounds; round++) {
        const D3DCOLOR colour = colourOf(static_cast<unsigned>(round));
        device->ColorFill(target.surface, nullptr, colour);
        expect(read.queue(device, target.surface), "queue");
        // Drawn over after the queue: the copy must still be this round's colour.
        device->ColorFill(target.surface, nullptr, colourOf(static_cast<unsigned>(round) + 1000));
        if (!read.ready()) notReadyAtOnce++;
        int waitedFrames = 0;
        while (!read.ready() && waitedFrames < 100) {
            device->Clear(0, nullptr, D3DCLEAR_TARGET, colour, 1.0f, 0);
            device->Present(nullptr, nullptr, nullptr, nullptr);
            waitedFrames++;
            frames++;
        }
        expect(read.ready(), "the copy is done within 100 frames");
        bool ok = false;
        const unsigned waitedBefore = read.waited;
        expect(read.read([&](const AsyncReadback::Data& d) { ok = d.width == 256 && d.height == 128 && allOf(d, colour); }), "read");
        expect(read.waited == waitedBefore, "a ready copy is read without waiting");
        expect(!read.pending(), "nothing pending after the read");
        right += ok;
    }
    expect(right == static_cast<unsigned>(rounds), "every round's own colour came out");

    // Another size: the system memory surface is made again.
    device->ColorFill(other.surface, nullptr, colourOf(5));
    expect(read.queue(device, other.surface), "queue another size");
    bool smallOk = false;
    expect(read.read([&](const AsyncReadback::Data& d) { smallOk = d.width == 32 && d.height == 16 && allOf(d, colourOf(5)); }),
           "read at once (waits if need be)");
    expect(smallOk, "the other size's colour");

    // Dropped: nothing to read.
    device->ColorFill(target.surface, nullptr, colourOf(6));
    read.queue(device, target.surface);
    read.cancel();
    expect(!read.pending() && !read.read([](const AsyncReadback::Data&) {}), "a dropped copy isn't read");

    // A device reset drops the copy in flight and the query; the next one works again.
    read.queue(device, target.surface);
    read.releaseQueries();
    expect(!read.pending(), "the reset drops the copy");
    device->ColorFill(target.surface, nullptr, colourOf(7));
    expect(read.queue(device, target.surface), "queue after the reset");
    bool again = false;
    while (!read.ready()) device->Present(nullptr, nullptr, nullptr, nullptr);
    read.read([&](const AsyncReadback::Data& d) { again = allOf(d, colourOf(7)); });
    expect(again, "the copy after the reset");

    // Read straight after the queue (it may have to wait: counted as blocked, perfcpu).
    device->ColorFill(target.surface, nullptr, colourOf(8));
    read.queue(device, target.surface);
    bool waitedOk = false;
    read.read([&](const AsyncReadback::Data& d) { waitedOk = allOf(d, colourOf(8)); });
    expect(waitedOk, "a read that had to wait still has the colour");
    printf("readbacktest: %d rounds, %u frames presented while waiting, %u copies not done at once, reads that waited: %u\n", rounds, frames,
           notReadyAtOnce, read.waited);

    read.release();
    target.destroy();
    other.destroy();
    device->Release();
    d3d->Release();
    if (g_failed) {
        printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    printf("readbacktest: all checks passed\n");
    return 0;
}
