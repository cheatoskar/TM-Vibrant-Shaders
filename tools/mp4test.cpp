// Offline test of the video encoder (src/videoencoder.cpp) with synthetic frames and sound, the
// way the mod hands them over: NV12 / P010 from the GPU path, or bottom-up BGR from the game
// when the effects are off, the sound in pieces between the frames, possibly before the video.
//
//   tmvs_mp4test <out.mp4> <width> <height> <fps> <frames> [options]
//     --codec h264|hevc|av1   --10bit   --hdr   --quality 0..100   --bitrate Mbit/s
//     --rc auto|quality|qp|vbr   --input nv12|p010|bgr24|bgr32   --ffmpeg [path]   --preset 0..2
//     --sound rate channels   --sound-first   --psnr   --software (no hardware MFT)   --dump src.yuv
//     --discard-at n (discard() after n frames, then check that no file stays behind)
//   tmvs_mp4test --matrix <outdir> [--frames n] [--no-psnr] [--quick]
//     every codec x 8/10-bit x quality/bitrate, every input format, ffmpeg when it is on PATH,
//     then 1440p and 4K for the speed; one line per run.
//   tmvs_mp4test --sweep <outdir> [--frames n]   quality 40..100 per codec (for calibrating)
//
// The picture has four bands: a dark sky gradient that bands in 8 bits, a high-contrast track
// texture scrolling fast (racing footage), fine noise that changes every frame (grain, foliage)
// and 75 % colour bars. Every output is read back with a Media Foundation source reader: frame
// count, duration, codec, profile (bit depth) and colour tags, and with --psnr decoded and
// compared with the source (luma PSNR on the 8-bit scale, against the 10-bit source).
#include "log.h"
#include "videoencoder.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using namespace tmshaders;

namespace {

double nowMs() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

uint32_t hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

// The synthetic picture. A "world" twice the frame's width (periodic with the frame's width),
// built once in 10-bit YUV and once per input format; each frame copies rows from it with a
// band-specific offset, so producing a frame costs only memcpy.
struct Synth {
    int w = 0, h = 0, ww = 0;
    int b1 = 0, b2 = 0, b3 = 0; // band starts (even rows): track, noise, bars
    std::vector<uint16_t> Y, U, V; // 10-bit
    std::vector<uint8_t> nv12Y, nv12UV;
    std::vector<uint16_t> p010Y, p010UV;
    std::vector<uint8_t> bgr;
    int bgrBpp = 0;

