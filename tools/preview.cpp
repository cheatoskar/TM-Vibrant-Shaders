// Offline previewer: renders recorded game frames (.tmcap) through the shader pipeline.
//
//   tmvs_preview <capture.tmcap> <out.bmp> [options]
//     --preset <name|index>   Vibrant, Realistic, Golden Hour, Competition, Performance, ...
//     --set Key=Value         override a setting (keys as in settings.ini), repeatable;
//                             RenderScale=0.67: the capture point sampled to that size (what the
//                             game would draw there), shaded at it and brought up with FSR 1
//     --debug <n>             debug view (1 depth, 2 normals, 3 AO, 4 shadows, 5 shafts, 6 bloom)
//     --shaders <dir>         load tmvs.hlsl from <dir> instead of the embedded copy
//     --before <file.bmp>     also write the unprocessed frame
//     --sun <x,y,z>           override the sun direction (world space, towards the sun)
//     --suncolor <r,g,b>      override the game's light colour
//     --move <x,y,z>          previous frame's camera offset in metres (motion blur test)
//     --trail <m/s>           neon trail test: a car driving to where the depth finder sees it
//     --bank <deg>            ... rolled by that much (its up tilted: a banked curve)
//     --clip                  ... while a clip of the map plays (driving off, clipInRace on)
//     --drive <x,y,z>         camera motion per frame (camera axes, m) towards the captured
//                             view, 0.1 s apart (neon trail test)
//     --carpose <deg>         spray test: the engine's car pose where the depth finder sees the
//                             car, turned by deg from the camera's level forward (sliding), moving
//                             with --drive
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
//     --own [10|hdr]          the video export's own frames: the last frame and its repeats summed
//                             (hdr: HDR10 at 1000 nits, white 203; the P010 planes go to <out>.p010)
//                             in float and graded once (Inputs::accumulate), then the encoder's
//                             planes (8 bits NV12, with 10: P010) read back and turned into RGB
//                             again: <output> is the frame, <output>.yuv.bmp the planes' round trip
//     --export <n>            with --own: n export frames in a row (each with the --repeat samples),
//                             their planes read back straight after each frame (as before 1.4.2) and
//                             through PlaneReadback (a frame later); prints ms a frame for both
//     --cpu <ms>              ... with that much busy CPU a frame, standing in for the game's work
//     --plain                 a plain Direct3D 9 device like the game's (default 9Ex, which keeps
//                             working while a fullscreen game owns the display)
//     --jitter                with RenderScale 1/n (0.5): TAA jitter as in game (TAAJitter), each
//                             frame takes another of the n x n capture pixels under a scene pixel
//                             (a raster shifted by sub-pixels, with its aliasing), Halton (2, 3)
//     --bars <rows>           the replay menu's band: the image goes between <rows> above and
//                             below (magenta), which Pipeline::fillBars fills as in game
//     --nohdr                 ignore capture_NNN.hdr16 next to the capture (the game's float
//                             image, GameHdr): render from the 8-bit image only
//     --nospillhistory        the neon spill of each frame on its own, not over frames (A/B)
//     --noneonhistory         the neon strips' glow of each frame on its own, not over frames (A/B)
//     --nomat                 ignore capture_NNN.mat next to the capture (which material drew
//                             each pixel): no material masks, as before F12 wrote them
//     --nogbuf                ignore capture_NNN.nrm16 and .lit16 next to the capture (the game's
//                             normals, GameGBuffer, and its light, GameLight; used with
//                             --set GameGBuffer=1 / GameLight=1)
//     --batch <jobs.txt>      many images in one run (shaders compile once); each line:
//                             out.bmp [cap=file.tmcap] [preset=Golden_Hour] [move=x,y,z] [Key=Value ...]
//                             [keep=1 frames=n]: n frames on from the last job's history (a
//                             sequence: one capture a frame, e.g. a camera pan; time= per job)
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
#include "planeread.h"
#include <algorithm>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

using namespace tmshaders;

namespace {

// A float in the half floats' normal range as their bits (rounded down; exact for the ceiling).
uint16_t floatToHalfBits(float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    const int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 15;
    return static_cast<uint16_t>(((bits >> 16) & 0x8000u) | (static_cast<uint32_t>(exponent) << 10) | ((bits >> 13) & 0x3FFu));
}

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
    float bank = 0.0f;  // trail test: the car rolled by this many degrees (its up tilted to the right: a banked curve)
    bool clip = false;  // trail test: a clip of the map plays while the car drives (driving off, clipInRace on)
    bool keep = false;  // keep=1: go on from the last job's history (a sequence of captures, one per job)
    int frames = 0;     // frames=n: render n frames (0 = as many as usual)
};

