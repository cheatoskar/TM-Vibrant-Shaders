#include "soundcapture.h"
#include "engine.h"
#include "hook.h"
#include "log.h"
#include "loopback.h"
#include <windows.h>
#include <climits>
#include <memory>
#include <mutex>

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
bool g_soundWanted = false; // this export's dialog has its sound on
bool g_captureSeen = false; // the game opened a capture device during this export

void logSummary() {
    if (!g_capture) return;
    const loopback::Stats s = g_capture->stats();
    const double rate = g_rate > 0 ? static_cast<double>(g_rate) : 44100.0;
    TMVS_LOG("sound capture: %.2f s of sound handed to the game (from the loopback %.2f s; silence put in where it sent nothing %.2f s, "
             "missing at the game's reads %.2f s, dropped %.2f s; %u glitches)",
             s.delivered / rate, s.captured / rate, s.filled / rate, s.underrun / rate, s.dropped / rate, s.glitches);
}

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
                TMVS_LOG("sound capture: the export's sound from %s instead of the recording device (the game asked for %u Hz, %d channels, "
                         "a buffer of %d)",
                         g_capture->description().c_str(), frequency, channels, bufferSize);
                if (v.clipStart > 0.05)
                    TMVS_LOG("sound capture: the clip starts at %.2f s but the game plays its sound pass from 0: the first %.2f s of the sound "
                             "come before the video's first frame",
                             v.clipStart, v.clipStart);
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
    return 1;
}

void __cdecl captureStartDetour(void* device) {
    if (device != &g_fake) {
        g_start(device);
        return;
    }
    std::lock_guard<std::mutex> l(g_lock);
    if (g_capture && g_capture->start()) TMVS_LOG("sound capture: started");
}

void __cdecl captureStopDetour(void* device) {
    if (device != &g_fake) {
        g_stop(device);
        return;
    }
    std::lock_guard<std::mutex> l(g_lock);
    if (g_capture) g_capture->stop();
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
}

void exportClosed() {
    if (!g_installed) return;
    std::lock_guard<std::mutex> l(g_lock);
    if (g_soundWanted && !g_captureSeen)
        TMVS_LOG("sound capture: the export ended without its sound pass (aborted, or the game's sound is off in its settings)");
    if (!g_fakeOpen) g_capture.reset();
    g_soundWanted = false;
}

} // namespace soundcapture
} // namespace tmshaders
