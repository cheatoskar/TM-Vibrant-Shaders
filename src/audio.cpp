#include "audio.h"
#include "log.h"
#include "weather.h"
#include <mmsystem.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace tmshaders {
namespace audio {
namespace {

constexpr int kRate = 44100;
constexpr int kBufferFrames = 2048; // ~46 ms per buffer
constexpr int kBuffers = 4;
constexpr float kPi = 3.14159265f;

// Shared with the mixer thread.
std::atomic<float> g_rain{0.0f};      // 0 .. 2
std::atomic<float> g_volume{0.0f};    // master, already faded for focus / menus
std::atomic<int> g_thunderSerial{0};  // bumped for every new thunder
std::atomic<float> g_thunderPower{0.0f};
std::atomic<float> g_thunderDistance{0.0f}; // 0 close .. 1 far
std::atomic<bool> g_running{false};

HWAVEOUT g_device = nullptr;
std::atomic<bool> g_failed{false};

// Main-thread bookkeeping.
int g_lastSlot = -1;
struct PendingThunder {
    float at = -1.0f;
    float power = 0.0f;
    float distance = 0.0f;
};
PendingThunder g_pending[4];
float g_fade = 0.0f;

// --- Synthesis ---------------------------------------------------------------------

struct Noise {
    uint32_t state;
    explicit Noise(uint32_t seed) : state(seed) {}
    float next() { // white noise, -1..1
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(static_cast<int32_t>(state)) / 2147483648.0f;
    }
    float uniform() { return next() * 0.5f + 0.5f; }
};

struct OnePole {
    float a = 0.0f, y = 0.0f;
    void setCutoff(float hz) { a = 1.0f - expf(-2.0f * kPi * hz / kRate); }
    float process(float x) { return y += a * (x - y); }
};

// Rain: a broad wash of countless far drops (noise shaped to 400 Hz - 5 kHz), a patter of
// single near drops (a short tick plus the "plink" of the bubble each drop leaves on wet
// ground, randomly panned) and, in a downpour, a low roar.
struct Rain {
    Noise noise{0x1234567u};
    OnePole washLow1[2], washLow2[2], washHigh[2], roar[2], tick[2];
    float dropEnv[2] = {}, dropGain[2] = {};
    float plinkPhase[2] = {}, plinkFreq[2] = {}, plinkRise[2] = {};
    float level = 0.0f;

    Rain() {
        for (int c = 0; c < 2; c++) {
            washLow1[c].setCutoff(5000.0f);
            washLow2[c].setCutoff(5000.0f);
            washHigh[c].setCutoff(400.0f);
            roar[c].setCutoff(260.0f);
            tick[c].setCutoff(3000.0f);
        }
    }

    // gain: < 1 while it thunders (the thunder masks the rain, as the ear hears it).
    void render(float* out, int frames, float target, float gain) {
        for (int i = 0; i < frames; i++) {
            level += (target - level) * 0.00005f; // ~0.5 s glide
            if (level < 1e-4f) continue;
            const float steady = fminf(level, 1.0f);
            const float heavy = fmaxf(level - 0.8f, 0.0f);
            // Single drops: Poisson events.
            const float rate = (70.0f + 200.0f * level) / kRate;
            if (noise.uniform() < rate) {
                const int c = noise.uniform() < 0.5f ? 0 : 1;
                dropEnv[c] = 1.0f;
                dropGain[c] = 0.1f + 0.6f * noise.uniform() * noise.uniform();
                plinkPhase[c] = 0.0f;
                plinkFreq[c] = 1200.0f + 2600.0f * noise.uniform();
                plinkRise[c] = 1.0f + 0.00004f * noise.uniform(); // the bubble's pitch rises
            }
            for (int c = 0; c < 2; c++) {
                const float w = noise.next();
                const float low = washLow2[c].process(washLow1[c].process(w));
                const float wash = low - washHigh[c].process(low);
                const float rumble = roar[c].process(noise.next());
                dropEnv[c] *= 0.9975f; // ~9 ms
                plinkFreq[c] *= plinkRise[c];
                plinkPhase[c] += 2.0f * kPi * plinkFreq[c] / kRate;
                if (plinkPhase[c] > 2.0f * kPi) plinkPhase[c] -= 2.0f * kPi;
                const float env = dropEnv[c] * dropGain[c];
                const float drop = tick[c].process(noise.next()) * env * env * 2.0f + sinf(plinkPhase[c]) * env * 0.25f;
                out[i * 2 + c] += (wash * (0.5f * steady + 0.25f * heavy) + rumble * 0.6f * heavy + drop * steady) * gain;
            }
        }
    }
};

// Thunder: a sharp crack for close strikes, then a long low rumble that rolls in a few
// swells, louder and brighter the closer the strike. The rumble alone sits below 100 Hz,
// which laptop speakers and many headsets can't play: the "roll" (120 Hz - 1 kHz, with a
// fluttering level like the crackle of a real roll of thunder) is what most people hear.
struct Thunder {
    Noise noise{0xBADC0DEu};
    OnePole low[2], body[2], crackHigh[2], rollLow[2], rollHigh[2], flutter[2];
    float brown[2] = {};
    float t = -1.0f;      // seconds since start, < 0 = silent
    float power = 0.0f, distance = 0.0f;
    float swellAt[3] = {}, swellWidth[3] = {};
    float level = 0.0f;   // current loudness (0..1), ducks the rain

