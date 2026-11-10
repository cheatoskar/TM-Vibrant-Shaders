#include "clips/recorder.h"
#include "clips/encoder.h"
#include "clips/settings.h"
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
#include <exception>
#include <mutex>
#include <new>
#include <system_error>
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

constexpr int kSlots = 4; // frames on their way to the encoder

// The pictures' byte limit: the bitrate's, and never more than the memory limit leaves next to the
// sound.
size_t ringLimit(const RecorderSetup& s) {
    const size_t sound = s.sound ? soundBytes(s.seconds) : 0;
    const size_t room = sound + (16u << 20) < kMemoryLimit ? kMemoryLimit - sound : (16u << 20);
    return std::min(videoLimit(s.bitrate, s.seconds), room);
}

} // namespace

// What outlives one recording: the saves' results (a stopped recording's saves still report).
struct Recorder::Shared {
    std::mutex lock;
    std::deque<SaveResult> results;
    std::atomic<int> retiring{0};

    void post(SaveResult r) {
        std::lock_guard<std::mutex> guard(lock);
        results.push_back(std::move(r));
    }
};

struct Recorder::Impl {
    std::shared_ptr<Shared> shared;
    RecorderSetup setup;
    std::atomic<int> state{static_cast<int>(State::Off)};
    std::atomic<bool> cancel{false}; // stop() ran: the encoder stops waiting
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
    bool saveBusy = false;
    bool saveStop = false;
    std::thread saver;

    void run();
    void work();
    void runSaver();
    void saveLoop();
    SaveResult write(const SaveJob& job);
    // State::Failed, the buffer's memory given back. `lock` held / not held.
    void failLocked(const std::string& why);
    void fail(const std::string& why);
    // Ends the threads (the saves asked for are written first). Not on the game's thread.
    void finish();
};

void Recorder::Impl::failLocked(const std::string& why) {
    if (state != static_cast<int>(State::Failed)) failure = why;
    state = static_cast<int>(State::Failed);
    ring.clear();
    sound.clear();
    soundOn = false;
    stats.bufferBytes = 0;
    stats.bufferSeconds = 0.0;
}

void Recorder::Impl::fail(const std::string& why) {
    std::lock_guard<std::mutex> guard(lock);
    failLocked(why);
}

void Recorder::Impl::run() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // An exception here would end the game (std::terminate): the recording ends instead.
    try {
        work();
    } catch (const std::bad_alloc&) {
        fail("not enough memory (the buffer was freed)");
    } catch (const std::exception& e) {
        fail(std::string("the worker failed: ") + e.what());
    } catch (...) {
        fail("the worker failed");
    }
    if (com == S_OK || com == S_FALSE) CoUninitialize();
}

