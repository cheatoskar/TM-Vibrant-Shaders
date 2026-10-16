// Offline test of the MP4 writer (src/mp4writer.cpp) with synthetic frames and sound, the way
// the game's video export hands them over: 24-bit BGR rows bottom-up (a Windows bitmap), the
// sound in pieces between the frames, the sound's format possibly before the video's.
//
//   tmvs_mp4test <out.mp4> <width> <height> <fps> <frames> <codec 1 = H.264, 2 = HEVC>
//                [--sound rate channels] [--sound-first] [--bitrate Mbit/s] [--bpp 3|4]
//
// The picture: a grey ramp, six colour patches (75 % bars) and a white bar that moves one
// pixel column per frame, so a decoder can check the colours and the frame order.
#include "log.h"
#include "mp4writer.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace tmshaders;

int main(int argc, char** argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: tmvs_mp4test <out.mp4> <width> <height> <fps> <frames> <codec 1|2> [--sound rate channels] [--sound-first] "
                        "[--bitrate Mbit/s] [--bpp 3|4]\n");
        return 2;
    }
    log::setFileName(L"mp4test.log");
    const int w = atoi(argv[2]), h = atoi(argv[3]), frames = atoi(argv[5]);
    const double fps = atof(argv[4]);
    const mp4::Codec codec = atoi(argv[6]) == 2 ? mp4::Codec::HEVC : mp4::Codec::H264;
    int rate = 0, channels = 0, bpp = 3;
    bool soundFirst = false;
    double bitrate = 0.0;
    for (int i = 7; i < argc; i++) {
        if (!strcmp(argv[i], "--sound") && i + 2 < argc) {
            rate = atoi(argv[++i]);
            channels = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--sound-first")) {
            soundFirst = true;
        } else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc) {
            bitrate = atof(argv[++i]) * 1e6;
        } else if (!strcmp(argv[i], "--bpp") && i + 1 < argc) {
            bpp = atoi(argv[++i]);
        }
    }
    printf("encoders: %s\n", mp4::describeEncoders().c_str());

    wchar_t path[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, path, MAX_PATH);
    mp4::Writer writer;
    writer.setPath(path);
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<WORD>(channels);
    format.nSamplesPerSec = static_cast<DWORD>(rate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(channels * 2);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    if (rate && soundFirst) writer.openAudio(format);
    std::string error;
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    if (!writer.openVideo(w, h, fps, codec, bitrate, error)) {
        printf("openVideo failed: %s\n", error.c_str());
        return 1;
    }
    printf("video: %s\n", writer.encoderName().c_str());
    if (rate && !soundFirst) writer.openAudio(format);

    const int stride = w * bpp;
    std::vector<uint8_t> image(static_cast<size_t>(stride) * h);
    // 75 % bars: white, yellow, cyan, green, magenta, red (B, G, R).
    static const uint8_t bars[6][3] = {{191, 191, 191}, {0, 191, 191}, {191, 191, 0}, {0, 191, 0}, {191, 0, 191}, {0, 0, 191}};
    std::vector<int16_t> sound;
    long long soundDone = 0;
    for (int f = 0; f < frames; f++) {
        for (int y = 0; y < h; y++) {
            // Bottom-up: memory row 0 is the picture's last row.
            uint8_t* row = image.data() + static_cast<size_t>(h - 1 - y) * stride;
            for (int x = 0; x < w; x++) {
                uint8_t* p = row + x * bpp;
                if (y < h / 2) {
                    const uint8_t* c = bars[x * 6 / w];
                    p[0] = c[0];
                    p[1] = c[1];
                    p[2] = c[2];
                } else {
                    const uint8_t g = static_cast<uint8_t>(x * 255 / (w - 1));
                    p[0] = p[1] = p[2] = g;
                }
                if (x == f % w && y >= h * 3 / 4) p[0] = p[1] = p[2] = 255;
                if (bpp == 4) p[3] = 255;
            }
        }
        if (!writer.writeFrame(image.data(), stride, bpp, true)) {
            printf("frame %d failed\n", f);
            return 1;
        }
        if (rate) {
            // The sound up to the end of this frame: a 440 Hz tone, 0.3 of full scale.
            const long long until = static_cast<long long>(std::llround((f + 1) * static_cast<double>(rate) / fps));
            sound.clear();
            for (long long s = soundDone; s < until; s++) {
                const int16_t v = static_cast<int16_t>(9830.0 * sin(2.0 * 3.14159265358979 * 440.0 * s / rate));
                for (int c = 0; c < channels; c++) sound.push_back(v);
            }
            soundDone = until;
            writer.writeAudio(sound.data(), sound.size() * sizeof(int16_t));
        }
    }
    std::string report;
    const bool ok = writer.close(report);
    QueryPerformanceCounter(&t1);
    const double seconds = static_cast<double>(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
    printf("%s: %s; %.1f s, %.1f frames/s\n", ok ? "ok" : "FAILED", report.c_str(), seconds, frames / seconds);
    return ok ? 0 : 1;
}
