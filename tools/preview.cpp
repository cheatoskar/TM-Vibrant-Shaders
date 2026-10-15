// Offline previewer: renders recorded game frames (.tmcap) through the shader pipeline.
//
//   tmvs_preview <capture.tmcap> <out.bmp> [options]
//     --preset <name|index>   Vibrant, Realistic, Golden Hour, Competition, Performance, ...
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
//     --nohdr                 ignore capture_NNN.hdr16 next to the capture (the game's float
//                             image, GameHdr): render from the 8-bit image only
//     --nomat                 ignore capture_NNN.mat next to the capture (which material drew
//                             each pixel): no material masks, as before F12 wrote them
//     --batch <jobs.txt>      many images in one run (shaders compile once); each line:
//                             out.bmp [cap=file.tmcap] [preset=Golden_Hour] [move=x,y,z] [Key=Value ...]
//   An output ending in .jpg is a preset picture for the menu (384x216, the middle 16:9).
#include "audio.h"
#include "config.h"
#include "framecap.h"
#include "gbx.h"
#include "gfx.h"
#include "image.h"
#include "log.h"
#include "lut.h"
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
    IDirect3DTexture9* hdr = nullptr; // capture_NNN.hdr16 next to it (GameHdr)
    IDirect3DTexture9* materials = nullptr; // the class mask from capture_NNN.mat next to it
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
    // tmvs_preview --sound out.wav [rain] [volume] [wind]: the weather sound, without a capture.
    if (argc >= 3 && !strcmp(argv[1], "--sound")) {
        const float rain = argc > 3 ? static_cast<float>(atof(argv[3])) : 1.0f;
        const float volume = argc > 4 ? static_cast<float>(atof(argv[4])) : 1.0f;
        const float wind = argc > 5 ? static_cast<float>(atof(argv[5])) : 0.0f;
        return tmshaders::audio::renderWav(widen(argv[2]).c_str(), rain, volume, wind) ? 0 : 1;
    }
    // tmvs_preview --decode in.mp3 out.wav
    if (argc >= 4 && !strcmp(argv[1], "--decode")) return tmshaders::audio::decodeToWav(widen(argv[2]).c_str(), widen(argv[3]).c_str()) ? 0 : 1;
    // tmvs_preview --map <file.Challenge.Gbx>: the map's comments and the look they bring.
    if (argc >= 3 && !strcmp(argv[1], "--map")) {
        std::string comments;
        if (!tmshaders::gbx::readMapComments(widen(argv[2]), comments)) {
            printf("no comments (or not a map)\n");
            return 1;
        }
        tmshaders::Config& config = tmshaders::Config::get();
        config.onMapComments(comments);
        printf("comments: %s\nmap look: %s\n", comments.c_str(), config.mapLookActive() ? config.mapTag().c_str() : "none");
        return 0;
    }
    // tmvs_preview --tag <preset> [Key=Value ...]: a look as the line for a map's comments.
    if (argc >= 3 && !strcmp(argv[1], "--tag")) {
        tmshaders::Config& config = tmshaders::Config::get();
        if (!config.selectPreset(argv[2])) return 1;
        for (int i = 3; i < argc; i++) setByKey(config.settings, argv[i]);
        printf("%s\n", config.mapTag().c_str());
        return 0;
    }
    // tmvs_preview --play <seconds> [volume] [snow]: plays the weather sound live, as in the
    // game (with snow: the snow storm wind instead of rain and thunder).
    if (argc >= 3 && !strcmp(argv[1], "--play")) {
        tmshaders::Settings s;
        s.snow = argc > 4 ? static_cast<float>(atof(argv[4])) : 0.0f;
        s.rain = s.snow > 0.0f ? 0.0f : 1.6f;
        s.lightning = s.snow > 0.0f ? 0.0f : 1.0f;
        s.wind = 1.0f;
        s.weatherSound = argc > 3 ? static_cast<float>(atof(argv[3])) : 1.0f;
        tmshaders::audio::setAlwaysInFront(true);
        const DWORD start = GetTickCount();
        const float length = static_cast<float>(atof(argv[2]));
        for (float t = 0.0f; t < length; t = (GetTickCount() - start) / 1000.0f) {
            tmshaders::audio::update(s, t, true, nullptr);
            Sleep(16);
        }
        tmshaders::audio::shutdown();
        Sleep(300);
        return 0;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: tmvs_preview <capture.tmcap> <out.bmp> [--preset name] [--set Key=Value] [--debug n] "
                        "[--shaders dir] [--before file.bmp] [--sun x,y,z] [--suncolor r,g,b] [--move x,y,z] [--time s] [--water y] [--lut file.cube] [--lutmix 0..1] [--photo] [--focus u,v] [--bench] "
                        "[--batch jobs.txt]\n"
                        "       tmvs_preview --sound <out.wav> [rain] [volume] [wind] | --play <seconds> [volume] [snow] | --decode <in.mp3> <out.wav>\n");
        return 2;
    }

    float waterHeight = -1e30f; // world height of the map's water (the game reports it in game)
    float waterKind = 1.0f;     // 1 sea (--water), 2 Stadium water blocks (--blocks)
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
    const char* lutFile = nullptr;
    float lutMix = 1.0f;
    bool photoMode = false;              // the photo mode's look (3x reach, full-res blur)
    bool useHdr = true;                  // capture_NNN.hdr16 next to the capture (--nohdr: the 8-bit image only)
    bool useMaterials = true;            // capture_NNN.mat next to the capture (--nomat: no material masks)
    float focus[2] = {-1.0f, -1.0f};     // the photo mode's focus point (uv)
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
        } else if (!strcmp(argv[i], "--blocks") && i + 1 < argc) {
            waterHeight = static_cast<float>(atof(argv[++i]));
            waterKind = 2.0f;
        } else if (!strcmp(argv[i], "--nohdr")) {
            useHdr = false;
        } else if (!strcmp(argv[i], "--nomat")) {
            useMaterials = false;
        } else if (!strcmp(argv[i], "--photo")) {
            photoMode = true;
        } else if (!strcmp(argv[i], "--focus") && i + 1 < argc) {
            sscanf(argv[++i], "%f,%f", &focus[0], &focus[1]);
        } else if (!strcmp(argv[i], "--lut") && i + 1 < argc) {
            lutFile = argv[++i];
        } else if (!strcmp(argv[i], "--lutmix") && i + 1 < argc) {
            lutMix = static_cast<float>(atof(argv[++i]));
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
    if (lutFile) {
        lut::Table table;
        std::string error;
        if (!lut::loadCube(widen(lutFile), table, error)) {
            fprintf(stderr, "%s: %s\n", lutFile, error.c_str());
            return 1;
        }
        pipeline.setLut(table.texels, table.size);
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
        // The game's float image (GameHdr), written next to the capture by F12.
        std::string hdrPath = path;
        if (useHdr && hdrPath.size() > 6 && hdrPath.compare(hdrPath.size() - 6, 6, ".tmcap") == 0) {
            hdrPath.replace(hdrPath.size() - 6, 6, ".hdr16");
            if (FILE* f = fopen(hdrPath.c_str(), "rb")) {
                uint32_t header[3] = {};
                std::vector<uint16_t> half(static_cast<size_t>(c->cap.width) * c->cap.height * 4);
                if (fread(header, sizeof(header), 1, f) == 1 && header[0] == 0x31484D54u && header[1] == c->cap.width &&
                    header[2] == c->cap.height && fread(half.data(), sizeof(uint16_t), half.size(), f) == half.size()) {
                    std::vector<uint64_t> texels(half.size() / 4);
                    memcpy(texels.data(), half.data(), half.size() * sizeof(uint16_t));
                    c->hdr = upload(device, c->cap.width, c->cap.height, D3DFMT_A16B16G16R16F, texels);
                    printf("%s: with the game's HDR (%s)\n", path.c_str(), hdrPath.c_str());
                }
                fclose(f);
            }
        }
        // The game's materials (capture_NNN.mat, F12 with the material masks on): the class mask
        // the game would have built (r light, g car 1 / glass .5 / grass .25, b water).
        std::string matPath = path;
        if (useMaterials && matPath.size() > 6 && matPath.compare(matPath.size() - 6, 6, ".tmcap") == 0) {
            matPath.replace(matPath.size() - 6, 6, ".mat");
            if (FILE* f = fopen(matPath.c_str(), "rb")) {
                uint32_t header[4] = {};
                uint8_t classes[256] = {};
                bool ok = fread(header, sizeof(header), 1, f) == 1 && header[0] == 0x314D4D54u && header[1] == c->cap.width &&
                          header[2] == c->cap.height && header[3] <= 255;
                for (uint32_t i = 0; ok && i < header[3]; i++) {
                    uint8_t kind[2] = {};
                    uint32_t draws = 0;
                    uint16_t length = 0;
                    ok = fread(kind, sizeof(kind), 1, f) == 1 && fread(&draws, sizeof(draws), 1, f) == 1 && fread(&length, sizeof(length), 1, f) == 1 &&
                         fseek(f, length, SEEK_CUR) == 0;
                    classes[i + 1] = kind[0];
                }
                std::vector<uint8_t> ids(static_cast<size_t>(c->cap.width) * c->cap.height);
                if (ok && fread(ids.data(), 1, ids.size(), f) == ids.size()) {
                    std::vector<uint32_t> texels(ids.size());
                    for (size_t i = 0; i < ids.size(); i++) {
                        const int k = classes[ids[i]];
                        const uint32_t r = k == 1 ? 255u : 0u, g = k == 2 ? 255u : (k == 3 ? 128u : (k == 4 ? 64u : 0u)), b = k == 5 ? 255u : 0u;
                        texels[i] = (r << 16) | (g << 8) | b;
                    }
                    c->materials = upload(device, c->cap.width, c->cap.height, D3DFMT_A8R8G8B8, texels);
                    printf("%s: with the game's materials (%s, %u)\n", path.c_str(), matPath.c_str(), header[3]);
                }
                fclose(f);
            }
        }
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
        in.lut = lutFile ? lutMix : 0.0f;
        in.photo = photoMode;
        in.focusPoint[0] = focus[0];
        in.focusPoint[1] = focus[1];
        in.color = loaded->color;
        in.hdr = loaded->hdr;
        in.materialMask = loaded->materials;
        in.materials = loaded->materials != nullptr;
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
            in.water[2] = waterKind;
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
        const size_t length = job.output.size();
        if (length > 4 && !_stricmp(job.output.c_str() + length - 4, ".jpg")) {
            image::writeThumbnail(widen(job.output.c_str()), pixels.data(), static_cast<int>(cap.width), static_cast<int>(cap.height),
                                  static_cast<int>(cap.width));
        } else {
            writeBmp(job.output.c_str(), pixels, cap.width, cap.height);
        }
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
        if (c->hdr) c->hdr->Release();
        if (c->materials) c->materials->Release();
        delete c;
    }
    device->Release();
    d3d->Release();
    DestroyWindow(window);
    return failures ? 1 : 0;
}