void Recorder::Impl::work() {
    EncoderSetup e;
    e.width = setup.width;
    e.height = setup.height;
    e.fps = setup.fps;
    e.bitrate = setup.bitrate;
    e.gopSeconds = 1.0;
    e.choice = setup.encoder;
    e.softwareOnly = setup.softwareOnly;
    e.cancel = &cancel;
    std::string why;
    std::unique_ptr<PictureEncoder> encoder = openEncoder(e, why);
    if (!encoder) {
        std::lock_guard<std::mutex> guard(lock);
        failure = why.empty() ? std::string("no encoder") : why;
        state = static_cast<int>(State::Failed);
        return;
    }
    loopback::Capture capture;
    // "Game + mic": the default recording device, mixed into the same track (it starts with the
    // game's sound, both kept on time by their own clocks, loopback.h).
    loopback::Capture microphone;
    bool micOn = false;
    std::string soundText = "no sound";
    if (setup.sound) {
        capture.prepare(kSoundRate, kSoundChannels, setup.soundSource == 2 ? loopback::Mode::SystemOnly : loopback::Mode::Auto);
        if (setup.soundSource == 1) microphone.prepare(kSoundRate, kSoundChannels, loopback::Mode::Microphone);
        const bool micReady = setup.soundSource == 1 && microphone.waitReady(3000);
        if (capture.waitReady(3000) && capture.start()) {
            if (micReady) micOn = microphone.start();
            // The circle is allocated here, not under the lock the game's thread takes too.
            SoundRing fresh;
            try {
                fresh.reset(kSoundRate, kSoundChannels, setup.seconds + 10.0, counterTo100ns(capture.startCounter()));
                std::lock_guard<std::mutex> guard(lock);
                sound = std::move(fresh);
                soundOn = true;
                soundText = "sound: " + capture.description();
                if (setup.soundSource == 1) {
                    soundText += micOn ? ", with " + microphone.description()
                                       : ", no microphone (" + (microphone.error().empty() ? std::string("not ready") : microphone.error()) + ")";
                }
            } catch (const std::bad_alloc&) {
                capture.stop();
                if (micOn) microphone.stop();
                micOn = false;
                soundText = "no sound (not enough memory for it)";
            }
        } else {
            soundText = "no sound (" + (capture.error().empty() ? std::string("no loopback") : capture.error()) + ")";
        }
    }
    {
        std::lock_guard<std::mutex> guard(lock);
        description = encoder->description() + "; " + soundText;
        if (state == static_cast<int>(State::Failed)) return;
        state = static_cast<int>(State::Running);
    }
    std::deque<int64_t> times; // the frames in the encoder, by number from `base`
    int64_t base = 0;
    std::vector<int16_t> pcm, mic;
    bool outOfMemory = false;
    int sinceKey = 0; // pictures since the last key frame (or since the start)
    const int noKeyLimit = std::max(600, static_cast<int>(setup.fps * 10.0));
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
        sinceKey = p.key ? 0 : sinceKey + 1;
        // Out of memory inside the encoder's call is noted, not thrown through it.
        try {
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
        } catch (const std::bad_alloc&) {
            outOfMemory = true;
        }
    };
    for (;;) {
        Queued job{-1, 0};
        {
            std::unique_lock<std::mutex> guard(lock);
            wake.wait_for(guard, std::chrono::milliseconds(10), [this] { return stopping || !queue.empty(); });
            if (stopping || state == static_cast<int>(State::Failed)) break;
            if (!queue.empty()) {
                job = queue.front();
                queue.pop_front();
            }
        }
        if (job.slot >= 0) {
            times.push_back(job.time);
            const int64_t t0 = now100ns();
            std::string error;
            const bool ok = encoder->encode(slots[job.slot].data(), sink, error);
            const double ms = msSince(t0);
            std::lock_guard<std::mutex> guard(lock);
            freeSlots.push_back(job.slot);
            stats.encodeMs += ms;
            stats.encodeMaxMs = std::max(stats.encodeMaxMs, ms);
            if (!ok) {
                if (!cancel) failLocked("the encoder stopped: " + error);
                break;
            }
            if (outOfMemory) {
                failLocked("not enough memory for the buffer (it was freed)");
                break;
            }
            if (sinceKey > noKeyLimit) {
                failLocked("the encoder sends no key frames: clips could not start anywhere");
                break;
            }
        }
        if (soundOn) {
            const size_t n = capture.available();
            if (n) {
                pcm.resize(n * kSoundChannels);
                capture.read(pcm.data(), n);
                if (micOn) {
                    // As many frames of the microphone (silence where it has fewer), added.
                    mic.resize(n * kSoundChannels);
                    microphone.read(mic.data(), n);
                    for (size_t k = 0; k < pcm.size(); k++) {
                        const int v = static_cast<int>(pcm[k]) + mic[k];
                        pcm[k] = static_cast<int16_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                    }
                }
                std::lock_guard<std::mutex> guard(lock);
                sound.push(pcm.data(), n);
            }
        }
    }
    if (soundOn) capture.stop();
    if (micOn) microphone.stop();
    encoder.reset();
}

