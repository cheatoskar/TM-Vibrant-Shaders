// Clips test (src/clips): the run tracker (start, respawn, restart, finish), the last runs (Save last
// run after a restart, what the buffer still holds), the adaptive buffer, the buffer's cuts (key
// frames, the sound in chunks against the old cut), the memory limit, key frames from the bitstream,
// the file names, the recorder's stop (never waits), a run saved from the buffer after more driving, then a synthetic run through each encoder this PC has (NVENC,
// the graphics card's Media Foundation encoder, and with --software Windows' own) into an MP4 with a
// tone: 20 s at 60 fps, the run from 5 s to 15 s, cut 3 s before and after. The file is checked with
// ffprobe when it is on the PATH (duration, streams, size), else by its MP4 boxes. --device: the NV12
// passes on a Direct3D 9 device (colours, cost per frame on the "game's" thread), and the recorder
// end to end with them.
//
//   tmvs_clipstest [--software] [--device] [--keep] [--out <folder>]     (exit code 0 = passed)
#include "clips/encoder.h"
#include "clips/grabber.h"
#include "clips/race.h"
#include "clips/recorder.h"
#include "clips/ring.h"
#include "clips/settings.h"
#include "videoencoder.h"
#include <windows.h>
#include <mmreg.h>
#include <mmsystem.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace tmshaders;
using namespace tmshaders::clips;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failures++;
}

std::string narrow(const std::wstring& w) {
    return utf8(w);
}

// --- The run tracker --------------------------------------------------------------------------

void testTracker() {
    printf("run tracker\n");
    RunTracker t;
    int race = 0;
    RaceSample s;
    s.valid = true;
    s.race = &race;
    double now = 100.0;
    RunEvent e;
    // Countdown.
    s.started = false;
    e = t.update(now, s);
    check(e.kind == RunEvent::None, "countdown: no event");
    // The run starts at clock 50000 (now = 101.0); the first frame sees 16 ms already.
    s.started = true;
    s.startClock = 50000;
    s.time = 16;
    now = 101.016;
    e = t.update(now, s);
    check(e.kind == RunEvent::Start && !e.restart && std::fabs(e.runStart - 101.0) < 1e-6, "start seen, run start = now - time");
    // Frames, one seen late (the estimate keeps the earliest).
    s.time = 1000;
    e = t.update(102.010, s);
    check(e.kind == RunEvent::None && std::fabs(t.runStart() - 101.0) < 1e-6, "the earliest start estimate is kept");
    // A respawn at a checkpoint: the clock runs on, same start.
    s.time = 5000;
    e = t.update(106.0, s);
    check(e.kind == RunEvent::None, "respawn: the same run");
    // Restart (Backspace): countdown, then a new start clock.
    s.started = false;
    s.startClock = 0;
    s.time = 0;
    t.update(107.0, s);
    s.started = true;
    s.startClock = 58000;
    s.time = 0;
    e = t.update(108.5, s);
    check(e.kind == RunEvent::Start && e.restart && std::fabs(e.runStart - 108.5) < 1e-6, "restart: a new run, flagged as restart");
    // Finish at 42.37 s.
    s.time = 42370;
    s.finished = true;
    e = t.update(150.9, s);
    check(e.kind == RunEvent::Finish && e.raceTime == 42370 && std::fabs(e.runStart - 108.5) < 1e-6 && std::fabs(e.finishAt - 150.9) < 1e-6,
          "finish: final time and times");
    e = t.update(151.0, s);
    check(e.kind == RunEvent::None, "finish reported once");
    // A pause (clock stops): the start estimate stays.
    RunTracker p;
    RaceSample q = s;
    q.finished = false;
    q.startClock = 1;
    q.time = 1000;
    p.update(10.0, q);
    p.update(20.0, q); // paused for 10 s
    q.time = 2000;
    p.update(21.0, q);
    check(std::fabs(p.runStart() - 9.0) < 1e-6, "pause: run start unchanged");
    // The map is left mid-run (two reads without a race), a new map's run starts: not a restart.
    RaceSample none;
    p.update(22.0, none);
    p.update(22.01, none);
    int race2 = 0;
    q.race = &race2;
    q.startClock = 7;
    q.time = 10;
    e = p.update(30.0, q);
    check(e.kind == RunEvent::Start && !e.restart, "new map: a new run, not a restart");
    // First seen already finished: no finish event.
    RunTracker f;
    RaceSample done = s;
    e = f.update(1.0, done);
    check(e.kind == RunEvent::None, "a run first seen finished gives no event");
    // One read without a race right at the finish (the time read out of range for a frame): the
    // finish still counts.
    RunTracker g;
    RaceSample r = s;
    r.finished = false;
    r.startClock = 9;
    r.time = 20;
    e = g.update(50.02, r);
    check(e.kind == RunEvent::Start, "transient: run started");
    r.time = 30000;
    g.update(80.0, r);
    g.update(80.016, none);
    r.finished = true;
    r.time = 30020;
    e = g.update(80.033, r);
    check(e.kind == RunEvent::Finish && e.raceTime == 30020 && std::fabs(e.runStart - 50.0) < 1e-6, "one invalid read doesn't lose the finish");
    // Two in a row do end the run: a finish seen after that has no start, no event.
    RunTracker h;
    r.finished = false;
    r.time = 20;
    h.update(50.02, r);
    h.update(60.0, none);
    h.update(60.016, none);
    r.finished = true;
    r.time = 10000;
    e = h.update(60.033, r);
    check(e.kind == RunEvent::None, "two invalid reads end the run");
}

