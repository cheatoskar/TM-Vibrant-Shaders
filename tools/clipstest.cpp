// Clips test (src/clips): the run tracker (start, respawn, restart, finish), the buffer's cuts (key
// frames, the sound), the file names, then a synthetic run through each encoder this PC has (NVENC,
// the graphics card's Media Foundation encoder, and with --software Windows' own) into an MP4 with a
// tone: 20 s at 60 fps, the run from 5 s to 15 s, cut 3 s before and after. The file is checked with
// ffprobe when it is on the PATH (duration, streams, size), else by its MP4 boxes. --device: the NV12
// passes on a Direct3D 9 device (colours, cost per frame on the "game's" thread), and the recorder
// end to end with them.
//
//   tmvs_clipstest [--software] [--device] [--keep] [--out <folder>]     (exit code 0 = passed)
#include "clips/encoder.h"
#include "clips/grabber.h"
#include "clips/race.h"
#include "clips/recorder.h"
#include "clips/ring.h"
#include "videoencoder.h"
#include <windows.h>
#include <mmreg.h>
#include <mmsystem.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace tmshaders;
using namespace tmshaders::clips;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failures++;
}

std::string narrow(const std::wstring& w) {
    char out[1024] = {};
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out, sizeof(out), nullptr, nullptr);
    return out;
}

// --- The run tracker --------------------------------------------------------------------------

void testTracker() {
    printf("run tracker\n");
    RunTracker t;
    int race = 0;
    RaceSample s;
    s.valid = true;
    s.race = &race;
    double now = 100.0;
    RunEvent e;
    // Countdown.
    s.started = false;
    e = t.update(now, s);
    check(e.kind == RunEvent::None, "countdown: no event");
    // The run starts at clock 50000 (now = 101.0); the first frame sees 16 ms already.
    s.started = true;
    s.startClock = 50000;
    s.time = 16;
    now = 101.016;
    e = t.update(now, s);
    check(e.kind == RunEvent::Start && !e.restart && std::fabs(e.runStart - 101.0) < 1e-6, "start seen, run start = now - time");
    // Frames, one seen late (the estimate keeps the earliest).
    s.time = 1000;
    e = t.update(102.010, s);
    check(e.kind == RunEvent::None && std::fabs(t.runStart() - 101.0) < 1e-6, "the earliest start estimate is kept");
    // A respawn at a checkpoint: the clock runs on, same start.
    s.time = 5000;
    e = t.update(106.0, s);
    check(e.kind == RunEvent::None, "respawn: the same run");
    // Restart (Backspace): countdown, then a new start clock.
    s.started = false;
    s.startClock = 0;
    s.time = 0;
    t.update(107.0, s);
    s.started = true;
    s.startClock = 58000;
    s.time = 0;
    e = t.update(108.5, s);
    check(e.kind == RunEvent::Start && e.restart && std::fabs(e.runStart - 108.5) < 1e-6, "restart: a new run, flagged as restart");
    // Finish at 42.37 s.
    s.time = 42370;
    s.finished = true;
    e = t.update(150.9, s);
    check(e.kind == RunEvent::Finish && e.raceTime == 42370 && std::fabs(e.runStart - 108.5) < 1e-6 && std::fabs(e.finishAt - 150.9) < 1e-6,
          "finish: final time and times");
    e = t.update(151.0, s);
    check(e.kind == RunEvent::None, "finish reported once");
    // A pause (clock stops): the start estimate stays.
    RunTracker p;
    RaceSample q = s;
    q.finished = false;
    q.startClock = 1;
    q.time = 1000;
    p.update(10.0, q);
    p.update(20.0, q); // paused for 10 s
    q.time = 2000;
    p.update(21.0, q);
    check(std::fabs(p.runStart() - 9.0) < 1e-6, "pause: run start unchanged");
    // The map is left mid-run, a new map's run starts: not a restart.
    RaceSample none;
    p.update(22.0, none);
    int race2 = 0;
    q.race = &race2;
    q.startClock = 7;
    q.time = 10;
    e = p.update(30.0, q);
    check(e.kind == RunEvent::Start && !e.restart, "new map: a new run, not a restart");
    // First seen already finished: no finish event.
    RunTracker f;
    RaceSample done = s;
    e = f.update(1.0, done);
    check(e.kind == RunEvent::None, "a run first seen finished gives no event");
}

