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
//     --compare <preset>      photo mode's compare split with a preset: that preset's look
//                             from a second pipeline (its own history, as in game) left of
//                             the line, the job's look right of it; "game" (with --photo):
//                             the game's image through the same lens (depth of field only)
//     --split <0..1>          where the compare line sits (default 0.5)
//     --labels <left|right>   the compare split's names burnt in at the bottom, as in photos
//     --refocus <u,v>         photo mode: focus there in the first half of the frames, then
//                             on --focus (a click on another point; the focus must follow)
//     --car <u,v>             DofAutofocus Car: the engine reports the car where the capture
//                             shows the point u,v (its depth, lowered to the wheel centres)
//     --video                 the video export's quality (photo mode's depth of field, moving)
//     --repeat <n>            n more sub-pixel samples of the last frame (the export's
//                             supersampling: history read, not advanced); the last is written
//     --bars <rows>           the replay menu's band: the image goes between <rows> above and
//                             below (magenta), which Pipeline::fillBars fills as in game
//     --nohdr                 ignore capture_NNN.hdr16 next to the capture (the game's float
//                             image, GameHdr): render from the 8-bit image only
//     --nomat                 ignore capture_NNN.mat next to the capture (which material drew
//                             each pixel): no material masks, as before F12 wrote them
//     --nogbuf                ignore capture_NNN.nrm16 next to the capture (the game's normals,
//                             GameGBuffer; used with --set GameGBuffer=1)
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
    // F7 / the menu's switch (General, not a look setting): 0 = the shaders off, with --photo
    // only photo mode's camera effects on the game's image.
    if (key == "Enabled") {
        s.enabled = atoi(value) != 0;
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
    float trail = 0.0f; // > 0: the neon trail of a car driving at this speed (m/s) to where the car is in the capture
};