void testNames() {
    printf("names\n");
    check(stripCodes("$o$fffA$0f0-01$z") == "A-01", "colour codes out");
    check(stripCodes("$l[http://x]Link$l") == "Link", "link codes out");
    check(stripCodes("Cost $$5") == "Cost $5", "$$ stays a $");
    check(fileSafe("a/b:c*?") == "a_b_c__", "file name characters");
    check(fileSafe("") == "Clip", "empty name");
    check(formatTime(42370) == "0:42.37", "0:42.37");
    check(formatTime(3723450) == "1:02:03.45", "1:02:03.45");
    // Two saves in the same second: names handed out are taken until their save reports
    // (clips.cpp's clipPath keeps them in a set), so the second gets " (2)".
    std::set<std::wstring> reserved;
    auto taken = [&](const std::wstring& p) { return reserved.count(p) > 0; };
    const std::wstring dir = L"C:\\Clips";
    const std::wstring a = uniqueClipPath(dir, L"A01_2026-11-10_12-00-00", taken);
    reserved.insert(a);
    const std::wstring b = uniqueClipPath(dir, L"A01_2026-11-10_12-00-00", taken);
    reserved.insert(b);
    const std::wstring c = uniqueClipPath(dir, L"A01_2026-11-10_12-00-00", taken);
    check(a == dir + L"\\A01_2026-11-10_12-00-00.mp4" && b == dir + L"\\A01_2026-11-10_12-00-00 (2).mp4" &&
              c == dir + L"\\A01_2026-11-10_12-00-00 (3).mp4",
          "same second: (2), (3)");
    reserved.erase(a); // its save reported
    check(uniqueClipPath(dir, L"A01_2026-11-10_12-00-00", taken) == a, "a name is free again once its save reported");
    // Long names: the temporary .part.mp4 of the longest name still under MAX_PATH.
    const std::wstring longDir(200, L'd');
    std::wstring longName(100, L'n');
    const size_t room = MAX_PATH - 1 - longDir.size() - 1 - clipNameReserve();
    longName[room - 1] = 0xD83D; // a surrogate pair across the cut
    longName[room] = 0xDE00;
    const std::wstring fitted = fitName(longName, room);
    check(fitted.size() <= room && fitted.back() != 0xD83D, "name cut to fit, not inside a surrogate pair");
    const std::wstring worst = longDir + L"\\" + fitted + L" (999).part.mp4";
    check(worst.size() < MAX_PATH, "the temporary file's path stays under MAX_PATH");
    // Dynamic text conversion (no fixed buffers).
    const std::wstring wideText(3000, L'\x00e9');
    check(utf8(wideText).size() == 6000 && utf16(utf8(wideText)) == wideText, "UTF-8 / UTF-16 of 3000 characters");
}

// --- The memory limit ---------------------------------------------------------------------------

void testMemory() {
    printf("memory\n");
    bool fits = true;
    for (int bitrate = 2; bitrate <= 80; bitrate++) {
        for (int sound = 0; sound < 2; sound++) {
            const int most = maxBufferSeconds(bitrate, sound != 0);
            const size_t total = videoLimit(bitrate * 1e6, most) + (sound ? soundBytes(most) : 0);
            if (most > 10 && total > kMemoryLimit) fits = false;
            // One second more would not fit (the longest that fits, not less).
            const size_t more = videoLimit(bitrate * 1e6, most + 1) + (sound ? soundBytes(most + 1) : 0);
            if (most > 10 && more <= kMemoryLimit) fits = false;
        }
    }
    check(fits, "the longest buffer fits the limit at every bitrate (2..80 Mbit/s)");
    Settings s;
    s.bitrate = 50;
    s.bufferSeconds = 300;
    s.keySeconds = 120;
    const bool clamped = clampBuffer(s);
    char what[160];
    snprintf(what, sizeof(what), "50 Mbit/s x 300 s shortened to %d s (%.0f MB)", s.bufferSeconds,
             (videoLimit(50e6, s.bufferSeconds) + soundBytes(s.bufferSeconds)) / 1048576.0);
    check(clamped && s.bufferSeconds < 300 && videoLimit(50e6, s.bufferSeconds) + soundBytes(s.bufferSeconds) <= kMemoryLimit &&
              s.keySeconds <= s.bufferSeconds,
          what);
    Settings defaults;
    check(!clampBuffer(defaults) && defaults.bufferSeconds == 60, "the default (60 s, 12 Mbit/s) stays");
    Settings ini;
    ini.bitrate = 80;
    ini.bufferSeconds = 600;
    clampBuffer(ini);
    check(videoLimit(80e6, ini.bufferSeconds) + soundBytes(ini.bufferSeconds) <= kMemoryLimit, "clips.ini's most (80 Mbit/s, 600 s) clamped too");
    // The sound's circle is chunks, about what soundBytes says.
    check(SoundRing::bytesFor(kSoundRate, kSoundChannels, 310.0) <= soundBytes(300) + SoundRing::kChunkFrames * 4, "the sound's chunks");
}

// --- The buffer -------------------------------------------------------------------------------

std::shared_ptr<const std::vector<uint8_t>> bytes(size_t n) {
    return std::make_shared<const std::vector<uint8_t>>(n, static_cast<uint8_t>(0));
}

void testRing() {
    printf("buffer\n");
    PictureRing r;
    r.setLimits(10.0, 1u << 30);
    const int64_t frame = 166667; // 60 fps
    for (int i = 0; i < 60 * 30; i++) r.push({i * frame, i % 60 == 0, bytes(1000)});
    // 30 s pushed, 10 kept: whole seconds from a key frame, at least 10 s.
    const double kept = (r.newest() - r.oldest()) / 1e7;
    check(kept >= 10.0 && kept < 11.1, "keeps 10 s in whole groups");
    std::vector<Picture> c = r.cut(r.oldest(), r.newest());
    check(!c.empty() && c.front().key, "the oldest kept is a key frame");
    // A cut starting at 25.5 s starts at the key frame at 25 s.
    c = r.cut(static_cast<int64_t>(25.5e7), static_cast<int64_t>(27.0e7));
    check(!c.empty() && c.front().time == 25 * 60 * frame && c.front().key, "starts at the last key frame before the start");
    check(!c.empty() && c.back().time <= static_cast<int64_t>(27.0e7) && c.back().time > static_cast<int64_t>(27.0e7) - frame,
          "ends at the last picture before the end");
    // A cut asking for more than is kept starts at the oldest kept.
    c = r.cut(0, static_cast<int64_t>(22.0e7));
    check(!c.empty() && c.front().time == r.oldest(), "too early: from the oldest kept");
    // A byte limit.
    PictureRing b;
    b.setLimits(100.0, 60 * 1000 * 3);
    for (int i = 0; i < 60 * 10; i++) b.push({i * frame, i % 60 == 0, bytes(1000)});
    check(b.bytes() <= 60 * 1000 * 4 && b.bytes() >= 60 * 1000 * 3, "byte limit keeps whole groups");
    // Sound: 10 s at 48 kHz, frame n = n.
    SoundRing s;
    s.reset(48000, 2, 5.0, 1000000000);
    std::vector<int16_t> pcm(48000 * 2);
    for (int sec = 0; sec < 10; sec++) {
        for (int i = 0; i < 48000; i++) pcm[i * 2] = pcm[i * 2 + 1] = static_cast<int16_t>((sec * 48000 + i) & 0x7fff);
        s.push(pcm.data(), 48000);
    }
    size_t frames = 0;
    std::vector<int16_t> cut = s.cut(1000000000 + 70000000, 1000000000 + 80000000, frames); // 7..8 s
    check(frames == 48000 && cut[0] == static_cast<int16_t>((7 * 48000) & 0x7fff), "sound cut at the right sample");
    cut = s.cut(1000000000 + 20000000, 1000000000 + 30000000, frames); // 2..3 s: no longer kept
    check(frames == 48000 && cut[100] == 0, "sound no longer kept is silence");
    cut = s.cut(1000000000 + 95000000, 1000000000 + 105000000, frames); // 9.5..10.5 s: half there
    check(frames == 48000 && cut[2 * 100] != 0 && cut[2 * 47000] == 0, "sound after the end is silence");
    // An encoder that marks no key frame: the byte limit holds anyway (it grew without end).
    PictureRing none;
    none.setLimits(10.0, 100 * 1000);
    for (int i = 0; i < 5000; i++) none.push({i * frame, false, bytes(1000)});
    check(none.bytes() <= 100 * 1000 && none.count() > 0, "no key frames: the byte limit holds");
    PictureRing one;
    one.setLimits(10.0, 100 * 1000);
    for (int i = 0; i < 5000; i++) one.push({i * frame, i == 0, bytes(1000)});
    check(one.bytes() <= 100 * 1000, "one key frame only: the byte limit holds");
}