    Synth(int width, int height) : w(width), h(height), ww(width * 2) {
        b1 = (h * 35 / 100) & ~1;
        b2 = (h * 60 / 100) & ~1;
        b3 = (h * 80 / 100) & ~1;
        Y.resize(static_cast<size_t>(ww) * h);
        U.resize(static_cast<size_t>(ww / 2) * (h / 2));
        V.resize(U.size());
        // 75 % bars (white, yellow, cyan, green, magenta, red) in 10-bit BT.709 studio range.
        static const double bars[6][3] = {{0.75, 0.75, 0.75}, {0.75, 0.75, 0}, {0, 0.75, 0.75}, {0, 0.75, 0}, {0.75, 0, 0.75}, {0.75, 0, 0}};
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < ww; x++) {
                const int px = x % w;
                double yv = 512, uv = 512, vv = 512;
                if (y < b1) {
                    const double t = static_cast<double>(px) / w, tri = 1.0 - fabs(2.0 * t - 1.0);
                    yv = 110.0 + 90.0 * tri + 24.0 * y / b1;
                    uv = 530.0 + 20.0 * tri;
                    vv = 500.0 - 10.0 * tri;
                } else if (y < b2) {
                    const bool block = (((px / 32) + (y / 32)) & 1) != 0;
                    const bool stripe = (((px / 2) + (y / 2)) & 1) != 0;
                    yv = block ? (stripe ? 820 : 300) : 450 + ((px * 7 + y * 13) % 64);
                    uv = 512 + (block ? 120 : -80);
                    vv = 512 + (((px / 64) & 1) ? 100 : -60);
                } else if (y < b3) {
                    const uint32_t r = hash(static_cast<uint32_t>(px) * 7919u + static_cast<uint32_t>(y) * 104729u);
                    const double n = ((r & 255) + ((r >> 8) & 255) - 255) / 255.0; // triangle -1..1
                    yv = 480 + 24.0 * n + (((px / 200) & 1) ? 120 : 0); // grain of about +-6 in 8 bits
                    uv = 512 + 12.0 * (((r >> 16) & 255) / 127.5 - 1.0);
                    vv = 512 + 12.0 * (((r >> 24) & 255) / 127.5 - 1.0) + (((px / 300) & 1) ? 60 : -40);
                } else {
                    const double* c = bars[px * 6 / w];
                    const double ly = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
                    yv = 64 + 876 * ly;
                    uv = 512 + 896 * (c[2] - ly) / 1.8556;
                    vv = 512 + 896 * (c[0] - ly) / 1.5748;
                    if (px % 240 < 6) yv = 940, uv = vv = 512; // thin white posts: motion is easy to see
                }
                Y[static_cast<size_t>(y) * ww + x] = static_cast<uint16_t>(std::clamp(yv, 64.0, 940.0));
                if (!(x & 1) && !(y & 1)) {
                    const size_t i = static_cast<size_t>(y / 2) * (ww / 2) + x / 2;
                    U[i] = static_cast<uint16_t>(std::clamp(uv, 64.0, 960.0));
                    V[i] = static_cast<uint16_t>(std::clamp(vv, 64.0, 960.0));
                }
            }
        }
    }

    // Where row y of frame f comes from: the world row and the (even) column offset.
    void source(int f, int y, int& row, int& off) const {
        row = y;
        if (y < b1) off = (f * 2) % w;
        else if (y < b2) off = (f * (std::max(2, w / 80) & ~1)) % w;
        else if (y < b3) {
            off = (f * 74) % w;
            row = b2 + ((y - b2) + f * 14) % (b3 - b2);
        } else off = (f * 8) % w;
    }

    void build(venc::Pixels p) {
        if (p == venc::Pixels::NV12 && nv12Y.empty()) {
            nv12Y.resize(Y.size());
            for (size_t i = 0; i < Y.size(); i++) nv12Y[i] = static_cast<uint8_t>(std::min(255, (Y[i] + 2) >> 2));
            nv12UV.resize(U.size() * 2);
            for (size_t i = 0; i < U.size(); i++) {
                nv12UV[i * 2] = static_cast<uint8_t>(std::min(255, (U[i] + 2) >> 2));
                nv12UV[i * 2 + 1] = static_cast<uint8_t>(std::min(255, (V[i] + 2) >> 2));
            }
        } else if (p == venc::Pixels::P010 && p010Y.empty()) {
            p010Y.resize(Y.size());
            for (size_t i = 0; i < Y.size(); i++) p010Y[i] = static_cast<uint16_t>(Y[i] << 6);
            p010UV.resize(U.size() * 2);
            for (size_t i = 0; i < U.size(); i++) {
                p010UV[i * 2] = static_cast<uint16_t>(U[i] << 6);
                p010UV[i * 2 + 1] = static_cast<uint16_t>(V[i] << 6);
            }
        } else if ((p == venc::Pixels::BGR24 || p == venc::Pixels::BGR32) && bgrBpp != (p == venc::Pixels::BGR32 ? 4 : 3)) {
            bgrBpp = p == venc::Pixels::BGR32 ? 4 : 3;
            bgr.assign(static_cast<size_t>(ww) * h * bgrBpp, 255);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < ww; x++) {
                    const double yy = (Y[static_cast<size_t>(y) * ww + x] - 64) / 876.0;
                    const size_t c = static_cast<size_t>(y / 2) * (ww / 2) + x / 2;
                    const double cb = (U[c] - 512) / 896.0, cr = (V[c] - 512) / 896.0;
                    const double r = yy + 1.5748 * cr, g = yy - 0.1873 * cb - 0.4681 * cr, b = yy + 1.8556 * cb;
                    uint8_t* d = bgr.data() + (static_cast<size_t>(y) * ww + x) * bgrBpp;
                    d[0] = static_cast<uint8_t>(std::clamp(std::lround(b * 255), 0L, 255L));
                    d[1] = static_cast<uint8_t>(std::clamp(std::lround(g * 255), 0L, 255L));
                    d[2] = static_cast<uint8_t>(std::clamp(std::lround(r * 255), 0L, 255L));
                }
            }
        }
    }

    void fill(venc::Pixels p, int f, uint8_t* dst) const {
        int row = 0, off = 0;
        if (p == venc::Pixels::NV12) {
            for (int y = 0; y < h; y++) {
                source(f, y, row, off);
                memcpy(dst + static_cast<size_t>(y) * w, nv12Y.data() + static_cast<size_t>(row) * ww + off, w);
            }
            uint8_t* uv = dst + static_cast<size_t>(w) * h;
            for (int y = 0; y < h / 2; y++) {
                source(f, y * 2, row, off);
                memcpy(uv + static_cast<size_t>(y) * w, nv12UV.data() + static_cast<size_t>(row / 2) * ww + off, w);
            }
        } else if (p == venc::Pixels::P010) {
            uint16_t* d = reinterpret_cast<uint16_t*>(dst);
            for (int y = 0; y < h; y++) {
                source(f, y, row, off);
                memcpy(d + static_cast<size_t>(y) * w, p010Y.data() + static_cast<size_t>(row) * ww + off, w * 2);
            }
            uint16_t* uv = d + static_cast<size_t>(w) * h;
            for (int y = 0; y < h / 2; y++) {
                source(f, y * 2, row, off);
                memcpy(uv + static_cast<size_t>(y) * w, p010UV.data() + static_cast<size_t>(row / 2) * ww + off, w * 2);
            }
        } else {
            // Bottom-up: memory row 0 is the picture's last row.
            for (int y = 0; y < h; y++) {
                source(f, y, row, off);
                memcpy(dst + static_cast<size_t>(h - 1 - y) * w * bgrBpp, bgr.data() + (static_cast<size_t>(row) * ww + off) * bgrBpp,
                       static_cast<size_t>(w) * bgrBpp);
            }
        }
    }

    // The source's luma of frame f in 10 bits, for the PSNR.
    void luma(int f, std::vector<uint16_t>& out) const {
        out.resize(static_cast<size_t>(w) * h);
        int row = 0, off = 0;
        for (int y = 0; y < h; y++) {
            source(f, y, row, off);
            memcpy(out.data() + static_cast<size_t>(y) * w, Y.data() + static_cast<size_t>(row) * ww + off, w * 2);
        }
    }
};

