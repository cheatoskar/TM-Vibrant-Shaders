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

#pragma warning(push, 0)
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_STDIO
#include "minimp3_ex.h"
#pragma warning(pop)

namespace tmshaders {
namespace audio {
namespace {

constexpr int kRate = 44100;
constexpr int kBufferFrames = 2048; // ~46 ms per buffer
constexpr int kBuffers = 4;
constexpr float kPi = 3.14159265f;

// Recordings in sounds/ (CC0, see sounds/CREDITS.md), embedded as RCDATA (sounds/sounds.rc).
constexpr int kRainClip = 1;
constexpr int kThunderClips[3][4] = {
    {11, 12, 13, 14}, // close: a crack, then the roll
    {21, 22, 23, 0},  // medium
    {31, 32, 33, 0},  // far: a low rumble
};

// Shared with the mixer thread.
std::atomic<float> g_rain{0.0f};      // 0 .. 2
std::atomic<float> g_volume{0.0f};    // master, already faded for focus / menus
std::atomic<int> g_thunderSerial{0};  // bumped for every new thunder
std::atomic<float> g_thunderDistance{0.0f}; // 0 close .. 1 far
std::atomic<float> g_thunderPan{0.0f};      // -0.5 left .. 0.5 right
std::atomic<bool> g_running{false};

HWAVEOUT g_device = nullptr;
std::atomic<bool> g_failed{false};

// Main-thread bookkeeping.
int g_lastSlot = -1;
struct PendingThunder {
    float at = -1.0f;
    float distance = 0.0f;
    float pan = 0.0f;
};
PendingThunder g_pending[4];
float g_fade = 0.0f;
bool g_alwaysInFront = false;

// --- Recordings ----------------------------------------------------------------------

// A decoded recording: interleaved stereo at kRate, and the gain that brings its loudest
// moment to a common level (the recordings differ a lot).
struct Clip {
    std::vector<int16_t> samples;
    float gain = 1.0f;
    size_t frames() const { return samples.size() / 2; }
};

bool decodeMp3(const void* data, size_t size, Clip& clip) {
    mp3dec_t decoder;
    mp3dec_file_info_t info{};
    if (mp3dec_load_buf(&decoder, static_cast<const uint8_t*>(data), size, &info, nullptr, nullptr) || !info.buffer || !info.samples) {
        free(info.buffer);
        return false;
    }
    const size_t frames = info.samples / info.channels;
    const double step = static_cast<double>(info.hz) / kRate; // resample to kRate (linear)
    const size_t outFrames = static_cast<size_t>(frames / step);
    clip.samples.resize(outFrames * 2);
    for (size_t i = 0; i < outFrames; i++) {
        const double at = i * step;
        const size_t a = static_cast<size_t>(at), b = a + 1 < frames ? a + 1 : a;
        const float f = static_cast<float>(at - a);
        for (int c = 0; c < 2; c++) {
            const int sc = info.channels > 1 ? c : 0;
            const float va = info.buffer[a * info.channels + sc], vb = info.buffer[b * info.channels + sc];
            clip.samples[i * 2 + c] = static_cast<int16_t>(va + (vb - va) * f);
        }
    }
    free(info.buffer);
    return true;
}

// Loudest 100 ms (rms, 0..1).
float peakLoudness(const Clip& clip) {
    const size_t window = kRate / 10;
    float loudest = 0.0f;
    for (size_t start = 0; start + window <= clip.frames(); start += window / 2) {
        double sum = 0.0;
        for (size_t i = start; i < start + window; i++) {
            const float m = (clip.samples[i * 2] + clip.samples[i * 2 + 1]) * (0.5f / 32768.0f);
            sum += m * m;
        }
        loudest = fmaxf(loudest, static_cast<float>(sqrt(sum / window)));
    }
    return loudest;
}

bool loadClip(int id, float loudness, Clip& clip) {
    static const int s_anchor = 0; // any address inside this module
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&s_anchor), &module);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)); // RT_RCDATA
    HGLOBAL handle = resource ? LoadResource(module, resource) : nullptr;
    const void* data = handle ? LockResource(handle) : nullptr;
    if (!data || !decodeMp3(data, SizeofResource(module, resource), clip) || clip.frames() < kRate) {
        TMVS_LOG("audio: sound %d failed to load", id);
        return false;
    }
    const float peak = peakLoudness(clip);
    clip.gain = peak > 1e-4f ? loudness / peak / 32768.0f : 0.0f;
    return true;
}

struct OnePole {
    float a = 1.0f, y = 0.0f;
    void setCutoff(float hz) { a = 1.0f - expf(-2.0f * kPi * hz / kRate); }
    float process(float x) { return y += a * (x - y); }
};

