#include "clips/clips.h"
#include "clips/grabber.h"
#include "clips/race.h"
#include "clips/recorder.h"
#include "clips/settings.h"
#include "keys.h"
#include "log.h"
#include "imgui.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace tmshaders {
namespace clips {
namespace {

Host g_host = Host::Tmvs;
bool g_booted = false;
Settings g_settings;
Settings g_saved;           // what clips.ini holds
double g_lastEdit = -1.0;   // the panel changed a setting (seconds)
Recorder* g_recorder = nullptr; // made at boot, never freed (its threads may still run at process exit)
Grabber g_grabber;
RunTracker g_tracker;
bool g_paused = false;
bool g_idle = false;
HANDLE g_hostMutex = nullptr;
IDirect3DDevice9* g_lastDevice = nullptr; // the device present() saw last
std::set<std::wstring> g_reserved;        // clip names handed out whose saves haven't reported yet

// TM Vibrant Shaders' clips ran Present: a counter in a small named mapping
// (Local\TMClips_Beat_<pid>), counted up by TM Vibrant Shaders every frame, read by the standalone.
volatile LONG* g_beat = nullptr;
struct Handshake {
    bool announced = false;     // the standalone logged that TM Vibrant Shaders is loaded
    bool seen = false;          // a beat came
    LONG last = 0;
    int framesWithout = 0;      // the standalone's usable frames without a beat
    bool tmvsRuns = false;
} g_hand;

// What the recorder runs with.
struct Active {
    bool have = false;
    int width = 0, height = 0;
    Settings settings;
    bool startLogged = false;
    bool failLogged = false;
} g_active;

double g_nextDue = 0.0;     // the next frame to take (seconds)
bool g_sceneThisFrame = false; // "Without HUD": the host shaded a scene this frame (sceneDone)
int64_t g_frames = 0;

struct Pending {
    double at;              // seconds: hand it to the recorder then
    SaveJob job;
};
std::vector<Pending> g_pending;

struct Toast {
    std::string text;
    double start = -100.0;
    bool ok = true;
} g_toast;

// Per-frame cost on the game's thread, for the log.
struct Cost {
    double grabMs = 0.0, collectMs = 0.0, maxMs = 0.0;
    int frames = 0, grabbed = 0, skipped = 0;
    double since = 0.0;
} g_cost;

std::string g_status = "Starting";

double seconds() {
    return static_cast<double>(now100ns()) / 1e7;
}

std::string narrow(const std::wstring& text) {
    return utf8(text);
}

std::wstring widen(const std::string& text) {
    return utf16(text);
}

const char* hostName() {
    return g_host == Host::Tmvs ? "TM Vibrant Shaders" : "TM Clips";
}

std::wstring mutexName(const wchar_t* what) {
    wchar_t name[96];
    swprintf(name, 96, L"Local\\TMClips_%s_%lu", what, GetCurrentProcessId());
    return name;
}

bool mutexExists(const wchar_t* what) {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, mutexName(what).c_str());
    if (!h) return false;
    CloseHandle(h);
    return true;
}

void showToast(const std::string& text, bool ok) {
    g_toast.text = text;
    g_toast.start = seconds();
    g_toast.ok = ok;
}

// The game's window is in front (keys pressed elsewhere don't count).
bool gameInFront() {
    HWND front = GetForegroundWindow();
    DWORD pid = 0;
    if (front) GetWindowThreadProcessId(front, &pid);
    return pid == GetCurrentProcessId();
}

unsigned modsHeld() {
    unsigned m = 0;
    if (GetAsyncKeyState(VK_SHIFT) < 0) m |= keys::kShift;
    if (GetAsyncKeyState(VK_CONTROL) < 0) m |= keys::kCtrl;
    if (GetAsyncKeyState(VK_MENU) < 0) m |= keys::kAlt;
    return m;
}

// Saves whose span hasn't ended (a run's last seconds after its finish) go to the recording that
// holds their start before it stops: the clip ends where the recording stopped, but it is written.
void handOverPending() {
    if (g_pending.empty()) return;
    const bool running = g_recorder && g_recorder->state() == Recorder::State::Running;
    for (const Pending& p : g_pending) {
        if (running) {
            g_recorder->save(p.job);
            TMVS_LOG("clips: the save of %s is written now, up to where the recording stops", p.job.what.c_str());
        } else {
            g_reserved.erase(p.job.path);
            TMVS_LOG("clips: the save of %s was dropped: the recording wasn't running", p.job.what.c_str());
        }
    }
    g_pending.clear();
}

void stopRecording(const char* why) {
    if (!g_active.have) return;
    handOverPending();
    if (g_recorder) g_recorder->stop(); // returns at once (the recording ends on a thread of its own)
    g_grabber.releaseDevice();
    g_active = Active();
    TMVS_LOG("clips: recording stopped (%s)", why);
}

void startRecording(int width, int height) {
    handOverPending();
    if (g_recorder) g_recorder->stop();
    g_grabber.releaseDevice();
    RecorderSetup r;
    r.width = width;
    r.height = height;
    r.fps = g_settings.fps;
    r.bitrate = g_settings.bitrate * 1e6;
    r.encoder = g_settings.encoder;
    r.sound = g_settings.sound;
    r.seconds = g_settings.bufferSeconds;
    g_recorder->start(r);
    g_active = Active();
    g_active.have = true;
    g_active.width = width;
    g_active.height = height;
    g_active.settings = g_settings;
    g_nextDue = 0.0;
    g_cost = Cost();
    g_cost.since = seconds();
}

// The video's size for a back buffer: the game's own, or scaled down to the setting's rows; width a
// multiple of 4 (NV12 read back four pixels a texel), height even.
void videoSize(UINT backW, UINT backH, int& w, int& h) {
    double scale = 1.0;
    if (g_settings.height > 0 && static_cast<int>(backH) > g_settings.height) scale = static_cast<double>(g_settings.height) / backH;
    w = static_cast<int>(std::floor(backW * scale)) & ~3;
    h = static_cast<int>(std::floor(backH * scale + 0.5)) & ~1;
}

std::string dateText() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char text[32];
    snprintf(text, sizeof(text), "%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return text;
}

// A clip's file: never one that exists or that a save still writes (two saves in the same second get
// " (2)"), short enough for the MP4 writer's temporary name (".part.mp4") under MAX_PATH.
std::wstring clipPath(const std::string& map, const std::string& raceTime) {
    std::string name = fileSafe(map.empty() ? std::string("Clip") : map) + "_" + dateText();
    if (!raceTime.empty()) {
        std::string t = raceTime;
        std::replace(t.begin(), t.end(), ':', '-');
        name += "_" + t;
    }
    const std::wstring dir = clipsDir();
    const int room = static_cast<int>(MAX_PATH) - 1 - static_cast<int>(dir.size()) - 1 - static_cast<int>(clipNameReserve());
    const std::wstring path = uniqueClipPath(dir, fitName(widen(name), static_cast<size_t>(std::max(room, 8))), [](const std::wstring& p) {
        return g_reserved.count(p) > 0 || GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
    });
    g_reserved.insert(path);
    return path;
}

bool recording() {
    return g_recorder && g_recorder->state() == Recorder::State::Running;
}

void queueSave(double at, const SaveJob& job) {
    g_pending.push_back({at, job});
}

void onFinish(const RunEvent& e) {
    const std::string map = mapName();
    const std::string uid = mapUid();
    const std::string key = !uid.empty() ? uid : fileSafe(map);
    const int best = bestTime(key);
    const bool pb = best < 0 || e.raceTime < best;
    if (pb) setBestTime(key, e.raceTime);
    const bool save = g_settings.onFinish == 2 || (g_settings.onFinish == 1 && pb);
    const std::string time = formatTime(e.raceTime);
    char line[512];
    snprintf(line, sizeof(line), "clips: finish %s on \"%s\" (%s): %s", time.c_str(), map.c_str(),
             best < 0 ? "no best saved before" : (pb ? ("best was " + formatTime(best)).c_str() : ("best " + formatTime(best)).c_str()),
             !save ? (g_settings.onFinish == 0 ? "not saved (Save on finish is off)" : "not saved (not a personal best)")
                   : (recording() ? "saving the run" : "not saved (not recording)"));
    TMVS_LOG("%s", line);
    if (!save || !recording()) return;
    SaveJob job;
    job.from = static_cast<int64_t>((e.runStart - g_settings.before) * 1e7);
    job.to = static_cast<int64_t>((e.finishAt + g_settings.after) * 1e7);
    job.what = time;
    job.path = clipPath(map, time);
    queueSave(e.finishAt + g_settings.after, job);
}

void saveLast() {
    if (!recording()) {
        TMVS_LOG("clips: save key: not recording (%s)", g_status.c_str());
        showToast("Not recording", false);
        return;
    }
    const double now = seconds();
    SaveJob job;
    job.from = static_cast<int64_t>((now - g_settings.keySeconds) * 1e7);
    job.to = static_cast<int64_t>(now * 1e7);
    char what[32];
    snprintf(what, sizeof(what), "last %d s", g_settings.keySeconds);
    job.what = what;
    job.path = clipPath(mapName(), std::string());
    TMVS_LOG("clips: save key: saving the last %d s", g_settings.keySeconds);
    queueSave(now, job);
}

// The settings file changed (the other build, or by hand), the panel's edits saved.
void syncSettings(double now) {
    static double s_checked = 0.0;
    if (g_lastEdit >= 0.0 && now - g_lastEdit > 0.5) {
        save(g_settings);
        g_saved = g_settings;
        g_lastEdit = -1.0;
        return;
    }
    if (now - s_checked < 2.0) return;
    s_checked = now;
    if (g_lastEdit < 0.0 && changedOnDisk()) {
        load(g_settings);
        g_saved = g_settings;
        TMVS_LOG("clips: settings read again (clips.ini changed)");
    }
}

class KeyEdge {
public:
    bool pressed(const std::string& binding) {
        keys::Binding b;
        if (!keys::parse(binding.c_str(), b) || !b.vk) return false;
        const bool down = (GetAsyncKeyState(static_cast<int>(b.vk)) & 0x8000) != 0;
        const bool fire = down && !m_down && gameInFront() && modsHeld() == b.mods;
        m_down = down;
        return fire;
    }

private:
    bool m_down = false;
};
KeyEdge g_saveKey;

void logStats(double now) {
    if (!recording() || now - g_cost.since < 60.0) return;
    const RecorderStats r = g_recorder->stats();
    static RecorderStats s_last;
    const int in = r.framesIn - s_last.framesIn, encoded = r.framesEncoded - s_last.framesEncoded;
    const double encodeMs = encoded > 0 ? (r.encodeMs - s_last.encodeMs) / encoded : 0.0;
    TMVS_LOG("clips: last %.0f s: %d frames taken, %d left out (read-back busy), %d dropped (encoder behind); game thread %.2f ms a frame "
             "taken (copy and convert %.2f, read back %.2f, slowest %.2f); encoder %.2f ms a frame (slowest %.1f); buffer %.0f s, %.1f MB",
             now - g_cost.since, g_cost.grabbed, g_cost.skipped, r.framesDropped - s_last.framesDropped,
             g_cost.grabbed ? (g_cost.grabMs + g_cost.collectMs) / g_cost.grabbed : 0.0, g_cost.grabbed ? g_cost.grabMs / g_cost.grabbed : 0.0,
             g_cost.grabbed ? g_cost.collectMs / g_cost.grabbed : 0.0, g_cost.maxMs, encodeMs, r.encodeMaxMs, r.bufferSeconds,
             r.bufferBytes / 1048576.0);
    (void)in;
    s_last = r;
    g_cost = Cost();
    g_cost.since = now;
}

void takeResults() {
    SaveResult r;
    while (g_recorder && g_recorder->takeResult(r)) {
        g_reserved.erase(r.path);
        TMVS_LOG("clips: %s", r.line.c_str());
        if (!g_settings.toast) continue;
        if (r.ok) showToast("Clip saved " + (r.what.rfind("last", 0) == 0 ? std::string("(") + r.what + ")" : r.what), true);
        else showToast("Clip not saved", false);
    }
}

// The MP4s a save was writing when the game ended (*.part.mp4) are deleted: the ones not written to
// for 5 minutes (another game running may be writing one now).
void removeUnfinished() {
    const std::wstring dir = clipsDir();
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW((dir + L"\\*.part.mp4").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    FILETIME nowFile;
    GetSystemTimeAsFileTime(&nowFile);
    const ULONGLONG now = (static_cast<ULONGLONG>(nowFile.dwHighDateTime) << 32) | nowFile.dwLowDateTime;
    int removed = 0;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const ULONGLONG written = (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
        if (now > written && now - written < 5ull * 60 * 10000000) continue;
        if (DeleteFileW((dir + L"\\" + data.cFileName).c_str())) removed++;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    if (removed) TMVS_LOG("clips: %d unfinished clip%s deleted (the game ended while saving)", removed, removed == 1 ? "" : "s");
}

std::wstring beatName() {
    return mutexName(L"Beat");
}

// The standalone: may it record? Not while TM Vibrant Shaders' clips run in this game (their
// counter moves); when TM Vibrant Shaders is loaded, not before its first beat either (no time in
// which both record). It records again when TM Vibrant Shaders' clips stop: 300 usable frames of
// ours without a beat (a lost device stops both, those frames don't count).
bool standaloneMayRecord(IDirect3DDevice9* device) {
    if (!g_beat) {
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, beatName().c_str());
        if (mapping) {
            g_beat = static_cast<volatile LONG*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(LONG))); // kept for the process's life
            if (!g_beat) CloseHandle(mapping);
        }
        if (!g_beat) return true;
    }
    if (!g_hand.announced) {
        g_hand.announced = true;
        TMVS_LOG("clips: TM Vibrant Shaders is loaded: TM Clips waits for its clips and stays idle while they run");
    }
    if (device->TestCooperativeLevel() != D3D_OK) return false;
    const LONG v = *g_beat;
    if (v != g_hand.last) {
        g_hand.last = v;
        g_hand.seen = true;
        g_hand.framesWithout = 0;
    } else if (g_hand.framesWithout < 1000000) {
        g_hand.framesWithout++;
    }
    const bool runs = g_hand.seen ? g_hand.framesWithout < 300 : g_hand.framesWithout < 600;
    if (runs != g_hand.tmvsRuns) {
        g_hand.tmvsRuns = runs;
        if (runs) TMVS_LOG("clips: TM Vibrant Shaders records the clips: TM Clips stays idle");
        else TMVS_LOG("clips: TM Vibrant Shaders is loaded but its clips don't run (%d frames without them): TM Clips records", g_hand.framesWithout);
    }
    return !runs;
}

} // namespace

