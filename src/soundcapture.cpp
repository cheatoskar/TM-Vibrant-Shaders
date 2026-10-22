#include "soundcapture.h"
#include "engine.h"
#include "hook.h"
#include "log.h"
#include "loopback.h"
#include <windows.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tmshaders {
namespace soundcapture {
namespace {

// OpenAL's ALC entry points (ALC_APIENTRY is __cdecl on Windows).
using CaptureOpenFn = void*(__cdecl*)(const char* name, unsigned frequency, int format, int bufferSize);
using CaptureCloseFn = char(__cdecl*)(void* device);
using CaptureStartStopFn = void(__cdecl*)(void* device);
using CaptureSamplesFn = void(__cdecl*)(void* device, void* buffer, int samples);
using GetIntegervFn = void(__cdecl*)(void* device, int param, int size, int* values);
using GetErrorFn = int(__cdecl*)(void* device);

constexpr int kAlFormatMono16 = 0x1101;
constexpr int kAlFormatStereo16 = 0x1103;
constexpr int kAlcCaptureSamples = 0x312;

CaptureOpenFn g_open = nullptr;
CaptureCloseFn g_close = nullptr;
CaptureStartStopFn g_start = nullptr;
CaptureStartStopFn g_stop = nullptr;
CaptureSamplesFn g_samples = nullptr;
GetIntegervFn g_getIntegerv = nullptr;
GetErrorFn g_getError = nullptr;
bool g_installed = false;

// The device handed to the game during an export: only its address matters (never passed to
// OpenAL itself).
struct FakeDevice {
    char tag[8] = "TMVSCAP";
} g_fake;

// The game's capture calls come from its audio update; guarded against the export's end.
std::mutex g_lock;
std::unique_ptr<loopback::Capture> g_capture;
int g_rate = 0, g_channels = 0;
bool g_fakeOpen = false;
bool g_started = false;     // the game started the capture (its sound pass runs)
bool g_soundWanted = false; // this export's dialog has its sound on
bool g_captureSeen = false; // the game opened a capture device during this export
// The game's sound output latency (s): what the game plays at a moment reaches the loopback this
// much later. Measured at the export's start (measureLatency), 0 = not known.
double g_latency = 0.0;
double g_tail = 0.0;        // the capture runs this much longer at the game's stop
int64_t g_passStart = 0;    // QueryPerformanceCounter at the game's capture start

void logSummary() {
    if (!g_capture) return;
    const loopback::Stats s = g_capture->stats();
    const double rate = g_rate > 0 ? static_cast<double>(g_rate) : 44100.0;
    TMVS_LOG("sound capture: %.2f s of sound handed to the game (from the loopback %.2f s, left out at the start %.3f s; silence put in "
             "where it sent nothing %.2f s, missing at the game's reads %.2f s, dropped %.2f s; %u glitches)",
             s.delivered / rate, s.captured / rate, s.skipped / rate, s.filled / rate, s.underrun / rate, s.dropped / rate, s.glitches);
}

// --- The output latency -------------------------------------------------------------
// The game plays its sound through OpenAL, which mixes ahead into its output buffer: a sound the
// game starts at clip time t reaches the loopback at t + latency, and the sound pass's sound would
// lag the picture by it. Measured once per export, right after its .avi is made (the clip stopped,
// the game waits in DoShoot on the main thread, where its own OpenAL calls come from too): a short
// quiet tone of 19 kHz (inaudible for most; -40 dB, 50 ms) on a source of the game's own context,
// found again in the loopback the same way the sound pass is timed (frame 0 = the capture's
// start). Anything that fails leaves the latency at 0 (logged).
using AlGenFn = void(__cdecl*)(int, unsigned*);
using AlDeleteFn = void(__cdecl*)(int, const unsigned*);
using AlBufferDataFn = void(__cdecl*)(unsigned, int, const void*, int, int);
using AlSourceiFn = void(__cdecl*)(unsigned, int, int);
using AlSourcefFn = void(__cdecl*)(unsigned, int, float);
using AlSource3fFn = void(__cdecl*)(unsigned, int, float, float, float);
using AlSourceFn = void(__cdecl*)(unsigned);
using AlGetErrorFn = int(__cdecl*)();
using AlcGetCurrentContextFn = void*(__cdecl*)();
using AlcGetContextsDeviceFn = void*(__cdecl*)(void*);
using AlcGetStringFn = const char*(__cdecl*)(void*, int);

constexpr int kAlBuffer = 0x1009, kAlGain = 0x100A, kAlSourceRelative = 0x202, kAlPosition = 0x1004;
constexpr int kAlcFrequency = 0x1007, kAlcDeviceSpecifier = 0x1005;

double measureLatency(loopback::Capture& capture, std::string& why) {
    HMODULE al = GetModuleHandleW(L"OpenAL32.dll");
    const auto get = [al](const char* name) { return al ? GetProcAddress(al, name) : nullptr; };
    const auto genSources = reinterpret_cast<AlGenFn>(get("alGenSources"));
    const auto deleteSources = reinterpret_cast<AlDeleteFn>(get("alDeleteSources"));
    const auto genBuffers = reinterpret_cast<AlGenFn>(get("alGenBuffers"));
    const auto deleteBuffers = reinterpret_cast<AlDeleteFn>(get("alDeleteBuffers"));
    const auto bufferData = reinterpret_cast<AlBufferDataFn>(get("alBufferData"));
    const auto sourcei = reinterpret_cast<AlSourceiFn>(get("alSourcei"));
    const auto sourcef = reinterpret_cast<AlSourcefFn>(get("alSourcef"));
    const auto source3f = reinterpret_cast<AlSource3fFn>(get("alSource3f"));
    const auto play = reinterpret_cast<AlSourceFn>(get("alSourcePlay"));
    const auto stopSource = reinterpret_cast<AlSourceFn>(get("alSourceStop"));
    const auto getError = reinterpret_cast<AlGetErrorFn>(get("alGetError"));
    const auto currentContext = reinterpret_cast<AlcGetCurrentContextFn>(get("alcGetCurrentContext"));
    const auto contextsDevice = reinterpret_cast<AlcGetContextsDeviceFn>(get("alcGetContextsDevice"));
    const auto getString = reinterpret_cast<AlcGetStringFn>(get("alcGetString"));
    if (!genSources || !deleteSources || !genBuffers || !deleteBuffers || !bufferData || !sourcei || !sourcef || !source3f || !play ||
        !stopSource || !getError || !currentContext || !contextsDevice) {
        why = "OpenAL functions not found";
        return 0.0;
    }
    void* context = currentContext();
    if (!context) {
        why = "the game has no OpenAL context here (its sound is off?)";
        return 0.0;
    }
    void* device = contextsDevice(context);
    int mixRate = 0;
    if (device && g_getIntegerv) g_getIntegerv(device, kAlcFrequency, 1, &mixRate);
    const char* name = device && getString ? getString(device, kAlcDeviceSpecifier) : nullptr;
    TMVS_LOG("sound capture: the game's sound device: %s, mixing at %d Hz", name ? name : "?", mixRate);
    // The tone stays below the mixer's Nyquist frequency (at a low mixing rate it gets audible).
    const int rate = 44100;
    const double tone = (std::min)(19000.0, 0.4 * (mixRate > 0 ? mixRate : rate));
    std::vector<int16_t> burst(static_cast<size_t>(rate) / 20); // 50 ms
    for (size_t i = 0; i < burst.size(); i++) {
        const double t = static_cast<double>(i) / rate;
        const double fade = (std::min)(1.0, (std::min)(t, 0.05 - t) / 0.005);
        burst[i] = static_cast<int16_t>(std::sin(2.0 * 3.14159265358979 * tone * t) * 330.0 * fade);
    }
    getError(); // a pending error of the game's is not ours to keep
    unsigned buffer = 0, source = 0;
    genBuffers(1, &buffer);
    genSources(1, &source);
    if (getError() != 0 || !buffer || !source) {
        if (source) deleteSources(1, &source);
        if (buffer) deleteBuffers(1, &buffer);
        getError();
        why = "no free OpenAL source";
        return 0.0;
    }
    bufferData(buffer, kAlFormatMono16, burst.data(), static_cast<int>(burst.size() * sizeof(int16_t)), rate);
    sourcei(source, kAlBuffer, static_cast<int>(buffer));
    sourcei(source, kAlSourceRelative, 1);
    source3f(source, kAlPosition, 0.0f, 0.0f, 0.0f);
    sourcef(source, kAlGain, 1.0f);
    double result = 0.0;
    if (getError() == 0 && capture.start()) {
        LARGE_INTEGER frequency, played;
        QueryPerformanceFrequency(&frequency);
        Sleep(80); // the loopback runs before the tone starts
        QueryPerformanceCounter(&played);
        play(source);
        const double playedAt = static_cast<double>(played.QuadPart - capture.startCounter()) / static_cast<double>(frequency.QuadPart);
        std::vector<int16_t> heard;
        for (int i = 0; i < 70; i++) {
            Sleep(10);
            const size_t n = capture.available();
            if (n) {
                const size_t at = heard.size();
                heard.resize(at + n * 2);
                capture.read(heard.data() + at, n);
            }
        }
        capture.stop();
        // Goertzel at the tone over blocks of 32 frames (0.7 ms); the onset is where it first
        // reaches half its peak, less half the fade-in.
        const size_t block = 32, frames = heard.size() / 2;
        const double coeff = 2.0 * std::cos(2.0 * 3.14159265358979 * tone / rate);
        std::vector<double> level;
        for (size_t b = 0; b + block <= frames; b += block) {
            double s1 = 0.0, s2 = 0.0;
            for (size_t i = 0; i < block; i++) {
                const double s0 = heard[2 * (b + i)] + coeff * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            level.push_back(2.0 * std::sqrt((std::max)(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2)) / block);
        }
        double peak = 0.0;
        for (double v : level) peak = (std::max)(peak, v);
        size_t onset = level.size();
        for (size_t i = 0; i < level.size(); i++) {
            if (level[i] >= 0.5 * peak) {
                onset = i;
                break;
            }
        }
        if (peak < 5.0 || onset == level.size()) {
            why = "the tone was not heard (the game's sound volume at 0?)";
        } else {
            const double heardAt = static_cast<double>(onset * block) / rate - 0.0025;
            result = heardAt - playedAt;
            if (result < 0.0 || result > 0.4) {
                why = "implausible (" + std::to_string(static_cast<int>(result * 1000.0)) + " ms)";
                result = 0.0;
            }
        }
    } else {
        why = "the tone could not be played";
    }
    stopSource(source);
    sourcei(source, kAlBuffer, 0);
    deleteSources(1, &source);
    deleteBuffers(1, &buffer);
    getError();
    return result;
}

// --- The game's capture calls ---------------------------------------------------------

void* __cdecl captureOpenDetour(const char* name, unsigned frequency, int format, int bufferSize) {
    engine::VideoExport v;
    const int channels = format == kAlFormatStereo16 ? 2 : format == kAlFormatMono16 ? 1 : 0;
    if (engine::videoExport(v) && channels && frequency >= 8000 && frequency <= 192000) {
        std::lock_guard<std::mutex> l(g_lock);
        g_captureSeen = true;
        if (!g_fakeOpen) {
            if (!g_capture || g_rate != static_cast<int>(frequency) || g_channels != channels) {
                g_capture = std::make_unique<loopback::Capture>();
                g_capture->prepare(static_cast<int>(frequency), channels);
            }
            g_rate = static_cast<int>(frequency);
            g_channels = channels;
            // Normally prepared long ago (at the export's start); the game measures its sound
            // pass from before this call, so a wait here would only cost the first moments.
            if (g_capture->waitReady(3000)) {
                g_fakeOpen = true;
                g_started = false;
                TMVS_LOG("sound capture: the export's sound from %s instead of the recording device (the game asked for %u Hz, %d channels, "
                         "a buffer of %d)",
                         g_capture->description().c_str(), frequency, channels, bufferSize);
                return &g_fake;
            }
            TMVS_LOG("sound capture: no loopback (%s): the game records its recording device as before", g_capture->error().c_str());
            g_capture.reset();
        }
    }
    return g_open(name, frequency, format, bufferSize);
}

char __cdecl captureCloseDetour(void* device) {
    if (device != &g_fake) return g_close(device);
    std::lock_guard<std::mutex> l(g_lock);
    if (g_capture) g_capture->stop();
    logSummary();
    g_capture.reset();
    g_fakeOpen = false;
    g_started = false;
    return 1;
}

void __cdecl captureStartDetour(void* device) {
    if (device != &g_fake) {
        g_start(device);
        return;
    }
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_capture) return;
    // The sound pass plays the clip from 0 while the video starts at the clip's start (DoShoot
    // sets the clip's time from 0 there), and what the game plays reaches the loopback the output
    // latency later: both are left out at the start, and the capture runs the latency longer at
    // the end, so the sound starts with the video's first frame and keeps the clip's length.
    engine::VideoExport v;
    const double clipStart = engine::videoExport(v) && v.clipStart > 0.0 && v.clipStart < 3600.0 ? v.clipStart : 0.0;
    const double skip = clipStart + g_latency;
    g_tail = g_latency;
    if (g_capture->start(static_cast<size_t>(skip * g_rate + 0.5))) {
        g_started = true;
        g_passStart = g_capture->startCounter();
        TMVS_LOG("sound capture: started; the first %.3f s left out (the clip's start %.3f s, the output latency %.3f s)", skip, clipStart,
                 g_latency);
    }
}

void __cdecl captureStopDetour(void* device) {
    if (device != &g_fake) {
        g_stop(device);
        return;
    }
    // The game's last sound is still on its way through the output latency.
    if (g_tail > 0.0) Sleep(static_cast<DWORD>((std::min)(g_tail, 0.25) * 1000.0 + 0.5));
    std::lock_guard<std::mutex> l(g_lock);
    if (g_capture) g_capture->stop();
    g_started = false;
}

void __cdecl captureSamplesDetour(void* device, void* buffer, int samples) {
    if (device != &g_fake) {
        g_samples(device, buffer, samples);
        return;
    }
    std::lock_guard<std::mutex> l(g_lock);
    if (!buffer || samples <= 0) return;
    if (g_capture) g_capture->read(static_cast<int16_t*>(buffer), static_cast<size_t>(samples));
    else memset(buffer, 0, static_cast<size_t>(samples) * static_cast<size_t>(g_channels > 0 ? g_channels : 2) * sizeof(int16_t));
}

void __cdecl getIntegervDetour(void* device, int param, int size, int* values) {
    if (device != &g_fake) {
        g_getIntegerv(device, param, size, values);
        return;
    }
    if (!values || size < 1) return;
    std::lock_guard<std::mutex> l(g_lock);
    const size_t available = param == kAlcCaptureSamples && g_capture ? g_capture->available() : 0;
    values[0] = available > static_cast<size_t>(INT_MAX) ? INT_MAX : static_cast<int>(available);
}

int __cdecl getErrorDetour(void* device) {
    if (device == &g_fake) return 0; // ALC_NO_ERROR
    return g_getError(device);
}

} // namespace

void install() {
    if (g_installed) return;
    struct Import {
        const char* name;
        void* detour;
        void** original;
    };
    const Import imports[] = {
        {"alcCaptureOpenDevice", reinterpret_cast<void*>(&captureOpenDetour), reinterpret_cast<void**>(&g_open)},
        {"alcCaptureCloseDevice", reinterpret_cast<void*>(&captureCloseDetour), reinterpret_cast<void**>(&g_close)},
        {"alcCaptureStart", reinterpret_cast<void*>(&captureStartDetour), reinterpret_cast<void**>(&g_start)},
        {"alcCaptureStop", reinterpret_cast<void*>(&captureStopDetour), reinterpret_cast<void**>(&g_stop)},
        {"alcCaptureSamples", reinterpret_cast<void*>(&captureSamplesDetour), reinterpret_cast<void**>(&g_samples)},
        {"alcGetIntegerv", reinterpret_cast<void*>(&getIntegervDetour), reinterpret_cast<void**>(&g_getIntegerv)},
        {"alcGetError", reinterpret_cast<void*>(&getErrorDetour), reinterpret_cast<void**>(&g_getError)},
    };
    // All or none: a fake device must never reach the real OpenAL. No export runs at start-up,
    // so no fake device exists while they are patched one by one; a missing one undoes the rest.
    constexpr int kCount = static_cast<int>(sizeof(imports) / sizeof(imports[0]));
    int patched = 0;
    for (; patched < kCount; patched++) {
        const Import& i = imports[patched];
        if (!hook::patchGameImport("OpenAL32.dll", i.name, i.detour, i.original)) break;
    }
    if (patched != kCount) {
        for (int k = 0; k < patched; k++) {
            void* ours = nullptr;
            hook::patchGameImport("OpenAL32.dll", imports[k].name, *imports[k].original, &ours);
        }
        TMVS_LOG("sound capture: the game's OpenAL import %s not found: the export records its recording device as before", imports[patched].name);
        return;
    }
    g_installed = true;
    TMVS_LOG("sound capture: the game's OpenAL capture hooked (the export's sound comes from the game's own output)");
}

void exportOpened() {
    if (!g_installed) return;
    engine::VideoExport v;
    if (!engine::videoExport(v)) return;
    std::lock_guard<std::mutex> l(g_lock);
    g_soundWanted = v.sound && v.writesVideo;
    g_captureSeen = false;
    if (!v.writesVideo) return;
    if (!v.sound) {
        TMVS_LOG("sound capture: the export's sound is off in the Shoot video dialog (its sound button): the game records none, the video "
                 "gets no sound");
        return;
    }
    // Activating the loopback takes a moment: done now, the sound pass comes after the frames.
    if (!g_capture) {
        g_capture = std::make_unique<loopback::Capture>();
        g_capture->prepare(44100, 2);
        g_rate = 44100;
        g_channels = 2;
        TMVS_LOG("sound capture: the export's sound is on: loopback prepared for the sound pass after the frames");
    }
    g_latency = 0.0;
    if (g_capture->waitReady(2000)) {
        std::string why;
        g_latency = measureLatency(*g_capture, why);
        if (g_latency > 0.0)
            TMVS_LOG("sound capture: the game's sound output latency %.1f ms (measured): the sound is moved earlier by it", g_latency * 1000.0);
        else
            TMVS_LOG("sound capture: output latency not measured (%s): not compensated", why.c_str());
    }
}

void exportClosed() {
    if (!g_installed) return;
    std::lock_guard<std::mutex> l(g_lock);
    if (g_soundWanted && !g_captureSeen)
        TMVS_LOG("sound capture: the export ended without its sound pass (aborted, or the game's sound is off in its settings)");
    if (!g_fakeOpen) g_capture.reset();
    g_soundWanted = false;
}

bool passClock(double& seconds) {
    if (!g_started || !g_passStart) return false;
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    seconds = static_cast<double>(now.QuadPart - g_passStart) / static_cast<double>(frequency.QuadPart);
    return true;
}

} // namespace soundcapture
} // namespace tmshaders