// The sound's cut as it was before the chunks (one circle, a % per frame): what the chunks' copies
// must give, sample for sample.
struct OldSound {
    int channels = 2;
    int64_t start = 0;
    uint64_t total = 0, capacity = 0;
    int rate = 48000;
    std::vector<int16_t> data;
    void push(const int16_t* samples, size_t frames) {
        for (size_t i = 0; i < frames; i++) {
            const size_t at = static_cast<size_t>((total + i) % capacity) * channels;
            for (int c = 0; c < channels; c++) data[at + c] = samples[i * channels + c];
        }
        total += frames;
    }
    std::vector<int16_t> cut(int64_t from, int64_t to, size_t& frames) const {
        frames = to > from ? static_cast<size_t>((to - from) * rate / 10000000) : 0;
        std::vector<int16_t> out(frames * channels, 0);
        if (!capacity || !frames) return out;
        const uint64_t kept = std::min<uint64_t>(total, capacity);
        const uint64_t firstKept = total - kept;
        const int64_t first = from >= start ? static_cast<int64_t>((from - start) * rate / 10000000) : -static_cast<int64_t>((start - from) * rate / 10000000);
        for (size_t i = 0; i < frames; i++) {
            const int64_t n = first + static_cast<int64_t>(i);
            if (n < static_cast<int64_t>(firstKept) || n >= static_cast<int64_t>(total)) continue;
            const size_t at = static_cast<size_t>(static_cast<uint64_t>(n) % capacity) * channels;
            for (int c = 0; c < channels; c++) out[i * channels + c] = data[at + c];
        }
        return out;
    }
};

void testSoundChunks() {
    printf("sound in chunks\n");
    const int64_t start = 7000000000;
    SoundRing ring;
    ring.reset(48000, 2, 5.0, start);
    OldSound old;
    old.start = start;
    old.capacity = ring.capacity();
    old.data.assign(static_cast<size_t>(old.capacity) * 2, 0);
    check(ring.capacity() >= 5 * 48000 && ring.capacity() % SoundRing::kChunkFrames == 0, "whole chunks, at least the length asked");
    // 17 s in odd pieces (as the capture hands them over): the circle wraps three times.
    std::mt19937 random(12345);
    std::vector<int16_t> piece;
    uint64_t pushed = 0;
    while (pushed < 17 * 48000) {
        const size_t n = 1 + random() % 5000;
        piece.resize(n * 2);
        for (size_t i = 0; i < n; i++) {
            piece[i * 2] = static_cast<int16_t>((pushed + i) & 0x7fff);
            piece[i * 2 + 1] = static_cast<int16_t>(~(pushed + i) & 0x7fff);
        }
        ring.push(piece.data(), n);
        old.push(piece.data(), n);
        pushed += n;
    }
    bool same = true;
    int worstCopies = 0;
    size_t worstFrames = 0;
    for (int k = 0; k < 400 && same; k++) {
        // Spans before the stream, across the wrap, past the end, short and long.
        const int64_t from = start + static_cast<int64_t>(random() % 200000000) - 10000000;
        const int64_t to = from + static_cast<int64_t>(random() % 80000000);
        size_t a = 0, b = 0;
        const std::vector<int16_t> mine = ring.cut(from, to, a);
        const std::vector<int16_t> theirs = old.cut(from, to, b);
        if (a != b || mine != theirs) same = false;
        int copies = 0;
        SoundRing::copy(ring.span(from, to), &copies);
        if (copies > worstCopies) {
            worstCopies = copies;
            worstFrames = a;
        }
    }
    check(same, "chunked cut = the old cut, 400 spans (before the start, across the wrap, past the end)");
    char what[160];
    snprintf(what, sizeof(what), "one memcpy per chunk touched (at most %d for %zu frames)", worstCopies, worstFrames);
    check(worstCopies <= static_cast<int>(worstFrames / SoundRing::kChunkFrames) + 2, what);
    // The span holds its chunks: the ring freed meanwhile, the copy still reads them.
    const int64_t lastSecond = start + static_cast<int64_t>(pushed * 10000000 / 48000) - 10000000;
    SoundRing::Span held = ring.span(lastSecond, lastSecond + 5000000);
    size_t frames = 0;
    const std::vector<int16_t> before = ring.cut(lastSecond, lastSecond + 5000000, frames);
    ring.clear();
    check(SoundRing::copy(held) == before && ring.bytes() == 0, "a span outlives the ring's clear()");
    // The margin: frames the capture writes over next count as gone.
    SoundRing m;
    m.reset(48000, 2, 2.0, 0);
    std::vector<int16_t> ones(48000 * 2, 1);
    for (int i = 0; i < 4; i++) m.push(ones.data(), 48000);
    const uint64_t oldestKept = m.total() - m.capacity();
    const int64_t oldestTime = m.timeOf(oldestKept);
    const std::vector<int16_t> withMargin = SoundRing::copy(m.span(oldestTime, oldestTime + 10000000, 24000));
    check(withMargin.size() == 48000 * 2 && withMargin[0] == 0 && withMargin[2 * 23990] == 0 && withMargin[2 * 24010] == 1,
          "the margin's frames are silence, the rest is there");
}

// --- Key frames from the bitstream -----------------------------------------------------------------