void testNames() {
    printf("names\n");
    check(stripCodes("$o$fffA$0f0-01$z") == "A-01", "colour codes out");
    check(stripCodes("$l[http://x]Link$l") == "Link", "link codes out");
    check(stripCodes("Cost $$5") == "Cost $5", "$$ stays a $");
    check(fileSafe("a/b:c*?") == "a_b_c__", "file name characters");
    check(fileSafe("") == "Clip", "empty name");
    check(formatTime(42370) == "0:42.37", "0:42.37");
    check(formatTime(3723450) == "1:02:03.45", "1:02:03.45");
}

// --- The buffer -------------------------------------------------------------------------------

std::shared_ptr<const std::vector<uint8_t>> bytes(size_t n) {
    return std::make_shared<const std::vector<uint8_t>>(n, static_cast<uint8_t>(0));
}

void testRing() {
    printf("buffer\n");
    PictureRing r;
    r.setLimits(10.0, 1u << 30);
    const int64_t frame = 166667; // 60 fps
    for (int i = 0; i < 60 * 30; i++) r.push({i * frame, i % 60 == 0, bytes(1000)});
    // 30 s pushed, 10 kept: whole seconds from a key frame, at least 10 s.
    const double kept = (r.newest() - r.oldest()) / 1e7;
    check(kept >= 10.0 && kept < 11.1, "keeps 10 s in whole groups");
    std::vector<Picture> c = r.cut(r.oldest(), r.newest());
    check(!c.empty() && c.front().key, "the oldest kept is a key frame");
    // A cut starting at 25.5 s starts at the key frame at 25 s.
    c = r.cut(static_cast<int64_t>(25.5e7), static_cast<int64_t>(27.0e7));
    check(!c.empty() && c.front().time == 25 * 60 * frame && c.front().key, "starts at the last key frame before the start");
    check(!c.empty() && c.back().time <= static_cast<int64_t>(27.0e7) && c.back().time > static_cast<int64_t>(27.0e7) - frame,
          "ends at the last picture before the end");
    // A cut asking for more than is kept starts at the oldest kept.
    c = r.cut(0, static_cast<int64_t>(22.0e7));
    check(!c.empty() && c.front().time == r.oldest(), "too early: from the oldest kept");
    // A byte limit.
    PictureRing b;
    b.setLimits(100.0, 60 * 1000 * 3);
    for (int i = 0; i < 60 * 10; i++) b.push({i * frame, i % 60 == 0, bytes(1000)});
    check(b.bytes() <= 60 * 1000 * 4 && b.bytes() >= 60 * 1000 * 3, "byte limit keeps whole groups");
    // Sound: 10 s at 48 kHz, frame n = n.
    SoundRing s;
    s.reset(48000, 2, 5.0, 1000000000);
    std::vector<int16_t> pcm(48000 * 2);
    for (int sec = 0; sec < 10; sec++) {
        for (int i = 0; i < 48000; i++) pcm[i * 2] = pcm[i * 2 + 1] = static_cast<int16_t>((sec * 48000 + i) & 0x7fff);
        s.push(pcm.data(), 48000);
    }
    size_t frames = 0;
    std::vector<int16_t> cut = s.cut(1000000000 + 70000000, 1000000000 + 80000000, frames); // 7..8 s
    check(frames == 48000 && cut[0] == static_cast<int16_t>((7 * 48000) & 0x7fff), "sound cut at the right sample");
    cut = s.cut(1000000000 + 20000000, 1000000000 + 30000000, frames); // 2..3 s: no longer kept
    check(frames == 48000 && cut[100] == 0, "sound no longer kept is silence");
    cut = s.cut(1000000000 + 95000000, 1000000000 + 105000000, frames); // 9.5..10.5 s: half there
    check(frames == 48000 && cut[2 * 100] != 0 && cut[2 * 47000] == 0, "sound after the end is silence");
}

// --- Encoders and the MP4 ---------------------------------------------------------------------