void boot(Host host) {
    if (g_booted) return;
    g_booted = true;
    g_host = host;
    if (load(g_settings))
        TMVS_LOG("clips: buffer shortened to %d s: at %d Mbit/s a longer one would take more than %.0f MB", g_settings.bufferSeconds,
                 g_settings.bitrate, kMemoryLimit / 1048576.0);
    g_saved = g_settings;
    if (!save(g_settings)) TMVS_LOG("clips: can't write %s", narrow(dataDir() + L"\\clips.ini").c_str());
    removeUnfinished();
    g_recorder = new Recorder();
    if (host == Host::Tmvs) {
        g_hostMutex = CreateMutexW(nullptr, FALSE, mutexName(L"Host").c_str()); // kept for the life of the process
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, beatName().c_str());
        if (mapping) g_beat = static_cast<volatile LONG*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(LONG))); // kept for the life of the process
    }
    const char* build = raceBuild();
    TMVS_LOG("clips: %s records the clips (settings %s; clips into %s); %s; save key %s, save on finish %s", hostName(),
             narrow(dataDir() + L"\\clips.ini").c_str(), narrow(clipsDir()).c_str(),
             build ? (std::string("race read from TrackMania ") + build).c_str() : "this game's race is unknown: only the save key works",
             g_settings.saveKey.c_str(), g_settings.onFinish == 2 ? "every finish" : g_settings.onFinish == 1 ? "personal best only" : "off");
    if (host == Host::Tmvs) {
        keys::Binding b;
        if (keys::parse(g_settings.saveKey.c_str(), b)) {
            for (int a = 0; a < static_cast<int>(keys::Action::Count); a++) {
                const keys::Action action = static_cast<keys::Action>(a);
                if (keys::info(action).available && keys::get(action) == b)
                    TMVS_LOG("clips: the save key %s is also the menu's %s: change one of them", g_settings.saveKey.c_str(), keys::info(action).label);
            }
        }
    }
}