std::string fourcc(const GUID& g) {
    char t[5] = {static_cast<char>(g.Data1 & 255), static_cast<char>((g.Data1 >> 8) & 255), static_cast<char>((g.Data1 >> 16) & 255),
                 static_cast<char>((g.Data1 >> 24) & 255), 0};
    return t;
}

struct Check {
    bool opened = false;
    int frames = 0;
    double seconds = 0.0;
    std::string codec, colour;
    UINT32 width = 0, height = 0, profile = 0;
    std::string audio = "none";
    double psnr = -1.0, psnrMin = -1.0;
    int decoded = 0;
};

// Reads the file back: the compressed samples are counted, the format and the colour tags
// read; with `synth` every frame is decoded and compared with the source.
Check verify(const std::wstring& path, double fps, const Synth* synth, bool tenBit) {
    Check c;
    IMFSourceReader* reader = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader))) return c;
    c.opened = true;
    IMFMediaType* native = nullptr;
    if (SUCCEEDED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native))) {
        GUID sub{};
        native->GetGUID(MF_MT_SUBTYPE, &sub);
        c.codec = fourcc(sub);
        MFGetAttributeSize(native, MF_MT_FRAME_SIZE, &c.width, &c.height);
        native->GetUINT32(MF_MT_MPEG2_PROFILE, &c.profile);
        UINT32 prim = 0, trc = 0, matrix = 0, range = 0;
        native->GetUINT32(MF_MT_VIDEO_PRIMARIES, &prim);
        native->GetUINT32(MF_MT_TRANSFER_FUNCTION, &trc);
        native->GetUINT32(MF_MT_YUV_MATRIX, &matrix);
        native->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range);
        char t[64];
        snprintf(t, sizeof(t), "p%u/t%u/m%u/r%u", prim, trc, matrix, range);
        c.colour = t;
        reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, native);
        native->Release();
    }
    IMFMediaType* audio = nullptr;
    if (SUCCEEDED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &audio))) {
        GUID sub{};
        audio->GetGUID(MF_MT_SUBTYPE, &sub);
        UINT32 rate = 0, ch = 0;
        audio->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        audio->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
        char t[64];
        snprintf(t, sizeof(t), "%s %u Hz %u ch", sub == MFAudioFormat_AAC ? "AAC" : fourcc(sub).c_str(), rate, ch);
        c.audio = t;
        audio->Release();
    }
    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration))) {
        c.seconds = duration.uhVal.QuadPart / 1e7;
    }
    PropVariantClear(&duration);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    for (;;) {
        DWORD flags = 0;
        LONGLONG time = 0;
        IMFSample* sample = nullptr;
        if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &time, &sample))) break;
        if (sample) {
            c.frames++;
            sample->Release();
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    reader->Release();
    if (!synth) return c;

    // Decoded, in presentation order, compared with the source.
    IMFAttributes* attributes = nullptr;
    MFCreateAttributes(&attributes, 1);
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), attributes, &reader);
    attributes->Release();
    if (FAILED(hr)) return c;
    bool p010 = tenBit;
    for (int attempt = 0; attempt < 2; attempt++) {
        IMFMediaType* type = nullptr;
        MFCreateMediaType(&type);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, p010 ? MFVideoFormat_P010 : MFVideoFormat_NV12);
        hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type);
        type->Release();
        if (SUCCEEDED(hr)) break;
        p010 = false;
    }
    if (FAILED(hr)) {
        reader->Release();
        return c;
    }
    LONG defaultStride = 0;
    IMFMediaType* current = nullptr;
    if (SUCCEEDED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current))) {
        UINT32 s = 0;
        if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &s))) defaultStride = static_cast<LONG>(s);
        current->Release();
    }
    std::vector<uint16_t> ref;
    double sum = 0.0, worst = 1e9;
    LONGLONG first = -1; // the first frame's time: files with B-frames may start later than 0 (an edit list MF ignores)
    for (;;) {
        DWORD flags = 0;
        LONGLONG time = 0;
        IMFSample* sample = nullptr;
        if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &time, &sample))) break;
        if (sample) {
            if (first < 0) first = time;
            const int f = static_cast<int>(std::llround((time - first) * fps / 1e7));
            IMFMediaBuffer* buffer = nullptr;
            sample->GetBufferByIndex(0, &buffer);
            IMF2DBuffer* buffer2d = nullptr;
            BYTE* scan = nullptr;
            LONG pitch = 0;
            bool locked = false;
            if (buffer && SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d))) && SUCCEEDED(buffer2d->Lock2D(&scan, &pitch))) {
                locked = true;
            } else if (buffer) {
                DWORD len = 0;
                if (SUCCEEDED(buffer->Lock(&scan, nullptr, &len))) {
                    locked = true;
                    pitch = defaultStride ? defaultStride : synth->w * (p010 ? 2 : 1);
                }
            }
            if (locked) {
                synth->luma(f, ref);
                double se = 0.0;
                for (int y = 0; y < synth->h; y++) {
                    const BYTE* row = scan + static_cast<ptrdiff_t>(y) * pitch;
                    const uint16_t* r = ref.data() + static_cast<size_t>(y) * synth->w;
                    for (int x = 0; x < synth->w; x++) {
                        const double d = p010 ? (reinterpret_cast<const uint16_t*>(row)[x] >> 6) / 4.0 : row[x];
                        const double e = d - r[x] / 4.0;
                        se += e * e;
                    }
                }
                const double mse = se / (static_cast<double>(synth->w) * synth->h);
                const double psnr = mse > 0 ? 10.0 * log10(255.0 * 255.0 / mse) : 99.0;
                sum += psnr;
                worst = std::min(worst, psnr);
                c.decoded++;
                if (buffer2d) buffer2d->Unlock2D();
                else buffer->Unlock();
            }
            if (buffer2d) buffer2d->Release();
            if (buffer) buffer->Release();
            sample->Release();
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    reader->Release();
    if (c.decoded) {
        c.psnr = sum / c.decoded;
        c.psnrMin = worst;
    }
    return c;
}

