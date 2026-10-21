#include "loopback.h"
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmreg.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace tmshaders {
namespace loopback {
namespace {

// ActivateAudioInterfaceAsync (Windows 8 and newer), looked up at run time: a static import of
// Mmdevapi would keep the whole mod from loading where it is missing.
using ActivateFn = HRESULT(WINAPI*)(LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*,
                                    IActivateAudioInterfaceAsyncOperation**);
constexpr wchar_t kProcessLoopbackDevice[] = L"VAD\\Process_Loopback"; // VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK

// The activation completes on a thread of the audio service's choosing: the handler must be agile.
class ActivationHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    ActivationHandler() : m_done(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    HANDLE done() const { return m_done; }
    HRESULT result() const { return m_hr; }
    IAudioClient* takeClient() {
        IAudioClient* c = m_client;
        m_client = nullptr;
        return c;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (riid == __uuidof(IAgileObject)) {
            *out = static_cast<IAgileObject*>(this);
        } else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&m_refs)); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG n = InterlockedDecrement(&m_refs);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT hr = E_FAIL;
        IUnknown* unknown = nullptr;
        const HRESULT got = operation->GetActivateResult(&hr, &unknown);
        if (FAILED(got)) hr = got;
        if (SUCCEEDED(hr) && unknown) hr = unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&m_client));
        if (unknown) unknown->Release();
        m_hr = hr;
        SetEvent(m_done);
        return S_OK;
    }

private:
    ~ActivationHandler() {
        if (m_client) m_client->Release();
        CloseHandle(m_done);
    }
    LONG m_refs = 1;
    HANDLE m_done;
    HRESULT m_hr = E_PENDING;
    IAudioClient* m_client = nullptr;
};

std::string hex(HRESULT hr) {
    char text[16];
    snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(hr));
    return text;
}

// The sample formats a shared-mode stream can come in.
enum class Kind { Unknown, Pcm16, Pcm24, Pcm32, Float32 };

Kind kindOf(const WAVEFORMATEX& f) {
    unsigned tag = f.wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && f.cbSize >= 22) {
        const GUID& sub = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(f).SubFormat;
        // KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT: the format tag in Data1 on a fixed base.
        static const GUID base = {0, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
        if (sub.Data2 != base.Data2 || sub.Data3 != base.Data3 || memcmp(sub.Data4, base.Data4, 8) != 0) return Kind::Unknown;
        tag = sub.Data1;
    }
    if (tag == WAVE_FORMAT_PCM) {
        if (f.wBitsPerSample == 16) return Kind::Pcm16;
        if (f.wBitsPerSample == 24) return Kind::Pcm24;
        if (f.wBitsPerSample == 32) return Kind::Pcm32;
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && f.wBitsPerSample == 32) return Kind::Float32;
    return Kind::Unknown;
}

float sampleAt(const BYTE* p, Kind kind) {
    switch (kind) {
    case Kind::Pcm16: return static_cast<float>(*reinterpret_cast<const int16_t*>(p)) / 32768.0f;
    case Kind::Pcm24: {
        const int32_t v = static_cast<int32_t>(static_cast<uint32_t>(p[0]) << 8 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 24);
        return static_cast<float>(v) / 2147483648.0f;
    }
    case Kind::Pcm32: return static_cast<float>(*reinterpret_cast<const int32_t*>(p)) / 2147483648.0f;
    case Kind::Float32: return *reinterpret_cast<const float*>(p);
    default: return 0.0f;
    }
}

int16_t toPcm16(float v) {
    const float s = v * 32767.0f;
    return static_cast<int16_t>(s >= 32767.0f ? 32767 : s <= -32768.0f ? -32768 : static_cast<int>(s < 0.0f ? s - 0.5f : s + 0.5f));
}

} // namespace

const char* sourceName(Source source) {
    switch (source) {
    case Source::Process: return "the game's own sound (process loopback)";
    case Source::System: return "everything the PC plays (loopback of the default playback device)";
    default: return "none";
    }
}

struct Capture::Impl {
    int rate = 44100, channels = 2;
    Mode mode = Mode::Auto;
    std::thread thread;
    mutable std::mutex lock;
    std::condition_variable cv;
    enum class State { Idle, Preparing, Ready, Failed } state = State::Idle;
    bool wantStart = false, wantStop = false, quit = false, stopped = false;
    Source source = Source::None;
    std::string description, error;
    Stats stats;
    LARGE_INTEGER startQpc{}, qpcFrequency{};
    // The ring of converted frames (interleaved), read from head.
    std::vector<int16_t> ring;
    size_t capacity = 0, head = 0, count = 0;

