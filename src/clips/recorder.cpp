#include "clips/recorder.h"
#include "clips/encoder.h"
#include "loopback.h"
#include "videoencoder.h"
#include <windows.h>
#include <mmreg.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace tmshaders {
namespace clips {

int64_t counterTo100ns(int64_t counter) {
    static LARGE_INTEGER frequency{};
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    const int64_t f = frequency.QuadPart;
    return (counter / f) * 10000000 + (counter % f) * 10000000 / f;
}

int64_t now100ns() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return counterTo100ns(t.QuadPart);
}

namespace {

double msSince(int64_t start) {
    return static_cast<double>(now100ns() - start) / 1e4;
}

std::string narrow(const std::wstring& text) {
    char out[1024] = {};
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out, sizeof(out), nullptr, nullptr);
    return out;
}

constexpr int kSlots = 4;         // frames on their way to the encoder
constexpr int kSoundRate = 48000;
constexpr int kSoundChannels = 2;

} // namespace

struct Recorder::Impl {
    RecorderSetup setup;
    std::atomic<int> state{static_cast<int>(State::Off)};
    std::string failure, description;
    size_t frameBytes = 0;

    // Frames: the game's thread fills a free slot, the worker encodes it.
    mutable std::mutex lock;
    std::condition_variable wake;
    std::vector<std::vector<uint8_t>> slots;
    std::deque<int> freeSlots;
    struct Queued {
        int slot;
        int64_t time;
    };
    std::deque<Queued> queue;
    int acquired = -1;
    bool stopping = false;
    std::thread worker;

    // The buffer (under `lock`).
    PictureRing ring;
    SoundRing sound;
    bool soundOn = false;
    std::vector<uint8_t> header;
    RecorderStats stats;

    // Saving.
    mutable std::mutex saveLock;
    std::condition_variable saveWake;
    std::deque<SaveJob> jobs;
    std::deque<SaveResult> results;
    bool saveBusy = false;
    bool saveStop = false;
    std::thread saver;

    void run();
    void runSaver();
    SaveResult write(const SaveJob& job);
};

void Recorder::Impl::run() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    EncoderSetup e;
    e.width = setup.width;
    e.height = setup.height;
    e.fps = setup.fps;
    e.bitrate = setup.bitrate;
    e.gopSeconds = 1.0;
    e.choice = setup.encoder;
    e.softwareOnly = setup.softwareOnly;
    std::string why;
    std::unique_ptr<PictureEncoder> encoder = openEncoder(e, why);
    if (!encoder) {
        std::lock_guard<std::mutex> guard(lock);
        failure = why.empty() ? std::string("no encoder") : why;
        state = static_cast<int>(State::Failed);
        if (com == S_OK || com == S_FALSE) CoUninitialize();
        return;
    }
    loopback::Capture capture;
    std::string soundText = "no sound";
    if (setup.sound) {
        capture.prepare(kSoundRate, kSoundChannels);
        if (capture.waitReady(3000) && capture.start()) {
            std::lock_guard<std::mutex> guard(lock);
            sound.reset(kSoundRate, kSoundChannels, setup.seconds + 10.0, counterTo100ns(capture.startCounter()));
            soundOn = true;
            soundText = "sound: " + capture.description();
        } else {
            soundText = "no sound (" + (capture.error().empty() ? std::string("no loopback") : capture.error()) + ")";
        }
    }
    {
        std::lock_guard<std::mutex> guard(lock);
        description = encoder->description() + "; " + soundText;
        state = static_cast<int>(State::Running);
    }
    std::deque<int64_t> times; // the frames in the encoder, by number from `base`
    int64_t base = 0, sent = 0;
    std::vector<int16_t> pcm;
    const PictureSink sink = [&](const EncodedPicture& p) {
        int64_t time = 0;
        while (base <= p.frame && !times.empty()) {
            time = times.front();
            times.pop_front();
            base++;
        }
        Picture picture;
        picture.time = time;
        picture.key = p.key;
        picture.data = std::make_shared<const std::vector<uint8_t>>(p.data, p.data + p.size);
        std::lock_guard<std::mutex> guard(lock);
        if (header.empty() && p.key) {
            header = parameterSets(p.data, p.size);
            if (header.empty()) header = encoder->header();
        }
        ring.push(std::move(picture));
        stats.framesEncoded++;
        stats.bufferBytes = ring.bytes();
        stats.bufferSeconds = static_cast<double>(ring.newest() - ring.oldest()) / 1e7;
    };
    for (;;) {
        Queued job{-1, 0};
        {
            std::unique_lock<std::mutex> guard(lock);
            wake.wait_for(guard, std::chrono::milliseconds(10), [this] { return stopping || !queue.empty(); });
            if (stopping) break;
            if (!queue.empty()) {
                job = queue.front();
                queue.pop_front();
            }
        }
        if (job.slot >= 0) {
            times.push_back(job.time);
            sent++;
            const int64_t t0 = now100ns();
            std::string error;
            const bool ok = encoder->encode(slots[job.slot].data(), sink, error);
            const double ms = msSince(t0);
            std::lock_guard<std::mutex> guard(lock);
            freeSlots.push_back(job.slot);
            stats.encodeMs += ms;
            stats.encodeMaxMs = std::max(stats.encodeMaxMs, ms);
            if (!ok) {
                failure = "the encoder stopped: " + error;
                state = static_cast<int>(State::Failed);
                break;
            }
        }
        if (soundOn) {
            const size_t n = capture.available();
            if (n) {
                pcm.resize(n * kSoundChannels);
                capture.read(pcm.data(), n);
                std::lock_guard<std::mutex> guard(lock);
                sound.push(pcm.data(), n);
            }
        }
    }
    if (soundOn) capture.stop();
    encoder.reset();
    if (com == S_OK || com == S_FALSE) CoUninitialize();
}

