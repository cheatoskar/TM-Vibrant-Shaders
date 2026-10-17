// Tests the F12 capture's writer thread (src/capwriter.cpp): tasks run in order and one at a time,
// busy() and drain() see them, files appear under their name only when complete, and what the
// writer writes is byte for byte what the code before it wrote on the game's thread (the .tmcap,
// the half-float images, and with --device the screenshot BMP from a real D3D9 surface, read and
// written the old way and the new way). Files go to a folder of their own (argument, default
// agent-out\capwritertest). Exit code 0 = all passed.
#include "capture.h"
#include "capwriter.h"
#include "framecap.h"
#include "log.h"
#include <d3d9.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include <windows.h>

using namespace tmshaders;

static int g_failed = 0;
#define CHECK(...) \
    do { \
        if (!(__VA_ARGS__)) { \
            printf("FAIL line %d: %s\n", __LINE__, #__VA_ARGS__); \
            g_failed++; \
        } \
    } while (0)

static std::vector<unsigned char> readAll(const std::wstring& path) {
    std::vector<unsigned char> bytes;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return bytes;
    unsigned char buffer[65536];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) bytes.insert(bytes.end(), buffer, buffer + n);
    fclose(f);
    return bytes;
}

static bool exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// --- The screenshot as src/capture.cpp wrote it before the writer thread (copied verbatim) ----
namespace old {

float halfToFloat(unsigned short h) {
    unsigned sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
    float value;
    if (exponent == 0) value = ldexpf(static_cast<float>(mantissa), -24);
    else if (exponent == 31) value = mantissa ? 0.0f : 65504.0f;
    else value = ldexpf(static_cast<float>(mantissa | 1024), static_cast<int>(exponent) - 25);
    return sign ? -value : value;
}

unsigned char toByte(float v) {
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<unsigned char>(v * 255.0f + 0.5f);
}

IDirect3DSurface9* readback(IDirect3DDevice9* device, IDirect3DSurface9* surface, D3DSURFACE_DESC& desc) {
    surface->GetDesc(&desc);
    IDirect3DSurface9* source = surface;
    IDirect3DSurface9* resolved = nullptr;
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        if (FAILED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)))
            return nullptr;
        device->StretchRect(surface, nullptr, resolved, nullptr, D3DTEXF_NONE);
        source = resolved;
    }
    IDirect3DSurface9* system = nullptr;
    if (SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &system, nullptr))) {
        if (FAILED(device->GetRenderTargetData(source, system))) {
            system->Release();
            system = nullptr;
        }
    }
    if (resolved) resolved->Release();
    return system;
}

bool saveSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface, const std::wstring& path) {
    D3DSURFACE_DESC desc{};
    IDirect3DSurface9* system = readback(device, surface, desc);
    if (!system) return false;
    D3DLOCKED_RECT locked{};
    if (FAILED(system->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        system->Release();
        return false;
    }
    const UINT w = desc.Width, h = desc.Height;
    const UINT rowBytes = (w * 3 + 3) & ~3u;
    std::vector<unsigned char> pixels(rowBytes * h);
    for (UINT y = 0; y < h; y++) {
        const BYTE* row = static_cast<const BYTE*>(locked.pBits) + static_cast<size_t>(locked.Pitch) * y;
        unsigned char* out = &pixels[static_cast<size_t>(rowBytes) * (h - 1 - y)];
        for (UINT x = 0; x < w; x++) {
            unsigned char r = 0, g = 0, b = 0;
            switch (desc.Format) {
                case D3DFMT_A8R8G8B8:
                case D3DFMT_X8R8G8B8:
                    b = row[x * 4 + 0]; g = row[x * 4 + 1]; r = row[x * 4 + 2];
                    break;
                case D3DFMT_A16B16G16R16F: {
                    const unsigned short* p = reinterpret_cast<const unsigned short*>(row) + x * 4;
                    r = toByte(halfToFloat(p[0])); g = toByte(halfToFloat(p[1])); b = toByte(halfToFloat(p[2]));
                    break;
                }
                case D3DFMT_A2R10G10B10: {
                    DWORD v = reinterpret_cast<const DWORD*>(row)[x];
                    r = static_cast<unsigned char>(((v >> 20) & 1023) >> 2);
                    g = static_cast<unsigned char>(((v >> 10) & 1023) >> 2);
                    b = static_cast<unsigned char>((v & 1023) >> 2);
                    break;
                }
                default:
                    break;
            }
            out[x * 3 + 0] = b; out[x * 3 + 1] = g; out[x * 3 + 2] = r;
        }
    }
    system->UnlockRect();
    system->Release();
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(pixels.size());
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(w);
    ih.biHeight = static_cast<LONG>(h);
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biSizeImage = static_cast<DWORD>(pixels.size());
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(pixels.data(), 1, pixels.size(), f);
    fclose(f);
    return true;
}

// plugin.cpp's writeHalfFile before it moved to framecap.cpp.
void writeHalfFile(const std::wstring& path, UINT width, UINT height, const std::vector<uint16_t>& half) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    const uint32_t header[3] = {0x31484D54u, width, height}; // "TMH1"
    fwrite(header, sizeof(header), 1, f);
    fwrite(half.data(), sizeof(uint16_t), half.size(), f);
    fclose(f);
}

} // namespace old