bool tmvsRecords() {
    return mutexExists(L"Host");
}

bool idle() {
    return g_idle;
}

void setPaused(bool paused) {
    g_paused = paused;
}

void setInternalCalls(void (*begin)(), void (*end)()) {
    Grabber::setInternalCalls(begin, end);
}

bool wantsScene() {
    return g_host == Host::Tmvs && g_settings.noHud && g_active.have && !g_paused && !g_idle;
}

void sceneDone(IDirect3DDevice9* device, IDirect3DSurface9* target) {
    if (!wantsScene() || !recording() || !target) return;
    g_sceneThisFrame = true;
    // Only for a frame that will be taken (the same test as present()'s, a quarter frame early).
    const double now = seconds();
    const double period = 1.0 / std::max(1, g_settings.fps);
    if (now + period * 0.25 < g_nextDue) return;
    g_grabber.copy(device, target, g_active.width, g_active.height);
}

void present(IDirect3DDevice9* device) {
    if (!g_booted || !device) return;
    g_frames++;
    const double now = seconds();
    if (g_host == Host::Tmvs && g_beat) InterlockedIncrement(g_beat); // the standalone sees our clips run
    // The standalone: TM Vibrant Shaders (with clips) came along: it records, we stop (and record
    // again should its clips stop).
    if (g_host == Host::Standalone) {
        const bool may = standaloneMayRecord(device);
        if (!may && !g_idle) {
            g_idle = true;
            stopRecording("TM Vibrant Shaders records the clips");
        } else if (may && g_idle) {
            g_idle = false;
        }
    }
    if (g_idle) {
        takeResults(); // a save the stopped recording still writes
        return;
    }
    // Another device (the game made a new one): what belongs to the old one is left behind.
    if (g_lastDevice && device != g_lastDevice) {
        TMVS_LOG("clips: the game's device changed (%p, was %p): recording starts over", static_cast<void*>(device), static_cast<void*>(g_lastDevice));
        g_grabber.abandonDevice(); // first: stopRecording would release them on the old device
        stopRecording("the device changed");
    }
    g_lastDevice = device;
    if (g_host == Host::Tmvs && g_frames == 1 && mutexExists(L"Standalone"))
        TMVS_LOG("clips: TM Clips (the standalone mod) is loaded too: it stays idle, TM Vibrant Shaders records");
    syncSettings(now);
    takeResults();
    if (g_settings.enabled && g_saveKey.pressed(g_settings.saveKey)) saveLast();
    // The race, for the runs' clips (read even before the recorder is ready: a run's start counts).
    {
        RaceSample s;
        readRace(s);
        const RunEvent e = g_tracker.update(now, s);
        if (e.kind == RunEvent::Finish && g_settings.enabled) onFinish(e);
    }
    // Saves whose span has ended go to the recorder.
    for (size_t i = 0; i < g_pending.size();) {
        if (now >= g_pending[i].at) {
            if (recording()) {
                g_recorder->save(g_pending[i].job);
            } else {
                g_reserved.erase(g_pending[i].job.path);
                TMVS_LOG("clips: a save was dropped: recording stopped meanwhile");
            }
            g_pending.erase(g_pending.begin() + static_cast<long>(i));
        } else {
            i++;
        }
    }
    logStats(now);

    if (!g_settings.enabled) {
        g_status = "Off";
        stopRecording("switched off");
        return;
    }
    if (device->TestCooperativeLevel() != D3D_OK) return;
    IDirect3DSurface9* back = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return;
    D3DSURFACE_DESC desc{};
    back->GetDesc(&desc);
    int w = 0, h = 0;
    videoSize(desc.Width, desc.Height, w, h);
    if (w < 64 || h < 64) {
        back->Release();
        return;
    }
    const bool differs = !g_active.have || g_active.width != w || g_active.height != h || !g_active.settings.sameRecording(g_settings);
    if (differs && (g_lastEdit < 0.0 || now - g_lastEdit > 1.0)) {
        if (g_active.have) TMVS_LOG("clips: recording again (%dx%d, the size or a setting changed)", w, h);
        startRecording(w, h);
    }
    switch (g_recorder->state()) {
    case Recorder::State::Starting:
        g_status = "Starting";
        break;
    case Recorder::State::Failed:
        if (!g_active.failLogged) {
            g_active.failLogged = true;
            TMVS_LOG("clips: can't record: %s", g_recorder->failure().c_str());
        }
        g_status = "Can't record: " + g_recorder->failure();
        break;
    case Recorder::State::Running: {
        if (!g_active.startLogged) {
            g_active.startLogged = true;
            const RecorderStats limits = g_recorder->stats();
            TMVS_LOG("clips: recording %dx%d (the game's %ux%u) at %d fps, %d Mbit/s, into a %d s buffer (about %.0f MB, at most %.0f)%s: %s", w, h,
                     desc.Width, desc.Height, g_settings.fps, g_settings.bitrate, g_settings.bufferSeconds, memoryEstimate(g_settings) / 1048576.0,
                     (limits.bufferLimit + limits.soundBytes) / 1048576.0, wantsScene() ? ", without the HUD" : "", g_recorder->description().c_str());
        }
        char text[128];
        const RecorderStats st = g_recorder->stats();
        snprintf(text, sizeof(text), "Recording %dx%d, %.0f s in memory (%.0f MB)", w, h, st.bufferSeconds, st.bufferBytes / 1048576.0);
        g_status = text;
        // Frames back from the card go to the encoder.
        const int64_t c0 = now100ns();
        g_grabber.collect([](const PlaneReadback::Frame& f, int64_t time) {
            uint8_t* dst = g_recorder->acquire();
            if (!dst) return;
            PlaneReadback::pack(f, dst);
            g_recorder->submit(time);
        });
        const double collectMs = static_cast<double>(now100ns() - c0) / 1e4;
        // This frame, if it is due.
        const double period = 1.0 / std::max(1, g_settings.fps);
        // "Without HUD": a frame whose scene came without a copy (it wasn't due yet then) is left out
        // rather than taken with its HUD; a frame without a scene (the menus) is taken as it is.
        const bool scene = wantsScene() && g_grabber.fresh();
        const bool sceneMissed = wantsScene() && g_sceneThisFrame && !scene;
        g_sceneThisFrame = false;
        if (!g_paused && !sceneMissed && now + period * 0.25 >= g_nextDue) {
            g_nextDue += period;
            if (g_nextDue < now - period) g_nextDue = now;
            if (g_grabber.grab(device, scene ? nullptr : back, w, h, static_cast<int64_t>(now * 1e7))) {
                g_cost.grabbed++;
                g_cost.grabMs += g_grabber.lastGrabMs;
                g_cost.collectMs += collectMs;
                g_cost.maxMs = std::max(g_cost.maxMs, g_grabber.lastGrabMs + collectMs);
            } else {
                g_cost.skipped++;
                if (!g_grabber.lastError.empty()) {
                    TMVS_LOG("clips: %s", g_grabber.lastError.c_str());
                    g_grabber.lastError.clear();
                }
            }
        }
        if (g_paused) g_status = "Paused (video export or photo)";
        break;
    }
    case Recorder::State::Off:
        break;
    }
    back->Release();
}