struct LoadedCapture {
    std::string path;
    FrameCapture cap;
    IDirect3DTexture9* color = nullptr;
    IDirect3DTexture9* depth = nullptr;
    IDirect3DTexture9* hdr = nullptr; // capture_NNN.hdr16 next to it (GameHdr)
    IDirect3DTexture9* added = nullptr; // capture_NNN.add16: the light the game's additive draws added
    IDirect3DTexture9* materials = nullptr; // the class mask from capture_NNN.mat next to it
    IDirect3DTexture9* normals = nullptr; // capture_NNN.nrm16: the game's normals (GameGBuffer)
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
            else if (!_strnicmp(w.c_str(), "trail=", 6)) job.trail = static_cast<float>(atof(w.c_str() + 6));
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
                        "[--shaders dir] [--before file.bmp] [--sun x,y,z] [--suncolor r,g,b] [--move x,y,z] [--time s] [--water y] [--lut file.cube] [--lutmix 0..1] [--photo] [--video] [--repeat n] [--focus u,v] [--bench] [--moon] "
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
    bool moonReplaced = false;           // --moon: as in game with EngineMoon on a night map under the black hole
    float sun[3] = {}, sunColor[3] = {};
    const char* lutFile = nullptr;
    float lutMix = 1.0f;
    bool photoMode = false;              // the photo mode's look (3x reach, full-res blur)
    bool videoMode = false;              // --video: the video export's quality (the same paths, moving)
    int repeats = 0;                     // --repeat n: n more sub-pixel samples of the last frame (export supersampling)
    bool useHdr = true;                  // capture_NNN.hdr16 next to the capture (--nohdr: the 8-bit image only)
    bool useMaterials = true;            // capture_NNN.mat next to the capture (--nomat: no material masks)
    bool useNormals = true;              // capture_NNN.nrm16 next to the capture (--nogbuf: none)
    float focus[2] = {-1.0f, -1.0f};     // the photo mode's focus point (uv)
    float refocus[2] = {-2.0f, -2.0f};   // --refocus: the point clicked before (first half of the frames)
    float carAt[2] = {-1.0f, -1.0f};     // --car: where the car is on screen (uv), as the engine would report it
    const char* compare = nullptr;       // --compare: the preset on the split's other side
    float splitAt = 0.5f;
    const char* labels = nullptr;        // --labels "left|right": the names in a photo of the split
    UINT bars = 0;                       // --bars: rows above and below the image (replay menu test)
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--preset") && i + 1 < argc) {
            applyPresetByName(defaults.settings, argv[++i]);
        } else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
            overrides.push_back(argv[++i]);
        } else if (!strcmp(argv[i], "--debug") && i + 1 < argc) {
            defaults.settings.debugView = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--shaders") && i + 1 < argc) {
            shaderDir = widen(argv[++i]);
        } else if (!strcmp(argv[i], "--bars") && i + 1 < argc) {
            bars = static_cast<UINT>(atoi(argv[++i]));
        } else if (!strcmp(argv[i], "--bench")) {
            bench = true;
        } else if (!strcmp(argv[i], "--moon")) {
            moonReplaced = true;
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
        } else if (!strcmp(argv[i], "--nogbuf")) {
            useNormals = false;
        } else if (!strcmp(argv[i], "--photo")) {
            photoMode = true;
        } else if (!strcmp(argv[i], "--video")) {
            videoMode = true;
        } else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) {
            repeats = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--focus") && i + 1 < argc) {
            sscanf(argv[++i], "%f,%f", &focus[0], &focus[1]);
        } else if (!strcmp(argv[i], "--compare") && i + 1 < argc) {
            compare = argv[++i];
        } else if (!strcmp(argv[i], "--split") && i + 1 < argc) {
            splitAt = static_cast<float>(atof(argv[++i]));
        } else if (!strcmp(argv[i], "--labels") && i + 1 < argc) {
            labels = argv[++i];
        } else if (!strcmp(argv[i], "--car") && i + 1 < argc) {
            sscanf(argv[++i], "%f,%f", &carAt[0], &carAt[1]);
        } else if (!strcmp(argv[i], "--refocus") && i + 1 < argc) {
            sscanf(argv[++i], "%f,%f", &refocus[0], &refocus[1]);
        } else if (!strcmp(argv[i], "--lut") && i + 1 < argc) {
            lutFile = argv[++i];
        } else if (!strcmp(argv[i], "--lutmix") && i + 1 < argc) {
            lutMix = static_cast<float>(atof(argv[++i]));
        } else if (!strcmp(argv[i], "--trail") && i + 1 < argc) {
            defaults.trail = static_cast<float>(atof(argv[++i]));
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
    // The split's other side as the game's image (plugin.cpp renderSideB): the vanilla photo path.
    const bool compareGame = compare && !_stricmp(compare, "game");
    if (compareGame && !photoMode) {
        fprintf(stderr, "--compare game needs --photo (the game's side gets photo mode's lens)\n");
        return 1;
    }
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
    // The compare split's preset side (plugin.cpp renderSideB): a pipeline of its own.
    Pipeline pipelineB;
    if (compare && !pipelineB.init(device, shaderDir)) return 1;
    if (lutFile) {
        lut::Table table;
        std::string error;
        if (!lut::loadCube(widen(lutFile), table, error)) {
            fprintf(stderr, "%s: %s\n", lutFile, error.c_str());
            return 1;
        }
        pipeline.setLut(table.texels, table.size);
        pipelineB.setLut(table.texels, table.size);
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
        // The game's float image (GameHdr) and the light its additive draws added, written next to
        // the capture by F12 (capture_NNN.hdr16, .add16: "TMH1", width, height, RGBA half floats).
        // GameGBuffer's normals (capture_NNN.nrm16) come the same way.
        auto readHalf = [&](const char* extension, const char* what, bool wanted) -> IDirect3DTexture9* {
            std::string halfPath = path;
            if (!wanted || halfPath.size() <= 6 || halfPath.compare(halfPath.size() - 6, 6, ".tmcap") != 0) return nullptr;
            halfPath.replace(halfPath.size() - 6, 6, extension);
            FILE* f = fopen(halfPath.c_str(), "rb");
            if (!f) return nullptr;
            IDirect3DTexture9* texture = nullptr;
            uint32_t header[3] = {};
            std::vector<uint16_t> half(static_cast<size_t>(c->cap.width) * c->cap.height * 4);
            if (fread(header, sizeof(header), 1, f) == 1 && header[0] == 0x31484D54u && header[1] == c->cap.width &&
                header[2] == c->cap.height && fread(half.data(), sizeof(uint16_t), half.size(), f) == half.size()) {
                std::vector<uint64_t> texels(half.size() / 4);
                memcpy(texels.data(), half.data(), half.size() * sizeof(uint16_t));
                texture = upload(device, c->cap.width, c->cap.height, D3DFMT_A16B16G16R16F, texels);
                printf("%s: with the game's %s (%s)\n", path.c_str(), what, halfPath.c_str());
            }
            fclose(f);
            return texture;
        };
        c->hdr = readHalf(".hdr16", "HDR", useHdr);
        c->added = readHalf(".add16", "added light", useHdr);
        c->normals = readHalf(".nrm16", "normals (GameGBuffer)", useNormals);
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
        // The other side: the preset's look on the job's machine settings.
        Settings settingsB = job.settings;
        if (compare && !compareGame && !applyPresetByName(settingsB, compare)) return 1;
        settingsB.enabled = !compareGame;
        if (compareGame) settingsB.sharpen = 0.0f; // the depth of field and nothing else of the mod
        settingsB.debugView = job.settings.debugView;
        // As renderSideB: the lens is the camera's, the same on both sides.
        settingsB.depthOfField = job.settings.depthOfField;
        settingsB.focusDistance = job.settings.focusDistance;
        settingsB.dofAutofocus = job.settings.dofAutofocus;
        settingsB.bokehSize = job.settings.bokehSize;
        gfx::Target outputB;
        if (compare && !outputB.create(device, loaded->cap.width, loaded->cap.height, D3DFMT_A8R8G8B8)) return 1;
        Pipeline::Inputs in;
        in.lut = lutFile ? lutMix : 0.0f;
        in.photo = photoMode;
        in.video = videoMode;
        in.focusPoint[0] = focus[0];
        in.focusPoint[1] = focus[1];
        in.color = loaded->color;
        in.hdr = loaded->hdr;
        in.added = loaded->added;
        in.gbuffer = loaded->normals;
        in.materialMask = loaded->materials;
        in.materials = loaded->materials != nullptr;
        in.depth = loaded->depth;
        in.width = cap.width;
        in.height = cap.height;
        memcpy(in.view, cap.view, sizeof(in.view));
        memcpy(in.projection, cap.projection, sizeof(in.projection));
        if (carAt[0] >= 0.0f && cap.depth.size() == static_cast<size_t>(cap.width) * cap.height) {
            // The world point seen there, 0.3 m lower: about the wheel centres, as the engine gives it.
            const float* V = cap.view;
            const float* P = cap.projection;
            const UINT x = static_cast<UINT>(carAt[0] * cap.width), y = static_cast<UINT>(carAt[1] * cap.height);
            const float d = cap.depth[static_cast<size_t>(y < cap.height ? y : cap.height - 1) * cap.width + (x < cap.width ? x : cap.width - 1)];
            const float z = P[14] / (d - P[10]);
            const float pv[3] = {((carAt[0] * 2.0f - 1.0f) - P[8]) * z / P[0], ((1.0f - 2.0f * carAt[1]) - P[9]) * z / P[5], z};
            for (int j = 0; j < 3; j++) {
                const float camera = -(V[12] * V[j * 4 + 0] + V[13] * V[j * 4 + 1] + V[14] * V[j * 4 + 2]);
                in.focusCar[j] = camera + pv[0] * V[j * 4 + 0] + pv[1] * V[j * 4 + 1] + pv[2] * V[j * 4 + 2];
            }
            in.focusCar[1] -= 0.3f;
            in.focusCarKnown = true;
            printf("%s: car at %.1f %.1f %.1f, %.1f m from the camera\n", job.output.c_str(), in.focusCar[0], in.focusCar[1], in.focusCar[2], z);
        }
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
        in.moonReplaced = moonReplaced;
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
        // Trail test: the car where the pipeline's depth finder would see it (below the screen's
        // centre, lowered to the wheel centres), driving there along the line from 1.6 m below
        // the camera (the road it came along).
        float carEnd[3] = {}, carDir[3] = {};
        bool trail = false;
        if (job.trail > 0.0f && cap.depth.size() == static_cast<size_t>(cap.width) * cap.height) {
            const float* V = cap.view;
            const float* P = cap.projection;
            float best = 1e9f, bestV = 0.6f;
            for (float v : {0.6f, 0.65f, 0.7f}) {
                const float d = cap.depth[static_cast<size_t>(v * cap.height) * cap.width + cap.width / 2];
                const float z = P[14] / (d - P[10]);
                if (z > 1.5f && z < 16.0f && z < best) best = z, bestV = v;
            }
            if (best < 16.0f) {
                const float pv[3] = {(0.0f - P[8]) * best / P[0], ((1.0f - 2.0f * bestV) - P[9]) * best / P[5], best};
                float camera[3];
                for (int j = 0; j < 3; j++) {
                    camera[j] = -(V[12] * V[j * 4 + 0] + V[13] * V[j * 4 + 1] + V[14] * V[j * 4 + 2]);
                    carEnd[j] = camera[j] + pv[0] * V[j * 4 + 0] + pv[1] * V[j * 4 + 1] + pv[2] * V[j * 4 + 2] - 0.5f * V[j * 4 + 1] -
                                0.25f * V[j * 4 + 2];
                }
                const float anchor[3] = {camera[0], camera[1] - 1.6f, camera[2]};
                float len = 0.0f;
                for (int j = 0; j < 3; j++) len += (anchor[j] - carEnd[j]) * (anchor[j] - carEnd[j]);
                len = sqrtf(len);
                for (int j = 0; j < 3; j++) carDir[j] = (anchor[j] - carEnd[j]) / len;
                trail = true;
                printf("%s: trail test, car at %.1f %.1f %.1f, %.1f m/s\n", job.output.c_str(), carEnd[0], carEnd[1], carEnd[2], job.trail);
            }
        }

        // Several frames so auto exposure and TAA settle like they would in game.
        LARGE_INTEGER t0{}, t1{}, freq{};
        QueryPerformanceFrequency(&freq);
        pipeline.resetHistory();
        if (compare) pipelineB.resetHistory();
        const int frames = bench ? 200 : (trail ? 150 : 40);
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
            if (refocus[0] > -1.5f && frame < frames / 2) {
                driven.focusPoint[0] = refocus[0];
                driven.focusPoint[1] = refocus[1];
            }
            if (trail) {
                driven.driving = driven.carKnown = driven.carFromEngine = true;
                // Per frame as far as the frame's time step (time=: 0.02 s, else 0.1 s).
                const float step = job.trail * (job.time >= 0.0f ? 0.02f : 0.1f);
                for (int j = 0; j < 3; j++) driven.carPosition[j] = carEnd[j] + carDir[j] * back * step;
            }
            if (frame == frames - 1) QueryPerformanceCounter(&t0);
            pipeline.render(device, driven, job.settings, output.surface);
            // The export's supersampling: the same moment again (the output is the last one).
            for (int r = 0; r < repeats && frame == frames - 1; r++) {
                Pipeline::Inputs again = driven;
                again.temporal = false;
                again.repeat = true;
                pipeline.render(device, again, job.settings, output.surface);
            }
            if (compare) pipelineB.render(device, driven, settingsB, outputB.surface);
            if (bench) pipeline.collectProfile(true);
        }
        if (compare) {
            // As photo::applySplit: the preset's side left of the line, the line light with dark edges.
            const LONG w = static_cast<LONG>(cap.width), h = static_cast<LONG>(cap.height);
            const LONG x = static_cast<LONG>(lroundf((splitAt < 0.0f ? 0.0f : (splitAt > 1.0f ? 1.0f : splitAt)) * w));
            const RECT side{0, 0, x, h};
            if (x > 0) device->StretchRect(outputB.surface, &side, output.surface, &side, D3DTEXF_NONE);
            const LONG t = h >= 1080 ? h / 540 : 1;
            const RECT dark{x - 2 * t < 0 ? 0 : x - 2 * t, 0, x + 2 * t > w ? w : x + 2 * t, h};
            const RECT light{x - t < 0 ? 0 : x - t, 0, x + t > w ? w : x + t, h};
            if (dark.right > dark.left) device->ColorFill(output.surface, &dark, D3DCOLOR_XRGB(16, 16, 16));
            if (light.right > light.left) device->ColorFill(output.surface, &light, D3DCOLOR_XRGB(235, 235, 235));
            outputB.destroy();
        }
        // The replay menu's band (plugin.cpp fillMenuBars): the image between `bars` rows above
        // and below, filled by the pipeline as in game; magenta where it leaves rows out.
        UINT outHeight = cap.height;
        if (bars > 0) {
            gfx::Target tall;
            if (tall.create(device, cap.width, cap.height + 2 * bars, D3DFMT_A8R8G8B8)) {
                device->ColorFill(tall.surface, nullptr, D3DCOLOR_XRGB(255, 0, 255));
                const RECT area = {0, static_cast<LONG>(bars), static_cast<LONG>(cap.width), static_cast<LONG>(bars + cap.height)};
                device->StretchRect(output.surface, nullptr, tall.surface, &area, D3DTEXF_NONE);
                if (!pipeline.fillBars(device, output.texture, tall.surface, area, true, true)) fprintf(stderr, "fillBars failed\n");
                output.destroy();
                output = tall;
                tall = gfx::Target();
                outHeight = cap.height + 2 * bars;
            }
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
        if (labels) {
            // As photo.cpp's save: the names on the finished image (left|right of the line).
            const std::string both = labels;
            const size_t bar = both.find('|');
            image::drawSplitLabels(reinterpret_cast<unsigned char*>(pixels.data()), static_cast<int>(cap.width), static_cast<int>(outHeight), 4,
                                   static_cast<size_t>(cap.width) * 4, splitAt, both.substr(0, bar),
                                   bar == std::string::npos ? std::string() : both.substr(bar + 1));
        }
        QueryPerformanceCounter(&t1);
        const size_t length = job.output.size();
        if (length > 4 && !_stricmp(job.output.c_str() + length - 4, ".jpg")) {
            image::writeThumbnail(widen(job.output.c_str()), pixels.data(), static_cast<int>(cap.width), static_cast<int>(outHeight),
                                  static_cast<int>(cap.width));
        } else {
            writeBmp(job.output.c_str(), pixels, cap.width, outHeight);
        }
        printf("%s: %ux%u, sun %s (%.3f %.3f %.3f), last frame incl. readback %.1f ms\n", job.output.c_str(), cap.width, cap.height,
               in.sunKnown ? "known" : "unknown", in.sunDirection[0], in.sunDirection[1], in.sunDirection[2],
               1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart));
        output.destroy();
    }
    if (before && !captures.empty()) writeBmp(before, captures[0]->cap.color, captures[0]->cap.width, captures[0]->cap.height);

    pipeline.release();
    pipelineB.release();
    for (LoadedCapture* c : captures) {
        if (c->color) c->color->Release();
        if (c->depth) c->depth->Release();
        if (c->hdr) c->hdr->Release();
        if (c->added) c->added->Release();
        if (c->normals) c->normals->Release();
        if (c->materials) c->materials->Release();
        delete c;
    }
    device->Release();
    d3d->Release();
    DestroyWindow(window);
    return failures ? 1 : 0;
}