void makeFrame(std::vector<uint8_t>& f, int w, int h, int n) {
    // A moving gradient with a bright box: something for the encoder to do.
    uint8_t* y = f.data();
    for (int row = 0; row < h; row++) {
        const uint8_t base = static_cast<uint8_t>(16 + ((row + n * 4) % 200));
        memset(y + static_cast<size_t>(row) * w, base, w);
    }
    const int bx = (n * 13) % (w - 200), by = (n * 7) % (h - 200);
    for (int row = by; row < by + 200; row++) memset(y + static_cast<size_t>(row) * w + bx, 235, 200);
    uint8_t* uv = f.data() + static_cast<size_t>(w) * h;
    for (int row = 0; row < h / 2; row++) {
        for (int x = 0; x < w; x += 2) {
            uv[static_cast<size_t>(row) * w + x] = static_cast<uint8_t>(128 + ((x / 8 + n) % 40) - 20);
            uv[static_cast<size_t>(row) * w + x + 1] = static_cast<uint8_t>(128 + ((row / 8) % 40) - 20);
        }
    }
}

struct Probe {
    bool ran = false;
    double duration = 0.0, videoDuration = 0.0, audioDuration = 0.0;
    int streams = 0, width = 0, height = 0;
    std::string codecs;
};

std::string run(const std::string& cmd) {
    std::string out;
    FILE* p = _popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[512];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    _pclose(p);
    return out;
}

Probe ffprobe(const std::wstring& path) {
    Probe p;
    const std::string where = run("where ffprobe 2>nul");
    if (where.empty()) return p;
    const std::string out = run("ffprobe -v error -show_entries format=duration:stream=codec_name,width,height,duration -of default=nw=1 \"" +
                                narrow(path) + "\" 2>&1");
    p.ran = true;
    size_t at = 0;
    std::string stream;
    while (at < out.size()) {
        size_t end = out.find('\n', at);
        if (end == std::string::npos) end = out.size();
        std::string line = out.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        at = end + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "codec_name") {
            p.streams++;
            stream = value;
            p.codecs += (p.codecs.empty() ? "" : ",") + value;
        } else if (key == "width" && value != "N/A") {
            p.width = atoi(value.c_str());
        } else if (key == "height" && value != "N/A") {
            p.height = atoi(value.c_str());
        } else if (key == "duration" && value != "N/A") {
            // Stream durations come after their codec_name; the format's last.
            if (stream == "h264") p.videoDuration = atof(value.c_str());
            else if (stream == "aac") p.audioDuration = atof(value.c_str());
            p.duration = atof(value.c_str());
        }
    }
    return p;
}

// Without ffprobe: the boxes (moov / mvhd duration, trak count).
Probe boxes(const std::wstring& path) {
    Probe p;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return p;
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + n);
    fclose(f);
    auto be32 = [&](size_t at) { return (uint32_t(d[at]) << 24) | (uint32_t(d[at + 1]) << 16) | (uint32_t(d[at + 2]) << 8) | d[at + 3]; };
    std::function<void(size_t, size_t)> walk = [&](size_t at, size_t end) {
        while (at + 8 <= end) {
            uint32_t size = be32(at);
            const std::string type(reinterpret_cast<const char*>(&d[at + 4]), 4);
            if (size < 8 || at + size > end) break;
            if (type == "moov") walk(at + 8, at + size);
            if (type == "trak") p.streams++;
            if (type == "mvhd") {
                const uint32_t scale = be32(at + 20), duration = be32(at + 24);
                p.duration = scale ? static_cast<double>(duration) / scale : 0.0;
            }
            at += size;
        }
    };
    walk(0, d.size());
    p.ran = true;
    return p;
}

void checkFile(const std::wstring& path, double expected, bool sound, int w, int h) {
    Probe p = ffprobe(path);
    if (p.ran) {
        printf("  ffprobe: %s, %d streams, %dx%d, %.3f s (video %.3f, sound %.3f)\n", p.codecs.c_str(), p.streams, p.width, p.height, p.duration,
               p.videoDuration, p.audioDuration);
        check(p.streams == (sound ? 2 : 1), "stream count");
        check(p.width == w && p.height == h, "size");
        check(p.codecs.find("h264") != std::string::npos && (!sound || p.codecs.find("aac") != std::string::npos), "codecs h264 + aac");
        check(std::fabs(p.videoDuration - expected) < 0.1, "video length");
        if (sound) check(std::fabs(p.audioDuration - expected) < 0.15, "sound length");
        // Every picture decodes (ffprobe decodes them all and counts; ffmpeg's null output would
        // complain about frames closer than its 1/60 s time base, which a varying frame rate has).
        const std::string counted = run("ffprobe -v error -select_streams v -count_frames -count_packets -show_entries "
                                        "stream=nb_read_frames,nb_read_packets -of csv=p=0 \"" + narrow(path) + "\" 2>&1");
        int frames = -1, packets = -2;
        sscanf_s(counted.c_str(), "%d,%d", &frames, &packets);
        char what[200];
        snprintf(what, sizeof(what), "every picture decodes (%d of %d)%s", frames, packets, frames == packets ? "" : (": " + counted.substr(0, 120)).c_str());
        check(frames == packets && frames > 0, what);
    } else {
        p = boxes(path);
        printf("  boxes: %d tracks, %.3f s\n", p.streams, p.duration);
        check(p.streams == (sound ? 2 : 1), "track count");
        check(std::fabs(p.duration - expected) < 0.15, "length");
    }
}