struct LoadedCapture {
    std::string path;
    FrameCapture cap;
    IDirect3DTexture9* color = nullptr;
    IDirect3DTexture9* depth = nullptr;
    IDirect3DTexture9* hdr = nullptr; // capture_NNN.hdr16 next to it (GameHdr)
    IDirect3DTexture9* added = nullptr; // capture_NNN.add16: the light the game's additive draws added
    IDirect3DTexture9* materials = nullptr; // the class mask from capture_NNN.mat next to it
    bool neonMaterials = false;             // ... with the neon class: the strips known by their material
    IDirect3DTexture9* normals = nullptr; // capture_NNN.nrm16: the game's normals (GameGBuffer)
    IDirect3DTexture9* light = nullptr;   // capture_NNN.lit16: the light its shaders lit each surface with (GameLight)
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
            else if (!_strnicmp(w.c_str(), "bank=", 5)) job.bank = static_cast<float>(atof(w.c_str() + 5));
            else if (!_stricmp(w.c_str(), "clip=1")) job.clip = true;
            else if (!_stricmp(w.c_str(), "keep=1")) job.keep = true;
            else if (!_strnicmp(w.c_str(), "frames=", 7)) job.frames = atoi(w.c_str() + 7);
            else setByKey(job.settings, w.c_str());
        }
        jobs.push_back(job);
    }
    fclose(f);
    return true;
}

// --casters d: stand-in CasterShadows maps (the game's meshes aren't in a capture), both of the
// game's size (4096^2 INTZ) and filled with one depth d (1 nothing casts, 0.5 everything farther
// along the light than the middle is shadowed): what sampling the maps costs the passes.
float g_casterDepth = -1.0f;
IDirect3DTexture9* g_casterTex[2] = {};

