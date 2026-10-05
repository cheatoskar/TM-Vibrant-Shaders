// Offline previewer: renders recorded game frames (.tmcap) through the shader pipeline.
//
//   tmvs_preview <capture.tmcap> <out.bmp> [options]
//     --preset <name|index>   Vibrant, Cinematic, Balanced, Competition, Performance
//     --set Key=Value         override a setting (keys as in settings.ini), repeatable
//     --debug <n>             debug view (1 depth, 2 normals, 3 AO, 4 shadows, 5 shafts, 6 bloom)
//     --shaders <dir>         load tmvs.hlsl from <dir> instead of the embedded copy
//     --before <file.bmp>     also write the unprocessed frame
//     --sun <x,y,z>           override the sun direction (world space, towards the sun)
//     --suncolor <r,g,b>      override the game's light colour
#include "config.h"
#include "framecap.h"
#include "gfx.h"
#include "pipeline.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

using namespace tmshaders;

namespace {

std::wstring widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

bool writeBmp(const char* path, const std::vector<uint32_t>& pixels, uint32_t w, uint32_t h) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const uint32_t rowBytes = (w * 3 + 3) & ~3u;
    std::vector<unsigned char> data(static_cast<size_t>(rowBytes) * h);
    for (uint32_t y = 0; y < h; y++) {
        unsigned char* out = &data[static_cast<size_t>(rowBytes) * (h - 1 - y)];
        for (uint32_t x = 0; x < w; x++) {
            uint32_t p = pixels[static_cast<size_t>(y) * w + x];
            out[x * 3 + 0] = static_cast<unsigned char>(p & 0xFF);
            out[x * 3 + 1] = static_cast<unsigned char>((p >> 8) & 0xFF);
            out[x * 3 + 2] = static_cast<unsigned char>((p >> 16) & 0xFF);
        }
    }
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(data.size());
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(w);
    ih.biHeight = static_cast<LONG>(h);
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biSizeImage = static_cast<DWORD>(data.size());
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}

template <typename T>
IDirect3DTexture9* upload(IDirect3DDevice9* device, uint32_t w, uint32_t h, D3DFORMAT format, const std::vector<T>& pixels) {
    // D3D9Ex has no managed pool: fill a system-memory copy and upload it.
    IDirect3DTexture9* staging = nullptr;
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(device->CreateTexture(w, h, 1, 0, format, D3DPOOL_SYSTEMMEM, &staging, nullptr))) return nullptr;
    D3DLOCKED_RECT locked{};
    staging->LockRect(0, &locked, nullptr, 0);
    for (uint32_t y = 0; y < h; y++) {
        memcpy(static_cast<BYTE*>(locked.pBits) + static_cast<size_t>(locked.Pitch) * y, &pixels[static_cast<size_t>(y) * w],
               w * sizeof(T));
    }
    staging->UnlockRect(0);
    if (SUCCEEDED(device->CreateTexture(w, h, 1, 0, format, D3DPOOL_DEFAULT, &texture, nullptr))) {
        device->UpdateTexture(staging, texture);
    }
    staging->Release();
    return texture;
}