// A synthetic run straight through an encoder: 20 s, frames every 1/60 s but with a stretch at
// 45 fps (frames left out, as a slow game gives), a 440 Hz tone; the run 5 s .. 15 s, cut -3 / +3.
void testEncoder(const char* label, std::unique_ptr<PictureEncoder> enc, const std::wstring& folder, bool keep) {
    printf("%s\n", label);
    if (!enc) return;
    printf("  %s\n", enc->description().c_str());
    const int w = 1920, h = 1080;
    std::vector<uint8_t> frame(static_cast<size_t>(w) * h * 3 / 2);
    PictureRing ring;
    ring.setLimits(60.0, 512u << 20);
    std::vector<int64_t> times;
    std::vector<uint8_t> header;
    double encodeMs = 0.0, worst = 0.0;
    int encoded = 0, keys = 0;
    const int64_t origin = 50000000000; // an arbitrary clock
    const PictureSink sink = [&](const EncodedPicture& p) {
        Picture pic;
        pic.time = times[static_cast<size_t>(p.frame)];
        pic.key = p.key;
        pic.data = std::make_shared<const std::vector<uint8_t>>(p.data, p.data + p.size);
        if (p.key) keys++;
        if (header.empty() && p.key) header = parameterSets(p.data, p.size);
        ring.push(pic);
        encoded++;
    };
    std::string error;
    bool ok = true;
    int n = 0;
    for (int i = 0; i < 20 * 60 && ok; i++) {
        // 8 s .. 10 s: every fourth frame left out (45 fps).
        if (i >= 8 * 60 && i < 10 * 60 && i % 4 == 3) continue;
        times.push_back(origin + static_cast<int64_t>(i * 1e7 / 60.0 + 0.5));
        makeFrame(frame, w, h, i);
        LARGE_INTEGER t0, t1, f;
        QueryPerformanceCounter(&t0);
        ok = enc->encode(frame.data(), sink, error);
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        const double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
        encodeMs += ms;
        worst = std::max(worst, ms);
        n++;
    }
    if (ok) ok = enc->flush(sink, error);
    check(ok, ok ? "encoded" : ("encoded: " + error).c_str());
    if (header.empty()) header = enc->header();
    printf("  %d frames in, %d pictures out, %d key frames, %.2f ms a frame (slowest %.1f), %.1f MB\n", n, encoded, keys, encodeMs / n, worst,
           ring.bytes() / 1048576.0);
    check(encoded == n, "a picture for every frame");
    check(keys >= 19 && keys <= 22, "a key frame every second");
    check(!header.empty(), "parameter sets found");
    // The tone.
    SoundRing sound;
    sound.reset(48000, 2, 30.0, origin - 5000000); // the sound started half a second before the first frame
    std::vector<int16_t> pcm(48000 * 2);
    for (int sec = 0; sec < 21; sec++) {
        for (int i = 0; i < 48000; i++) {
            const double t = sec + i / 48000.0;
            pcm[i * 2] = pcm[i * 2 + 1] = static_cast<int16_t>(8000.0 * sin(2.0 * 3.14159265358979 * 440.0 * t));
        }
        sound.push(pcm.data(), 48000);
    }
    // Cut: run 5 s .. 15 s, 3 s before and after.
    const int64_t from = origin + 20000000, to = origin + 180000000;
    std::vector<Picture> pictures = ring.cut(from, to);
    check(!pictures.empty() && pictures.front().time == from, "the cut starts on the key frame at 2 s");
    const std::vector<int64_t> durations = PictureRing::durations(pictures, 166667);
    std::vector<venc::Mp4Picture> list(pictures.size());
    for (size_t i = 0; i < pictures.size(); i++) {
        list[i].data = pictures[i].data->data();
        list[i].size = static_cast<uint32_t>(pictures[i].data->size());
        list[i].time = pictures[i].time;
        list[i].duration = durations[i];
        list[i].key = pictures[i].key;
    }
    size_t frames = 0;
    const std::vector<int16_t> cut = sound.cut(pictures.front().time, pictures.back().time + durations.back(), frames);
    venc::Mp4Sound s;
    s.format.wFormatTag = WAVE_FORMAT_PCM;
    s.format.nChannels = 2;
    s.format.nSamplesPerSec = 48000;
    s.format.wBitsPerSample = 16;
    s.format.nBlockAlign = 4;
    s.format.nAvgBytesPerSec = 48000 * 4;
    s.samples = cut.data();
    s.frames = frames;
    s.time = pictures.front().time;
    std::wstring path = folder + L"\\clipstest_" + std::wstring(label, label + strlen(label)) + L".mp4";
    std::replace(path.begin() + folder.size() + 1, path.end(), L' ', L'_');
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    ok = venc::writeMp4(path, venc::Codec::H264, w, h, 60.0, header, list, &s, error);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    printf("  MP4 written in %.0f ms: %s\n", (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart, narrow(path).c_str());
    check(ok, ok ? "MP4 written" : ("MP4 written: " + error).c_str());
    if (ok) checkFile(path, 16.0, true, w, h);
    if (ok && !keep) DeleteFileW(path.c_str());
}

// --- The NV12 passes on a device, and the recorder end to end ---------------------------------

void testDevice(const std::wstring& folder, bool keep) {
    printf("device\n");
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TMClipsTest";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"clipstest", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.BackBufferWidth = 2560; // a 1440p game, recorded at 1080p
    pp.BackBufferHeight = 1440;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    IDirect3DDevice9* device = nullptr;
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp,
                                         &device))) {
        check(false, "a Direct3D 9 device");
        return;
    }
    IDirect3DSurface9* back = nullptr;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
    // Colours: a solid colour through the passes, compared with BT.709 studio range.
    struct Colour {
        int r, g, b;
    } colours[] = {{200, 100, 50}, {16, 16, 16}, {255, 255, 255}, {0, 0, 255}};
    Grabber g;
    for (const Colour& c : colours) {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(c.r, c.g, c.b), 1.0f, 0);
        const bool grabbed = g.grab(device, back, 1920, 1080, 0);
        int y = -1, u = -1, v = -1;
        g.collect(
            [&](const PlaneReadback::Frame& f, int64_t) {
                std::vector<uint8_t> nv12(PlaneReadback::packedBytes(f));
                PlaneReadback::pack(f, nv12.data());
                y = nv12[1920 * 540 + 960];
                u = nv12[1920 * 1080 + 1920 * 270 + 960];
                v = nv12[1920 * 1080 + 1920 * 270 + 961];
            },
            true);
        const double r = c.r / 255.0, gg = c.g / 255.0, b = c.b / 255.0;
        const double l = 0.2126 * r + 0.7152 * gg + 0.0722 * b;
        const int ey = static_cast<int>(std::lround(16 + 219 * l)), eu = static_cast<int>(std::lround(128 + 224 * (b - l) / 1.8556)),
                  ev = static_cast<int>(std::lround(128 + 224 * (r - l) / 1.5748));
        char what[160];
        snprintf(what, sizeof(what), "RGB %d %d %d -> Y %d U %d V %d (expected %d %d %d)", c.r, c.g, c.b, y, u, v, ey, eu, ev);
        check(grabbed && std::abs(y - ey) <= 1 && std::abs(u - eu) <= 1 && std::abs(v - ev) <= 1, what);
    }
    // Cost on the "game's" thread: 600 frames of a changing picture, read back as the game would
    // (the frame before's), into the recorder.
    Recorder rec;
    RecorderSetup setup;
    setup.width = 1920;
    setup.height = 1080;
    setup.fps = 60;
    setup.bitrate = 12e6;
    setup.seconds = 30;
    setup.sound = true;
    rec.start(setup);
    for (int i = 0; i < 300 && rec.state() == Recorder::State::Starting; i++) Sleep(10);
    check(rec.state() == Recorder::State::Running, ("recorder running: " + rec.description() + rec.failure()).c_str());
    double grabMs = 0.0, collectMs = 0.0, worst = 0.0;
    int grabbed = 0;
    const int64_t start = now100ns();
    for (int i = 0; i < 600; i++) {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(i % 256, (i * 3) % 256, 128), 1.0f, 0);
        const int64_t c0 = now100ns();
        g.collect([&](const PlaneReadback::Frame& f, int64_t time) {
            uint8_t* dst = rec.acquire();
            if (!dst) return;
            PlaneReadback::pack(f, dst);
            rec.submit(time);
        });
        const double cm = (now100ns() - c0) / 1e4;
        if (g.grab(device, back, 1920, 1080, now100ns())) {
            grabbed++;
            grabMs += g.lastGrabMs;
            collectMs += cm;
            worst = std::max(worst, g.lastGrabMs + cm);
        }
        device->Present(nullptr, nullptr, nullptr, nullptr);
        // About 60 frames a second, as a game would present.
        while (now100ns() - start < static_cast<int64_t>((i + 1) * 1e7 / 60.0)) Sleep(1);
    }
    g.collect([&](const PlaneReadback::Frame& f, int64_t time) {
        uint8_t* dst = rec.acquire();
        if (!dst) return;
        PlaneReadback::pack(f, dst);
        rec.submit(time);
    }, true);
    Sleep(300);
    const RecorderStats st = rec.stats();
    printf("  game thread: %.2f ms a frame (copy and convert %.2f, read back and hand over %.2f, slowest %.2f); %d of 600 taken\n",
           (grabMs + collectMs) / std::max(1, grabbed), grabMs / std::max(1, grabbed), collectMs / std::max(1, grabbed), worst, grabbed);
    printf("  encoder thread: %d frames, %.2f ms a frame (slowest %.1f), %d dropped; buffer %.1f s, %.1f MB\n", st.framesEncoded,
           st.encodeMs / std::max(1, st.framesEncoded), st.encodeMaxMs, st.framesDropped, st.bufferSeconds, st.bufferBytes / 1048576.0);
    check(grabbed >= 590 && st.framesDropped < 10 && st.framesEncoded >= 580, "every frame through");
    // Save the last 5 s.
    SaveJob job;
    job.path = folder + L"\\clipstest_recorder.mp4";
    job.to = now100ns();
    job.from = job.to - 50000000;
    job.what = "last 5 s";
    rec.save(job);
    SaveResult r;
    for (int i = 0; i < 500 && !rec.takeResult(r); i++) Sleep(10);
    printf("  %s\n", r.line.c_str());
    check(r.ok, "recorder saved the last 5 s");
    rec.stop();
    if (r.ok) checkFile(job.path, r.seconds, true, 1920, 1080);
    if (r.ok && !keep) DeleteFileW(job.path.c_str());
    back->Release();
    g.releaseDevice();
    device->Release();
    d3d->Release();
    DestroyWindow(window);
}

} // namespace