struct Random {
    uint32_t state;
    explicit Random(uint32_t seed) : state(seed | 1u) {}
    float uniform() { // 0..1
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (state >> 8) * (1.0f / 16777216.0f);
    }
};

// Rain (a seamless loop, louder with the rain) and up to two thunders at once.
class Mixer {
public:
    Mixer() : m_random(GetTickCount() ^ 0x9E3779B9u) {}

    bool loadRain() {
        if (!loadClip(kRainClip, 0.08f, m_rain)) return false;
        // Make the loop seamless: blend its last 0.75 s into its start (equal power, the
        // rain is noise), so the encoder's gaps at both ends never play.
        const size_t blend = kRate * 3 / 4, n = m_rain.frames();
        if (n > blend * 3) {
            for (size_t i = 0; i < blend; i++) {
                const float t = static_cast<float>(i) / blend;
                const float a = sqrtf(t), b = sqrtf(1.0f - t);
                for (int c = 0; c < 2; c++) {
                    int16_t& s = m_rain.samples[i * 2 + c];
                    s = static_cast<int16_t>(fmaxf(fminf(s * a + m_rain.samples[(n - blend + i) * 2 + c] * b, 32767.0f), -32768.0f));
                }
            }
            m_rain.samples.resize((n - blend) * 2);
        }
        TMVS_LOG("audio: rain recording loaded (%.1f s loop)", static_cast<float>(m_rain.frames()) / kRate);
        return true;
    }

    // distance 0 close .. 1 far, pan -0.5 left .. 0.5 right.
    void startThunder(float distance, float pan) {
        const int group = distance < 0.3f ? 0 : (distance < 0.65f ? 1 : 2);
        int count = 0;
        while (count < 4 && kThunderClips[group][count]) count++;
        // A different recording than last time in this group.
        int pick = static_cast<int>(m_random.uniform() * count) % count;
        if (pick == m_lastPick[group] && count > 1) pick = (pick + 1 + static_cast<int>(m_random.uniform() * (count - 1))) % count;
        m_lastPick[group] = pick;
        Voice& v = m_voices[m_nextVoice];
        m_nextVoice ^= 1;
        const DWORD start = GetTickCount();
        if (!loadClip(kThunderClips[group][pick], 0.6f, v.clip)) {
            v.active = false;
            return;
        }
        // Every strike sounds a bit different: pitch (and with it the length), loudness, and
        // the farther, the duller and lower.
        v.rate = (0.9 + 0.18 * m_random.uniform()) * (1.0 - 0.08 * distance);
        v.gain = v.clip.gain * (1.0f - 0.5f * distance) * (0.85f + 0.3f * m_random.uniform());
        const float cutoff = 14000.0f * powf(1200.0f / 14000.0f, distance);
        for (int c = 0; c < 2; c++) {
            for (auto& f : v.low[c]) {
                f.setCutoff(cutoff);
                f.y = 0.0f;
            }
        }
        const float p = fmaxf(fminf(pan * 1.2f, 0.6f), -0.6f);
        v.side[0] = fminf(1.0f, 1.0f - p);
        v.side[1] = fminf(1.0f, 1.0f + p);
        v.position = 0.0;
        v.active = true;
        TMVS_LOG("audio: thunder (distance %.2f, sound %d, decoded in %u ms)", distance, kThunderClips[group][pick], GetTickCount() - start);
    }