struct Run {
    std::string name;
    std::wstring path;
    venc::Options o;
    venc::Pixels input = venc::Pixels::NV12;
    int frames = 120;
    int soundRate = 48000, soundChannels = 2;
    bool soundFirst = false;
    bool psnr = true;
    int discardAt = -1; // discard() after this many frames: nothing may stay behind
    std::wstring dump; // the source frames as raw NV12 / P010 too (to check the PSNR with ffmpeg's psnr filter)
};

struct Result {
    bool ok = false;
    std::string description, report, error;
    venc::Stats stats;
    double callerMs = 0.0; // the game thread's cost per frame: acquire/submit/pushBgr (not the synthesis)
    uint64_t bytes = 0;
    Check check;
};

const char* pixelsName(venc::Pixels p) {
    return p == venc::Pixels::NV12 ? "NV12" : p == venc::Pixels::P010 ? "P010" : p == venc::Pixels::BGR24 ? "BGR24" : "BGR32";
}

Result execute(const Run& r, Synth& synth) {
    Result res;
    venc::Encoder enc;
    enc.setPath(r.path);
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<WORD>(r.soundChannels);
    format.nSamplesPerSec = static_cast<DWORD>(r.soundRate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(r.soundChannels * 2);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    if (r.soundRate && r.soundFirst) enc.openAudio(format);
    if (!enc.open(r.path, r.o, res.error)) return res;
    res.description = enc.description();
    if (r.soundRate && !r.soundFirst) enc.openAudio(format);
    synth.build(r.input);
    const bool bgr = r.input == venc::Pixels::BGR24 || r.input == venc::Pixels::BGR32;
    std::vector<uint8_t> game(bgr ? venc::Encoder::frameBytes(r.input, r.o.width, r.o.height) : 0);
    std::vector<int16_t> sound;
    long long soundDone = 0;
    double caller = 0.0;
    FILE* dump = r.dump.empty() ? nullptr : _wfopen(r.dump.c_str(), L"wb");
    for (int f = 0; f < r.frames; f++) {
        if (bgr) {
            synth.fill(r.input, f, game.data());
            const double t0 = nowMs();
            const bool ok = enc.pushBgr(game.data(), r.input == venc::Pixels::BGR32 ? 4 : 3);
            caller += nowMs() - t0;
            if (!ok) break;
        } else {
            double t0 = nowMs();
            uint8_t* p = enc.acquire(r.input);
            caller += nowMs() - t0;
            if (!p) break;
            synth.fill(r.input, f, p);
            if (dump) fwrite(p, 1, venc::Encoder::frameBytes(r.input, r.o.width, r.o.height), dump);
            t0 = nowMs();
            enc.submit();
            caller += nowMs() - t0;
        }
        if (f + 1 == r.discardAt) break;
        if (r.soundRate) {
            // The sound up to the end of this frame: a 440 Hz tone, 0.3 of full scale.
            const long long until = static_cast<long long>(std::llround((f + 1) * static_cast<double>(r.soundRate) / r.o.fps));
            sound.clear();
            for (long long s = soundDone; s < until; s++) {
                const int16_t v = static_cast<int16_t>(9830.0 * sin(2.0 * 3.14159265358979 * 440.0 * s / r.soundRate));
                for (int c = 0; c < r.soundChannels; c++) sound.push_back(v);
            }
            soundDone = until;
            enc.writeAudio(sound.data(), sound.size() * sizeof(int16_t));
        }
    }
    if (dump) fclose(dump);
    if (r.discardAt > 0) {
        enc.discard();
        // Whatever starts with the file's name (temporaries, .wav, ffmpeg's log) must be gone.
        std::wstring pattern = r.path.substr(0, r.path.find_last_of(L'.')) + L"*";
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileW(pattern.c_str(), &data);
        int left = 0;
        if (find != INVALID_HANDLE_VALUE) {
            do left++; while (FindNextFileW(find, &data));
            FindClose(find);
        }
        res.ok = left == 0;
        res.report = "discarded after " + std::to_string(r.discardAt) + " frames, " + std::to_string(left) + " files left behind";
        res.stats = enc.stats();
        return res;
    }
    res.ok = enc.close(res.report);
    res.stats = enc.stats();
    res.callerMs = caller / std::max(1, r.frames);
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExW(r.path.c_str(), GetFileExInfoStandard, &data)) res.bytes = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    const venc::Options& eff = enc.options();
    res.check = verify(r.path, r.o.fps, r.psnr ? &synth : nullptr, eff.tenBit);
    return res;
}