void testNal() {
    printf("key frames from the bitstream\n");
    const std::vector<uint8_t> idr = {0, 0, 0, 1, 0x67, 0x64, 0x00, 0x28, 0xac, 0, 0, 0, 1, 0x68, 0xee, 0x3c, 0x80, 0, 0, 1, 0x65, 0x88, 0x84, 0x00, 0x10};
    const std::vector<uint8_t> p = {0, 0, 0, 1, 0x41, 0x9a, 0x02, 0x04, 0x10};
    const std::vector<uint8_t> seiIdr = {0, 0, 0, 1, 0x09, 0x10, 0, 0, 1, 0x06, 0x05, 0x01, 0x80, 0, 0, 1, 0x25, 0x88, 0x80}; // nal_ref_idc 1
    const std::vector<uint8_t> audP = {0, 0, 0, 1, 0x09, 0x30, 0, 0, 1, 0x01, 0x9a, 0x02};
    check(hasIdr(idr.data(), idr.size()), "SPS, PPS, IDR: a key frame");
    check(!hasIdr(p.data(), p.size()), "a P slice: no key frame");
    check(hasIdr(seiIdr.data(), seiIdr.size()), "AUD, SEI, IDR: a key frame");
    check(!hasIdr(audP.data(), audP.size()), "AUD, non-reference slice: no key frame");
    check(!hasIdr(nullptr, 0) && !hasIdr(idr.data(), 3), "nothing / cut short: no key frame");
    const std::vector<uint8_t> sets = parameterSets(idr.data(), idr.size());
    check(sets.size() == 17 && sets[4] == 0x67 && sets[13] == 0x68, "the parameter sets of a key frame");
}

// --- Encoders and the MP4 ---------------------------------------------------------------------

void makeFrame(std::vector<uint8_t>& f, int w, int h, int n) {
    // A moving gradient with a bright box: something for the encoder to do.
    uint8_t* y = f.data();
    for (int row = 0; row < h; row++) {
        const uint8_t base = static_cast<uint8_t>(16 + ((row + n * 4) % 200));
        memset(y + static_cast<size_t>(row) * w, base, w);
    }
    const int bx = (n * 13) % (w - 200), by = (n * 7) % (h - 200);
    for (int row = by; row < by + 200; row++) memset(y + static_cast<size_t>(row) * w + bx, 235, 200);
    uint8_t* uv = f.data() + static_cast<size_t>(w) * h;
    for (int row = 0; row < h / 2; row++) {
        for (int x = 0; x < w; x += 2) {
            uv[static_cast<size_t>(row) * w + x] = static_cast<uint8_t>(128 + ((x / 8 + n) % 40) - 20);
            uv[static_cast<size_t>(row) * w + x + 1] = static_cast<uint8_t>(128 + ((row / 8) % 40) - 20);
        }
    }
}

struct Probe {
    bool ran = false;
    double duration = 0.0, videoDuration = 0.0, audioDuration = 0.0;
    int streams = 0, width = 0, height = 0;
    std::string codecs;
};

std::string run(const std::string& cmd) {
    std::string out;
    FILE* p = _popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[512];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    _pclose(p);
    return out;
}

Probe ffprobe(const std::wstring& path) {
    Probe p;
    const std::string where = run("where ffprobe 2>nul");
    if (where.empty()) return p;
    const std::string out = run("ffprobe -v error -show_entries format=duration:stream=codec_name,width,height,duration -of default=nw=1 \"" +
                                narrow(path) + "\" 2>&1");
    p.ran = true;
    size_t at = 0;
    std::string stream;
    while (at < out.size()) {
        size_t end = out.find('\n', at);
        if (end == std::string::npos) end = out.size();
        std::string line = out.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        at = end + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "codec_name") {
            p.streams++;
            stream = value;
            p.codecs += (p.codecs.empty() ? "" : ",") + value;
        } else if (key == "width" && value != "N/A") {
            p.width = atoi(value.c_str());
        } else if (key == "height" && value != "N/A") {
            p.height = atoi(value.c_str());
        } else if (key == "duration" && value != "N/A") {
            // Stream durations come after their codec_name; the format's last.
            if (stream == "h264") p.videoDuration = atof(value.c_str());
            else if (stream == "aac") p.audioDuration = atof(value.c_str());
            p.duration = atof(value.c_str());
        }
    }
    return p;
}

// Without ffprobe: the boxes (moov / mvhd duration, trak count).
Probe boxes(const std::wstring& path) {
    Probe p;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return p;
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + n);
    fclose(f);
    auto be32 = [&](size_t at) { return (uint32_t(d[at]) << 24) | (uint32_t(d[at + 1]) << 16) | (uint32_t(d[at + 2]) << 8) | d[at + 3]; };
    std::function<void(size_t, size_t)> walk = [&](size_t at, size_t end) {
        while (at + 8 <= end) {
            uint32_t size = be32(at);
            const std::string type(reinterpret_cast<const char*>(&d[at + 4]), 4);
            if (size < 8 || at + size > end) break;
            if (type == "moov") walk(at + 8, at + size);
            if (type == "trak") p.streams++;
            if (type == "mvhd") {
                const uint32_t scale = be32(at + 20), duration = be32(at + 24);
                p.duration = scale ? static_cast<double>(duration) / scale : 0.0;
            }
            at += size;
        }
    };
    walk(0, d.size());
    p.ran = true;
    return p;
}

void checkFile(const std::wstring& path, double expected, bool sound, int w, int h) {
    Probe p = ffprobe(path);
    if (p.ran) {
        printf("  ffprobe: %s, %d streams, %dx%d, %.3f s (video %.3f, sound %.3f)\n", p.codecs.c_str(), p.streams, p.width, p.height, p.duration,
               p.videoDuration, p.audioDuration);
        check(p.streams == (sound ? 2 : 1), "stream count");
        check(p.width == w && p.height == h, "size");
        check(p.codecs.find("h264") != std::string::npos && (!sound || p.codecs.find("aac") != std::string::npos), "codecs h264 + aac");
        check(std::fabs(p.videoDuration - expected) < 0.1, "video length");
        if (sound) check(std::fabs(p.audioDuration - expected) < 0.15, "sound length");
        // Every picture decodes (ffprobe decodes them all and counts; ffmpeg's null output would
        // complain about frames closer than its 1/60 s time base, which a varying frame rate has).
        const std::string counted = run("ffprobe -v error -select_streams v -count_frames -count_packets -show_entries "
                                        "stream=nb_read_frames,nb_read_packets -of csv=p=0 \"" + narrow(path) + "\" 2>&1");
        int frames = -1, packets = -2;
        sscanf_s(counted.c_str(), "%d,%d", &frames, &packets);
        char what[200];
        snprintf(what, sizeof(what), "every picture decodes (%d of %d)%s", frames, packets, frames == packets ? "" : (": " + counted.substr(0, 120)).c_str());
        check(frames == packets && frames > 0, what);
    } else {
        p = boxes(path);
        printf("  boxes: %d tracks, %.3f s\n", p.streams, p.duration);
        check(p.streams == (sound ? 2 : 1), "track count");
        check(std::fabs(p.duration - expected) < 0.15, "length");
    }
}