    // Adds `frames` stereo frames to out.
    void render(float* out, int frames, float rain) {
        const size_t rainFrames = m_rain.frames();
        for (int i = 0; i < frames; i++) {
            float thunder[2] = {0.0f, 0.0f};
            for (auto& v : m_voices) {
                if (!v.active) continue;
                const size_t n = v.clip.frames();
                const size_t a = static_cast<size_t>(v.position);
                if (a + 1 >= n) {
                    v.active = false;
                    continue;
                }
                const float f = static_cast<float>(v.position - a);
                const float t = static_cast<float>(v.position) / kRate;
                const float left = static_cast<float>(n - a) / static_cast<float>(v.rate * kRate);
                const float env = fminf(t / 0.005f, 1.0f) * fminf(left / 1.5f, 1.0f); // no clicks, a soft end
                for (int c = 0; c < 2; c++) {
                    const float s0 = v.clip.samples[a * 2 + c], s1 = v.clip.samples[(a + 1) * 2 + c];
                    float s = (s0 + (s1 - s0) * f) * v.gain * env;
                    s = v.low[c][1].process(v.low[c][0].process(s));
                    thunder[c] += s * v.side[c];
                }
                v.position += v.rate;
            }
            // The thunder masks the rain, as the ear hears it.
            const float loud = fmaxf(fabsf(thunder[0]), fabsf(thunder[1]));
            m_thunderLevel += (loud > m_thunderLevel ? 0.01f : 0.00005f) * (loud - m_thunderLevel);
            m_rainLevel += (rain - m_rainLevel) * 0.00005f; // ~0.5 s glide
            const float rainGain = m_rainLevel > 1e-3f && rainFrames
                                       ? powf(fminf(m_rainLevel, 2.0f), 0.7f) * 0.9f * m_rain.gain * (1.0f - 0.35f * fminf(m_thunderLevel * 4.0f, 1.0f))
                                       : 0.0f;
            for (int c = 0; c < 2; c++) {
                const float r = rainFrames ? m_rain.samples[m_rainPosition * 2 + c] * rainGain : 0.0f;
                out[i * 2 + c] += r + thunder[c];
            }
            if (rainFrames && ++m_rainPosition >= rainFrames) m_rainPosition = 0;
        }
    }

private:
    struct Voice {
        Clip clip;
        double position = 0.0, rate = 1.0;
        float gain = 0.0f, side[2] = {1.0f, 1.0f};
        OnePole low[2][2];
        bool active = false;
    };
    Clip m_rain;
    size_t m_rainPosition = 0;
    float m_rainLevel = 0.0f, m_thunderLevel = 0.0f;
    Voice m_voices[2];
    int m_nextVoice = 0;
    int m_lastPick[3] = {-1, -1, -1};
    Random m_random;
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
    Mixer mix;
    mix.loadRain();
    int thunderSerial = g_thunderSerial.load();
    float volume = 0.0f;
    std::vector<float> buffer(static_cast<size_t>(kBufferFrames) * 2);
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
                mix.startThunder(g_thunderDistance.load(), g_thunderPan.load());
            }
            std::fill(buffer.begin(), buffer.end(), 0.0f);
            mix.render(buffer.data(), kBufferFrames, g_rain.load());
            const float target = g_volume.load();
            int16_t* out = reinterpret_cast<int16_t*>(h.lpData);
            for (int i = 0; i < kBufferFrames * 2; i++) {
                volume += (target - volume) * 0.0005f;
                const float v = tanhf(buffer[static_cast<size_t>(i)] * volume); // soft limit
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

bool writeWav(const wchar_t* path, const std::vector<int16_t>& pcm) {
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

} // namespace

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
        for (auto& p : g_pending) {
            if (p.at >= 0.0f) continue;
            p.at = strike.start + 0.4f + strike.distance * 5.0f;
            p.distance = strike.distance;
            p.pan = strike.side;
            break;
        }
    }
    g_lastSlot = slot;
    for (auto& p : g_pending) {
        if (p.at < 0.0f || time < p.at) continue;
        if (time - p.at < 1.0f && audible) {
            g_thunderDistance = p.distance;
            g_thunderPan = p.pan;
            g_thunderSerial++;
        }
        p.at = -1.0f;
    }
}

void shutdown() {
    g_running = false; // the mixer thread closes the device on its own
}

bool decodeToWav(const wchar_t* mp3Path, const wchar_t* wavPath) {
    FILE* f = _wfopen(mp3Path, L"rb");
    if (!f) return false;
    std::vector<uint8_t> data;
    uint8_t chunk[65536];
    for (size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0;) data.insert(data.end(), chunk, chunk + n);
    fclose(f);
    Clip clip;
    return decodeMp3(data.data(), data.size(), clip) && writeWav(wavPath, clip.samples);
}

bool renderWav(const wchar_t* path, float rainAmount, float volume) {
    // Rain with six thunders, close to far, from both sides.
    const float thunderAt[6] = {2.0f, 11.0f, 20.0f, 29.0f, 37.0f, 46.0f};
    const float thunderDistance[6] = {0.1f, 0.5f, 0.9f, 0.2f, 0.75f, 0.0f};
    const float thunderPan[6] = {-0.3f, 0.4f, 0.0f, 0.35f, -0.4f, 0.1f};
    const int seconds = 60;
    Mixer mix;
    mix.loadRain();
    int started = 0;
    std::vector<float> buffer(static_cast<size_t>(kBufferFrames) * 2);
    std::vector<int16_t> pcm;
    pcm.reserve(static_cast<size_t>(kRate) * seconds * 2);
    for (int frame = 0; frame < kRate * seconds; frame += kBufferFrames) {
        const float t = static_cast<float>(frame) / kRate;
        if (started < 6 && t >= thunderAt[started]) {
            mix.startThunder(thunderDistance[started], thunderPan[started]);
            started++;
        }
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        mix.render(buffer.data(), kBufferFrames, rainAmount);
        for (float v : buffer) pcm.push_back(static_cast<int16_t>(tanhf(v * volume) * 32000.0f));
    }
    return writeWav(path, pcm);
}

} // namespace audio
} // namespace tmshaders
