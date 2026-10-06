// Offline previewer: renders recorded game frames (.tmcap) through the shader pipeline.
//
//   tmvs_preview <capture.tmcap> <out.bmp> [options]
//     --preset <name|index>   Vibrant, Cinematic, Golden Hour, Competition, Performance, ...
//     --set Key=Value         override a setting (keys as in settings.ini), repeatable
//     --debug <n>             debug view (1 depth, 2 normals, 3 AO, 4 shadows, 5 shafts, 6 bloom)
//     --shaders <dir>         load tmvs.hlsl from <dir> instead of the embedded copy
//     --before <file.bmp>     also write the unprocessed frame
//     --sun <x,y,z>           override the sun direction (world space, towards the sun)
//     --suncolor <r,g,b>      override the game's light colour
//     --move <x,y,z>          previous frame's camera offset in metres (motion blur test)
//     --drive <x,y,z>         camera motion per frame (camera axes, m) towards the captured
//                             view, 0.1 s apart (neon trail test)
//     --bench                 GPU time per pass
//     --batch <jobs.txt>      many images in one run (shaders compile once); each line:
//                             out.bmp [cap=file.tmcap] [preset=Golden_Hour] [move=x,y,z] [Key=Value ...]
#include "config.h"
#include "framecap.h"
#include "gfx.h"
#include "log.h"
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

bool applyPresetByName(Settings& settings, const char* name) {
    for (int p = 0; p < static_cast<int>(Preset::Custom); p++) {
        if (!_stricmp(name, presetName(static_cast<Preset>(p))) || (isdigit(static_cast<unsigned char>(name[0])) && atoi(name) == p)) {
            applyPreset(settings, static_cast<Preset>(p));
            return true;
        }
    }
    fprintf(stderr, "unknown preset %s\n", name);
    return false;
}

// One image to render: capture, settings, output file.
struct Job {
    std::string capture;
    std::string output;
    Settings settings;
    float move[3] = {}; // previous frame's camera offset (world), for motion blur tests
    float drive[3] = {}; // camera motion per frame (camera axes), for trail tests
    float time = -1.0f; // scene time of the last frame (s), -1 = the capture's
};

struct LoadedCapture {
    std::string path;
    FrameCapture cap;
    IDirect3DTexture9* color = nullptr;
    IDirect3DTexture9* depth = nullptr;
};

