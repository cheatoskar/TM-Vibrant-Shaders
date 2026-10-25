// Loopback test: plays a tone through waveOut (as the game's and the mod's sound would play) and
// records this process's own output with src/loopback.cpp, as the video export's sound pass does.
// The tone is 19 kHz at -40 dB, as the export's latency measurement: inaudible for most, found in
// the recording with a Goertzel filter. Checks that it came through, writes the recording as a WAV.
// Still: it plays through the speakers or headphones; ask before running it on someone's PC.
//
//   tmvs_loopbacktest [--system] [out.wav]
//     --system: the loopback of the default playback device instead of the process loopback
//     --mix: the same in the device's own format, through the converter
//   tmvs_loopbacktest --restart
//     silent: plays digital silence (nothing to hear) and starts the capture twice, as the export
//     does (the latency probe, then the sound pass); both runs must capture
#include "loopback.h"
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace tmshaders;

namespace {

constexpr int kRate = 44100;
constexpr double kTone = 19000.0;
constexpr double kLevel = 330.0; // -40 dB

bool writeWav(const char* path, const std::vector<int16_t>& pcm, int rate, int channels) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const uint32_t data = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    const uint32_t riff = 36 + data;
    const uint16_t format = 1, ch = static_cast<uint16_t>(channels), bits = 16, align = static_cast<uint16_t>(channels * 2);
    const uint32_t r = static_cast<uint32_t>(rate), bytes = r * align, fmtSize = 16;
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtSize, 4, 1, f);
    fwrite(&format, 2, 1, f);
    fwrite(&ch, 2, 1, f);
    fwrite(&r, 4, 1, f);
    fwrite(&bytes, 4, 1, f);
    fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data, 4, 1, f);
    fwrite(pcm.data(), 1, data, f);
    fclose(f);
    return true;
}