void printHeader() {
    printf("%-26s %-4s %6s %6s %-5s %4s %-15s %8s %7s %6s %6s %7s %7s %7s %5s %s\n", "run", "ok", "frames", "secs", "codec", "prof", "audio", "MB",
           "Mbit/s", "PSNR", "min", "enc ms", "conv ms", "game ms", "peak", "waits");
}

void printResult(const Run& r, const Result& res) {
    if (!res.error.empty()) {
        printf("%-26s FAIL open: %s\n", r.name.c_str(), res.error.c_str());
        return;
    }
    const venc::Stats& s = res.stats;
    const double n = std::max(1, s.frames);
    const double mbit = res.check.seconds > 0 ? res.bytes * 8.0 / res.check.seconds / 1e6 : 0.0;
    printf("%-26s %-4s %3d/%-3d %6.2f %-5s %4u %-15s %8.2f %7.2f %6.2f %6.2f %7.2f %7.2f %7.3f %2d/%-2d %d\n", r.name.c_str(), res.ok ? "ok" : "FAIL",
           res.check.frames, r.frames, res.check.seconds, res.check.codec.c_str(), res.check.profile, res.check.audio.c_str(), res.bytes / 1048576.0, mbit,
           res.check.psnr, res.check.psnrMin, s.encodeMs / n, s.convertMs / n, res.callerMs, s.queuePeak, s.queueSlots, s.waits);
    printf("    %s | %s | colour %s\n", res.description.c_str(), res.report.c_str(), res.check.colour.c_str());
    fflush(stdout);
}