SaveResult Recorder::Impl::write(const SaveJob& job) {
    SaveResult r;
    r.path = job.path;
    r.what = job.what;
    const int64_t t0 = now100ns();
    std::vector<Picture> pictures;
    SoundRing::Span span;
    std::vector<uint8_t> parameters;
    int rate = kSoundRate, channels = kSoundChannels;
    {
        // Only what is quick under the lock (the game's thread takes it every frame): the pictures
        // are shared pointers, the sound's chunks too; the samples are copied after it.
        std::lock_guard<std::mutex> guard(lock);
        pictures = ring.cut(job.from, job.to);
        parameters = header;
        if (!pictures.empty() && soundOn) {
            const int64_t end = pictures.back().time + static_cast<int64_t>(1e7 / setup.fps);
            span = sound.span(pictures.front().time, end, static_cast<uint64_t>(sound.rate())); // 1 s of margin
            rate = sound.rate();
            channels = sound.channels();
        }
    }
    if (pictures.empty()) {
        r.line = "nothing in the buffer for that time";
        return r;
    }
    const std::vector<int16_t> pcm = SoundRing::copy(span);
    span.chunks.clear();
    const size_t frames = span.frames;
    const bool withSound = frames > 0 && !pcm.empty();
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
    const std::string path = utf8(job.path);
    char line[512];
    if (r.ok) {
        snprintf(line, sizeof(line), ": %.1f s, %d pictures, %.1f MB, %s, in %.0f ms (from %.2f s %s the asked start)", r.seconds,
                 static_cast<int>(pictures.size()), bytes / 1048576.0, withSound ? "with sound" : "no sound", msSince(t0),
                 std::abs(static_cast<double>(job.from - pictures.front().time)) / 1e7, pictures.front().time <= job.from ? "before" : "after");
        r.line = "saved " + path + line;
    } else {
        r.line = "could not save " + path + ": " + error;
    }
    return r;
}

void Recorder::Impl::runSaver() {
    // As run(): nothing thrown may leave the thread.
    try {
        saveLoop();
    } catch (const std::bad_alloc&) {
        fail("not enough memory for a save (the buffer was freed)");
    } catch (...) {
        fail("the saving thread failed");
    }
    std::lock_guard<std::mutex> guard(saveLock);
    saveBusy = false;
}

void Recorder::Impl::saveLoop() {
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
        SaveResult r;
        try {
            r = write(job);
        } catch (const std::bad_alloc&) {
            // The game needs the memory more than the buffer does.
            fail("not enough memory for a save (the buffer was freed)");
            r = SaveResult();
            r.path = job.path;
            r.what = job.what;
            r.line = "could not save " + utf8(job.path) + ": not enough memory (the recording stopped and freed its buffer)";
            DeleteFileW((job.path.substr(0, job.path.size() >= 4 ? job.path.size() - 4 : 0) + L".part.mp4").c_str());
        }
        shared->post(std::move(r));
        std::lock_guard<std::mutex> guard(saveLock);
        saveBusy = false;
    }
}

void Recorder::Impl::finish() {
    cancel = true;
    {
        std::lock_guard<std::mutex> guard(lock);
        stopping = true;
    }
    wake.notify_all();
    if (worker.joinable()) worker.join();
    // No frame comes any more: the saves take what the buffer holds, at once.
    if (state != static_cast<int>(State::Failed)) state = static_cast<int>(State::Off);
    {
        std::lock_guard<std::mutex> guard(saveLock);
        saveStop = true;
    }
    saveWake.notify_all();
    if (saver.joinable()) saver.join();
}

namespace {

DWORD WINAPI retireThread(LPVOID param) {
    Recorder::Impl* impl = static_cast<Recorder::Impl*>(param);
    std::shared_ptr<Recorder::Shared> shared = impl->shared;
    try {
        impl->finish();
        delete impl; // the buffer's memory goes here, not on the game's thread
    } catch (...) {
    }
    shared->retiring--;
    return 0;
}

} // namespace

Recorder::Recorder() : m(std::make_unique<Impl>()), m_shared(std::make_shared<Shared>()) {
    m->shared = m_shared;
}