void preReset() {
    g_grabber.releaseDevice();
}

void postReset() {}

void shutdown() {
    // Nothing is joined here: at process exit the threads are gone already, and the DLL is never
    // unloaded while the game runs. A clip being written then stays as *.part.mp4 (not listed);
    // the next boot deletes it (removeUnfinished).
}

// --- UI ----------------------------------------------------------------------------------------

namespace {

bool g_capturingKey = false;
bool g_keyWasDown[256] = {};

// The next key pressed (with its modifiers), for "Change". Esc cancels.
bool captureKey(std::string& out) {
    for (int vk = 8; vk < 256; vk++) {
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        const bool edge = down && !g_keyWasDown[vk];
        g_keyWasDown[vk] = down;
        if (!edge || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL ||
            vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN)
            continue;
        if (vk == VK_ESCAPE) {
            g_capturingKey = false;
            return false;
        }
        keys::Binding b;
        b.vk = static_cast<unsigned>(vk);
        b.mods = modsHeld();
        const std::string name = keys::name(b);
        keys::Binding check;
        if (name.empty() || !keys::parse(name.c_str(), check)) continue;
        // Letters, digits and the game's own keys need Ctrl or Alt (as the menu's hotkeys).
        const bool plain = (vk >= VK_F1 && vk <= VK_F24) || (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) || vk == VK_INSERT || vk == VK_DELETE ||
                           vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_PAUSE || vk == VK_SCROLL;
        if (!plain && !(b.mods & (keys::kCtrl | keys::kAlt))) continue;
        out = name;
        g_capturingKey = false;
        return true;
    }
    return false;
}

void edited() {
    g_lastEdit = seconds();
}

struct ClipFile {
    std::wstring name;
    ULONGLONG size = 0;
    FILETIME time{};
};

std::vector<ClipFile> listClips() {
    std::vector<ClipFile> out;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW((clipsDir() + L"\\*.mp4").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring name = data.cFileName;
        if (name.size() > 9 && name.compare(name.size() - 9, 9, L".part.mp4") == 0) continue;
        ClipFile f;
        f.name = name;
        f.size = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        f.time = data.ftLastWriteTime;
        out.push_back(f);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    std::sort(out.begin(), out.end(), [](const ClipFile& a, const ClipFile& b) { return CompareFileTime(&a.time, &b.time) > 0; });
    return out;
}

} // namespace

void drawPanel() {
    Settings& s = g_settings;
    if (g_idle) {
        ImGui::TextWrapped("TM Vibrant Shaders records the clips: its menu (Video page) has these settings.");
        return;
    }
    ImGui::TextDisabled("%s", g_status.c_str());
    if (ImGui::Checkbox("Record clips", &s.enabled)) edited();
    ImGui::BeginDisabled(!s.enabled);
    const char* finishItems[] = {"Off", "Personal best", "Every finish"};
    if (ImGui::Combo("Save on finish", &s.onFinish, finishItems, 3)) edited();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Saves the run from %.0f s before the start to %.0f s after the finish.", s.before, s.after);
    if (ImGui::SliderFloat("Before start", &s.before, 0.0f, 10.0f, "%.1f s")) edited();
    if (ImGui::SliderFloat("After finish", &s.after, 0.0f, 10.0f, "%.1f s")) edited();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Save key: %s", s.saveKey.c_str());
    ImGui::SameLine();
    if (g_capturingKey) {
        std::string key;
        ImGui::TextDisabled("press a key (Esc: cancel)");
        if (captureKey(key)) {
            s.saveKey = key;
            edited();
        }
    } else if (ImGui::SmallButton("Change##clipkey")) {
        g_capturingKey = true;
        for (int vk = 0; vk < 256; vk++) g_keyWasDown[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
    if (ImGui::SliderInt("Key saves", &s.keySeconds, 5, std::max(10, s.bufferSeconds), "last %d s")) edited();
    // The buffer's length is held to what fits the memory limit at the bitrate (the game is 32-bit).
    const int longest = std::min(300, maxBufferSeconds(s.bitrate, s.sound));
    if (ImGui::SliderInt("Buffer", &s.bufferSeconds, 10, longest, "%d s")) {
        s.bufferSeconds = std::clamp(s.bufferSeconds, 10, longest);
        edited();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("about %.0f MB", memoryEstimate(s) / 1048576.0);
    if (longest < 300) ImGui::TextDisabled("At most %d s at %d Mbit/s (memory)", longest, s.bitrate);
    const char* sizeItems[] = {"Game size", "720p", "1080p", "1440p"};
    int size = s.height == 0 ? 0 : s.height <= 720 ? 1 : s.height <= 1080 ? 2 : 3;
    if (ImGui::Combo("Size", &size, sizeItems, 4)) {
        const int heights[] = {0, 720, 1080, 1440};
        s.height = heights[size];
        edited();
    }
    int rate = s.fps >= 60 ? 1 : 0;
    const char* rateItems[] = {"30 fps", "60 fps"};
    if (ImGui::Combo("Frame rate", &rate, rateItems, 2)) {
        s.fps = rate ? 60 : 30;
        edited();
    }
    if (ImGui::SliderInt("Bitrate", &s.bitrate, 4, 50, "%d Mbit/s")) {
        clampBuffer(s); // a higher bitrate shortens the buffer
        edited();
    }
    const char* encoderItems[] = {"Auto", "NVIDIA", "Media Foundation"};
    if (ImGui::Combo("Encoder", &s.encoder, encoderItems, 3)) edited();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Auto: NVIDIA's encoder, else the graphics card's through Windows.");
    if (ImGui::Checkbox("Sound", &s.sound)) {
        clampBuffer(s);
        edited();
    }
    if (g_host == Host::Tmvs) {
        ImGui::SameLine();
        if (ImGui::Checkbox("Without HUD", &s.noHud)) edited();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Saved message", &s.toast)) edited();
    ImGui::EndDisabled();
    if (ImGui::Button("Save now")) saveLast();
    ImGui::SameLine();
    if (ImGui::Button("Open folder")) ShellExecuteW(nullptr, L"open", clipsDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    // The last clips.
    static std::vector<ClipFile> s_files;
    static double s_listed = -10.0;
    if (seconds() - s_listed > 2.0) {
        s_files = listClips();
        s_listed = seconds();
    }
    if (!s_files.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Last clips");
        const size_t shown = std::min<size_t>(s_files.size(), 6);
        for (size_t i = 0; i < shown; i++) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("Play")) ShellExecuteW(nullptr, L"open", (clipsDir() + L"\\" + s_files[i].name).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            ImGui::SameLine();
            ImGui::Text("%s  (%.1f MB)", narrow(s_files[i].name).c_str(), s_files[i].size / 1048576.0);
            ImGui::PopID();
        }
    }
}

std::string menuKey() {
    return g_settings.menuKey;
}

bool toastActive() {
    return !g_toast.text.empty() && seconds() - g_toast.start < 3.0;
}

void drawToast() {
    if (!toastActive()) return;
    const double age = seconds() - g_toast.start;
    const float alpha = age < 2.0 ? 1.0f : static_cast<float>(std::max(0.0, 3.0 - age));
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.08f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.65f * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::Begin("##clipToast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    if (g_toast.ok) ImGui::TextUnformatted(g_toast.text.c_str());
    else ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", g_toast.text.c_str());
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace clips
} // namespace tmshaders