SaveResult Recorder::Impl::write(const SaveJob& job) {
    SaveResult r;
    r.path = job.path;
    r.what = job.what;
    const int64_t t0 = now100ns();
    std::vector<Picture> pictures;
    std::vector<int16_t> pcm;
    size_t frames = 0;
    std::vector<uint8_t> parameters;
    bool withSound = false;
    int rate = kSoundRate, channels = kSoundChannels;
    {
        std::lock_guard<std::mutex> guard(lock);
        pictures = ring.cut(job.from, job.to);
        parameters = header;
        if (!pictures.empty() && soundOn) {
            const int64_t end = pictures.back().time + static_cast<int64_t>(1e7 / setup.fps);
            pcm = sound.cut(pictures.front().time, end, frames);
            withSound = frames > 0;
            rate = sound.rate();
            channels = sound.channels();
        }
    }
    if (pictures.empty()) {
        r.line = "nothing in the buffer for that time";
        return r;
    }
    const std::vector<int64_t> durations = PictureRing::durations(pictures, static_cast<int64_t>(1e7 / setup.fps));
    std::vector<venc::Mp4Picture> list(pictures.size());
    uint64_t bytes = 0;
    for (size_t i = 0; i < pictures.size(); i++) {
        list[i].data = pictures[i].data->data();
        list[i].size = static_cast<uint32_t>(pictures[i].data->size());
        list[i].time = pictures[i].time;
        list[i].duration = durations[i];
        list[i].key = pictures[i].key;
        bytes += list[i].size;
    }
    venc::Mp4Sound s;
    if (withSound) {
        s.format.wFormatTag = WAVE_FORMAT_PCM;
        s.format.nChannels = static_cast<WORD>(channels);
        s.format.nSamplesPerSec = static_cast<DWORD>(rate);
        s.format.wBitsPerSample = 16;
        s.format.nBlockAlign = static_cast<WORD>(channels * 2);
        s.format.nAvgBytesPerSec = s.format.nSamplesPerSec * s.format.nBlockAlign;
        s.samples = pcm.data();
        s.frames = frames;
        s.time = pictures.front().time;
    }
    std::string error;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    r.ok = venc::writeMp4(job.path, venc::Codec::H264, setup.width, setup.height, setup.fps, parameters, list, withSound ? &s : nullptr, error);
    if (com == S_OK || com == S_FALSE) CoUninitialize();
    r.seconds = static_cast<double>(pictures.back().time + durations.back() - pictures.front().time) / 1e7;
    char line[1400];
    if (r.ok) {
        snprintf(line, sizeof(line), "saved %s: %.1f s, %d pictures, %.1f MB, %s, in %.0f ms (from %.2f s %s the asked start)", narrow(job.path).c_str(), r.seconds,
                 static_cast<int>(pictures.size()), bytes / 1048576.0, withSound ? "with sound" : "no sound", msSince(t0),
                 std::abs(static_cast<double>(job.from - pictures.front().time)) / 1e7, pictures.front().time <= job.from ? "before" : "after");
    } else {
        snprintf(line, sizeof(line), "could not save %s: %s", narrow(job.path).c_str(), error.c_str());
    }
    r.line = line;
    return r;
}