// Two runs of one capture while this process plays zeros: the capture must deliver in both.
int restartTest() {
    loopback::Capture capture;
    capture.prepare(kRate, 2, loopback::Mode::ProcessOnly);
    if (!capture.waitReady(5000)) {
        printf("FAIL: no loopback: %s\n", capture.error().c_str());
        return 1;
    }
    printf("source: %s\n", capture.description().c_str());
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = kRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = kRate * 4;
    HWAVEOUT device = nullptr;
    if (waveOutOpen(&device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        printf("FAIL: no playback device\n");
        return 1;
    }
    std::vector<int16_t> silence(static_cast<size_t>(kRate) * 6 * 2, 0);
    WAVEHDR header{};
    header.lpData = reinterpret_cast<LPSTR>(silence.data());
    header.dwBufferLength = static_cast<DWORD>(silence.size() * sizeof(int16_t));
    waveOutPrepareHeader(device, &header, sizeof(header));
    waveOutWrite(device, &header, sizeof(header));
    bool ok = true;
    for (int run = 0; run < 2; run++) {
        if (!capture.waitReady(3000) || !capture.start()) {
            printf("FAIL: run %d did not start\n", run + 1);
            ok = false;
            break;
        }
        std::vector<int16_t> sink(static_cast<size_t>(kRate) * 2);
        for (int i = 0; i < 30; i++) {
            Sleep(50);
            const size_t n = (std::min)(capture.available(), static_cast<size_t>(kRate));
            if (n) capture.read(sink.data(), n);
        }
        capture.stop();
        const loopback::Stats s = capture.stats();
        printf("run %d: captured %.2f s, silence put in %.2f s, glitches %u\n", run + 1, s.captured / static_cast<double>(kRate),
               s.filled / static_cast<double>(kRate), s.glitches);
        if (s.captured < kRate) ok = false;
        Sleep(300);
    }
    waveOutReset(device);
    waveOutUnprepareHeader(device, &header, sizeof(header));
    waveOutClose(device);
    printf("%s\n", ok ? "OK: both runs captured" : "FAIL: a run captured nothing");
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--restart") == 0) return restartTest();
    bool system = false, mix = false;
    const char* out = "loopbacktest.wav";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--system") == 0) system = true;
        else if (strcmp(argv[i], "--mix") == 0) mix = true;
        else out = argv[i];
    }

    // 4 s of tone, quiet enough not to startle, with a short fade in and out.
    std::vector<int16_t> tone(static_cast<size_t>(kRate) * 4 * 2);
    for (size_t i = 0; i < tone.size() / 2; i++) {
        const double t = static_cast<double>(i) / kRate;
        const double fade = (std::min)(1.0, (std::min)(t, 4.0 - t) / 0.05);
        const int16_t v = static_cast<int16_t>(std::sin(2.0 * 3.14159265358979 * kTone * t) * kLevel * fade);
        tone[2 * i] = v;
        tone[2 * i + 1] = v;
    }

    loopback::Capture capture;
    capture.prepare(kRate, 2, mix ? loopback::Mode::SystemMixFormat : system ? loopback::Mode::SystemOnly : loopback::Mode::ProcessOnly);
    if (!capture.waitReady(5000)) {
        printf("FAIL: no loopback: %s\n", capture.error().c_str());
        return 1;
    }
    printf("source: %s\n", capture.description().c_str());

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = kRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = kRate * 4;
    HWAVEOUT device = nullptr;
    if (waveOutOpen(&device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        printf("FAIL: no playback device to play the tone\n");
        return 1;
    }
    // Recording starts first (as in the game: the capture opens, the clip plays): a moment of
    // silence, then the tone.
    capture.start();
    Sleep(300);
    WAVEHDR header{};
    header.lpData = reinterpret_cast<LPSTR>(tone.data());
    header.dwBufferLength = static_cast<DWORD>(tone.size() * sizeof(int16_t));
    waveOutPrepareHeader(device, &header, sizeof(header));
    waveOutWrite(device, &header, sizeof(header));
    // Read as the game does: whenever a second has come in, a second.
    std::vector<int16_t> recorded;
    for (int i = 0; i < 50; i++) {
        Sleep(50);
        const size_t n = capture.available();
        if (n >= kRate) {
            std::vector<int16_t> chunk(static_cast<size_t>(kRate) * 2);
            capture.read(chunk.data(), kRate);
            recorded.insert(recorded.end(), chunk.begin(), chunk.end());
        }
    }
    capture.stop();
    const size_t rest = capture.available();
    std::vector<int16_t> chunk(rest * 2);
    if (rest) capture.read(chunk.data(), rest);
    recorded.insert(recorded.end(), chunk.begin(), chunk.end());
    waveOutReset(device);
    waveOutUnprepareHeader(device, &header, sizeof(header));
    waveOutClose(device);

    // The tone's level over blocks of 32 frames (Goertzel at kTone): where it reaches half its peak.
    const size_t frames = recorded.size() / 2, block = 32;
    const double coeff = 2.0 * std::cos(2.0 * 3.14159265358979 * kTone / kRate);
    double peak = 0.0;
    size_t first = 0, last = 0, blocks = 0;
    std::vector<double> level;
    for (size_t b = 0; b + block <= frames; b += block) {
        double s1 = 0.0, s2 = 0.0;
        for (size_t i = 0; i < block; i++) {
            const double s0 = recorded[2 * (b + i)] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        level.push_back(2.0 * std::sqrt((std::max)(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2)) / block);
        peak = (std::max)(peak, level.back());
    }
    for (size_t i = 0; i < level.size(); i++) {
        if (level[i] < 0.5 * peak) continue;
        if (!blocks) first = i * block;
        last = i * block;
        blocks++;
    }
    const double span = static_cast<double>(last - first) / kRate;
    const loopback::Stats s = capture.stats();
    printf("recorded %.2f s (tone heard from %.3f s for %.2f s), its level %.0f (played %.0f)\n", frames / static_cast<double>(kRate),
           first / static_cast<double>(kRate), span, peak, kLevel);
    printf("stats: captured %.2f s, delivered %.2f s, silence put in %.2f s, missing at reads %.2f s, dropped %.2f s, glitches %u\n",
           s.captured / static_cast<double>(kRate), s.delivered / static_cast<double>(kRate), s.filled / static_cast<double>(kRate),
           s.underrun / static_cast<double>(kRate), s.dropped / static_cast<double>(kRate), s.glitches);
    if (writeWav(out, recorded, kRate, 2)) printf("written: %s\n", out);
    const bool ok = peak > kLevel * 0.3 && span > 2.0;
    printf("%s\n", ok ? "OK: the tone came through the loopback" : "FAIL: no tone in the recording");
    return ok ? 0 : 1;
}