// Batch file: one job per line, "out.bmp [cap=file.tmcap] [preset=Name] [Key=Value ...]".
// Lines starting with # are comments. Without cap= the command line capture is used.
bool readBatch(const char* path, const Job& defaults, std::vector<Job>& jobs) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        std::vector<std::string> words;
        for (char* tok = strtok(line, " \t\r\n"); tok; tok = strtok(nullptr, " \t\r\n")) words.push_back(tok);
        if (words.empty() || words[0][0] == '#') continue;
        Job job = defaults;
        job.output = words[0];
        for (size_t i = 1; i < words.size(); i++) {
            const std::string& w = words[i];
            if (!_strnicmp(w.c_str(), "cap=", 4)) job.capture = w.substr(4);
            else if (!_strnicmp(w.c_str(), "preset=", 7)) {
                std::string name = w.substr(7);
                for (char& c : name) if (c == '_') c = ' ';
                applyPresetByName(job.settings, name.c_str());
            } else if (!_strnicmp(w.c_str(), "move=", 5)) sscanf(w.c_str() + 5, "%f,%f,%f", &job.move[0], &job.move[1], &job.move[2]);
            else if (!_strnicmp(w.c_str(), "drive=", 6)) sscanf(w.c_str() + 6, "%f,%f,%f", &job.drive[0], &job.drive[1], &job.drive[2]);
            else if (!_strnicmp(w.c_str(), "time=", 5)) job.time = static_cast<float>(atof(w.c_str() + 5));
            else setByKey(job.settings, w.c_str());
        }
        jobs.push_back(job);
    }
    fclose(f);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    tmshaders::log::setFileName(L"preview.log"); // never truncate the game's tmvs.log
    if (argc < 3) {
        fprintf(stderr, "usage: tmvs_preview <capture.tmcap> <out.bmp> [--preset name] [--set Key=Value] [--debug n] "
                        "[--shaders dir] [--before file.bmp] [--sun x,y,z] [--suncolor r,g,b] [--move x,y,z] [--time s] [--water y] [--bench] "
                        "[--batch jobs.txt]\n");
        return 2;
    }

    float waterHeight = -1e30f; // world height of the map's water (the game reports it in game)
    Job defaults;
    defaults.capture = argv[1];
    defaults.output = argv[2];
    applyPreset(defaults.settings, Preset::Vibrant);
    std::wstring shaderDir;
    const char* before = nullptr;
    const char* batch = nullptr;
    std::vector<const char*> overrides;
    bool sunOverride = false, sunColorOverride = false, bench = false;
    float sun[3] = {}, sunColor[3] = {};
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--preset") && i + 1 < argc) {
            applyPresetByName(defaults.settings, argv[++i]);
        } else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
            overrides.push_back(argv[++i]);
        } else if (!strcmp(argv[i], "--debug") && i + 1 < argc) {
            defaults.settings.debugView = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--shaders") && i + 1 < argc) {
            shaderDir = widen(argv[++i]);
        } else if (!strcmp(argv[i], "--bench")) {
            bench = true;
        } else if (!strcmp(argv[i], "--before") && i + 1 < argc) {
            before = argv[++i];
        } else if (!strcmp(argv[i], "--batch") && i + 1 < argc) {
            batch = argv[++i];
        } else if (!strcmp(argv[i], "--water") && i + 1 < argc) {
            waterHeight = static_cast<float>(atof(argv[++i]));
        } else if (!strcmp(argv[i], "--time") && i + 1 < argc) {
            defaults.time = static_cast<float>(atof(argv[++i]));
        } else if (!strcmp(argv[i], "--drive") && i + 1 < argc) {
            sscanf(argv[++i], "%f,%f,%f", &defaults.drive[0], &defaults.drive[1], &defaults.drive[2]);
        } else if (!strcmp(argv[i], "--move") && i + 1 < argc) {
            sscanf(argv[++i], "%f,%f,%f", &defaults.move[0], &defaults.move[1], &defaults.move[2]);
        } else if (!strcmp(argv[i], "--sun") && i + 1 < argc) {
            sunOverride = sscanf(argv[++i], "%f,%f,%f", &sun[0], &sun[1], &sun[2]) == 3;
        } else if (!strcmp(argv[i], "--suncolor") && i + 1 < argc) {
            sunColorOverride = sscanf(argv[++i], "%f,%f,%f", &sunColor[0], &sunColor[1], &sunColor[2]) == 3;
        }
    }
    for (const char* o : overrides) setByKey(defaults.settings, o);
    std::vector<Job> jobs;
    if (batch) {
        if (!readBatch(batch, defaults, jobs)) {
            fprintf(stderr, "cannot read %s\n", batch);
            return 1;
        }
    } else {
        jobs.push_back(defaults);
    }

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

    Pipeline pipeline;
    if (!pipeline.init(device, shaderDir)) {
        fprintf(stderr, "shader compilation failed:\n%s\n", pipeline.lastError().c_str());
        return 1;
    }

    std::vector<LoadedCapture*> captures;
    auto loadCapture = [&](const std::string& path) -> LoadedCapture* {
        for (LoadedCapture* c : captures) {
            if (c->path == path) return c;
        }
        LoadedCapture* c = new LoadedCapture;
        c->path = path;
        if (!c->cap.load(widen(path.c_str()))) {
            fprintf(stderr, "cannot read %s\n", path.c_str());
            delete c;
            return nullptr;
        }
        c->color = upload(device, c->cap.width, c->cap.height, D3DFMT_A8R8G8B8, c->cap.color);
        c->depth = upload(device, c->cap.width, c->cap.height, D3DFMT_R32F, c->cap.depth);
        captures.push_back(c);
        return c;
    };

    pipeline.setProfiling(bench);
    int failures = 0;
    for (const Job& job : jobs) {
        LoadedCapture* loaded = loadCapture(job.capture);
        gfx::Target output;
        if (!loaded || !loaded->color || !loaded->depth || !output.create(device, loaded->cap.width, loaded->cap.height, D3DFMT_A8R8G8B8)) {
            failures++;
            continue;
        }
        const FrameCapture& cap = loaded->cap;
        Pipeline::Inputs in;
        in.color = loaded->color;
        in.depth = loaded->depth;
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
        // The previous frame's camera, moved by job.move (world space): view translation
        // t' = t - move * R.
        if (waterHeight > -1e29f) {
            in.water[0] = in.water[1] = waterHeight;
            in.water[2] = 1.0f;
        }
        Pipeline::Inputs moved = in;
        for (int j = 0; j < 3; j++) {
            moved.view[12 + j] -= job.move[0] * in.view[0 * 4 + j] + job.move[1] * in.view[1 * 4 + j] + job.move[2] * in.view[2 * 4 + j];
        }
        const bool moving = job.move[0] != 0.0f || job.move[1] != 0.0f || job.move[2] != 0.0f;

        // Several frames so auto exposure and TAA settle like they would in game.
        LARGE_INTEGER t0{}, t1{}, freq{};
        QueryPerformanceFrequency(&freq);
        pipeline.resetHistory();
        const int frames = bench ? 200 : 40;
        device->BeginScene();
        for (int frame = 0; frame < frames; frame++) {
            const bool previousFrame = moving && frame == frames - 2;
            Pipeline::Inputs& frameIn = previousFrame ? moved : in;
            Pipeline::Inputs driven = frameIn;
            const float back = static_cast<float>(frames - 1 - frame);
            // The view translation is in camera axes: earlier frames see everything shifted by
            // the distance still to drive.
            for (int j = 0; j < 3; j++) driven.view[12 + j] += back * job.drive[j];
            driven.time = job.time >= 0.0f ? job.time - (frames - 1 - frame) * 0.02f : cap.time + frame * 0.1f;
            if (frame == frames - 1) QueryPerformanceCounter(&t0);
            pipeline.render(device, driven, job.settings, output.surface);
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
            failures++;
            continue;
        }
        QueryPerformanceCounter(&t1);
        writeBmp(job.output.c_str(), pixels, cap.width, cap.height);
        printf("%s: %ux%u, sun %s (%.3f %.3f %.3f), last frame incl. readback %.1f ms\n", job.output.c_str(), cap.width, cap.height,
               in.sunKnown ? "known" : "unknown", in.sunDirection[0], in.sunDirection[1], in.sunDirection[2],
               1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart));
        output.destroy();
    }
    if (before && !captures.empty()) writeBmp(before, captures[0]->cap.color, captures[0]->cap.width, captures[0]->cap.height);

    pipeline.release();
    for (LoadedCapture* c : captures) {
        if (c->color) c->color->Release();
        if (c->depth) c->depth->Release();
        delete c;
    }
    device->Release();
    d3d->Release();
    DestroyWindow(window);
    return failures ? 1 : 0;
}
