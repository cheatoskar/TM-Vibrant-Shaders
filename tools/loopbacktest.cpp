// Loopback test: plays a 440 Hz tone through waveOut (as the game's and the mod's sound would
// play) and records this process's own output with src/loopback.cpp, as the video export's sound
// pass does. Checks that the recording holds the tone at the right pitch and writes it as a WAV.
//
//   tmvs_loopbacktest [--system] [out.wav]
//     --system: the loopback of the default playback device instead of the process loopback
//     --mix: the same in the device's own format, through the converter
#include "loopback.h"
#include <windows.h>
#include <mmsystem.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace tmshaders;

namespace {

constexpr int kRate = 44100;
constexpr double kTone = 440.0;

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

} // namespace

int main(int argc, char** argv) {
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
        const int16_t v = static_cast<int16_t>(std::sin(2.0 * 3.14159265358979 * kTone * t) * 6000.0 * fade);
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

    // Loudness and pitch (zero crossings) of the part where the tone plays.
    const size_t frames = recorded.size() / 2;
    double energy = 0.0;
    size_t loud = 0, crossings = 0, first = 0, last = 0;
    for (size_t i = 0; i < frames; i++) {
        const double v = recorded[2 * i] / 32768.0;
        energy += v * v;
        if (std::fabs(v) > 0.05) {
            if (!loud) first = i;
            last = i;
            loud++;
        }
    }
    for (size_t i = first + 1; i <= last && last > first; i++) {
        if ((recorded[2 * (i - 1)] < 0) != (recorded[2 * i] < 0)) crossings++;
    }
    const double rms = frames ? std::sqrt(energy / frames) : 0.0;
    const double span = last > first ? static_cast<double>(last - first) / kRate : 0.0;
    const double pitch = span > 0.0 ? crossings / 2.0 / span : 0.0;
    const loopback::Stats s = capture.stats();
    printf("recorded %.2f s (tone heard from %.3f s for %.2f s), rms %.3f, pitch %.1f Hz\n", frames / static_cast<double>(kRate),
           first / static_cast<double>(kRate), span, rms, pitch);
    printf("stats: captured %.2f s, delivered %.2f s, silence put in %.2f s, missing at reads %.2f s, dropped %.2f s, glitches %u\n",
           s.captured / static_cast<double>(kRate), s.delivered / static_cast<double>(kRate), s.filled / static_cast<double>(kRate),
           s.underrun / static_cast<double>(kRate), s.dropped / static_cast<double>(kRate), s.glitches);
    if (writeWav(out, recorded, kRate, 2)) printf("written: %s\n", out);
    const bool ok = rms > 0.02 && pitch > kTone - 5.0 && pitch < kTone + 5.0;
    printf("%s\n", ok ? "OK: the tone came through the loopback" : "FAIL: no tone in the recording");
    return ok ? 0 : 1;
}