// A synthetic run straight through an encoder: 20 s, frames every 1/60 s but with a stretch at
// 45 fps (frames left out, as a slow game gives), a 440 Hz tone; the run 5 s .. 15 s, cut -3 / +3.
void testEncoder(const char* label, std::unique_ptr<PictureEncoder> enc, const std::wstring& folder, bool keep) {
    printf("%s\n", label);
    if (!enc) return;
    printf("  %s\n", enc->description().c_str());
    const int w = 1920, h = 1080;
    std::vector<uint8_t> frame(static_cast<size_t>(w) * h * 3 / 2);
    PictureRing ring;
    ring.setLimits(60.0, 512u << 20);
    std::vector<int64_t> times;
    std::vector<uint8_t> header;
    double encodeMs = 0.0, worst = 0.0;
    int encoded = 0, keys = 0, keysAgree = 0;
    const int64_t origin = 50000000000; // an arbitrary clock
    const PictureSink sink = [&](const EncodedPicture& p) {
        Picture pic;
        pic.time = times[static_cast<size_t>(p.frame)];
        pic.key = p.key;
        pic.data = std::make_shared<const std::vector<uint8_t>>(p.data, p.data + p.size);
        if (p.key) keys++;
        if (hasIdr(p.data, p.size) == p.key) keysAgree++;
        if (header.empty() && p.key) header = parameterSets(p.data, p.size);
        ring.push(pic);
        encoded++;
    };
    std::string error;
    bool ok = true;
    int n = 0;
    for (int i = 0; i < 20 * 60 && ok; i++) {
        // 8 s .. 10 s: every fourth frame left out (45 fps).
        if (i >= 8 * 60 && i < 10 * 60 && i % 4 == 3) continue;
        times.push_back(origin + static_cast<int64_t>(i * 1e7 / 60.0 + 0.5));
        makeFrame(frame, w, h, i);
        LARGE_INTEGER t0, t1, f;
        QueryPerformanceCounter(&t0);
        ok = enc->encode(frame.data(), sink, error);
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        const double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
        encodeMs += ms;
        worst = std::max(worst, ms);
        n++;
    }
    if (ok) ok = enc->flush(sink, error);
    check(ok, ok ? "encoded" : ("encoded: " + error).c_str());
    if (header.empty()) header = enc->header();
    printf("  %d frames in, %d pictures out, %d key frames, %.2f ms a frame (slowest %.1f), %.1f MB\n", n, encoded, keys, encodeMs / n, worst,
           ring.bytes() / 1048576.0);
    check(encoded == n, "a picture for every frame");
    check(keys >= 19 && keys <= 22, "a key frame every second");
    check(keysAgree == encoded, "the bitstream's IDR slices are exactly the pictures marked key");
    check(!header.empty(), "parameter sets found");
    // The tone.
    SoundRing sound;
    sound.reset(48000, 2, 30.0, origin - 5000000); // the sound started half a second before the first frame
    std::vector<int16_t> pcm(48000 * 2);
    for (int sec = 0; sec < 21; sec++) {
        for (int i = 0; i < 48000; i++) {
            const double t = sec + i / 48000.0;
            pcm[i * 2] = pcm[i * 2 + 1] = static_cast<int16_t>(8000.0 * sin(2.0 * 3.14159265358979 * 440.0 * t));
        }
        sound.push(pcm.data(), 48000);
    }
    // Cut: run 5 s .. 15 s, 3 s before and after.
    const int64_t from = origin + 20000000, to = origin + 180000000;
    std::vector<Picture> pictures = ring.cut(from, to);
    check(!pictures.empty() && pictures.front().time == from, "the cut starts on the key frame at 2 s");
    const std::vector<int64_t> durations = PictureRing::durations(pictures, 166667);
    std::vector<venc::Mp4Picture> list(pictures.size());
    for (size_t i = 0; i < pictures.size(); i++) {
        list[i].data = pictures[i].data->data();
        list[i].size = static_cast<uint32_t>(pictures[i].data->size());
        list[i].time = pictures[i].time;
        list[i].duration = durations[i];
        list[i].key = pictures[i].key;
    }
    size_t frames = 0;
    const std::vector<int16_t> cut = sound.cut(pictures.front().time, pictures.back().time + durations.back(), frames);
    venc::Mp4Sound s;
    s.format.wFormatTag = WAVE_FORMAT_PCM;
    s.format.nChannels = 2;
    s.format.nSamplesPerSec = 48000;
    s.format.wBitsPerSample = 16;
    s.format.nBlockAlign = 4;
    s.format.nAvgBytesPerSec = 48000 * 4;
    s.samples = cut.data();
    s.frames = frames;
    s.time = pictures.front().time;
    std::wstring path = folder + L"\\clipstest_" + std::wstring(label, label + strlen(label)) + L".mp4";
    std::replace(path.begin() + folder.size() + 1, path.end(), L' ', L'_');
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    ok = venc::writeMp4(path, venc::Codec::H264, w, h, 60.0, header, list, &s, error);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    printf("  MP4 written in %.0f ms: %s\n", (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart, narrow(path).c_str());
    check(ok, ok ? "MP4 written" : ("MP4 written: " + error).c_str());
    if (ok) checkFile(path, 16.0, true, w, h);
    if (ok && !keep) DeleteFileW(path.c_str());
}

// --- The recorder's stop: never waits ----------------------------------------------------------

bool waitRunning(Recorder& rec) {
    for (int i = 0; i < 500 && rec.state() == Recorder::State::Starting; i++) Sleep(10);
    return rec.state() == Recorder::State::Running;
}

void testRecorderStop(const std::wstring& folder, bool keep, bool software) {
    printf("recorder: stop without waiting\n");
    Recorder rec;
    RecorderSetup setup;
    setup.width = 1280;
    setup.height = 720;
    setup.fps = 30;
    setup.bitrate = 80e6;
    setup.seconds = 600; // clips.ini's most: the recorder holds the memory limit itself too
    setup.sound = false;
    rec.start(setup);
    check(rec.stats().bufferLimit <= kMemoryLimit, "600 s at 80 Mbit/s: the pictures' limit stays under the memory limit");
    if (!waitRunning(rec) && software) {
        printf("  no hardware encoder (%s): Windows' own\n", rec.failure().c_str());
        setup.encoder = 2;
        setup.softwareOnly = true;
        rec.start(setup);
        waitRunning(rec);
    }
    if (rec.state() != Recorder::State::Running) {
        printf("  no encoder here (%s): skipped\n", rec.failure().c_str());
        return;
    }
    printf("  %s\n", rec.description().c_str());
    std::vector<uint8_t> frame(static_cast<size_t>(setup.width) * setup.height * 3 / 2);
    const int64_t t0 = now100ns();
    for (int i = 0; i < 90; i++) { // 3 s at 30 fps
        makeFrame(frame, setup.width, setup.height, i);
        while (now100ns() - t0 < static_cast<int64_t>(i) * 333333) Sleep(1);
        if (uint8_t* dst = rec.acquire()) {
            memcpy(dst, frame.data(), frame.size());
            rec.submit(now100ns());
        }
    }
    // A run's save whose span ends 2 s from now (the seconds after a finish); the recording stops
    // meanwhile (a setting changed): the caller must not wait for the save.
    SaveJob job;
    job.path = folder + L"\\clipstest_stop.mp4";
    job.to = now100ns() + 20000000;
    job.from = job.to - 40000000;
    job.what = "stop test";
    rec.save(job);
    Sleep(100); // the saver waits for the span's end now
    const int64_t s0 = now100ns();
    rec.stop();
    const double stopMs = static_cast<double>(now100ns() - s0) / 1e4;
    char what[200];
    snprintf(what, sizeof(what), "stop() returns at once while a save waits (%.2f ms)", stopMs);
    check(stopMs < 50.0, what);
    check(rec.state() == Recorder::State::Off && rec.acquire() == nullptr, "stopped: no frame taken any more");
    SaveResult r;
    bool got = false;
    for (int i = 0; i < 1000 && !(got = rec.takeResult(r)); i++) Sleep(10);
    printf("  %s\n", got ? r.line.c_str() : "(no result)");
    check(got && r.ok, "the waiting save is written by the stopped recording, its result comes through");
    for (int i = 0; i < 500 && rec.retiring() > 0; i++) Sleep(10);
    check(rec.retiring() == 0, "the stopped recording's thread ended");
    // Start and stop in quick turns (settings dragged in the menu): never a wait.
    double worst = 0.0;
    for (int k = 0; k < 5; k++) {
        rec.start(setup);
        Sleep(k * 40);
        const int64_t a = now100ns();
        rec.stop();
        worst = std::max(worst, static_cast<double>(now100ns() - a) / 1e4);
    }
    for (int i = 0; i < 1000 && rec.retiring() > 0; i++) Sleep(10);
    snprintf(what, sizeof(what), "start/stop five times: the slowest stop %.2f ms, all ended", worst);
    check(worst < 50.0 && rec.retiring() == 0, what);
    if (got && r.ok) checkFile(job.path, r.seconds, false, setup.width, setup.height);
    if (got && r.ok && !keep) DeleteFileW(job.path.c_str());
}

// --- The last runs, Save last run after a restart, the adaptive buffer ----------------------------

void testRunLog() {
    printf("last runs\n");
    RunLog log;
    check(log.last() == nullptr, "no run yet: nothing to save");
    int ids[7] = {};
    for (int i = 0; i < 7; i++) {
        RunMarker m;
        m.runStart = 100.0 + i * 50.0;
        m.finishAt = m.runStart + 40.0;
        m.raceTime = 40000 + i;
        m.pb = i == 3;
        ids[i] = log.add(m);
    }
    check(log.runs().size() == RunLog::kKept, "keeps the last five");
    check(log.runs().front().raceTime == 40002 && log.last() && log.last()->raceTime == 40006, "the oldest two went, the newest is last");
    check(log.find(ids[0]) == nullptr && log.find(ids[6]) == log.last(), "found by id; a dropped one is gone");
    check(ids[6] != ids[5], "every run its own id");
    if (RunMarker* r = log.find(ids[4])) r->saved = true;
    check(log.find(ids[4]) && log.find(ids[4])->saved, "marked saved");

    // The tracker and the log together: a run finishes, the player restarts at once and drives on;
    // the last run is still the finished one, its span unchanged.
    RunTracker t;
    RunLog runs;
    int race = 0;
    RaceSample s;
    s.valid = true;
    s.race = &race;
    double now = 10.0;
    s.started = false;
    t.update(now, s);
    s.started = true;
    s.startClock = 1000;
    s.time = 0;
    t.update(now, s); // start at 10.0
    now = 52.5;
    s.time = 42500;
    s.finished = true;
    RunEvent e = t.update(now, s);
    check(e.kind == RunEvent::Finish, "finish seen");
    RunMarker m;
    m.runStart = e.runStart;
    m.finishAt = e.finishAt;
    m.raceTime = e.raceTime;
    runs.add(m);
    // Restart: the countdown, then a new run that drives for 20 s.
    now = 54.0;
    s.finished = false;
    s.started = false;
    s.time = 0;
    t.update(now, s);
    now = 57.0;
    s.started = true;
    s.startClock = 9000;
    e = t.update(now, s);
    check(e.kind == RunEvent::Start, "the restart's run started");
    now = 77.0;
    s.time = 20000;
    t.update(now, s);
    RunMarker* last = runs.last();
    check(last && std::fabs(last->runStart - 10.0) < 1e-6 && std::fabs(last->finishAt - 52.5) < 1e-6 && last->raceTime == 42500,
          "after the restart the last run is still the finished one, its span as it was");
    check(t.running() && std::fabs(t.runStart() - 57.0) < 1e-6, "the run so far starts at the restart's start");
    // In the buffer? A 60 s buffer at 77 s holds 17 s on: the run's 7 s (its start minus 3 s) is
    // gone, its end is there; a buffer from 0 s on holds all of it.
    const double from = last->runStart - 3.0, to = last->finishAt + 3.0;
    check(spanInBuffer(from, to, 0.0) == InBuffer::Whole, "a buffer from before the run holds the whole run");
    check(spanInBuffer(from, to, 77.0 - 60.0) == InBuffer::Part, "a 60 s buffer holds its end only");
    check(spanInBuffer(from, to, 60.0) == InBuffer::Gone, "a buffer from after the finish holds nothing of it");
    check(spanInBuffer(from, to, -1.0) == InBuffer::Gone, "an empty buffer holds nothing");
    check(spanInBuffer(7.0, 55.5, 7.2) == InBuffer::Whole, "a quarter second of slack (key frames)");
}

void testAdaptiveBuffer() {
    printf("adaptive buffer\n");
    check(adaptiveBufferSeconds(-1, 30, 300) == 60, "no author time: 60 s");
    check(adaptiveBufferSeconds(25000, 30, 300) == 60, "a 25 s map: at least 60 s");
    check(adaptiveBufferSeconds(112300, 30, 300) == 179, "1:52.30: 1.5 x + 10 = 179 s");
    check(adaptiveBufferSeconds(25000, 90, 300) == 90, "at least what the save key saves (90 s)");
    check(adaptiveBufferSeconds(600000, 30, 300) == 300, "a 10 min map: held to the most");
    const int most = maxBufferSeconds(50, true);
    check(adaptiveBufferSeconds(600000, 30, most) == most && videoLimit(50e6, most) + soundBytes(most) <= kMemoryLimit,
          "at 50 Mbit/s: held to the memory limit");
    Settings s;
    check(s.adaptiveBuffer && s.soundSource == 0 && s.keySeconds == 30 && s.saveKey == "F4" && s.lastRunKey == "Ctrl+F4" &&
              s.runSoFarKey == "Ctrl+Shift+F4" && s.menuKey == "Shift+F4" && !s.enabled,
          "defaults: Instant replay off, adaptive, game sound, 30 s, F4 / Ctrl+F4 / Ctrl+Shift+F4 / Shift+F4");
    check(s.saveKey != s.lastRunKey && s.saveKey != s.runSoFarKey && s.lastRunKey != s.runSoFarKey && s.menuKey != s.lastRunKey &&
              s.menuKey != s.runSoFarKey,
          "the four keys differ (TM Clips' window: Shift+F4)");
    Settings other = s;
    other.soundSource = 1;
    check(!s.sameRecording(other), "another sound source records again");
    other = s;
    other.adaptiveBuffer = false;
    check(!s.sameRecording(other), "adaptive on / off records again");
}

// The recorder end to end: a run from 0.5 s to 2.5 s, then 3 s more of driving (a restart), then the
// run saved from the buffer: the clip is the run's span, not the last seconds.
void testLastRunSave(const std::wstring& folder, bool keep, bool software) {
    printf("recorder: last run saved after a restart\n");
    Recorder rec;
    RecorderSetup setup;
    setup.width = 1280;
    setup.height = 720;
    setup.fps = 30;
    setup.bitrate = 8e6;
    setup.seconds = 60;
    setup.sound = false;
    rec.start(setup);
    if (!waitRunning(rec) && software) {
        setup.encoder = 2;
        setup.softwareOnly = true;
        rec.start(setup);
        waitRunning(rec);
    }
    if (rec.state() != Recorder::State::Running) {
        printf("  no encoder here (%s): skipped\n", rec.failure().c_str());
        return;
    }
    check(rec.oldest() < 0, "an empty buffer: no oldest picture");
    std::vector<uint8_t> frame(static_cast<size_t>(setup.width) * setup.height * 3 / 2);
    const int64_t t0 = now100ns();
    for (int i = 0; i < 165; i++) { // 5.5 s at 30 fps
        makeFrame(frame, setup.width, setup.height, i);
        while (now100ns() - t0 < static_cast<int64_t>(i) * 333333) Sleep(1);
        if (uint8_t* dst = rec.acquire()) {
            memcpy(dst, frame.data(), frame.size());
            rec.submit(now100ns());
        }
    }
    Sleep(200); // the encoder's last pictures
    const double oldest = rec.oldest() / 1e7;
    const double runFrom = t0 / 1e7 + 0.5, runTo = t0 / 1e7 + 2.5;
    check(oldest > 0.0 && spanInBuffer(runFrom, runTo, oldest) == InBuffer::Whole, "the run is in the buffer 3 s after its end");
    SaveJob job;
    job.path = folder + L"\\clipstest_lastrun.mp4";
    job.from = static_cast<int64_t>(runFrom * 1e7);
    job.to = static_cast<int64_t>(runTo * 1e7);
    job.what = "0:02.00";
    rec.save(job);
    SaveResult r;
    bool got = false;
    for (int i = 0; i < 1000 && !(got = rec.takeResult(r)); i++) Sleep(10);
    printf("  %s\n", got ? r.line.c_str() : "(no result)");
    // The cut starts at the key frame before the span (one a second): 2.0 to 2.5 s long.
    char what[160];
    snprintf(what, sizeof(what), "saved the run's span, not the last seconds (%.2f s)", got ? r.seconds : 0.0);
    check(got && r.ok && r.seconds >= 1.9 && r.seconds <= 2.7, what);
    rec.stop();
    if (got && r.ok) checkFile(job.path, r.seconds, false, setup.width, setup.height);
    if (got && r.ok && !keep) DeleteFileW(job.path.c_str());
    for (int i = 0; i < 500 && rec.retiring() > 0; i++) Sleep(10);
}

// --- The NV12 passes on a device, and the recorder end to end ---------------------------------

void testDevice(const std::wstring& folder, bool keep) {
    printf("device\n");
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TMClipsTest";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"clipstest", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.BackBufferWidth = 2560; // a 1440p game, recorded at 1080p
    pp.BackBufferHeight = 1440;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    IDirect3DDevice9* device = nullptr;
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp,
                                         &device))) {
        check(false, "a Direct3D 9 device");
        return;
    }
    IDirect3DSurface9* back = nullptr;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
    // Colours: a solid colour through the passes, compared with BT.709 studio range.
    struct Colour {
        int r, g, b;
    } colours[] = {{200, 100, 50}, {16, 16, 16}, {255, 255, 255}, {0, 0, 255}};
    Grabber g;
    for (const Colour& c : colours) {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(c.r, c.g, c.b), 1.0f, 0);
        const bool grabbed = g.grab(device, back, 1920, 1080, 0);
        int y = -1, u = -1, v = -1;
        g.collect(
            [&](const PlaneReadback::Frame& f, int64_t) {
                std::vector<uint8_t> nv12(PlaneReadback::packedBytes(f));
                PlaneReadback::pack(f, nv12.data());
                y = nv12[1920 * 540 + 960];
                u = nv12[1920 * 1080 + 1920 * 270 + 960];
                v = nv12[1920 * 1080 + 1920 * 270 + 961];
            },
            true);
        const double r = c.r / 255.0, gg = c.g / 255.0, b = c.b / 255.0;
        const double l = 0.2126 * r + 0.7152 * gg + 0.0722 * b;
        const int ey = static_cast<int>(std::lround(16 + 219 * l)), eu = static_cast<int>(std::lround(128 + 224 * (b - l) / 1.8556)),
                  ev = static_cast<int>(std::lround(128 + 224 * (r - l) / 1.5748));
        char what[160];
        snprintf(what, sizeof(what), "RGB %d %d %d -> Y %d U %d V %d (expected %d %d %d)", c.r, c.g, c.b, y, u, v, ey, eu, ev);
        check(grabbed && std::abs(y - ey) <= 1 && std::abs(u - eu) <= 1 && std::abs(v - ev) <= 1, what);
    }
    // TM Vibrant Shaders' frame end (clips::frameEnd): the finished frame copied while the game's scene is
    // still open, then another mod's Present hook (Horizon Host) draws over the back buffer, then the
    // grab at Present takes the copy: the other mod's colour must not be in the clip.
    {
        device->BeginScene();
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(200, 100, 50), 1.0f, 0);
        const bool copied = g.copy(device, back, 1920, 1080);
        device->EndScene();
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 255), 1.0f, 0); // "Horizon's menu"
        const bool grabbed = g.grab(device, nullptr, 1920, 1080, 0);
        int y = -1;
        g.collect([&](const PlaneReadback::Frame& f, int64_t) {
            std::vector<uint8_t> nv12(PlaneReadback::packedBytes(f));
            PlaneReadback::pack(f, nv12.data());
            y = nv12[1920 * 540 + 960];
        }, true);
        const int game = static_cast<int>(std::lround(16 + 219 * (0.2126 * 200 / 255.0 + 0.7152 * 100 / 255.0 + 0.0722 * 50 / 255.0)));
        char what[160];
        snprintf(what, sizeof(what), "frame end copy inside the scene, other mod drew after: Y %d (the game's %d)", y, game);
        check(copied && grabbed && std::abs(y - game) <= 1, what);
    }
    // Cost on the "game's" thread: 600 frames of a changing picture, read back as the game would
    // (the frame before's), into the recorder.
    Recorder rec;
    RecorderSetup setup;
    setup.width = 1920;
    setup.height = 1080;
    setup.fps = 60;
    setup.bitrate = 12e6;
    setup.seconds = 30;
    setup.sound = true;
    rec.start(setup);
    waitRunning(rec); // the sound's source may take up to 3 s to be ready, the encoder on top
    check(rec.state() == Recorder::State::Running, ("recorder running: " + rec.description() + rec.failure()).c_str());
    double grabMs = 0.0, collectMs = 0.0, worst = 0.0;
    int grabbed = 0;
    const int64_t start = now100ns();
    for (int i = 0; i < 600; i++) {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(i % 256, (i * 3) % 256, 128), 1.0f, 0);
        const int64_t c0 = now100ns();
        g.collect([&](const PlaneReadback::Frame& f, int64_t time) {
            uint8_t* dst = rec.acquire();
            if (!dst) return;
            PlaneReadback::pack(f, dst);
            rec.submit(time);
        });
        const double cm = (now100ns() - c0) / 1e4;
        if (g.grab(device, back, 1920, 1080, now100ns())) {
            grabbed++;
            grabMs += g.lastGrabMs;
            collectMs += cm;
            worst = std::max(worst, g.lastGrabMs + cm);
        }
        device->Present(nullptr, nullptr, nullptr, nullptr);
        // About 60 frames a second, as a game would present.
        while (now100ns() - start < static_cast<int64_t>((i + 1) * 1e7 / 60.0)) Sleep(1);
    }
    g.collect([&](const PlaneReadback::Frame& f, int64_t time) {
        uint8_t* dst = rec.acquire();
        if (!dst) return;
        PlaneReadback::pack(f, dst);
        rec.submit(time);
    }, true);
    Sleep(300);
    const RecorderStats st = rec.stats();
    printf("  game thread: %.2f ms a frame (copy and convert %.2f, read back and hand over %.2f, slowest %.2f); %d of 600 taken\n",
           (grabMs + collectMs) / std::max(1, grabbed), grabMs / std::max(1, grabbed), collectMs / std::max(1, grabbed), worst, grabbed);
    printf("  encoder thread: %d frames, %.2f ms a frame (slowest %.1f), %d dropped; buffer %.1f s, %.1f MB\n", st.framesEncoded,
           st.encodeMs / std::max(1, st.framesEncoded), st.encodeMaxMs, st.framesDropped, st.bufferSeconds, st.bufferBytes / 1048576.0);
    check(grabbed >= 590 && st.framesDropped < 10 && st.framesEncoded >= 580, "every frame through");
    // Save the last 5 s.
    SaveJob job;
    job.path = folder + L"\\clipstest_recorder.mp4";
    job.to = now100ns();
    job.from = job.to - 50000000;
    job.what = "last 5 s";
    rec.save(job);
    SaveResult r;
    for (int i = 0; i < 500 && !rec.takeResult(r); i++) Sleep(10);
    printf("  %s\n", r.line.c_str());
    check(r.ok, "recorder saved the last 5 s");
    rec.stop();
    if (r.ok) checkFile(job.path, r.seconds, true, 1920, 1080);
    if (r.ok && !keep) DeleteFileW(job.path.c_str());
    back->Release();
    g.releaseDevice();
    device->Release();
    d3d->Release();
    DestroyWindow(window);
}

} // namespace