bool setByKey(Settings& s, const char* assignment) {
    const char* eq = strchr(assignment, '=');
    if (!eq) return false;
    std::string key(assignment, eq);
    const char* value = eq + 1;
    char* base = reinterpret_cast<char*>(&s);
    if (key == "DebugView") {
        s.debugView = atoi(value);
        return true;
    }
    for (const Field& f : fields()) {
        if (key != f.key) continue;
        if (f.kind == Field::Bool) *reinterpret_cast<bool*>(base + f.offset) = atoi(value) != 0;
        else if (f.kind == Field::Int) *reinterpret_cast<int*>(base + f.offset) = atoi(value);
        else if (f.kind == Field::Color) {
            float* c = reinterpret_cast<float*>(base + f.offset);
            sscanf(value, "%f,%f,%f", &c[0], &c[1], &c[2]);
        } else *reinterpret_cast<float*>(base + f.offset) = static_cast<float>(atof(value));
        return true;
    }
    fprintf(stderr, "unknown setting %s\n", key.c_str());
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: tmvs_preview <capture.tmcap> <out.bmp> [--preset name] [--set Key=Value] [--debug n] "
                        "[--shaders dir] [--before file.bmp] [--sun x,y,z]\n");
        return 2;
    }

    FrameCapture cap;
    if (!cap.load(widen(argv[1]))) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    Settings settings;
    applyPreset(settings, Preset::Vibrant);
    std::wstring shaderDir;
    const char* before = nullptr;
    std::vector<const char*> overrides;
    bool sunOverride = false, sunColorOverride = false, bench = false;
    float sun[3] = {}, sunColor[3] = {};
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--preset") && i + 1 < argc) {
            const char* name = argv[++i];
            for (int p = 0; p < static_cast<int>(Preset::Custom); p++) {
                if (!_stricmp(name, presetName(static_cast<Preset>(p))) || (isdigit(static_cast<unsigned char>(name[0])) && atoi(name) == p)) {
                    applyPreset(settings, static_cast<Preset>(p));
                    break;
                }
            }
        } else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
            overrides.push_back(argv[++i]);
        } else if (!strcmp(argv[i], "--debug") && i + 1 < argc) {
            settings.debugView = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--shaders") && i + 1 < argc) {
            shaderDir = widen(argv[++i]);
        } else if (!strcmp(argv[i], "--bench")) {
            bench = true;
        } else if (!strcmp(argv[i], "--before") && i + 1 < argc) {
            before = argv[++i];
        } else if (!strcmp(argv[i], "--sun") && i + 1 < argc) {
            sunOverride = sscanf(argv[++i], "%f,%f,%f", &sun[0], &sun[1], &sun[2]) == 3;
        } else if (!strcmp(argv[i], "--suncolor") && i + 1 < argc) {
            sunColorOverride = sscanf(argv[++i], "%f,%f,%f", &sunColor[0], &sunColor[1], &sunColor[2]) == 3;
        }
    }
    for (const char* o : overrides) setByKey(settings, o);

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TMVSPreview";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"preview", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);

    // D3D9Ex: keeps working while a fullscreen game owns the display.
    IDirect3D9Ex* d3d = nullptr;
    Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    IDirect3DDevice9Ex* device = nullptr;
    if (!d3d || FAILED(d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                           D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, nullptr, &device))) {
        fprintf(stderr, "cannot create Direct3D 9 device\n");
        return 1;
    }

    IDirect3DTexture9* color = upload(device, cap.width, cap.height, D3DFMT_A8R8G8B8, cap.color);
    IDirect3DTexture9* depthTex = upload(device, cap.width, cap.height, D3DFMT_R32F, cap.depth);
    gfx::Target output;
    if (!color || !depthTex || !output.create(device, cap.width, cap.height, D3DFMT_A8R8G8B8)) {
        fprintf(stderr, "cannot create textures\n");
        return 1;
    }

    Pipeline pipeline;
    if (!pipeline.init(device, shaderDir)) {
        fprintf(stderr, "shader compilation failed:\n%s\n", pipeline.lastError().c_str());
        return 1;
    }

    Pipeline::Inputs in;
    in.color = color;
    in.depth = depthTex;
    in.width = cap.width;
    in.height = cap.height;
    memcpy(in.view, cap.view, sizeof(in.view));
    memcpy(in.projection, cap.projection, sizeof(in.projection));
    if (sunOverride) {
        memcpy(in.sunDirection, sun, sizeof(sun));
        in.sunKnown = true;
    } else {
        memcpy(in.sunDirection, cap.sunDirection, sizeof(float) * 3);
        in.sunKnown = cap.sunDirection[3] > 0.5f;
    }

    if (sunColorOverride) {
        memcpy(in.sunColor, sunColor, sizeof(sunColor));
        in.sunColorKnown = true;
    } else {
        memcpy(in.sunColor, cap.sunColor, sizeof(float) * 3);
        in.sunColorKnown = cap.sunColor[3] > 0.5f;
    }

    // Several frames so auto exposure settles like it would in game.
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    pipeline.setProfiling(bench);
    const int frames = bench ? 200 : 40;
    device->BeginScene();
    for (int frame = 0; frame < frames; frame++) {
        in.time = cap.time + frame * 0.1f;
        if (frame == frames - 1) QueryPerformanceCounter(&t0);
        pipeline.render(device, in, settings, output.surface);
        if (bench) pipeline.collectProfile(true);
    }
    device->EndScene();
    if (bench) {
        printf("GPU time per pass (ms, %ux%u):\n", cap.width, cap.height);
        for (int p = 0; p < pipeline.passCount(); p++) {
            if (pipeline.passTime(p) > 0.0f) printf("  %-16s %6.3f\n", Pipeline::passName(p), pipeline.passTime(p));
        }
        printf("  %-16s %6.3f\n", "TOTAL", pipeline.totalTime());
    }

    std::vector<uint32_t> pixels;
    if (!pipeline.readColor(device, output.surface, pixels)) {
        fprintf(stderr, "readback failed\n");
        return 1;
    }
    QueryPerformanceCounter(&t1);
    writeBmp(argv[2], pixels, cap.width, cap.height);
    if (before) writeBmp(before, cap.color, cap.width, cap.height);
    printf("%s: %ux%u, sun %s (%.3f %.3f %.3f), last frame incl. readback %.1f ms\n", argv[2], cap.width, cap.height,
           in.sunKnown ? "known" : "unknown", in.sunDirection[0], in.sunDirection[1], in.sunDirection[2],
           1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart));

    output.destroy();
    pipeline.release();
    color->Release();
    depthTex->Release();
    device->Release();
    d3d->Release();
    DestroyWindow(window);
    return 0;
}