venc::Codec parseCodec(const char* s) {
    if (!_stricmp(s, "hevc") || !_stricmp(s, "2")) return venc::Codec::HEVC;
    if (!_stricmp(s, "av1") || !_stricmp(s, "3")) return venc::Codec::AV1;
    return venc::Codec::H264;
}

venc::Pixels parsePixels(const char* s) {
    if (!_stricmp(s, "p010")) return venc::Pixels::P010;
    if (!_stricmp(s, "bgr24")) return venc::Pixels::BGR24;
    if (!_stricmp(s, "bgr32")) return venc::Pixels::BGR32;
    return venc::Pixels::NV12;
}

std::wstring widen(const std::string& s) {
    wchar_t path[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, path, MAX_PATH);
    return path;
}

// One matrix entry.
Run make(const std::string& outdir, const char* name, int w, int h, venc::Codec codec, bool ten, venc::Pixels input, int frames, bool psnr) {
    Run r;
    r.name = name;
    r.path = widen(outdir + "\\" + name + ".mp4");
    r.o.width = w;
    r.o.height = h;
    r.o.fps = 60.0;
    r.o.codec = codec;
    r.o.tenBit = ten;
    r.input = input;
    r.frames = frames;
    r.psnr = psnr;
    return r;
}

int matrix(const std::string& outdir, int frames, bool psnr, bool quick, bool sweep) {
    CreateDirectoryW(widen(outdir).c_str(), nullptr);
    using venc::Codec;
    using venc::Pixels;
    std::vector<Run> runs;
    const int W = 1920, H = 1080;
    if (sweep) {
        const Codec codecs[2] = {Codec::H264, Codec::HEVC};
        for (Codec c : codecs) {
            for (int q = 40; q <= 100; q += 10) {
                char name[64];
                snprintf(name, sizeof(name), "sweep_%s_q%d", c == Codec::H264 ? "h264" : "hevc", q);
                Run r = make(outdir, name, W, H, c, false, Pixels::NV12, frames, psnr);
                r.o.quality = q;
                runs.push_back(r);
                snprintf(name, sizeof(name), "sweep_%s_qp_q%d", c == Codec::H264 ? "h264" : "hevc", q);
                r = make(outdir, name, W, H, c, false, Pixels::NV12, frames, psnr);
                r.o.quality = q;
                r.o.rateControl = venc::RateControl::ConstantQP;
                runs.push_back(r);
            }
        }
    } else {
        Run r;
        // Media Foundation, 1080p60: every codec x depth x rate control, every input format.
        runs.push_back(make(outdir, "mf_h264_q80", W, H, Codec::H264, false, Pixels::NV12, frames, psnr));
        r = make(outdir, "mf_h264_q60", W, H, Codec::H264, false, Pixels::NV12, frames, psnr);
        r.o.quality = 60;
        runs.push_back(r);
        r = make(outdir, "mf_h264_q95", W, H, Codec::H264, false, Pixels::NV12, frames, psnr);
        r.o.quality = 95;
        runs.push_back(r);
        r = make(outdir, "mf_h264_qp_q80", W, H, Codec::H264, false, Pixels::NV12, frames, psnr);
        r.o.rateControl = venc::RateControl::ConstantQP;
        runs.push_back(r);
        r = make(outdir, "mf_h264_vbr_q80", W, H, Codec::H264, false, Pixels::NV12, frames, psnr);
        r.o.rateControl = venc::RateControl::VBR;
        runs.push_back(r);
        r = make(outdir, "mf_h264_20mbit", W, H, Codec::H264, false, Pixels::NV12, frames, psnr);
        r.o.bitrate = 20e6;
        runs.push_back(r);
        r = make(outdir, "mf_h264_bgr24_soundfirst", W, H, Codec::H264, false, Pixels::BGR24, frames, psnr);
        r.soundFirst = true;
        runs.push_back(r);
        runs.push_back(make(outdir, "mf_h264_bgr32", W, H, Codec::H264, false, Pixels::BGR32, frames, psnr));
        runs.push_back(make(outdir, "mf_h264_p010in", W, H, Codec::H264, false, Pixels::P010, frames, psnr));
        r = make(outdir, "mf_h264_10bit_req", W, H, Codec::H264, true, Pixels::P010, frames, psnr);
        runs.push_back(r);
        runs.push_back(make(outdir, "mf_hevc_q80", W, H, Codec::HEVC, false, Pixels::NV12, frames, psnr));
        r = make(outdir, "mf_hevc_20mbit", W, H, Codec::HEVC, false, Pixels::NV12, frames, psnr);
        r.o.bitrate = 20e6;
        runs.push_back(r);
        runs.push_back(make(outdir, "mf_hevc10_q80", W, H, Codec::HEVC, true, Pixels::P010, frames, psnr));
        r = make(outdir, "mf_hevc10_qp_q80", W, H, Codec::HEVC, true, Pixels::P010, frames, psnr);
        r.o.rateControl = venc::RateControl::ConstantQP;
        runs.push_back(r);
        r = make(outdir, "mf_hevc10_20mbit", W, H, Codec::HEVC, true, Pixels::P010, frames, psnr);
        r.o.bitrate = 20e6;
        runs.push_back(r);
        runs.push_back(make(outdir, "mf_hevc10_nv12in", W, H, Codec::HEVC, true, Pixels::NV12, frames, psnr));
        runs.push_back(make(outdir, "mf_hevc10_bgr24", W, H, Codec::HEVC, true, Pixels::BGR24, frames, psnr));
        r = make(outdir, "mf_hevc10_hdr10", W, H, Codec::HEVC, true, Pixels::P010, frames, psnr);
        r.o.hdr10 = true;
        runs.push_back(r);
        runs.push_back(make(outdir, "mf_av1_q80", W, H, Codec::AV1, false, Pixels::NV12, frames, psnr));
        runs.push_back(make(outdir, "mf_av1_10_q80", W, H, Codec::AV1, true, Pixels::P010, frames, psnr));
        r = make(outdir, "mf_av1_20mbit", W, H, Codec::AV1, false, Pixels::NV12, frames, psnr);
        r.o.bitrate = 20e6;
        runs.push_back(r);
        // ffmpeg, when it is there.
        const venc::FfmpegInfo ff = venc::findFfmpeg();
        if (ff.path.empty()) {
            printf("ffmpeg: not found, its runs skipped\n");
        } else {
            const auto ffRun = [&](const char* name, Codec c, bool ten, Pixels in, int preset) {
                Run x = make(outdir, name, W, H, c, ten, in, frames, psnr);
                x.o.backend = venc::Backend::Ffmpeg;
                x.o.ffmpegPreset = preset;
                return x;
            };
            runs.push_back(ffRun("ff_x264_q80", Codec::H264, false, Pixels::NV12, 1));
            runs.push_back(ffRun("ff_x264_10_q80", Codec::H264, true, Pixels::P010, 1));
            r = ffRun("ff_x264_20mbit", Codec::H264, false, Pixels::NV12, 1);
            r.o.bitrate = 20e6;
            runs.push_back(r);
            runs.push_back(ffRun("ff_x264_bgr24", Codec::H264, false, Pixels::BGR24, 0));
            runs.push_back(ffRun("ff_x265_10_q80", Codec::HEVC, true, Pixels::P010, 1));
            r = ffRun("ff_x265_10_hdr10", Codec::HEVC, true, Pixels::P010, 0);
            r.o.hdr10 = true;
            runs.push_back(r);
            runs.push_back(ffRun("ff_svtav1_10_q80", Codec::AV1, true, Pixels::P010, 1));
            runs.push_back(ffRun("ff_svtav1_q80", Codec::AV1, false, Pixels::NV12, 1));
        }
        // Speed at 1440p and 4K (no PSNR: the decode would take longer than the encode).
        if (!quick) {
            const int pf = std::max(frames, 300);
            runs.push_back(make(outdir, "perf_1440_h264", 2560, 1440, Codec::H264, false, Pixels::NV12, pf, false));
            runs.push_back(make(outdir, "perf_1440_hevc10", 2560, 1440, Codec::HEVC, true, Pixels::P010, pf, false));
            runs.push_back(make(outdir, "perf_1440_h264_bgr24", 2560, 1440, Codec::H264, false, Pixels::BGR24, pf, false));
            runs.push_back(make(outdir, "perf_4k_h264", 3840, 2160, Codec::H264, false, Pixels::NV12, pf, false));
            runs.push_back(make(outdir, "perf_4k_hevc", 3840, 2160, Codec::HEVC, false, Pixels::NV12, pf, false));
            runs.push_back(make(outdir, "perf_4k_hevc10", 3840, 2160, Codec::HEVC, true, Pixels::P010, pf, false));
            runs.push_back(make(outdir, "perf_4k_hevc10_bgr32", 3840, 2160, Codec::HEVC, true, Pixels::BGR32, pf, false));
            runs.push_back(make(outdir, "perf_1080_h264", W, H, Codec::H264, false, Pixels::NV12, pf, false));
            runs.push_back(make(outdir, "perf_1080_hevc10", W, H, Codec::HEVC, true, Pixels::P010, pf, false));
        }
    }
    printHeader();
    int failures = 0;
    Synth* synth = nullptr;
    for (const Run& r : runs) {
        if (!synth || synth->w != r.o.width || synth->h != r.o.height) {
            delete synth;
            synth = new Synth(r.o.width, r.o.height);
        }
        const Result res = execute(r, *synth);
        printResult(r, res);
        if (!res.ok || res.check.frames != r.frames) failures++;
    }
    delete synth;
    printf("%d of %zu runs failed or lost frames\n", failures, runs.size());
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    log::setFileName(L"mp4test.log");
    MFStartup(MF_VERSION, MFSTARTUP_FULL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    printf("encoders: %s\n", venc::describeEncoders().c_str());
    fflush(stdout);
    if (argc >= 3 && (!strcmp(argv[1], "--matrix") || !strcmp(argv[1], "--sweep"))) {
        int frames = 120;
        bool psnr = true, quick = false;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--no-psnr")) psnr = false;
            else if (!strcmp(argv[i], "--quick")) quick = true;
        }
        return matrix(argv[2], frames, psnr, quick, !strcmp(argv[1], "--sweep"));
    }
    if (argc < 6) {
        fprintf(stderr, "usage: tmvs_mp4test <out.mp4> <width> <height> <fps> <frames> [--codec h264|hevc|av1] [--10bit] [--hdr] [--quality q] "
                        "[--bitrate Mbit/s] [--rc auto|quality|qp|vbr] [--input nv12|p010|bgr24|bgr32] [--ffmpeg [path]] [--preset 0..2] "
                        "[--sound rate channels] [--sound-first] [--psnr] [--software] [--dump src.yuv] [--discard-at n]\n"
                        "       tmvs_mp4test --matrix|--sweep <outdir> [--frames n] [--no-psnr] [--quick]\n");
        return 2;
    }
    Run r;
    r.name = "single";
    r.path = widen(argv[1]);
    r.o.width = atoi(argv[2]);
    r.o.height = atoi(argv[3]);
    r.o.fps = atof(argv[4]);
    r.frames = atoi(argv[5]);
    r.soundRate = 0;
    r.psnr = false;
    for (int i = 6; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "--codec") && i + 1 < argc) r.o.codec = parseCodec(argv[++i]);
        else if (!strcmp(a, "--10bit")) r.o.tenBit = true;
        else if (!strcmp(a, "--hdr")) r.o.hdr10 = true;
        else if (!strcmp(a, "--quality") && i + 1 < argc) r.o.quality = atoi(argv[++i]);
        else if (!strcmp(a, "--bitrate") && i + 1 < argc) r.o.bitrate = atof(argv[++i]) * 1e6;
        else if (!strcmp(a, "--rc") && i + 1 < argc) {
            const char* m = argv[++i];
            r.o.rateControl = !strcmp(m, "quality") ? venc::RateControl::Quality : !strcmp(m, "qp") ? venc::RateControl::ConstantQP
                            : !strcmp(m, "vbr") ? venc::RateControl::VBR : venc::RateControl::Auto;
        } else if (!strcmp(a, "--input") && i + 1 < argc) r.input = parsePixels(argv[++i]);
        else if (!strcmp(a, "--ffmpeg")) {
            r.o.backend = venc::Backend::Ffmpeg;
            if (i + 1 < argc && argv[i + 1][0] != '-') r.o.ffmpegPath = widen(argv[++i]);
        } else if (!strcmp(a, "--preset") && i + 1 < argc) r.o.ffmpegPreset = atoi(argv[++i]);
        else if (!strcmp(a, "--sound") && i + 2 < argc) {
            r.soundRate = atoi(argv[++i]);
            r.soundChannels = atoi(argv[++i]);
        } else if (!strcmp(a, "--sound-first")) r.soundFirst = true;
        else if (!strcmp(a, "--psnr")) r.psnr = true;
        else if (!strcmp(a, "--software")) r.o.hardware = false;
        else if (!strcmp(a, "--discard-at") && i + 1 < argc) r.discardAt = atoi(argv[++i]);
        else if (!strcmp(a, "--dump") && i + 1 < argc) r.dump = widen(argv[++i]);
    }
    Synth synth(r.o.width, r.o.height);
    printf("input %s\n", pixelsName(r.input));
    printHeader();
    const Result res = execute(r, synth);
    printResult(r, res);
    return res.ok && res.check.frames == r.frames ? 0 : 1;
}