int main(int argc, char** argv) {
    bool software = false, device = false, keep = false;
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    std::wstring folder = temp;
    if (!folder.empty() && folder.back() == L'\\') folder.pop_back();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--software")) software = true;
        else if (!strcmp(argv[i], "--device")) device = true;
        else if (!strcmp(argv[i], "--keep")) keep = true;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            wchar_t w[MAX_PATH] = {};
            MultiByteToWideChar(CP_UTF8, 0, argv[++i], -1, w, MAX_PATH);
            folder = w;
        }
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1); // Sleep(1) of 1 ms: the device test presents at 60 fps as a game does
    testTracker();
    testRunLog();
    testAdaptiveBuffer();
    testNames();
    testMemory();
    testRing();
    testSoundChunks();
    testNal();
    EncoderSetup e;
    e.width = 1920;
    e.height = 1080;
    e.fps = 60;
    e.bitrate = 12e6;
    e.gopSeconds = 1.0;
    std::string why;
    auto nv = openNvenc(e, why);
    if (!nv) printf("NVENC: not here (%s)\n", why.c_str());
    testEncoder("NVENC", std::move(nv), folder, keep);
    why.clear();
    auto mft = openMft(e, why);
    if (!mft) printf("Media Foundation hardware: not here (%s)\n", why.c_str());
    testEncoder("Media Foundation", std::move(mft), folder, keep);
    if (software) {
        // Windows' own encoder, which every PC has: the asynchronous path is the hardware one's.
        e.softwareOnly = true;
        why.clear();
        // Skip the hardware ones: their names are listed first; open with software allowed and see.
        auto sw = openMft(e, why);
        testEncoder("Media Foundation software", std::move(sw), folder, keep);
    }
    testRecorderStop(folder, keep, software);
    testLastRunSave(folder, keep, software);
    if (device) testDevice(folder, keep);
    printf("%s (%d failed)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