    // The converter (capture thread only).
    Kind kind = Kind::Unknown;
    int inChannels = 0, inRate = 0, inBytesPerFrame = 0, inBytesPerSample = 0;
    bool passthrough = false;
    double step = 1.0, phase = 0.0;
    float previous[2] = {};
    std::vector<int16_t> converted;

    void run();
    bool initProcess(IAudioClient*& client, std::string& why);
    bool initSystem(IAudioClient*& client, std::string& why);
    bool setFormat(const WAVEFORMATEX& f, std::string& why);
    void take(IAudioCaptureClient* capture);
    void convert(const BYTE* data, UINT32 frames, bool silent);
    void pushLocked(const int16_t* frames, size_t n); // null: silence
    void dropLocked(size_t n);
    void paceLocked();
};

Capture::Capture() : m(std::make_unique<Impl>()) {}

Capture::~Capture() {
    {
        std::lock_guard<std::mutex> l(m->lock);
        m->quit = true;
    }
    m->cv.notify_all();
    if (m->thread.joinable()) m->thread.join();
}

void Capture::prepare(int rate, int channels, Mode mode) {
    std::lock_guard<std::mutex> l(m->lock);
    if (m->state != Impl::State::Idle) return;
    m->rate = std::clamp(rate, 8000, 192000);
    m->channels = std::clamp(channels, 1, 2);
    m->mode = mode;
    m->capacity = static_cast<size_t>(m->rate) * 8; // 8 s: the game takes a second at a time
    m->ring.assign(m->capacity * m->channels, 0);
    QueryPerformanceFrequency(&m->qpcFrequency);
    m->state = Impl::State::Preparing;
    m->thread = std::thread([impl = m.get()] { impl->run(); });
}

bool Capture::waitReady(unsigned timeoutMs) {
    std::unique_lock<std::mutex> l(m->lock);
    m->cv.wait_for(l, std::chrono::milliseconds(timeoutMs), [this] { return m->state == Impl::State::Ready || m->state == Impl::State::Failed; });
    return m->state == Impl::State::Ready;
}

bool Capture::start() {
    std::lock_guard<std::mutex> l(m->lock);
    if (m->state != Impl::State::Ready) return false;
    QueryPerformanceCounter(&m->startQpc);
    m->wantStart = true;
    m->wantStop = false;
    m->stopped = false;
    m->cv.notify_all();
    return true;
}

void Capture::stop() {
    std::unique_lock<std::mutex> l(m->lock);
    if (!m->wantStart) return;
    m->wantStop = true;
    m->cv.notify_all();
    m->cv.wait_for(l, std::chrono::milliseconds(300), [this] { return m->stopped; });
}

size_t Capture::available() {
    std::lock_guard<std::mutex> l(m->lock);
    return m->count;
}

void Capture::read(int16_t* out, size_t frames) {
    std::lock_guard<std::mutex> l(m->lock);
    const size_t ch = static_cast<size_t>(m->channels);
    const size_t n = (std::min)(frames, m->count);
    for (size_t i = 0; i < n; i++) {
        const size_t at = (m->head + i) % m->capacity;
        memcpy(out + i * ch, &m->ring[at * ch], ch * sizeof(int16_t));
    }
    if (n < frames) memset(out + n * ch, 0, (frames - n) * ch * sizeof(int16_t));
    if (m->capacity) m->head = (m->head + n) % m->capacity;
    m->count -= n;
    m->stats.delivered += frames;
    m->stats.underrun += frames - n;
}

Source Capture::source() const {
    std::lock_guard<std::mutex> l(m->lock);
    return m->source;
}

std::string Capture::description() const {
    std::lock_guard<std::mutex> l(m->lock);
    return m->description;
}

std::string Capture::error() const {
    std::lock_guard<std::mutex> l(m->lock);
    return m->error;
}

Stats Capture::stats() const {
    std::lock_guard<std::mutex> l(m->lock);
    return m->stats;
}

// --- The capture thread -------------------------------------------------------------

bool Capture::Impl::setFormat(const WAVEFORMATEX& f, std::string& why) {
    kind = kindOf(f);
    if (kind == Kind::Unknown || f.nChannels < 1 || f.nSamplesPerSec < 8000 || f.nBlockAlign < f.nChannels) {
        why = "unusable format (tag " + std::to_string(f.wFormatTag) + ", " + std::to_string(f.wBitsPerSample) + " bits)";
        return false;
    }
    inChannels = f.nChannels;
    inRate = static_cast<int>(f.nSamplesPerSec);
    inBytesPerFrame = f.nBlockAlign;
    inBytesPerSample = f.nBlockAlign / f.nChannels;
    passthrough = kind == Kind::Pcm16 && inChannels == channels && inRate == rate;
    step = static_cast<double>(inRate) / rate;
    phase = 0.0;
    previous[0] = previous[1] = 0.0f;
    char text[128];
    snprintf(text, sizeof(text), "%d Hz, %d channels, %s%s", inRate, inChannels,
             kind == Kind::Float32 ? "float" : kind == Kind::Pcm16 ? "16-bit" : kind == Kind::Pcm24 ? "24-bit" : "32-bit",
             passthrough ? "" : " (converted)");
    description = std::string(sourceName(source)) + ", " + text;
    return true;
}