// The writer's order, one at a time, busy() and drain().
static void testQueue() {
    HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<int> running{0}, most{0};
    std::vector<int> order;
    SRWLOCK lock = SRWLOCK_INIT;
    CHECK(!capwriter::busy());
    CHECK(capwriter::drain(0));
    for (int i = 0; i < 5; i++) {
        capwriter::post([&, i] {
            const int now = ++running;
            if (now > most) most = now;
            if (i == 0) WaitForSingleObject(gate, 5000);
            AcquireSRWLockExclusive(&lock);
            order.push_back(i);
            ReleaseSRWLockExclusive(&lock);
            --running;
        });
    }
    CHECK(capwriter::busy());
    CHECK(!capwriter::drain(100)); // the first task waits at the gate
    SetEvent(gate);
    CHECK(capwriter::drain(5000));
    CHECK(!capwriter::busy());
    CHECK(most == 1);
    CHECK(order == std::vector<int>({0, 1, 2, 3, 4}));
    // A task that throws doesn't take the writer down.
    capwriter::post([] { throw std::bad_alloc(); });
    bool after = false;
    capwriter::post([&after] { after = true; });
    CHECK(capwriter::drain(5000));
    CHECK(after);
    CloseHandle(gate);
}

// Files appear complete or not at all.
static void testWriteVia(const std::wstring& dir) {
    const std::wstring good = dir + L"\\good.bin", bad = dir + L"\\bad.bin";
    DeleteFileW(good.c_str());
    DeleteFileW(bad.c_str());
    bool sawPartOnly = false;
    CHECK(capwriter::writeVia(good, [&](const std::wstring& path) {
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) return false;
        fputs("complete", f);
        fclose(f);
        sawPartOnly = exists(good + L".part") && !exists(good);
        return true;
    }));
    CHECK(sawPartOnly);
    CHECK(exists(good) && !exists(good + L".part"));
    CHECK(!capwriter::writeVia(bad, [](const std::wstring& path) {
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (f) fclose(f);
        return false;
    }));
    CHECK(!exists(bad) && !exists(bad + L".part"));
    // Leftovers of a session that was cut off.
    for (const wchar_t* name : {L"\\capture_900.tmcap.part", L"\\capture_900.hdr16.part", L"\\screen_900.bmp.part"}) {
        FILE* f = _wfopen((dir + name).c_str(), L"wb");
        if (f) fclose(f);
    }
    CHECK(capwriter::removeUnfinished(dir, L"capture_*.part") == 2);
    CHECK(capwriter::removeUnfinished(dir, L"screen_*.part") == 1);
    CHECK(exists(good));
}

// The .tmcap and the half-float images: the old direct write and the writer's are the same bytes,
// and the previewer's FrameCapture::load reads the writer's back exactly.
static void testFormats(const std::wstring& dir) {
    FrameCapture cap;
    cap.width = 640;
    cap.height = 360;
    for (int i = 0; i < 16; i++) cap.view[i] = 0.25f * i, cap.projection[i] = -0.5f * i;
    cap.sunDirection[0] = 0.3f, cap.sunDirection[3] = 1.0f;
    cap.sunColor[1] = 0.9f, cap.sunColor[3] = 1.0f;
    cap.time = 12.5f;
    cap.color.resize(640 * 360);
    cap.depth.resize(640 * 360);
    std::vector<uint16_t> half(640 * 360 * 4);
    for (size_t i = 0; i < cap.color.size(); i++) {
        cap.color[i] = static_cast<uint32_t>(i * 2654435761u);
        cap.depth[i] = static_cast<float>(i) / cap.color.size();
    }
    for (size_t i = 0; i < half.size(); i++) half[i] = static_cast<uint16_t>(i * 40503u);
    const std::wstring oldCap = dir + L"\\old.tmcap", newCap = dir + L"\\new.tmcap";
    const std::wstring oldHalf = dir + L"\\old.hdr16", newHalf = dir + L"\\new.hdr16";
    cap.save(oldCap);
    old::writeHalfFile(oldHalf, 640, 360, half);
    capwriter::post([&] {
        capwriter::writeVia(newCap, [&](const std::wstring& path) { return cap.save(path); });
        capwriter::writeVia(newHalf, [&](const std::wstring& path) { return writeHalfFile(path, 640, 360, half); });
    });
    CHECK(capwriter::drain(10000));
    const auto a = readAll(oldCap), b = readAll(newCap);
    CHECK(!a.empty() && a == b);
    const auto c = readAll(oldHalf), d = readAll(newHalf);
    CHECK(!c.empty() && c == d);
    FrameCapture back;
    CHECK(back.load(newCap));
    CHECK(back.width == 640 && back.height == 360 && back.color == cap.color && back.depth == cap.depth && back.time == cap.time);
    printf("formats: .tmcap %zu bytes, .hdr16 %zu bytes, old == new\n", b.size(), d.size());
}