void Recorder::Impl::runSaver() {
    for (;;) {
        SaveJob job;
        {
            std::unique_lock<std::mutex> guard(saveLock);
            saveWake.wait(guard, [this] { return saveStop || !jobs.empty(); });
            if (jobs.empty()) return; // stopping with nothing left
            job = jobs.front();
            jobs.pop_front();
            saveBusy = true;
        }
        // Until the buffer holds the span's end (the encoder runs a few frames behind), at most 2 s
        // longer than the span asks.
        const int64_t giveUp = std::max(now100ns(), job.to) + 20000000;
        for (;;) {
            int64_t newest = 0;
            {
                std::lock_guard<std::mutex> guard(lock);
                newest = ring.newest();
            }
            if (newest >= job.to || now100ns() > giveUp || state != static_cast<int>(State::Running)) break;
            Sleep(20);
        }
        SaveResult r = write(job);
        std::lock_guard<std::mutex> guard(saveLock);
        results.push_back(std::move(r));
        saveBusy = false;
    }
}

Recorder::Recorder() : m(std::make_unique<Impl>()) {}

Recorder::~Recorder() {
    stop();
}

void Recorder::start(const RecorderSetup& setup) {
    stop();
    m->setup = setup;
    m->frameBytes = static_cast<size_t>(setup.width) * setup.height * 3 / 2;
    m->failure.clear();
    m->description.clear();
    m->stats = RecorderStats();
    m->header.clear();
    m->ring.clear();
    // At most a quarter more than the bitrate asks: a picture-heavy stretch must not take the game's
    // address space (32-bit, 2 GB).
    m->ring.setLimits(setup.seconds, static_cast<size_t>(setup.bitrate / 8.0 * setup.seconds * 1.25) + (16u << 20));
    m->soundOn = false;
    try {
        m->slots.assign(kSlots, std::vector<uint8_t>(m->frameBytes));
    } catch (const std::bad_alloc&) {
        m->failure = "not enough memory for the frames";
        m->state = static_cast<int>(State::Failed);
        return;
    }
    m->freeSlots.clear();
    for (int i = 0; i < kSlots; i++) m->freeSlots.push_back(i);
    m->queue.clear();
    m->acquired = -1;
    m->stopping = false;
    m->saveStop = false;
    m->state = static_cast<int>(State::Starting);
    m->worker = std::thread([this] { m->run(); });
    m->saver = std::thread([this] { m->runSaver(); });
}

void Recorder::stop() {
    {
        std::lock_guard<std::mutex> guard(m->saveLock);
        m->saveStop = true;
    }
    m->saveWake.notify_all();
    if (m->saver.joinable()) m->saver.join(); // the saves asked for are written first
    {
        std::lock_guard<std::mutex> guard(m->lock);
        m->stopping = true;
    }
    m->wake.notify_all();
    if (m->worker.joinable()) m->worker.join();
    if (m->state != static_cast<int>(State::Failed)) m->state = static_cast<int>(State::Off);
    std::lock_guard<std::mutex> guard(m->lock);
    m->ring.clear();
    m->slots.clear();
    m->slots.shrink_to_fit();
    m->freeSlots.clear();
    m->queue.clear();
    m->acquired = -1;
    m->soundOn = false;
}

Recorder::State Recorder::state() const {
    return static_cast<State>(m->state.load());
}

std::string Recorder::failure() const {
    std::lock_guard<std::mutex> guard(m->lock);
    return m->failure;
}

std::string Recorder::description() const {
    std::lock_guard<std::mutex> guard(m->lock);
    return m->description;
}

uint8_t* Recorder::acquire() {
    std::lock_guard<std::mutex> guard(m->lock);
    if (m->state != static_cast<int>(State::Running)) return nullptr;
    m->stats.framesIn++;
    if (m->freeSlots.empty()) {
        m->stats.framesDropped++;
        return nullptr;
    }
    m->acquired = m->freeSlots.front();
    m->freeSlots.pop_front();
    return m->slots[m->acquired].data();
}

void Recorder::submit(int64_t time) {
    {
        std::lock_guard<std::mutex> guard(m->lock);
        if (m->acquired < 0) return;
        m->queue.push_back({m->acquired, time});
        m->acquired = -1;
    }
    m->wake.notify_one();
}

void Recorder::save(const SaveJob& job) {
    {
        std::lock_guard<std::mutex> guard(m->saveLock);
        m->jobs.push_back(job);
    }
    m->saveWake.notify_one();
}

bool Recorder::takeResult(SaveResult& out) {
    std::lock_guard<std::mutex> guard(m->saveLock);
    if (m->results.empty()) return false;
    out = std::move(m->results.front());
    m->results.pop_front();
    return true;
}

bool Recorder::saving() const {
    std::lock_guard<std::mutex> guard(m->saveLock);
    return m->saveBusy || !m->jobs.empty();
}

RecorderStats Recorder::stats() const {
    std::lock_guard<std::mutex> guard(m->lock);
    return m->stats;
}

int64_t Recorder::newest() const {
    std::lock_guard<std::mutex> guard(m->lock);
    return m->ring.newest();
}

} // namespace clips
} // namespace tmshaders
