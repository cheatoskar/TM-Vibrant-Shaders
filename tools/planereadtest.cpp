// PlaneReadback test: the video export's frames come out of the read-back exactly once each, in
// the order they were handed over, with repeats, flushes and a device reset (the queries gone) in
// between, the way plugin.cpp's ownDeliver drives it. Every frame's planes are filled with its own
// number (ColorFill) on a plain Direct3D 9 device like the game's.
//
//   tmvs_planereadtest [steps]     (default 5000; exit code 0 = passed)
#include "gfx.h"
#include "planeread.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

using namespace tmshaders;

int main(int argc, char** argv) {
    const int steps = argc > 1 ? atoi(argv[1]) : 5000;
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TMVSPlaneReadTest";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"planereadtest", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    IDirect3DDevice9* device = nullptr;
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp,
                                         &device))) {
        fprintf(stderr, "cannot create a Direct3D 9 device\n");
        return 1;
    }
    // A 256x64 video: luma 64x64 texels, chroma 64x32.
    gfx::Target luma, chroma;
    if (!luma.create(device, 64, 64, D3DFMT_A8R8G8B8) || !chroma.create(device, 64, 32, D3DFMT_A8R8G8B8)) {
        fprintf(stderr, "cannot create the planes\n");
        return 1;
    }

    PlaneReadback read;
    std::deque<unsigned> expected; // handed over, not yet out
    std::vector<uint8_t> packed;
    unsigned next = 1, last = 0, handed = 0, out = 0, wrong = 0, garbled = 0, flushes = 0, resets = 0;
    bool fresh = false;
    const auto take = [&](const PlaneReadback::Frame& f) {
        packed.resize(PlaneReadback::packedBytes(f));
        PlaneReadback::pack(f, packed.data());
        const unsigned id = *reinterpret_cast<const uint32_t*>(f.luma) & 0xffffff;
        // Every texel of both planes, packed, carries the frame's number.
        for (size_t i = 0; i + 4 <= packed.size(); i += 4) {
            if ((*reinterpret_cast<const uint32_t*>(&packed[i]) & 0xffffff) != id) {
                garbled++;
                break;
            }
        }
        out++;
        if (expected.empty() || expected.front() != id) {
            if (wrong++ < 10) fprintf(stderr, "frame %u came out, expected %u\n", id, expected.empty() ? 0u : expected.front());
        }
        if (!expected.empty()) expected.pop_front();
    };
    srand(12345);
    device->BeginScene();
    for (int step = 0; step < steps; step++) {
        const int dice = rand() % 100;
        if (dice < 70) {
            // A finished frame (sometimes replaced before the write, as with the sub-frames).
            last = next++ & 0xffffff;
            device->ColorFill(luma.surface, nullptr, D3DCOLOR_XRGB((last >> 16) & 255, (last >> 8) & 255, last & 255));
            device->ColorFill(chroma.surface, nullptr, D3DCOLOR_XRGB((last >> 16) & 255, (last >> 8) & 255, last & 255));
            fresh = true;
            if (rand() % 10 == 0) continue;
        }
        if (dice >= 97) {
            // The game's own image instead: everything on the way first (ownDeliver's false).
            while (read.pending() > 0) read.pop(take);
            flushes++;
            continue;
        }
        if (dice == 96) {
            read.releaseQueries(); // a device reset
            resets++;
        }
        // The game writes: ownQueue, then the frames that are done.
        if (fresh) {
            while (!read.canQueue() && read.pending() > 0) read.pop(take);
            if (!read.queue(device, luma, chroma, nullptr, false)) {
                fprintf(stderr, "queue failed at step %d\n", step);
                return 1;
            }
            expected.push_back(last);
        } else if (last) {
            if (!read.queueAgain()) {
                fprintf(stderr, "queueAgain failed at step %d\n", step);
                return 1;
            }
            expected.push_back(last);
        } else {
            continue;
        }
        fresh = false;
        handed++;
        while (read.pending() > 0 && read.oldestDone()) read.pop(take);
        if (step % 7 == 0) {
            // A Present now and then sends the commands (the game presents every frame).
            device->EndScene();
            device->Present(nullptr, nullptr, nullptr, nullptr);
            device->BeginScene();
        }
    }
    while (read.pending() > 0) read.pop(take);
    device->EndScene();
    printf("%u handed over, %u came out (%u flushes, %u resets), %u out of order, %u with wrong pixels, at most %d on the way, %u locks waited\n",
           handed, out, flushes, resets, wrong, garbled, read.pendingPeak, read.waited);
    read.release();
    luma.destroy();
    chroma.destroy();
    device->Release();
    d3d->Release();
    const bool ok = handed == out && wrong == 0 && garbled == 0 && expected.empty() && handed > 0;
    printf("%s\n", ok ? "PASSED" : "FAILED");
    return ok ? 0 : 1;
}