// The screenshot from a real D3D9 surface: the old saveSurface and readSurface + writeBmp on the
// writer give the same BMP, for the 8-bit and the half-float target.
static void testBmp(const std::wstring& dir) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"capwritertest";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(L"capwritertest", L"capwritertest", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    IDirect3DDevice9* device = nullptr;
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device))) {
        printf("FAIL: no D3D9 device\n");
        g_failed++;
        return;
    }
    const D3DFORMAT formats[] = {D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_A16B16G16R16F, D3DFMT_A2R10G10B10};
    for (D3DFORMAT format : formats) {
        IDirect3DSurface9* target = nullptr;
        if (FAILED(device->CreateRenderTarget(333, 187, format, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr))) {
            printf("format %d: no render target here, skipped\n", static_cast<int>(format));
            continue;
        }
        // Bands of colours (odd width: the BMP rows are padded).
        for (int i = 0; i < 12; i++) {
            const RECT r = {i * 28, 0, i * 28 + 28, 187};
            device->ColorFill(target, &r, D3DCOLOR_ARGB(255, (i * 53) & 255, (i * 97 + 30) & 255, (i * 151 + 7) & 255));
        }
        const std::wstring oldBmp = dir + L"\\old_" + std::to_wstring(format) + L".bmp";
        const std::wstring newBmp = dir + L"\\screen_" + std::to_wstring(format) + L".bmp";
        CHECK(old::saveSurface(device, target, oldBmp));
        auto pixels = std::make_shared<capture::Pixels>();
        CHECK(capture::readSurface(device, target, *pixels));
        target->Release();
        capwriter::post([pixels, newBmp] {
            capwriter::writeVia(newBmp, [&](const std::wstring& path) { return capture::writeBmp(*pixels, path); });
        });
        CHECK(capwriter::drain(10000));
        const auto a = readAll(oldBmp), b = readAll(newBmp);
        CHECK(!a.empty() && a == b);
        printf("format %d: BMP %zu bytes, old == new: %s\n", static_cast<int>(format), b.size(), !a.empty() && a == b ? "yes" : "NO");
    }
    // What stays on the game's thread: reading back one 2560x1440 target (the sysmem copy, the
    // GPU -> memory transfer and the copy out), per format.
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    for (D3DFORMAT format : {D3DFMT_X8R8G8B8, D3DFMT_A16B16G16R16F}) {
        IDirect3DSurface9* target = nullptr;
        if (FAILED(device->CreateRenderTarget(2560, 1440, format, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr))) continue;
        device->ColorFill(target, nullptr, D3DCOLOR_ARGB(255, 10, 20, 30));
        for (int run = 0; run < 3; run++) {
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            capture::Pixels pixels;
            capture::readSurface(device, target, pixels);
            QueryPerformanceCounter(&t1);
            printf("readback 2560x1440 format %d: %.1f ms (%zu MB)\n", static_cast<int>(format),
                   static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart, pixels.bytes.size() >> 20);
        }
        target->Release();
    }
    device->Release();
    d3d->Release();
    DestroyWindow(window);
}

// What a 2560x1440 capture costs: the writer's time for the files of one (all float images).
static void bench(const std::wstring& dir) {
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    FrameCapture cap;
    cap.width = 2560;
    cap.height = 1440;
    cap.color.assign(2560 * 1440, 0x80402010u);
    cap.depth.assign(2560 * 1440, 0.5f);
    std::vector<uint16_t> half(2560 * 1440 * 4, 0x3c00);
    QueryPerformanceCounter(&t0);
    capwriter::post([&] {
        capwriter::writeVia(dir + L"\\bench.tmcap", [&](const std::wstring& path) { return cap.save(path); });
        for (const wchar_t* ext : {L".hdr16", L".add16", L".nrm16", L".lit16"})
            capwriter::writeVia(dir + L"\\bench" + ext, [&](const std::wstring& path) { return writeHalfFile(path, 2560, 1440, half); });
    });
    LARGE_INTEGER posted;
    QueryPerformanceCounter(&posted);
    CHECK(capwriter::drain(60000));
    QueryPerformanceCounter(&t1);
    printf("bench 2560x1440: post %.2f ms on the caller, %.0f ms on the writer for 146 MB\n",
           static_cast<double>(posted.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart, static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    for (const wchar_t* ext : {L".tmcap", L".hdr16", L".add16", L".nrm16", L".lit16"}) DeleteFileW((dir + L"\\bench" + ext).c_str());
}

int main(int argc, char** argv) {
    log::setFileName(L"tmvs_capwritertest.log"); // never the game's log
    std::wstring dir = L"agent-out\\capwritertest";
    bool device = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--device")) device = true;
        else {
            wchar_t wide[MAX_PATH] = {};
            MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, wide, MAX_PATH);
            dir = wide;
        }
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    testQueue();
    testWriteVia(dir);
    testFormats(dir);
    if (device) testBmp(dir);
    bench(dir);
    printf(g_failed ? "%d FAILED\n" : "all passed\n", g_failed);
    return g_failed ? 1 : 0;
}