bool Capture::Impl::initProcess(IAudioClient*& client, std::string& why) {
    HMODULE mmdevapi = LoadLibraryW(L"Mmdevapi.dll");
    const auto activate = mmdevapi ? reinterpret_cast<ActivateFn>(GetProcAddress(mmdevapi, "ActivateAudioInterfaceAsync")) : nullptr;
    if (!activate) {
        why = "process loopback: no ActivateAudioInterfaceAsync";
        return false;
    }
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT activation{};
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(params);
    activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    ActivationHandler* handler = new ActivationHandler();
    IActivateAudioInterfaceAsyncOperation* operation = nullptr;
    HRESULT hr = activate(kProcessLoopbackDevice, __uuidof(IAudioClient), &activation, handler, &operation);
    if (SUCCEEDED(hr)) {
        if (WaitForSingleObject(handler->done(), 3000) != WAIT_OBJECT_0) hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        else hr = handler->result();
    }
    if (SUCCEEDED(hr)) client = handler->takeClient();
    if (operation) operation->Release();
    handler->Release();
    if (FAILED(hr) || !client) {
        // Before Windows 10 2004 the virtual device is unknown (activation fails).
        why = "process loopback: activation " + hex(hr);
        return false;
    }
    // No mix format here (GetMixFormat isn't implemented for it): the format is asked for and
    // the audio engine converts.
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<WORD>(channels);
    format.nSamplesPerSec = static_cast<DWORD>(rate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(channels * 2);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                            2000000, 0, &format, nullptr);
    if (FAILED(hr)) {
        why = "process loopback: Initialize " + hex(hr);
        client->Release();
        client = nullptr;
        return false;
    }
    source = Source::Process;
    return setFormat(format, why);
}

bool Capture::Impl::initSystem(IAudioClient*& client, std::string& why) {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (enumerator) enumerator->Release();
    if (FAILED(hr) || !device) {
        why = "playback device loopback: no default playback device (" + hex(hr) + ")";
        return false;
    }
    const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    // First the caller's format, converted by the audio engine; where it refuses, the mix format
    // and the converter here.
    WAVEFORMATEX wanted{};
    wanted.wFormatTag = WAVE_FORMAT_PCM;
    wanted.nChannels = static_cast<WORD>(channels);
    wanted.nSamplesPerSec = static_cast<DWORD>(rate);
    wanted.wBitsPerSample = 16;
    wanted.nBlockAlign = static_cast<WORD>(channels * 2);
    wanted.nAvgBytesPerSec = wanted.nSamplesPerSec * wanted.nBlockAlign;
    hr = mode == Mode::SystemMixFormat ? E_FAIL : device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    if (SUCCEEDED(hr)) {
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 2000000,
                                0, &wanted, nullptr);
        if (SUCCEEDED(hr)) {
            device->Release();
            source = Source::System;
            return setFormat(wanted, why);
        }
        client->Release();
        client = nullptr;
    }
    WAVEFORMATEX* mix = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&mix);
    if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, mix, nullptr);
    device->Release();
    if (FAILED(hr)) {
        why = "playback device loopback: Initialize " + hex(hr);
        if (mix) CoTaskMemFree(mix);
        if (client) client->Release();
        client = nullptr;
        return false;
    }
    source = Source::System;
    const bool ok = setFormat(*mix, why);
    CoTaskMemFree(mix);
    if (!ok) {
        client->Release();
        client = nullptr;
    }
    return ok;
}

void Capture::Impl::pushLocked(const int16_t* frames, size_t n) {
    const size_t ch = static_cast<size_t>(channels);
    if (n > capacity) {
        if (frames) frames += (n - capacity) * ch;
        stats.dropped += n - capacity;
        n = capacity;
    }
    if (count + n > capacity) dropLocked(count + n - capacity);
    for (size_t i = 0; i < n; i++) {
        const size_t at = (head + count + i) % capacity;
        if (frames) memcpy(&ring[at * ch], frames + i * ch, ch * sizeof(int16_t));
        else memset(&ring[at * ch], 0, ch * sizeof(int16_t));
    }
    count += n;
}