Recorder::~Recorder() {
    stop();
}

void Recorder::start(const RecorderSetup& setup) {
    stop();
    Impl& i = *m;
    i.setup = setup;
    i.frameBytes = static_cast<size_t>(setup.width) * setup.height * 3 / 2;
    i.failure.clear();
    i.description.clear();
    i.stats = RecorderStats();
    i.header.clear();
    i.ring.clear();
    // The game is 32-bit: the pictures and the sound together stay under kMemoryLimit (settings.h).
    i.ring.setLimits(setup.seconds, ringLimit(setup));
    i.stats.bufferLimit = i.ring.maxBytes();
    i.soundOn = false;
    i.cancel = false;
    try {
        i.slots.assign(kSlots, std::vector<uint8_t>(i.frameBytes));
    } catch (const std::bad_alloc&) {
        i.slots.clear();
        i.failure = "not enough memory for the frames";
        i.state = static_cast<int>(State::Failed);
        return;
    }
    i.freeSlots.clear();
    for (int k = 0; k < kSlots; k++) i.freeSlots.push_back(k);
    i.queue.clear();
    i.acquired = -1;
    i.stopping = false;
    i.saveStop = false;
    i.state = static_cast<int>(State::Starting);
    // The threads get the Impl itself: stop() hands it on while they run.
    Impl* impl = m.get();
    try {
        i.worker = std::thread([impl] { impl->run(); });
        i.saver = std::thread([impl] { impl->runSaver(); });
    } catch (const std::system_error&) {
        i.fail("no thread for the recording");
    }
}

void Recorder::stop() {
    Impl* old = m.get();
    if (!old->worker.joinable() && !old->saver.joinable()) {
        // Nothing runs: only tidy up.
        if (old->state != static_cast<int>(State::Failed)) old->state = static_cast<int>(State::Off);
        std::lock_guard<std::mutex> guard(old->lock);
        old->ring.clear();
        old->sound.clear();
        old->slots.clear();
        old->slots.shrink_to_fit();
        old->freeSlots.clear();
        old->queue.clear();
        old->acquired = -1;
        old->soundOn = false;
        return;
    }
    // The running recording goes to a thread of its own; a fresh one takes its place at once.
    auto fresh = std::make_unique<Impl>();
    fresh->shared = m_shared;
    if (old->state == static_cast<int>(State::Failed)) {
        std::lock_guard<std::mutex> guard(old->lock);
        fresh->failure = old->failure;
        fresh->state = static_cast<int>(State::Failed);
    }
    old->cancel = true;
    std::unique_ptr<Impl> retired = std::move(m);
    m = std::move(fresh);
    m_shared->retiring++;
    Impl* raw = retired.release();
    HANDLE thread = CreateThread(nullptr, 0, retireThread, raw, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        retireThread(raw); // no thread: here then
    }
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
    std::lock_guard<std::mutex> guard(m_shared->lock);
    if (m_shared->results.empty()) return false;
    out = std::move(m_shared->results.front());
    m_shared->results.pop_front();
    return true;
}

bool Recorder::saving() const {
    {
        std::lock_guard<std::mutex> guard(m->saveLock);
        if (m->saveBusy || !m->jobs.empty()) return true;
    }
    return m_shared->retiring > 0;
}

int Recorder::retiring() const {
    return m_shared->retiring;
}

RecorderStats Recorder::stats() const {
    std::lock_guard<std::mutex> guard(m->lock);
    RecorderStats s = m->stats;
    s.bufferLimit = m->ring.maxBytes();
    s.soundBytes = m->sound.bytes();
    return s;
}

int64_t Recorder::newest() const {
    std::lock_guard<std::mutex> guard(m->lock);
    return m->ring.newest();
}

int64_t Recorder::oldest() const {
    std::lock_guard<std::mutex> guard(m->lock);
    if (m->ring.count() == 0) return -1;
    return m->ring.oldest();
}

} // namespace clips
} // namespace tmshaders