bool previewCasters(IDirect3DDevice9* device, const float sunWorld[3], const float camera[3], casters::Maps& out) {
    const D3DFORMAT intz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
    const UINT size = 4096;
    for (int k = 0; k < 2; k++) {
        if (g_casterTex[k]) continue;
        IDirect3DSurface9* surface = nullptr;
        if (FAILED(device->CreateTexture(size, size, 1, D3DUSAGE_DEPTHSTENCIL, intz, D3DPOOL_DEFAULT, &g_casterTex[k], nullptr)) ||
            FAILED(g_casterTex[k]->GetSurfaceLevel(0, &surface)))
            return false;
        IDirect3DSurface9* savedDepth = nullptr;
        device->GetDepthStencilSurface(&savedDepth);
        device->SetDepthStencilSurface(surface);
        device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, g_casterDepth, 0);
        device->SetDepthStencilSurface(savedDepth);
        if (savedDepth) savedDepth->Release();
        surface->Release();
    }
    // The light's frame as casters.cpp builds it: f along the light, u across, v up.
    float f[3] = {-sunWorld[0], -sunWorld[1], -sunWorld[2]};
    const float lf = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (lf < 1e-6f || f[1] / lf > -0.02f) return false;
    for (float& x : f) x /= lf;
    float u[3] = {f[2], 0.0f, -f[0]};
    const float lu = sqrtf(u[0] * u[0] + u[2] * u[2]);
    if (lu < 1e-6f) u[0] = 1.0f, u[2] = 0.0f;
    else u[0] /= lu, u[2] /= lu;
    const float v[3] = {f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float halfSize[2] = {200.0f, 2500.0f}; // near map, whole track
    casters::MapInfo* maps[2] = {&out.nearMap, &out.farMap};
    for (int k = 0; k < 2; k++) {
        const float sa = 2.0f * halfSize[k], sb = sa, sd = 3000.0f;
        const float a0 = dot(camera, u) - halfSize[k], b0 = dot(camera, v) - halfSize[k], d0 = dot(camera, f) - 1500.0f;
        const float half = 0.5f / size;
        casters::MapInfo& m = *maps[k];
        for (int j = 0; j < 3; j++) {
            m.rows[0][j] = u[j] / sa;
            m.rows[1][j] = -v[j] / sb;
            m.rows[2][j] = f[j] / sd;
        }
        m.rows[0][3] = -a0 / sa + half;
        m.rows[1][3] = 1.0f + b0 / sb + half;
        m.rows[2][3] = -d0 / sd;
        m.texel[0] = m.texel[1] = 1.0f / size;
        m.bias = 0.3f / sd;
        m.texture = g_casterTex[k];
    }
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
    bool sunOverride = false, sunColorOverride = false, bench = false, precompile = false;
    bool moonReplaced = false;           // --moon: as in game with EngineMoon on a night map under the black hole
    float sun[3] = {}, sunColor[3] = {};
    const char* lutFile = nullptr;
    float lutMix = 1.0f;
    bool photoMode = false;              // the photo mode's look (3x reach, full-res blur)
    bool videoMode = false;              // --video: the video export's quality (the same paths, moving)
    int repeats = 0;                     // --repeat n: n more sub-pixel samples of the last frame (export supersampling)
    int ownBits = 0;                     // --own [10]: the export's own frame and its planes (8 or 10 bits), 0 = off
    bool ownHdr = false;                 // --own hdr: the planes in HDR10 (10 bits)
    int exportFrames = 0;                // --export n: n export frames in a row, read back straight and pipelined
    double cpuMs = 0.0;                  // --cpu ms: busy CPU a frame in --export (the game's own work)
    bool plainDevice = false;            // --plain: a plain Direct3D 9 device as the game's (not 9Ex)
    bool jitterRaster = false;           // --jitter: TAA jitter emulated by RenderScale's point sampling
    int jitterIndex = 0;                 // its Halton index, on across batch jobs (keep=1) as in game
    bool useHdr = true;                  // capture_NNN.hdr16 next to the capture (--nohdr: the 8-bit image only)
    bool useMaterials = true;            // capture_NNN.mat next to the capture (--nomat: no material masks)
    bool neonHistory = true;             // --noneonhistory: the strips' glow of each frame on its own (A/B)
    bool spillHistory = true;            // --nospillhistory: the neon spill of each frame on its own (A/B)
    bool useNormals = true;              // capture_NNN.nrm16 next to the capture (--nogbuf: none)
    float focus[2] = {-1.0f, -1.0f};     // the photo mode's focus point (uv)
    float refocus[2] = {-2.0f, -2.0f};   // --refocus: the point clicked before (first half of the frames)
    float carAt[2] = {-1.0f, -1.0f};     // --car: where the car is on screen (uv), as the engine would report it
    // --carpose yaw: the engine's car pose for the spray (Inputs::carPose*): where the depth search
    // finds the car (as --trail), turned by yaw degrees from the camera's level forward (a car
    // sliding sideways), moving with the camera's --drive.
    float carPoseYaw = 0.0f;
    bool carPoseSet = false;
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
        } else if (!strcmp(argv[i], "--casters") && i + 1 < argc) {
            g_casterDepth = static_cast<float>(atof(argv[++i]));
        } else if (!strcmp(argv[i], "--precompile")) {
            precompile = true; // as the plugin's boot: the runtime HLSL compiled before the pipeline
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
        } else if (!strcmp(argv[i], "--noneonhistory")) {
            neonHistory = false;
        } else if (!strcmp(argv[i], "--nospillhistory")) {
            spillHistory = false;
        } else if (!strcmp(argv[i], "--nogbuf")) {
            useNormals = false;
        } else if (!strcmp(argv[i], "--photo")) {
            photoMode = true;
        } else if (!strcmp(argv[i], "--video")) {
            videoMode = true;
        } else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) {
            repeats = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--own")) {
            ownBits = 8;
            if (i + 1 < argc && !strcmp(argv[i + 1], "10")) ownBits = (++i, 10);
            if (i + 1 < argc && !strcmp(argv[i + 1], "hdr")) ownBits = (++i, ownHdr = true, 10);
        } else if (!strcmp(argv[i], "--export") && i + 1 < argc) {
            exportFrames = atoi(argv[++i]);
            if (!ownBits) ownBits = 8;
        } else if (!strcmp(argv[i], "--cpu") && i + 1 < argc) {
            cpuMs = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--plain")) {
            plainDevice = true;
        } else if (!strcmp(argv[i], "--jitter")) {
            jitterRaster = true;
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
        } else if (!strcmp(argv[i], "--carpose") && i + 1 < argc) {
            carPoseYaw = static_cast<float>(atof(argv[++i]));
            carPoseSet = true;
        } else if (!strcmp(argv[i], "--refocus") && i + 1 < argc) {
            sscanf(argv[++i], "%f,%f", &refocus[0], &refocus[1]);
        } else if (!strcmp(argv[i], "--lut") && i + 1 < argc) {
            lutFile = argv[++i];
        } else if (!strcmp(argv[i], "--lutmix") && i + 1 < argc) {
            lutMix = static_cast<float>(atof(argv[++i]));
        } else if (!strcmp(argv[i], "--trail") && i + 1 < argc) {
            defaults.trail = static_cast<float>(atof(argv[++i]));
        } else if (!strcmp(argv[i], "--bank") && i + 1 < argc) {
            defaults.bank = static_cast<float>(atof(argv[++i]));
        } else if (!strcmp(argv[i], "--clip")) {
            defaults.clip = true;
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
    IDirect3DDevice9* device = nullptr;
    if (plainDevice) {
        // --plain: a device like the game's (no 9Ex), for timings that depend on the driver's path.
        IDirect3DDevice9* plain = nullptr;
        if (d3d && SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
                                               &pp, &plain)))
            device = plain;
    } else {
        IDirect3DDevice9Ex* ex = nullptr;
        if (d3d && SUCCEEDED(d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
                                                 &pp, nullptr, &ex)))
            device = ex;
    }
    if (!device) {
        fprintf(stderr, "cannot create Direct3D 9 device\n");
        return 1;
    }

    // CPU time of creating the pipelines and of their first frame (the hitch in game).
    LARGE_INTEGER clockFreq, initStart, initEnd;
    QueryPerformanceFrequency(&clockFreq);
    auto msSince = [&](const LARGE_INTEGER& from) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return 1000.0 * static_cast<double>(now.QuadPart - from.QuadPart) / static_cast<double>(clockFreq.QuadPart);
    };
    if (precompile) {
        QueryPerformanceCounter(&initStart);
        std::vector<gfx::ShaderSource> shaders;
        Pipeline::runtimeShaders(shaders);
        gfx::precompile(shaders);
        printf("CPU: %u shaders compiled ahead in %.1f ms\n", static_cast<unsigned>(shaders.size()), msSince(initStart));
    }
    QueryPerformanceCounter(&initStart);
    Pipeline pipeline;
    if (!pipeline.init(device, shaderDir)) {
        fprintf(stderr, "shader compilation failed:\n%s\n", pipeline.lastError().c_str());
        return 1;
    }
    // The compare split's preset side (plugin.cpp renderSideB): a pipeline of its own.
    const double initMs = msSince(initStart);
    QueryPerformanceCounter(&initEnd);
    Pipeline pipelineB;
    if (compare && !pipelineB.init(device, shaderDir)) return 1;
    const double initBMs = compare ? msSince(initEnd) : 0.0;
    bool firstFrame = true;
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
        // GameGBuffer's normals (capture_NNN.nrm16) and GameLight's light (.lit16) come the same way.
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
                // The game's float image as the pipeline gets it in game since 1.4.0: its colour no
                // higher than Pipeline::kSceneCeiling (capScene at the camera's end; older captures
                // can hold more, B13-Obstacle's sky that added up over frames). Infinity and NaN
                // become the ceiling as the GPU's MIN blending gives it (IEEE minNum).
                if (!strcmp(extension, ".hdr16")) {
                    const uint16_t ceiling = floatToHalfBits(Pipeline::kSceneCeiling);
                    size_t capped = 0;
                    for (size_t i = 0; i < half.size(); i++) {
                        if ((i & 3) == 3) continue; // alpha as it is
                        const uint16_t h = half[i];
                        const bool nan = (h & 0x7FFFu) > 0x7C00u;
                        if (nan || (!(h & 0x8000u) && h > ceiling)) {
                            half[i] = ceiling;
                            capped++;
                        }
                    }
                    if (capped) printf("%s: %zu colour values of the game's HDR above %.1f (or not finite) capped\n", path.c_str(), capped, Pipeline::kSceneCeiling);
                }
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
        c->light = readHalf(".lit16", "light (GameLight)", useNormals);
        // The game's materials (capture_NNN.mat, F12 with the material masks on): the class mask
        // the game would have built (r light, g car 1 / glass .5 / grass .25, b water).
        std::string matPath = path;
        if (useMaterials && matPath.size() > 6 && matPath.compare(matPath.size() - 6, 6, ".tmcap") == 0) {
            matPath.replace(matPath.size() - 6, 6, ".mat");
            if (FILE* f = fopen(matPath.c_str(), "rb")) {
                uint32_t header[4] = {};
                uint8_t classes[256] = {};
                bool texturesRead = false;
                bool ok = fread(header, sizeof(header), 1, f) == 1 && header[0] == 0x314D4D54u && header[1] == c->cap.width &&
                          header[2] == c->cap.height && header[3] <= 255;
                for (uint32_t i = 0; ok && i < header[3]; i++) {
                    uint8_t kind[2] = {};
                    uint32_t draws = 0;
                    uint16_t length = 0;
                    std::string name;
                    ok = fread(kind, sizeof(kind), 1, f) == 1 && fread(&draws, sizeof(draws), 1, f) == 1 && fread(&length, sizeof(length), 1, f) == 1;
                    if (ok) {
                        name.resize(length);
                        ok = length == 0 || fread(&name[0], 1, length, f) == length;
                    }
                    // Captures from before the dirt class: the Soil shaders are dirt (as
                    // engine::classify).
                    const size_t slash = name.substr(0, name.find(" | ")).find_last_of("\\/");
                    if (kind[0] == 0 && slash != std::string::npos && name.compare(slash + 1, 4, "Soil") == 0) kind[0] = Pipeline::kDirtClass;
                    classes[i + 1] = kind[0];
                    texturesRead = texturesRead || (kind[1] & 4) != 0;
                }
                std::vector<uint8_t> ids(static_cast<size_t>(c->cap.width) * c->cap.height);
                if (ok && fread(ids.data(), 1, ids.size(), f) == ids.size()) {
                    // The materials' textures were read (flag 4, or a material of the neon class):
                    // the strips are known by their material, as in game.
                    c->neonMaterials = texturesRead;
                    for (uint32_t i = 1; i <= header[3]; i++) c->neonMaterials = c->neonMaterials || classes[i] == Pipeline::kNeonClass;
                    std::vector<uint32_t> texels(ids.size());
                    for (size_t i = 0; i < ids.size(); i++) {
                        const int k = classes[ids[i]];
                        const uint32_t notNeon = c->neonMaterials && k != Pipeline::kNeonClass ? Pipeline::kNotNeonG : 0u;
                        const uint32_t r = k == 1 ? 255u : 0u, g = Pipeline::classG(k, notNeon), b = k == 5 ? 255u : 0u;
                        texels[i] = (r << 16) | (g << 8) | b;
                    }
                    c->materials = upload(device, c->cap.width, c->cap.height, D3DFMT_A8R8G8B8, texels);
                    printf("%s: with the game's materials (%s, %u%s)\n", path.c_str(), matPath.c_str(), header[3],
                           c->neonMaterials ? ", neon strips by their material" : "");
                }
                fclose(f);
            }
        }
        captures.push_back(c);
        return c;
    };

    pipeline.setProfiling(bench);
    pipeline.setSpillHistory(spillHistory);
    pipeline.setNeonHistory(neonHistory);
    int failures = 0;
    for (const Job& job : jobs) {
        LoadedCapture* loaded = loadCapture(job.capture);
        gfx::Target output;
        gfx::Target ownFrame, ownLuma, ownChroma; // --own: the export's frame and its planes
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
        in.gameLight = loaded->light;
        in.materialMask = loaded->materials;
        in.neonMaterials = loaded->neonMaterials;
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
        if (g_casterDepth >= 0.0f) in.casterMap = previewCasters;
        // The previous frame's camera, moved by job.move (world space): view translation
        // t' = t - move * R.
        if (waterHeight > -1e29f) {
            in.water[0] = in.water[1] = waterHeight;
            in.water[2] = waterKind;
        }
        // RenderScale (--set RenderScale=0.67): what the game would have drawn at that size, point
        // sampled from the capture (the game draws it at that size itself: one sample a pixel, no
        // detail averaged in), shaded at that size and brought up to the capture's with FSR 1 as
        // in game. Not with --photo or --video: the game keeps those at full resolution.
        gfx::Target scaledInputs[7];
        IDirect3DTexture9* fullInputs[7] = {}; // --jitter: sampled again every frame
        const D3DFORMAT formats[7] = {D3DFMT_A8R8G8B8, D3DFMT_R32F, D3DFMT_A16B16G16R16F, D3DFMT_A16B16G16R16F, D3DFMT_A16B16G16R16F,
                                      D3DFMT_A16B16G16R16F, D3DFMT_A8R8G8B8};
        int jitterGrid = 0; // --jitter: n, the scene is 1/n of the capture
        UINT sw = cap.width, sh = cap.height;
        const float renderScale = job.settings.renderScale;
        if (renderScale < 0.99f && !photoMode && !videoMode) {
            sw = static_cast<UINT>(cap.width * renderScale + 0.5f);
            sh = static_cast<UINT>(cap.height * renderScale + 0.5f);
            IDirect3DTexture9** const sources[7] = {&in.color, &in.depth, &in.hdr, &in.added, &in.gbuffer, &in.gameLight, &in.materialMask};
            device->BeginScene();
            for (int k = 0; k < 7; k++) {
                fullInputs[k] = *sources[k];
                if (!*sources[k]) continue;
                if (!pipeline.downscale(device, *sources[k], scaledInputs[k], sw, sh, formats[k], k == 1)) return 1;
                *sources[k] = scaledInputs[k].texture;
            }
            if (jitterRaster) jitterGrid = static_cast<int>(lroundf(1.0f / renderScale));
            if (jitterGrid < 2) jitterGrid = 0;
            device->EndScene();
            in.width = sw;
            in.height = sh;
            in.outputWidth = cap.width;
            in.outputHeight = cap.height;
            printf("%s: render scale %.2f, the scene at %ux%u\n", job.output.c_str(), renderScale, sw, sh);
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
        bool trail = false, posed = false;
        if ((job.trail > 0.0f || carPoseSet) && cap.depth.size() == static_cast<size_t>(cap.width) * cap.height) {
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
                trail = job.trail > 0.0f;
                posed = carPoseSet;
                if (trail) printf("%s: trail test, car at %.1f %.1f %.1f, %.1f m/s\n", job.output.c_str(), carEnd[0], carEnd[1], carEnd[2], job.trail);
            }
        }
        // --carpose: the car's axes, and how far the camera (and the car with it) moves each frame.
        float poseForward[3] = {}, poseRight[3] = {}, poseStep[3] = {};
        if (posed) {
            const float* V = cap.view;
            float f[3] = {V[0 * 4 + 2], 0.0f, V[2 * 4 + 2]};
            const float fl = sqrtf(f[0] * f[0] + f[2] * f[2]);
            if (fl < 1e-4f) posed = false;
            else {
                f[0] /= fl;
                f[2] /= fl;
                const float a = carPoseYaw * 3.14159265f / 180.0f;
                const float r[3] = {f[2], 0.0f, -f[0]};
                for (int j = 0; j < 3; j++) poseForward[j] = f[j] * cosf(a) + r[j] * sinf(a);
                poseRight[0] = poseForward[2];
                poseRight[2] = -poseForward[0];
                for (int j = 0; j < 3; j++) poseStep[j] = job.drive[0] * V[j * 4 + 0] + job.drive[1] * V[j * 4 + 1] + job.drive[2] * V[j * 4 + 2];
                printf("%s: car pose at %.1f %.1f %.1f, turned %.0f degrees from the camera, %.2f m a frame\n", job.output.c_str(), carEnd[0], carEnd[1],
                       carEnd[2], carPoseYaw, sqrtf(poseStep[0] * poseStep[0] + poseStep[1] * poseStep[1] + poseStep[2] * poseStep[2]));
            }
        }

        // Several frames so auto exposure and TAA settle like they would in game.
        LARGE_INTEGER t0{}, t1{}, freq{};
        QueryPerformanceFrequency(&freq);
        if (!job.keep) {
            pipeline.resetHistory();
            if (compare) pipelineB.resetHistory();
        }
        const int frames = job.frames > 0 ? job.frames : (bench ? 200 : (trail ? 150 : 40));
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
                if (job.clip) {
                    driven.driving = false;
                    driven.clipInRace = true;
                }
                // Rolled about the way it drives (carDir points back along the road).
                if (job.bank != 0.0f) {
                    const float a = job.bank * 3.14159265f / 180.0f;
                    const float f[3] = {-carDir[0], -carDir[1], -carDir[2]};
                    float r[3] = {f[2], 0.0f, -f[0]}; // right of the way, level
                    const float rl = sqrtf(r[0] * r[0] + r[2] * r[2]);
                    if (rl > 1e-4f) {
                        r[0] /= rl;
                        r[2] /= rl;
                        float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
                        if (u[1] < 0.0f) for (float& c : u) c = -c;
                        for (int j = 0; j < 3; j++) driven.carUp[j] = u[j] * cosf(a) + r[j] * sinf(a);
                    }
                }
                // Per frame as far as the frame's time step (time=: 0.02 s, else 0.1 s).
                const float step = job.trail * (job.time >= 0.0f ? 0.02f : 0.1f);
                for (int j = 0; j < 3; j++) driven.carPosition[j] = carEnd[j] + carDir[j] * back * step;
            }
            if (posed) {
                driven.carPoseKnown = true;
                // The depth finder hits the car's tail: its middle (between the axles) is about
                // 1.6 m ahead of that (and carEnd is 0.25 m towards the camera).
                const float* V = cap.view;
                float level[3] = {V[0 * 4 + 2], 0.0f, V[2 * 4 + 2]};
                const float ll = sqrtf(level[0] * level[0] + level[2] * level[2]);
                for (int j = 0; j < 3; j++) {
                    driven.carPosePosition[j] = carEnd[j] + level[j] / ll * 1.85f - poseStep[j] * back;
                    driven.carForward[j] = poseForward[j];
                    driven.carRight[j] = poseRight[j];
                    driven.carPoseUp[j] = j == 1 ? 1.0f : 0.0f;
                }
            }
            if (jitterGrid > 0) {
                // TAA jitter as plugin.cpp updateJitter: Halton (2, 3) over 8 frames, here picking
                // one of the n x n capture pixels under each scene pixel: the raster moves by
                // sub-pixels and aliases like the game's would.
                auto halton = [](int index, int base) {
                    float f = 1.0f, r = 0.0f;
                    for (int i = index; i > 0; i /= base) {
                        f /= static_cast<float>(base);
                        r += f * static_cast<float>(i % base);
                    }
                    return r;
                };
                jitterIndex = jitterIndex % 8 + 1;
                const int n = jitterGrid;
                const int ox = (std::min)(static_cast<int>(halton(jitterIndex, 2) * n), n - 1);
                const int oy = (std::min)(static_cast<int>(halton(jitterIndex, 3) * n), n - 1);
                for (int k = 0; k < 7; k++) {
                    if (!fullInputs[k]) continue;
                    pipeline.downscale(device, fullInputs[k], scaledInputs[k], sw, sh, formats[k], k == 1, (ox + 0.5f - n * 0.5f) / cap.width,
                                       (oy + 0.5f - n * 0.5f) / cap.height);
                }
                // A sample further right shows the image moved left. The projection carries it
                // as the game's does (P20, P21 in ndc).
                driven.jitter[0] = -(ox - (n - 1) * 0.5f) / n / static_cast<float>(sw);
                driven.jitter[1] = -(oy - (n - 1) * 0.5f) / n / static_cast<float>(sh);
                driven.projection[8] += 2.0f * driven.jitter[0];
                driven.projection[9] -= 2.0f * driven.jitter[1];
            }
            if (frame == frames - 1) QueryPerformanceCounter(&t0);
            LARGE_INTEGER renderStart;
            QueryPerformanceCounter(&renderStart);
            if (ownBits && frame == frames - 1) {
                // The export's own frame: every sample summed, the last one grades the sum.
                if (!ownFrame.create(device, cap.width, cap.height, D3DFMT_A16B16G16R16F)) fprintf(stderr, "own frame target failed\n");
                for (int r = 0; r <= repeats; r++) {
                    Pipeline::Inputs sample = driven;
                    if (r > 0) {
                        sample.temporal = false;
                        sample.repeat = true;
                    }
                    sample.accumulate = r == repeats ? 2 : 1;
                    sample.accumulateFirst = r == 0;
                    sample.floatOutput = r == repeats;
                    if (ownHdr) sample.hdrPeak = 1000.0f / 203.0f;
                    pipeline.render(device, sample, job.settings, r == repeats ? ownFrame.surface : output.surface);
                }
                const UINT vw = cap.width & ~7u, vh = cap.height & ~7u;
                const float crop[4] = {0.0f, 0.0f, static_cast<float>(vw) / cap.width, static_cast<float>(vh) / cap.height};
                if (!pipeline.exportPlanes(device, ownFrame.texture, vw, vh, crop, ownBits == 10, 1.0f, ownHdr ? 203.0f : 0.0f, ownLuma, ownChroma))
                    fprintf(stderr, "exportPlanes failed\n");
                if (exportFrames > 0) {
                    // --export: frames of the video export in a row, their planes read back as
                    // before (straight after each frame: the CPU waits for the GPU) and through
                    // PlaneReadback (locked a frame later), with `--cpu` ms of busy CPU a frame
                    // standing in for the game's own work. The planes are packed into a buffer
                    // as for the encoder.
                    auto drawFrame = [&]() {
                        for (int r = 0; r <= repeats; r++) {
                            Pipeline::Inputs sample = driven;
                            if (r > 0) {
                                sample.temporal = false;
                                sample.repeat = true;
                            }
                            sample.accumulate = r == repeats ? 2 : 1;
                            sample.accumulateFirst = r == 0;
                            sample.floatOutput = r == repeats;
                            if (ownHdr) sample.hdrPeak = 1000.0f / 203.0f;
                            pipeline.render(device, sample, job.settings, r == repeats ? ownFrame.surface : output.surface);
                        }
                        pipeline.exportPlanes(device, ownFrame.texture, vw, vh, crop, ownBits == 10, 1.0f, ownHdr ? 203.0f : 0.0f, ownLuma, ownChroma);
                    };
                    std::vector<uint8_t> packed;
                    unsigned packedFrames = 0;
                    const auto take = [&](const PlaneReadback::Frame& f) {
                        packed.resize(PlaneReadback::packedBytes(f));
                        PlaneReadback::pack(f, packed.data());
                        packedFrames++;
                    };
                    const auto now = [&]() {
                        LARGE_INTEGER t;
                        QueryPerformanceCounter(&t);
                        return 1000.0 * static_cast<double>(t.QuadPart) / static_cast<double>(freq.QuadPart);
                    };
                    for (int pipelined = 0; pipelined < 2; pipelined++) {
                        PlaneReadback read;
                        packedFrames = 0;
                        double readMs = 0.0;
                        drawFrame(); // warm up, then an idle GPU
                        if (read.queue(device, ownLuma, ownChroma, pipeline.focusTarget(), ownBits == 10)) read.pop(take);
                        read.lockMs = 0.0;
                        packedFrames = 0;
                        const double start = now();
                        for (int f = 0; f < exportFrames; f++) {
                            for (const double busy = now() + cpuMs; now() < busy;) {
                            }
                            drawFrame();
                            const double readStart = now();
                            if (!pipelined) {
                                if (read.queue(device, ownLuma, ownChroma, pipeline.focusTarget(), ownBits == 10)) read.pop(take);
                            } else {
                                while (!read.canQueue() && read.pending() > 0) read.pop(take);
                                read.queue(device, ownLuma, ownChroma, pipeline.focusTarget(), ownBits == 10);
                                while (read.pending() > 0 && read.oldestDone()) read.pop(take);
                            }
                            readMs += now() - readStart;
                        }
                        const double readStart = now();
                        while (read.pending() > 0) read.pop(take);
                        readMs += now() - readStart;
                        const double total = now() - start;
                        printf("export %s: %d frames (%u packed), %.1f ms a frame, reading back %.1f ms a frame (%.1f of it waiting in the "
                               "lock; %u found the copy unfinished, at most %d on the way), %.1f ms of CPU work a frame\n",
                               pipelined ? "pipelined" : "straight", exportFrames, packedFrames, total / exportFrames, readMs / exportFrames,
                               read.lockMs / exportFrames, read.waited, read.pendingPeak, cpuMs);
                        read.release();
                    }
                }
                const RECT all = {0, 0, static_cast<LONG>(cap.width), static_cast<LONG>(cap.height)};
                if (ownHdr) {
                    pipeline.showLinear(device, ownFrame.texture, output.surface, all);
                } else {
                    pipeline.resolveScene(device, ownFrame.texture, output.surface, all);
                }
                continue;
            }
            pipeline.render(device, driven, job.settings, output.surface);
            if (firstFrame) printf("CPU: pipeline created in %.1f ms, its first frame %.1f ms\n", initMs, msSince(renderStart));
            // The export's supersampling: the same moment again (the output is the last one).
            for (int r = 0; r < repeats && frame == frames - 1; r++) {
                Pipeline::Inputs again = driven;
                again.temporal = false;
                again.repeat = true;
                pipeline.render(device, again, job.settings, output.surface);
            }
            QueryPerformanceCounter(&renderStart);
            if (compare) {
                pipelineB.shareTrail(&pipeline); // as in game: the preset's side draws this side's trail
                pipelineB.render(device, driven, settingsB, outputB.surface);
            }
            if (compare && firstFrame) printf("CPU: second pipeline (compare) created in %.1f ms, its first frame %.1f ms\n", initBMs, msSince(renderStart));
            firstFrame = false;
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
        for (auto& t : scaledInputs) t.destroy();
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
        if (ownBits && ownLuma.valid() && ownChroma.valid()) {
            // The planes back to RGB (BT.709 studio range), next to the frame, and how far apart.
            const UINT vw = ownLuma.width * 4, vh = ownLuma.height;
            const bool ten = ownBits == 10;
            std::vector<uint8_t> y, uv;
            auto readPlane = [&](const gfx::Target& t, std::vector<uint8_t>& out) {
                IDirect3DSurface9* sys = nullptr;
                if (FAILED(device->CreateOffscreenPlainSurface(t.width, t.height, t.format, D3DPOOL_SYSTEMMEM, &sys, nullptr))) return false;
                D3DLOCKED_RECT lr{};
                bool ok = SUCCEEDED(device->GetRenderTargetData(t.surface, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
                if (ok) {
                    const size_t row = static_cast<size_t>(t.width) * (ten ? 8 : 4);
                    out.resize(row * t.height);
                    for (UINT r = 0; r < t.height; r++) memcpy(&out[row * r], static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(lr.Pitch) * r, row);
                    sys->UnlockRect();
                }
                sys->Release();
                return ok;
            };
            if (ownHdr && readPlane(ownLuma, y) && readPlane(ownChroma, uv)) {
                // HDR10: the planes as they are (P010, Y then interleaved Cb Cr), for ffmpeg; the
                // brightest luma code in nits as a check of the PQ curve.
                FILE* f = fopen((job.output + ".p010").c_str(), "wb");
                if (f) {
                    fwrite(y.data(), 1, y.size(), f);
                    fwrite(uv.data(), 1, uv.size(), f);
                    fclose(f);
                }
                int top = 0;
                for (size_t i = 0; i < y.size() / 2; i++) top = (std::max)(top, (y[2 * i] | (y[2 * i + 1] << 8)) >> 6);
                const double e = std::clamp((top / 4.0 - 16.0) / 219.0, 0.0, 1.0), p = pow(e, 1.0 / 78.84375);
                const double nits = 10000.0 * pow((std::max)(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);
                printf("own frame: HDR10 planes %ux%u -> %s.p010, brightest luma code %d (%.0f nits)\n", vw, vh, job.output.c_str(), top, nits);
            } else if (readPlane(ownLuma, y) && readPlane(ownChroma, uv)) {
                auto code = [ten](const std::vector<uint8_t>& p, size_t i) {
                    return ten ? ((p[2 * i] | (p[2 * i + 1] << 8)) >> 6) / 4.0f : static_cast<float>(p[i]);
                };
                std::vector<uint32_t> back(static_cast<size_t>(vw) * vh);
                double diff = 0.0, worst = 0.0;
                for (UINT py = 0; py < vh; py++) {
                    for (UINT px = 0; px < vw; px++) {
                        const float Y = (code(y, static_cast<size_t>(py) * vw + px) - 16.0f) / 219.0f;
                        const size_t c = static_cast<size_t>(py / 2) * vw + (px & ~1u);
                        const float cb = (code(uv, c) - 128.0f) / 224.0f, cr = (code(uv, c + 1) - 128.0f) / 224.0f;
                        const float r = Y + 1.5748f * cr, b = Y + 1.8556f * cb, g = (Y - 0.2126f * r - 0.0722f * b) / 0.7152f;
                        auto to8 = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
                        back[static_cast<size_t>(py) * vw + px] = 0xFF000000u | (to8(r) << 16) | (to8(g) << 8) | to8(b);
                        const uint32_t o = pixels[static_cast<size_t>(py) * cap.width + px];
                        const double dy = fabs((0.2126 * ((o >> 16) & 255) + 0.7152 * ((o >> 8) & 255) + 0.0722 * (o & 255)) / 255.0 - Y);
                        diff += dy;
                        worst = (std::max)(worst, dy);
                    }
                }
                writeBmp((job.output + ".yuv.bmp").c_str(), back, vw, vh);
                printf("own frame: %u samples, %s planes %ux%u, luma round trip off by %.4f on average, %.4f at most (of 1)\n", repeats + 1,
                       ten ? "P010" : "NV12", vw, vh, diff / (static_cast<double>(vw) * vh), worst);
            } else {
                fprintf(stderr, "planes readback failed\n");
                failures++;
            }
            ownLuma.destroy();
            ownChroma.destroy();
            ownFrame.destroy();
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
    for (auto& t : g_casterTex) {
        if (t) t->Release();
    }
    for (LoadedCapture* c : captures) {
        if (c->color) c->color->Release();
        if (c->depth) c->depth->Release();
        if (c->hdr) c->hdr->Release();
        if (c->added) c->added->Release();
        if (c->normals) c->normals->Release();
        if (c->light) c->light->Release();
        if (c->materials) c->materials->Release();
        delete c;
    }
    device->Release();
    d3d->Release();
    DestroyWindow(window);
    return failures ? 1 : 0;
}