void Capture::Impl::dropLocked(size_t n) {
    n = (std::min)(n, count);
    head = (head + n) % capacity;
    count -= n;
    stats.dropped += n;
}

// The clock: the reader should have about as many frames as time has passed since start. A
// source that sends nothing (a silent playback device) gets silence put in; one that runs ahead
// (it shouldn't) loses its oldest frames.
void Capture::Impl::paceLocked() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double seconds = static_cast<double>(now.QuadPart - startQpc.QuadPart) / static_cast<double>(qpcFrequency.QuadPart);
    const uint64_t due = static_cast<uint64_t>(seconds * rate);
    const uint64_t have = stats.delivered + count;
    const uint64_t tenth = static_cast<uint64_t>(rate) / 10;
    if (due > have + tenth) {
        const uint64_t n = due - have - tenth / 2;
        pushLocked(nullptr, static_cast<size_t>(n));
        stats.filled += n;
    } else if (have > due + 5 * tenth && count > 0) {
        const uint64_t excess = have - due - 2 * tenth;
        dropLocked(excess < count ? static_cast<size_t>(excess) : count);
    }
}

void Capture::Impl::convert(const BYTE* data, UINT32 frames, bool silent) {
    if (passthrough) {
        std::lock_guard<std::mutex> l(lock);
        pushLocked(silent ? nullptr : reinterpret_cast<const int16_t*>(data), frames);
        stats.captured += frames;
        return;
    }
    converted.clear();
    for (UINT32 i = 0; i < frames; i++) {
        float frame[2] = {0.0f, 0.0f};
        if (!silent && data) {
            const BYTE* p = data + static_cast<size_t>(i) * inBytesPerFrame;
            frame[0] = sampleAt(p, kind);
            frame[1] = inChannels > 1 ? sampleAt(p + inBytesPerSample, kind) : frame[0];
        }
        // Linear resampling between the previous input frame and this one.
        while (phase < 1.0) {
            const float t = static_cast<float>(phase);
            const float left = previous[0] + (frame[0] - previous[0]) * t;
            const float right = previous[1] + (frame[1] - previous[1]) * t;
            if (channels == 1) {
                converted.push_back(toPcm16((left + right) * 0.5f));
            } else {
                converted.push_back(toPcm16(left));
                converted.push_back(toPcm16(right));
            }
            phase += step;
        }
        phase -= 1.0;
        previous[0] = frame[0];
        previous[1] = frame[1];
    }
    const size_t n = converted.size() / static_cast<size_t>(channels);
    std::lock_guard<std::mutex> l(lock);
    pushLocked(converted.data(), n);
    stats.captured += n;
}

void Capture::Impl::take(IAudioCaptureClient* capture) {
    UINT32 packet = 0;
    while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
            std::lock_guard<std::mutex> l(lock);
            stats.glitches++;
        }
        convert(data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
        capture->ReleaseBuffer(frames);
    }
}

void Capture::Impl::run() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::string whyProcess, whySystem;
    bool ok = false;
    if (mode == Mode::Auto || mode == Mode::ProcessOnly) ok = initProcess(client, whyProcess);
    if (!ok && mode != Mode::ProcessOnly) ok = initSystem(client, whySystem);
    if (ok) ok = SUCCEEDED(client->SetEventHandle(event)) && SUCCEEDED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture)));
    {
        std::lock_guard<std::mutex> l(lock);
        state = ok ? State::Ready : State::Failed;
        if (!whyProcess.empty()) error = whyProcess;
        if (!whySystem.empty()) error += (error.empty() ? "" : "; ") + whySystem;
        if (!ok && error.empty()) error = "no capture client";
        if (!ok) source = Source::None;
    }
    cv.notify_all();
    while (ok) {
        {
            std::unique_lock<std::mutex> l(lock);
            cv.wait(l, [this] { return (wantStart && !stopped) || quit; });
            if (quit) break;
        }
        client->Start();
        for (;;) {
            WaitForSingleObject(event, 10);
            take(capture);
            std::lock_guard<std::mutex> l(lock);
            if (wantStop || quit) break;
            paceLocked();
        }
        client->Stop();
        take(capture);
        {
            std::lock_guard<std::mutex> l(lock);
            stopped = true;
            wantStart = false;
            wantStop = false;
            if (quit) ok = false;
        }
        cv.notify_all();
        client->Reset();
    }
    if (capture) capture->Release();
    if (client) client->Release();
    CloseHandle(event);
    if (SUCCEEDED(com)) CoUninitialize();
}

} // namespace loopback
} // namespace tmshaders