int main(int argc, char** argv) {
    bool software = false, device = false, keep = false;
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    std::wstring folder = temp;
    if (!folder.empty() && folder.back() == L'\\') folder.pop_back();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--software")) software = true;
        else if (!strcmp(argv[i], "--device")) device = true;
        else if (!strcmp(argv[i], "--keep")) keep = true;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            wchar_t w[MAX_PATH] = {};
            MultiByteToWideChar(CP_UTF8, 0, argv[++i], -1, w, MAX_PATH);
            folder = w;
        }
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1); // Sleep(1) of 1 ms: the device test presents at 60 fps as a game does
    testTracker();
    testNames();
    testRing();
    EncoderSetup e;
    e.width = 1920;
    e.height = 1080;
    e.fps = 60;
    e.bitrate = 12e6;
    e.gopSeconds = 1.0;
    std::string why;
    auto nv = openNvenc(e, why);
    if (!nv) printf("NVENC: not here (%s)\n", why.c_str());
    testEncoder("NVENC", std::move(nv), folder, keep);
    why.clear();
    auto mft = openMft(e, why);
    if (!mft) printf("Media Foundation hardware: not here (%s)\n", why.c_str());
    testEncoder("Media Foundation", std::move(mft), folder, keep);
    if (software) {
        // Windows' own encoder, which every PC has: the asynchronous path is the hardware one's.
        e.softwareOnly = true;
        why.clear();
        // Skip the hardware ones: their names are listed first; open with software allowed and see.
        auto sw = openMft(e, why);
        testEncoder("Media Foundation software", std::move(sw), folder, keep);
    }
    if (device) testDevice(folder, keep);
    printf("%s (%d failed)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