    void start(float p, float d) {
        t = 0.0f;
        power = p;
        distance = d;
        for (int k = 0; k < 3; k++) {
            swellAt[k] = 0.4f + noise.uniform() * (2.5f + d * 2.0f);
            swellWidth[k] = 0.6f + noise.uniform() * 1.2f;
        }
        const float cutoff = 90.0f + (1.0f - d) * 260.0f;
        for (int c = 0; c < 2; c++) {
            low[c].setCutoff(cutoff);
            body[c].setCutoff(cutoff * 3.0f);
            crackHigh[c].setCutoff(1500.0f);
            rollLow[c].setCutoff(450.0f + (1.0f - d) * 550.0f); // far thunder is duller
            rollHigh[c].setCutoff(120.0f);
            flutter[c].setCutoff(9.0f);
        }
    }

    void render(float* out, int frames) {
        if (t < 0.0f) return;
        const float dt = 1.0f / kRate;
        for (int i = 0; i < frames; i++) {
            float env = expf(-t / (1.8f + distance * 2.0f)) * fminf(t / 0.08f, 1.0f);
            for (int k = 0; k < 3; k++) {
                const float x = (t - swellAt[k]) / swellWidth[k];
                env += 0.7f * expf(-x * x);
            }
            const float crack = expf(-t * 14.0f) * (1.0f - distance * 0.7f); // the strike itself
            for (int c = 0; c < 2; c++) {
                const float w = noise.next();
                brown[c] = brown[c] * 0.995f + w * 0.05f;
                const float rumble = low[c].process(brown[c]) * 6.0f + (body[c].process(w) - low[c].y) * 0.6f;
                const float snap = (w - crackHigh[c].process(w)) * crack;
                const float band = rollLow[c].process(noise.next());
                const float roll = band - rollHigh[c].process(band);
                const float wobble = 0.35f + fabsf(flutter[c].process(noise.next() * 40.0f)); // ~0.35 .. 1.5, ~9 Hz
                out[i * 2 + c] += (rumble * env * 0.12f + roll * env * wobble * 3.2f + snap * 0.8f) * power;
            }
            this->level = fminf(env, 1.0f) * power;
            t += dt;
        }
        if (t > 12.0f) {
            t = -1.0f;
            this->level = 0.0f;
        }
    }
};

void mixer() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = kRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = kRate * 4;
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (waveOutOpen(&g_device, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(ready), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        TMVS_LOG("audio: no output device");
        CloseHandle(ready);
        g_failed = true;
        g_running = false;
        return;
    }
    std::vector<int16_t> data(static_cast<size_t>(kBufferFrames) * 2 * kBuffers);
    WAVEHDR headers[kBuffers] = {};
    for (int b = 0; b < kBuffers; b++) {
        headers[b].lpData = reinterpret_cast<LPSTR>(&data[static_cast<size_t>(b) * kBufferFrames * 2]);
        headers[b].dwBufferLength = kBufferFrames * 4;
        waveOutPrepareHeader(g_device, &headers[b], sizeof(WAVEHDR));
        headers[b].dwFlags |= WHDR_DONE; // free to fill (preparing clears every other flag)
    }
    Rain rain;
    Thunder thunder[2];
    int thunderSerial = g_thunderSerial.load();
    int nextThunder = 0;
    float volume = 0.0f;
    std::vector<float> mix(static_cast<size_t>(kBufferFrames) * 2);
    // Where the sound goes, and the Windows mixer volume of the game (it applies to us too).
    WAVEOUTCAPSW caps{};
    UINT deviceId = 0;
    waveOutGetID(g_device, &deviceId);
    waveOutGetDevCapsW(deviceId, &caps, sizeof(caps));
    DWORD mixerVolume = 0;
    waveOutGetVolume(g_device, &mixerVolume);
    char deviceName[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, deviceName, sizeof(deviceName) - 1, nullptr, nullptr);
    TMVS_LOG("audio: weather sound started (device \"%s\", mixer volume %d%%)", deviceName, static_cast<int>((mixerVolume & 0xFFFF) * 100 / 0xFFFF));
    // While audible, the loudness that goes out, now and then (to tell "silent" from "too quiet").
    double levelSum = 0.0;
    int levelSamples = 0, levelLogs = 0;

    DWORD lastReturn = GetTickCount();
    bool stallLogged = false;
    while (g_running) {
        bool wrote = false;
        for (auto& h : headers) {
            if (!(h.dwFlags & WHDR_DONE)) continue;
            const int serial = g_thunderSerial.load();
            if (serial != thunderSerial) {
                thunderSerial = serial;
                thunder[nextThunder].start(g_thunderPower.load(), g_thunderDistance.load());
                nextThunder ^= 1;
            }
            std::fill(mix.begin(), mix.end(), 0.0f);
            const float thunderLevel = fmaxf(thunder[0].level, thunder[1].level);
            rain.render(mix.data(), kBufferFrames, g_rain.load(), 1.0f - 0.45f * thunderLevel);
            for (auto& th : thunder) th.render(mix.data(), kBufferFrames);
            const float target = g_volume.load();
            int16_t* out = reinterpret_cast<int16_t*>(h.lpData);
            for (int i = 0; i < kBufferFrames * 2; i++) {
                volume += (target - volume) * 0.0005f;
                const float v = tanhf(mix[static_cast<size_t>(i)] * volume); // soft limit
                out[i] = static_cast<int16_t>(v * 32000.0f);
                levelSum += v * v;
            }
            levelSamples += kBufferFrames * 2;
            if (target < 0.01f) {
                levelSum = 0.0;
                levelSamples = 0;
            } else if (levelSamples >= kRate * 2 * 20 && levelLogs < 30) {
                levelLogs++;
                TMVS_LOG("audio: output level %.3f rms (volume %.2f)", sqrt(levelSum / levelSamples), target);
                levelSum = 0.0;
                levelSamples = 0;
            }
            h.dwFlags &= ~WHDR_DONE;
            const MMRESULT written = waveOutWrite(g_device, &h, sizeof(WAVEHDR));
            if (written != MMSYSERR_NOERROR) {
                static int s_errors = 0;
                if (s_errors++ < 5) TMVS_LOG("audio: waveOutWrite failed (%u)", written);
                h.dwFlags |= WHDR_DONE;
                continue;
            }
            wrote = true;
        }
        if (wrote) {
            lastReturn = GetTickCount();
            stallLogged = false;
        } else {
            WaitForSingleObject(ready, 100);
            if (!stallLogged && GetTickCount() - lastReturn > 2000) {
                stallLogged = true;
                TMVS_LOG("audio: the device has not played a buffer for 2 s");
            }
        }
    }
    waveOutReset(g_device);
    for (auto& h : headers) waveOutUnprepareHeader(g_device, &h, sizeof(WAVEHDR));
    waveOutClose(g_device);
    g_device = nullptr;
    CloseHandle(ready);
}

} // namespace

namespace {
bool g_alwaysInFront = false;
}

void setAlwaysInFront(bool on) {
    g_alwaysInFront = on;
}

void update(const Settings& s, float time, bool active, HWND window) {
    const bool wanted = s.enabled && s.weatherSound > 0.0f && (s.rain > 0.0f || s.lightning > 0.0f);
    // Fade with the scene: silent in menus and while the game is in the background. The game
    // counts as in front when any of its windows is (the device's focus window can be a
    // child window that is never the foreground window itself).
    (void)window;
    DWORD foregroundProcess = 0;
    const HWND foreground = GetForegroundWindow();
    if (foreground) GetWindowThreadProcessId(foreground, &foregroundProcess);
    const bool inFront = g_alwaysInFront || foregroundProcess == GetCurrentProcessId();
    const bool audible = wanted && active && inFront;
    static int s_logged = -1;
    const int state = (wanted ? 1 : 0) | (active ? 2 : 0) | (inFront ? 4 : 0);
    if (state != s_logged) {
        s_logged = state;
        TMVS_LOG("audio: %s (sound wanted %d, in a race %d, game in front %d)", audible ? "audible" : "silent", wanted, active, inFront);
    }
    g_fade += ((audible ? 1.0f : 0.0f) - g_fade) * 0.05f;
    if (!g_running && wanted && !g_failed) {
        g_running = true;
        // Detached: at process exit Windows ends the thread and closes the device; joining
        // from DllMain (loader lock) could deadlock.
        std::thread(mixer).detach();
    }
    if (!g_running) return;
    g_rain = s.rain;
    g_volume = s.weatherSound * g_fade;

    // Thunder follows each lightning flash after a delay that grows with the distance
    // (compressed: the real 3 s per kilometre would be up to 20 s).
    const int slot = static_cast<int>(floorf(time));
    if (g_lastSlot < 0 || slot < g_lastSlot || slot > g_lastSlot + 5) g_lastSlot = slot - 1;
    for (int k = g_lastSlot + 1; k <= slot; k++) {
        weather::Strike strike;
        if (!weather::lightningStrike(k, s.lightning, strike)) continue;
        const float distance = strike.distance;
        for (auto& p : g_pending) {
            if (p.at >= 0.0f) continue;
            p.at = strike.start + 0.4f + distance * 5.0f;
            p.power = 1.0f - distance * 0.35f;
            p.distance = distance;
            break;
        }
    }
    g_lastSlot = slot;
    for (auto& p : g_pending) {
        if (p.at < 0.0f || time < p.at) continue;
        if (time - p.at < 1.0f && audible) {
            g_thunderPower = p.power;
            g_thunderDistance = p.distance;
            g_thunderSerial++;
            TMVS_LOG("audio: thunder (distance %.2f)", p.distance);
        }
        p.at = -1.0f;
    }
}

void shutdown() {
    g_running = false; // the mixer thread closes the device on its own
}

bool renderWav(const wchar_t* path, float rainAmount, float volume) {
    // A close, a middle and a far thunder over the rain, 8 s apart.
    const float thunderAt[3] = {2.0f, 10.0f, 18.0f};
    const float thunderDistance[3] = {0.0f, 0.5f, 1.0f};
    const int seconds = 28;
    Rain rain;
    Thunder thunder[2];
    int nextThunder = 0, started = 0;
    std::vector<float> mix(static_cast<size_t>(kBufferFrames) * 2);
    std::vector<int16_t> pcm;
    pcm.reserve(static_cast<size_t>(kRate) * seconds * 2);
    for (int frame = 0; frame < kRate * seconds; frame += kBufferFrames) {
        const float t = static_cast<float>(frame) / kRate;
        if (started < 3 && t >= thunderAt[started]) {
            thunder[nextThunder].start(1.0f - thunderDistance[started] * 0.35f, thunderDistance[started]);
            nextThunder ^= 1;
            started++;
        }
        std::fill(mix.begin(), mix.end(), 0.0f);
        const float thunderLevel = fmaxf(thunder[0].level, thunder[1].level);
        rain.render(mix.data(), kBufferFrames, rainAmount, 1.0f - 0.45f * thunderLevel);
        for (auto& th : thunder) th.render(mix.data(), kBufferFrames);
        for (float v : mix) pcm.push_back(static_cast<int16_t>(tanhf(v * volume) * 32000.0f));
    }
    FILE* f = _wfopen(path, L"wb");
    if (!f) return false;
    const uint32_t bytes = static_cast<uint32_t>(pcm.size() * 2);
    const uint32_t riff = 36 + bytes, fmtSize = 16, rate = kRate, byteRate = kRate * 4;
    const uint16_t pcmFormat = 1, channels = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtSize, 4, 1, f);
    fwrite(&pcmFormat, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&byteRate, 4, 1, f);
    fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&bytes, 4, 1, f);
    fwrite(pcm.data(), 2, pcm.size(), f);
    fclose(f);
    return true;
}

} // namespace audio
} // namespace tmshaders
