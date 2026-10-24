#include "misc/internal.h"
#include "engine.h"
#include "log.h"
#include "menu.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

// Split times: at each checkpoint, in the moment the game registers it (the race's own list of
// checkpoint times, engine::raceTimes), the time and the car's speed there against a reference: the
// map's record from TMX (fetched by itself once per map), the ghost you race against (the race's
// fastest opponent ghost, engine::raceGhostTimes), your best run on this map, or a replay file. A
// sector clock runs between checkpoints (time against time). The splits stay until the run is
// restarted; after the finish each sector's time, and the Session panel: attempts, best, median and
// last times, the theoretical best from your best sectors, the spread of your sectors, respawns and
// each sector's top and average speed. Nothing says where a checkpoint is.
namespace tmshaders {
namespace misc {
namespace splits {
namespace {

constexpr int kVisibleRows = 5;    // checkpoint rows in view (the rest scroll)
constexpr size_t kMaxRows = 200;   // rows kept in the table
constexpr float kFontScale = 0.9f; // the panels' text
constexpr float kBigScale = 1.5f;  // the delta

struct ReplayFile {
    std::wstring path;
    std::string uid, driver;
    int time = -1;
};

struct Reference {
    const char* label = "";
    std::vector<int> times;    // race time at each checkpoint, the finish last
    std::vector<float> speeds; // m/s at each (-1: unknown)
    std::string note;          // why another one is used ("" none)
};

struct Checkpoint {
    int time = 0;
    float speed = -1.0f; // m/s, -1 unknown
};

struct SectorSpeed {
    float top = 0.0f, sum = 0.0f, seconds = 0.0f; // m/s; sum of speed x time
};

struct Run {
    std::vector<Checkpoint> cps;
    std::vector<SectorSpeed> sectors; // by sector (the checkpoints passed before it)
    int respawnsAtStart = 0, respawns = 0;
    bool started = false, finished = false;
};

struct State {
    const void* map = nullptr;
    std::string uid;
    // Your best run on this map (its file): times and speeds at each checkpoint.
    std::vector<int> best;
    std::vector<float> bestSpeeds;
    // Your best time of each sector from everything of yours on this map: your best run, every finished
    // run, your replays in Tracks\Replays (its file's third line, so it outlives the session).
    std::vector<int> bestSectors;
    // The map's record (TMX) and a replay file you chose: two slots, each with a line for the page.
    Replay record, file;
    std::wstring filePath;
    std::string recordNote, fileNote;
    std::wstring triedRecord; // the record file loaded (or failed) last: not read again every frame
    // The run.
    const void* race = nullptr;
    int lastPassed = 0, lastTime = 0;
    Run run;
    Reference finishedReference; // the reference when the run finished
    bool finishedIsBest = false;
    // The last checkpoint's comparison (stays until the run restarts).
    bool haveDelta = false;
    int delta = 0, deltaIndex = 0;
    float speedDelta = 0.0f;
    bool haveSpeedDelta = false;
    // For the finish effect.
    bool finishPending = false;
    int finishTime = 0;
    bool finishBest = false;
    // Speed from the car's positions (when the engine's isn't readable) and the frame clock.
    float measured = 0.0f, lastPos[3] = {};
    bool havePos = false;
    double lastPosTime = 0.0, lastFrameTime = 0.0; // s (now())
    // The race times read this frame.
    engine::RaceTimes times;
    bool timesValid = false;
    // This map's session.
    int attempts = 0;
    std::vector<int> finishes;
    std::vector<std::vector<int>> sectorTimes; // each finished run's sector times
    Run lastFinished;
};
State g;

// Local replays: the folders read on a thread (thousands of files: only their headers).
std::mutex g_listLock;
std::vector<ReplayFile> g_list;
std::atomic<bool> g_scanning{false};
bool g_scanned = false;

// Your replays of the map loaded: their sectors, read on a thread once per map after the scan.
struct SectorsJob {
    std::string uid;
    std::vector<int> sectors; // the best of each sector
    int files = 0;
    bool ready = false;
};
std::mutex g_jobLock;
SectorsJob g_job;
std::atomic<bool> g_jobRunning{false};
std::string g_jobUid; // the map the job ran (or runs) for

double now() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<double>(t.QuadPart) / static_cast<double>(f.QuadPart);
}

std::wstring trackManiaFolder() {
    std::wstring dir = log::dataDir(); // Documents\TrackMania\TMVS
    const size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? dir : dir.substr(0, slash);
}

void scanFolder(const std::wstring& folder, int depth, std::vector<ReplayFile>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring path = folder + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0) scanFolder(path, depth - 1, out);
        } else if (name.size() > 11 && _wcsicmp(name.c_str() + name.size() - 11, L".Replay.Gbx") == 0) {
            Replay r;
            std::string error;
            if (!readReplayHeader(path, r, error)) continue;
            ReplayFile f;
            f.path = path;
            f.uid = r.mapUid;
            f.driver = r.driver;
            f.time = r.raceTime;
            out.push_back(f);
        }
    } while (FindNextFileW(h, &fd) && out.size() < 50000);
    FindClose(h);
}

void startScan() {
    if (g_scanning.exchange(true)) return;
    g_scanned = true;
    std::thread([] {
        std::vector<ReplayFile> found;
        scanFolder(trackManiaFolder() + L"\\Tracks\\Replays", 3, found);
        scanFolder(dataFolder(L"Replays"), 0, found);
        TMVS_LOG("misc: split times: %zu replays found", found.size());
        std::lock_guard<std::mutex> lock(g_listLock);
        g_list.swap(found);
        g_scanning = false;
    }).detach();
}

std::wstring bestFile(const std::string& uid) {
    std::wstring name;
    for (char c : uid) name += (isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? static_cast<wchar_t>(c) : L'_';
    return dataFolder(L"splits") + L"\\" + name + L".txt";
}

// The file: your best run's times (ms) on the first line, its speeds (km/h x 10, -1 unknown) on the
// second, your best time of each sector (ms, from all your runs and replays) on the third.
void loadBest() {
    g.best.clear();
    g.bestSpeeds.clear();
    g.bestSectors.clear();
    if (g.uid.empty()) return;
    FILE* f = _wfopen(bestFile(g.uid).c_str(), L"r");
    if (!f) return;
    char line[16384];
    for (int row = 0; row < 3 && fgets(line, sizeof(line), f); row++) {
        const char* p = line;
        char* end = nullptr;
        for (long v = strtol(p, &end, 10); end != p && g.best.size() < 4096 && g.bestSectors.size() < 4096; v = strtol(p, &end, 10)) {
            if (row == 0) g.best.push_back(static_cast<int>(v));
            else if (row == 1) g.bestSpeeds.push_back(v < 0 ? -1.0f : static_cast<float>(v) / 36.0f);
            else g.bestSectors.push_back(static_cast<int>(v));
            p = end;
        }
    }
    fclose(f);
    if (g.bestSpeeds.size() != g.best.size()) g.bestSpeeds.assign(g.best.size(), -1.0f);
}

void saveBest() {
    FILE* f = _wfopen(bestFile(g.uid).c_str(), L"w");
    if (!f) return;
    for (size_t i = 0; i < g.best.size(); i++) fprintf(f, "%s%d", i ? " " : "", g.best[i]);
    fprintf(f, "\n");
    for (size_t i = 0; i < g.bestSpeeds.size(); i++) fprintf(f, "%s%d", i ? " " : "", g.bestSpeeds[i] < 0.0f ? -1 : static_cast<int>(g.bestSpeeds[i] * 36.0f + 0.5f));
    fprintf(f, "\n");
    for (size_t i = 0; i < g.bestSectors.size(); i++) fprintf(f, "%s%d", i ? " " : "", g.bestSectors[i]);
    fprintf(f, "\n");
    fclose(f);
}

void timeText(char* out, size_t size, int ms) {
    const int a = ms < 0 ? -ms : ms;
    if (a >= 60000) snprintf(out, size, "%d:%02d.%03d", a / 60000, a / 1000 % 60, a % 1000);
    else snprintf(out, size, "%d.%03d", a / 1000, a % 1000);
}

void deltaText(char* out, size_t size, int ms) {
    char t[32];
    timeText(t, sizeof(t), ms);
    snprintf(out, size, "%c%s", ms > 0 ? '+' : (ms < 0 ? '-' : ' '), t);
}

ImVec4 deltaColour(int ms) {
    return ms > 0 ? ImVec4(1.0f, 0.38f, 0.32f, 1.0f) : (ms < 0 ? ImVec4(0.35f, 0.75f, 1.0f, 1.0f) : ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
}

// Faster is better: blue for more speed than the reference.
ImVec4 speedColour(float kmh) {
    return kmh < -0.5f ? ImVec4(1.0f, 0.38f, 0.32f, 1.0f) : (kmh > 0.5f ? ImVec4(0.35f, 0.75f, 1.0f, 1.0f) : ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
}

bool loadReplay(const std::wstring& path, bool record) {
    Replay r;
    std::string error;
    std::string& note = record ? g.recordNote : g.fileNote;
    if (!readReplay(path, r, error)) {
        note = "can't read it: " + error;
        return false;
    }
    if (r.mapUid != g.uid) {
        note = "another map's replay";
        return false;
    }
    char line[160], t[32];
    timeText(t, sizeof(t), r.raceTime);
    snprintf(line, sizeof(line), "%s, %s", r.driver.empty() ? "?" : r.driver.c_str(), t);
    note = line;
    TMVS_LOG("misc: split times: %s loaded (%s, %zu checkpoints, %s)", record ? "the record" : "a replay", line, r.checkpoints.size(),
             r.speeds.empty() ? "no speeds" : "with speeds");
    if (record) {
        g.record = r;
    } else {
        g.file = r;
        g.filePath = path;
    }
    return true;
}

Reference fromBest(const char* note) {
    Reference r;
    r.label = "best";
    r.times = g.best;
    r.speeds = g.bestSpeeds;
    r.note = note ? note : "";
    return r;
}

Reference fromReplay(const Replay& replay, const char* label) {
    Reference r;
    r.label = label;
    r.times = replay.checkpoints;
    for (int t : r.times) r.speeds.push_back(speedAt(replay, t));
    return r;
}

// What the run is compared with now.
Reference reference() {
    switch (settings().splitCompare) {
    case kCompareRecord:
        if (!g.record.checkpoints.empty()) return fromReplay(g.record, "record");
        return fromBest(tmx::knownMissing(g.uid) ? "not on TMX" : (tmx::status().state == tmx::State::Busy ? "fetching the record" : (g.recordNote.empty() ? "no record yet" : "record not readable")));
    case kCompareGhost: {
        Reference r;
        int time = -1;
        if (engine::raceGhostTimes(r.times, time)) {
            r.label = "ghost";
            r.speeds.assign(r.times.size(), -1.0f);
            return r;
        }
        return fromBest("no ghost in the race");
    }
    case kCompareReplay:
        if (!g.file.checkpoints.empty()) return fromReplay(g.file, "replay");
        return fromBest("no replay chosen");
    default:
        return fromBest(nullptr);
    }
}

std::vector<int> sectorsOf(const std::vector<int>& times);

// Your best sectors take any faster one from `sectors` (a run or replay of this map with as many
// checkpoints; another count is another layout and is left out). Saved when one got faster.
void mergeSectors(const std::vector<int>& sectors, const char* from) {
    if (sectors.empty() || g.uid.empty()) return;
    bool better = false;
    if (g.bestSectors.size() != sectors.size()) {
        // Another count: taken only while there is none yet or it matches your best run's.
        if (!g.bestSectors.empty() && (g.best.empty() || g.best.size() != sectors.size())) return;
        g.bestSectors = sectors;
        better = true;
    } else {
        for (size_t i = 0; i < sectors.size(); i++) {
            if (sectors[i] > 0 && sectors[i] < g.bestSectors[i]) {
                g.bestSectors[i] = sectors[i];
                better = true;
            }
        }
    }
    if (better) {
        saveBest();
        int sum = 0;
        for (int v : g.bestSectors) sum += v;
        TMVS_LOG("misc: split times: your best sectors now add up to %d ms (from %s)", sum, from);
    }
}

// Reads your replays of this map (not the downloaded records) on a thread: the best of each sector
// among those with the most common checkpoint count. Yours: driven by a name found in the game's
// Autosaves folder (only your own runs land there), so someone else's replay you saved does not
// count as yours; without any autosave, every replay of the map counts.
void startSectorsJob() {
    if (g_jobRunning || g_scanning || !g_scanned || g.uid.empty() || g_jobUid == g.uid) return;
    g_jobUid = g.uid;
    const std::wstring downloads = dataFolder(L"Replays");
    std::vector<std::wstring> paths;
    {
        std::lock_guard<std::mutex> lock(g_listLock);
        std::vector<std::string> mine;
        for (const ReplayFile& f : g_list) {
            if (f.path.find(L"\\Autosaves\\") != std::wstring::npos && !f.driver.empty() && std::find(mine.begin(), mine.end(), f.driver) == mine.end()) {
                mine.push_back(f.driver);
            }
        }
        for (const ReplayFile& f : g_list) {
            if (f.uid != g.uid || f.path.compare(0, downloads.size(), downloads) == 0) continue;
            if (!mine.empty() && std::find(mine.begin(), mine.end(), f.driver) == mine.end()) continue;
            paths.push_back(f.path);
        }
    }
    if (paths.empty()) return;
    g_jobRunning = true;
    std::thread([paths, uid = g.uid] {
        std::vector<std::vector<int>> all;
        for (const std::wstring& path : paths) {
            Replay r;
            std::string error;
            if (readReplay(path, r, error) && r.mapUid == uid) all.push_back(sectorsOf(r.checkpoints));
        }
        size_t common = 0, most = 0;
        for (const std::vector<int>& a : all) {
            size_t n = 0;
            for (const std::vector<int>& b : all) n += a.size() == b.size() ? 1 : 0;
            if (n > most) {
                most = n;
                common = a.size();
            }
        }
        SectorsJob job;
        job.uid = uid;
        for (const std::vector<int>& a : all) {
            if (a.size() != common) continue;
            job.files++;
            if (job.sectors.empty()) job.sectors = a;
            for (size_t i = 0; i < a.size(); i++) job.sectors[i] = std::min(job.sectors[i], a[i]);
        }
        job.ready = true;
        std::lock_guard<std::mutex> lock(g_jobLock);
        g_job = job;
        g_jobRunning = false;
    }).detach();
}

void newRun() {
    g.run = Run();
    g.run.respawnsAtStart = engine::respawns();
    g.haveDelta = false;
    g.haveSpeedDelta = false;
}

void newMap(const void* map) {
    g.map = map;
    g.uid = map ? engine::currentMapUid() : std::string();
    g.record = Replay();
    g.file = Replay();
    g.filePath.clear();
    g.recordNote.clear();
    g.fileNote.clear();
    g.triedRecord.clear();
    g.race = nullptr;
    g.lastPassed = g.lastTime = 0;
    newRun();
    g.finishedReference = Reference();
    g.attempts = 0;
    g.finishes.clear();
    g.sectorTimes.clear();
    g.lastFinished = Run();
    loadBest();
    mergeSectors(sectorsOf(g.best), "your best run");
    // The record downloaded before for this map comes back by itself (no request).
    const std::wstring cached = tmx::cached(g.uid);
    if (!cached.empty()) {
        g.triedRecord = cached;
        loadReplay(cached, true);
    }
}

// The car's speed now: the engine's (the physics' world speed), else from its positions.
float carSpeed() {
    float v = 0.0f;
    if (engine::carSpeed(v)) return v;
    engine::CarPose pose;
    const double t = now();
    if (!engine::carPose(pose)) {
        g.havePos = false;
        return -1.0f;
    }
    if (g.havePos) {
        const float dt = static_cast<float>(t - g.lastPosTime);
        if (dt > 0.02f) {
            const float d[3] = {pose.position[0] - g.lastPos[0], pose.position[1] - g.lastPos[1], pose.position[2] - g.lastPos[2]};
            const float m = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / dt;
            if (m < 300.0f) g.measured = g.measured * 0.6f + m * 0.4f; // a respawn's jump is no speed
        } else {
            return g.measured;
        }
    }
    memcpy(g.lastPos, pose.position, sizeof(g.lastPos));
    g.lastPosTime = t;
    g.havePos = true;
    return g.measured;
}

std::vector<int> sectorsOf(const std::vector<int>& times) {
    std::vector<int> out;
    for (size_t i = 0; i < times.size(); i++) out.push_back(times[i] - (i ? times[i - 1] : 0));
    return out;
}

int median(std::vector<int> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
}

// Your best time for each sector (all sources, see mergeSectors).
const std::vector<int>& bestSectors() {
    return g.bestSectors;
}

} // namespace

void update() {
    const void* map = engine::currentMap();
    if (map != g.map) newMap(map);
    // The record: fetched by itself (once per map, never twice; tmx.cpp throttles), loaded when there.
    if (settings().splits && settings().splitCompare == kCompareRecord && !g.uid.empty() && g.record.checkpoints.empty()) {
        tmx::want(g.uid);
        const tmx::Status t = tmx::status();
        if (t.state == tmx::State::Done && t.uid == g.uid && !t.file.empty() && t.file != g.triedRecord) {
            g.triedRecord = t.file;
            loadReplay(t.file, true);
        }
    }
    // Your replays of this map: their sectors count for your best sectors (once per map).
    if (settings().splits && !g.uid.empty()) {
        if (!g_scanned) startScan();
        startSectorsJob();
        SectorsJob job;
        {
            std::lock_guard<std::mutex> lock(g_jobLock);
            if (g_job.ready && g_job.uid == g.uid) {
                job = g_job;
                g_job.ready = false;
            }
        }
        if (job.ready && !job.sectors.empty()) {
            TMVS_LOG("misc: split times: %d of your replays of this map read for their sectors", job.files);
            mergeSectors(job.sectors, "your replays");
        }
    }
    const double clock = now();
    const float dt = g.lastFrameTime > 0.0 ? static_cast<float>(std::min(clock - g.lastFrameTime, 0.2)) : 0.0f;
    g.lastFrameTime = clock;
    g.timesValid = map && engine::raceTimes(g.times);
    if (!g.timesValid) return;
    const engine::RaceTimes& rt = g.times;
    // A restart (the clock or the count went back) or another race: a new run.
    if (rt.race != g.race || rt.passed < g.lastPassed || rt.time + 500 < g.lastTime) {
        g.race = rt.race;
        newRun();
        g.lastPassed = rt.passed;
    }
    Run& run = g.run;
    if (!run.started && rt.started && rt.time > 300) {
        run.started = true;
        g.attempts++;
    }
    const float speed = carSpeed();
    if (run.started && !run.finished && speed >= 0.0f && dt > 0.0f) {
        const size_t k = run.cps.size();
        if (run.sectors.size() <= k) run.sectors.resize(k + 1);
        SectorSpeed& sector = run.sectors[k];
        sector.top = std::max(sector.top, speed);
        sector.sum += speed * dt;
        sector.seconds += dt;
    }
    run.respawns = engine::respawns() - run.respawnsAtStart;
    while (g.lastPassed < rt.passed && !run.finished) {
        const int k = g.lastPassed++;
        int t = rt.count > 0 ? rt.times[k % rt.count] : -1;
        if (t < 0 || t > rt.time + 50) t = rt.time;
        Checkpoint cp;
        cp.time = t;
        cp.speed = speed;
        run.cps.push_back(cp);
        const Reference ref = reference();
        if (static_cast<size_t>(k) < ref.times.size()) {
            g.haveDelta = true;
            g.delta = t - ref.times[k];
            g.deltaIndex = k;
            g.haveSpeedDelta = speed >= 0.0f && static_cast<size_t>(k) < ref.speeds.size() && ref.speeds[k] >= 0.0f;
            g.speedDelta = g.haveSpeedDelta ? (speed - ref.speeds[k]) * 3.6f : 0.0f;
        }
    }
    g.lastPassed = rt.passed;
    if (rt.finished && !run.finished && !run.cps.empty()) {
        run.finished = true;
        if (run.cps.back().time != rt.time) {
            Checkpoint cp;
            cp.time = rt.time;
            cp.speed = speed;
            run.cps.push_back(cp);
        }
        std::vector<int> times;
        for (const Checkpoint& cp : run.cps) times.push_back(cp.time);
        const bool better = g.best.empty() || rt.time < g.best.back();
        g.finishedReference = reference();
        g.finishedIsBest = better;
        g.finishes.push_back(rt.time);
        g.sectorTimes.push_back(sectorsOf(times));
        mergeSectors(g.sectorTimes.back(), "this run");
        g.lastFinished = run;
        if (better && !g.uid.empty()) {
            g.best = times;
            g.bestSpeeds.clear();
            for (const Checkpoint& cp : run.cps) g.bestSpeeds.push_back(cp.speed);
            saveBest();
        }
        g.finishPending = true;
        g.finishTime = rt.time;
        g.finishBest = better;
        char t[32];
        timeText(t, sizeof(t), rt.time);
        TMVS_LOG("misc: split times: finished in %s after %zu checkpoints, %d respawns%s", t, run.cps.size(), run.respawns, better ? " (your best)" : "");
    }
    g.lastTime = rt.time;
}

bool takeFinish(int& time, bool& best) {
    if (!g.finishPending) return false;
    g.finishPending = false;
    time = g.finishTime;
    best = g.finishBest;
    return true;
}

bool wanted() {
    // Until the run restarts or the map changes, also once the race stopped checking its player.
    return settings().splits && g.map && (g.timesValid || !g.run.cps.empty());
}

namespace {

// Attached under the minimap, the split times take its width; a new width (the panels' edges) then
// goes to the minimap, so the column keeps one width. Otherwise they have their own.
bool widthFromMap() {
    ImVec2 a, b;
    return settings().splitAttached && mapview::rect(a, b);
}

void setWidth(float pixels) {
    Settings& s = settings();
    const float k = screenScale();
    if (widthFromMap()) s.mapSize = std::clamp(pixels / k, 120.0f, 800.0f);
    else s.splitWidth = std::clamp(pixels / k, 160.0f, 800.0f);
}

// A window let go of moved but kept its size: dragged away (it detaches). A changed size is a resize
// (an edge on the left moves the window too) and keeps it attached.
bool draggedAway(ImVec2 before, ImVec2 placed, ImVec2 size, float width) {
    return fabsf(size.x - width) <= 0.5f && (fabsf(placed.x - before.x) > 0.5f || fabsf(placed.y - before.y) > 0.5f);
}

void drawSplitPanel(const Frame& frame, ImVec2& shownPos, ImVec2& shownSize) {
    Settings& s = settings();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float k = screenScale(), margin = 16.0f * k;
    // Attached: right under the minimap with its width, or at the top right without it.
    ImVec2 mapPos, mapSize;
    float width = s.splitWidth * k;
    ImVec2 pos(display.x - width - margin, margin);
    if (s.splitAttached && mapview::rect(mapPos, mapSize)) {
        width = mapSize.x;
        pos = ImVec2(mapPos.x, mapPos.y + mapSize.y);
    }
    if (!s.splitAttached) {
        if (s.splitX < 0.0f) { // just unticked on the page: it stays where it is
            s.splitX = pos.x;
            s.splitY = pos.y;
        }
        pos = ImVec2(s.splitX, s.splitY);
    }
    // A fixed height: the reference line, the delta line, the sector clock and the table's header
    // (their height measured in the frame before, with the menu open and closed), then kVisibleRows
    // checkpoint rows that scroll (the newest at the bottom). Nothing in it changes the window's size.
    const PanelStyle panelStyle;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float r = textScale(), font = ImGui::GetFont()->FontSize * kFontScale * r;
    const float rowHeight = font + style.CellPadding.y * 2.0f, rowsHeight = kVisibleRows * rowHeight + 1.0f;
    const float frameHeight = font + style.FramePadding.y * 2.0f;
    static float s_top[2] = {};
    float& top = s_top[frame.menuOpen ? 1 : 0];
    if (top <= 0.0f) top = style.WindowPadding.y + 6.0f * rowHeight; // a guess for the first frame
    const float height = top + rowsHeight + style.WindowPadding.y;
    static WindowPlace s_place;
    s_place.lockHeight = true;
    s_place.before(frame, pos, ImVec2(width, height));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("Split times##misc", nullptr, windowFlags(frame, true));
    ImVec2 p, size;
    if (s_place.after(frame, p, size)) {
        if (draggedAway(pos, p, size, width)) s.splitAttached = false; // it stays where it was put
        else setWidth(size.x);
        if (!s.splitAttached) {
            s.splitX = p.x;
            s.splitY = p.y;
        }
        settingsChanged();
    }
    windowChrome(frame, "Split times", true);
    ImGui::SetWindowFontScale(kFontScale * textScale());
    const Reference ref = g.run.finished ? g.finishedReference : reference();
    const engine::RaceTimes& rt = g.times;
    const Run& run = g.run;
    char text[64], speedText[32];
    // 1. What it is compared with (and this run's respawns).
    if (ref.times.empty()) {
        ImGui::TextDisabled("No reference%s%s", ref.note.empty() ? "" : ": ", ref.note.c_str());
    } else {
        timeText(text, sizeof(text), ref.times.back());
        ImGui::TextDisabled("vs %s %s%s%s", ref.label, text, ref.note.empty() ? "" : " - ", ref.note.c_str());
    }
    if (run.respawns > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("  Respawns: %d", run.respawns);
    }
    // 2. The finish, or the last checkpoint's delta (big), until the run restarts.
    ImGui::SetWindowFontScale(kBigScale * textScale());
    if (run.finished) {
        timeText(text, sizeof(text), run.cps.back().time);
        ImGui::TextUnformatted(text);
        if (ref.times.size() >= run.cps.size() && !ref.times.empty()) {
            const int total = run.cps.back().time - ref.times[run.cps.size() - 1];
            deltaText(text, sizeof(text), total);
            ImGui::SameLine();
            ImGui::TextColored(deltaColour(total), "%s", text);
        }
        if (g.finishedIsBest) {
            ImGui::SetWindowFontScale(kFontScale * textScale());
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "best");
        }
    } else if (g.haveDelta) {
        deltaText(text, sizeof(text), g.delta);
        ImGui::TextColored(deltaColour(g.delta), "%s", text);
        ImGui::SetWindowFontScale(kFontScale * textScale());
        if (g.haveSpeedDelta) {
            ImGui::SameLine();
            ImGui::TextColored(speedColour(g.speedDelta), "%+.0f km/h", g.speedDelta);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("CP %d", g.deltaIndex + 1);
    } else {
        ImGui::TextDisabled(" ");
    }
    ImGui::SetWindowFontScale(kFontScale * textScale());
    // 3. The sector clock: time since the last checkpoint against the reference's sector (an empty
    //    line without one, so the height stays).
    const size_t passed = run.cps.size();
    if (s.splitBar && g.timesValid && rt.started && !rt.finished && passed < ref.times.size()) {
        const int start = passed ? run.cps[passed - 1].time : 0, refSector = ref.times[passed] - (passed ? ref.times[passed - 1] : 0);
        const int elapsed = std::max(0, rt.time - start);
        const float fraction = refSector > 0 ? static_cast<float>(elapsed) / static_cast<float>(refSector) : 0.0f;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, fraction <= 1.0f ? ImVec4(0.30f, 0.70f, 1.0f, 0.9f) : ImVec4(1.0f, 0.35f, 0.30f, 0.9f));
        snprintf(text, sizeof(text), "%.1f / %.1f s", elapsed / 1000.0f, refSector / 1000.0f);
        ImGui::ProgressBar(std::min(fraction, 1.0f), ImVec2(-1.0f, frameHeight), text);
        ImGui::PopStyleColor();
    } else {
        ImGui::Dummy(ImVec2(0.0f, frameHeight));
    }
    // 4. This run's checkpoints under a header that stays, kVisibleRows in view, scrolled to the newest
    //    when one is added (the wheel scrolls them while the menu is open). After the finish: each
    //    sector, its time, your best time of it and the reference's.
    //    The header is a table of its own above a child region with the rows (same column weights, no
    //    scrollbar so the widths match); the child takes no mouse with the menu closed, so it never
    //    keeps a click from the game.
    static size_t s_rows = 0;
    static const float kWeights[] = {0.8f, 1.2f, 1.2f, 1.2f};
    static const char* const kRunning[] = {"CP", "Time", "vs ref", "km/h"};
    static const char* const kRunningTips[] = {"Checkpoint", "Race time at the checkpoint", "Behind (red, plus) or ahead (blue, minus) of the reference",
                                               "Your speed at the checkpoint and the difference to the reference's"};
    static const char* const kFinished[] = {"Sector", "Time", "Best", "Ref"};
    static const char* const kFinishedTips[] = {"From one checkpoint to the next", "Your time in this sector, red or blue against the reference",
                                                "Your best time in this sector ever, blue where it beats the reference", "The reference's time in this sector"};
    const char* const* names = run.finished ? kFinished : kRunning;
    const char* const* tips = run.finished ? kFinishedTips : kRunningTips;
    if (ImGui::BeginTable("head", 4, ImGuiTableFlags_SizingStretchProp)) {
        for (int c = 0; c < 4; c++) ImGui::TableSetupColumn(names[c], ImGuiTableColumnFlags_WidthStretch, kWeights[c]);
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int c = 0; c < 4; c++) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", names[c]);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tips[c]);
        }
        ImGui::EndTable();
    }
    top = ImGui::GetCursorPosY();
    ImGui::BeginChild("rows", ImVec2(0.0f, rowsHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | (frame.menuOpen ? 0 : ImGuiWindowFlags_NoInputs));
    ImGui::SetWindowFontScale(kFontScale * textScale());
    if (ImGui::BeginTable("cps", 4, ImGuiTableFlags_SizingStretchProp)) {
        for (int c = 0; c < 4; c++) ImGui::TableSetupColumn(names[c], ImGuiTableColumnFlags_WidthStretch, kWeights[c]);
        const std::vector<int> refSectors = sectorsOf(ref.times);
        const std::vector<int>& mine = bestSectors();
        const size_t last = std::min(run.cps.size(), kMaxRows);
        for (size_t i = 0; i < last; i++) {
            const Checkpoint& cp = run.cps[i];
            const int sector = cp.time - (i ? run.cps[i - 1].time : 0);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (run.finished && i + 1 == run.cps.size()) ImGui::TextDisabled("Finish");
            else ImGui::TextDisabled("%zu", i + 1);
            if (run.finished) {
                ImGui::TableNextColumn();
                timeText(text, sizeof(text), sector);
                if (i < refSectors.size()) ImGui::TextColored(deltaColour(sector - refSectors[i]), "%s", text);
                else ImGui::TextUnformatted(text);
                ImGui::TableNextColumn();
                if (i < mine.size()) {
                    timeText(text, sizeof(text), mine[i]);
                    if (i < refSectors.size() && mine[i] < refSectors[i]) ImGui::TextColored(deltaColour(-1), "%s", text);
                    else ImGui::TextUnformatted(text);
                }
                ImGui::TableNextColumn();
                if (i < refSectors.size()) {
                    timeText(text, sizeof(text), refSectors[i]);
                    ImGui::TextDisabled("%s", text);
                }
            } else {
                ImGui::TableNextColumn();
                timeText(text, sizeof(text), cp.time);
                ImGui::TextUnformatted(text);
                ImGui::TableNextColumn();
                if (i < ref.times.size()) {
                    const int d = cp.time - ref.times[i];
                    deltaText(text, sizeof(text), d);
                    ImGui::TextColored(deltaColour(d), "%s", text);
                }
                ImGui::TableNextColumn();
                if (cp.speed >= 0.0f) {
                    if (i < ref.speeds.size() && ref.speeds[i] >= 0.0f) {
                        const float d = (cp.speed - ref.speeds[i]) * 3.6f;
                        snprintf(speedText, sizeof(speedText), "%.0f %+.0f", cp.speed * 3.6f, d);
                        ImGui::TextColored(speedColour(d), "%s", speedText);
                    } else {
                        ImGui::TextDisabled("%.0f", cp.speed * 3.6f);
                    }
                }
            }
            if (i + 1 == last && last != s_rows) ImGui::SetScrollHereY(1.0f); // a new row: show it
        }
        ImGui::EndTable();
    }
    s_rows = std::min(run.cps.size(), kMaxRows);
    ImGui::EndChild();
    ImGui::SetWindowFontScale(1.0f);
    shownPos = ImGui::GetWindowPos();
    shownSize = ImGui::GetWindowSize();
    ImGui::End();
}

void drawSessionPanel(const Frame& frame, ImVec2 under, float width) {
    Settings& s = settings();
    const float k = screenScale();
    // Attached right under the split times with their width (moving or resizing them takes it
    // along); dragged away, it stays where it was put.
    if (!s.sessionAttached && s.sessionX < 0.0f) {
        s.sessionX = under.x;
        s.sessionY = under.y;
    }
    const ImVec2 pos = s.sessionAttached ? under : ImVec2(s.sessionX, s.sessionY);
    const PanelStyle panelStyle;
    static WindowPlace s_place;
    s_place.before(frame, pos, ImVec2(width, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("Session##misc", nullptr, windowFlags(frame, true));
    ImVec2 p, size;
    if (s_place.after(frame, p, size)) {
        if (draggedAway(pos, p, size, width)) s.sessionAttached = false;
        else setWidth(size.x);
        if (!s.sessionAttached) {
            s.sessionX = p.x;
            s.sessionY = p.y;
        }
        settingsChanged();
    }
    windowChrome(frame, "Session", true);
    ImGui::SetWindowFontScale(kFontScale * textScale());
    char a[32], b[64];
    // Labelled lines: what it is on the left (with a tooltip), the number on the right.
    const auto row = [](const char* label, const char* tip, const char* value, const ImVec4* colour) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", label);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        ImGui::TableNextColumn();
        if (colour) ImGui::TextColored(*colour, "%s", value);
        else ImGui::TextUnformatted(value);
    };
    // Your best sectors (all your runs and replays of this map) and the reference's.
    const std::vector<int>& mine = bestSectors();
    const Reference ref = reference();
    const std::vector<int> refSectors = sectorsOf(ref.times);
    int theoretical = 0, combined = 0, beaten = 0;
    for (int v : mine) theoretical += v;
    // (Against your own best run there is nothing to combine: its sectors are already among yours.)
    const bool comparable = !mine.empty() && mine.size() == refSectors.size() && strcmp(ref.label, "best") != 0;
    for (size_t i = 0; comparable && i < mine.size(); i++) {
        combined += std::min(mine[i], refSectors[i]);
        beaten += mine[i] < refSectors[i] ? 1 : 0;
    }
    if (ImGui::BeginTable("numbers", 2, ImGuiTableFlags_SizingStretchProp)) {
        snprintf(a, sizeof(a), "%d", g.attempts);
        row("Attempts", "Runs started on this map in this session", a, nullptr);
        snprintf(a, sizeof(a), "%zu", g.finishes.size());
        row("Finished", "Runs that reached the finish in this session", a, nullptr);
        if (!g.finishes.empty()) {
            timeText(a, sizeof(a), *std::min_element(g.finishes.begin(), g.finishes.end()));
            row("Best", "Your fastest finish in this session", a, nullptr);
            timeText(a, sizeof(a), median(g.finishes));
            row("Median", "The middle of your finish times in this session", a, nullptr);
            timeText(a, sizeof(a), g.finishes.back());
            row("Last", "Your last finish time", a, nullptr);
        }
        snprintf(a, sizeof(a), "%d", g.lastFinished.cps.empty() ? g.run.respawns : g.lastFinished.respawns);
        row("Respawns", "Respawns in your last finished run (or in this one before a finish)", a, nullptr);
        if (g.sectorTimes.size() >= 2) {
            // How much your sector times vary over this session's finished runs.
            double spread = 0.0;
            int sectors = 0;
            for (size_t i = 0; i < g.sectorTimes[0].size(); i++) {
                double sum = 0.0, square = 0.0;
                int n = 0;
                for (const std::vector<int>& run : g.sectorTimes) {
                    if (i >= run.size()) continue;
                    sum += run[i];
                    square += static_cast<double>(run[i]) * run[i];
                    n++;
                }
                if (n < 2) continue;
                const double mean = sum / n;
                spread += sqrt(std::max(0.0, square / n - mean * mean));
                sectors++;
            }
            if (sectors) {
                snprintf(a, sizeof(a), "%.3f s", spread / sectors / 1000.0);
                row("Spread", "How much your time in a sector varies between runs, on average", a, nullptr);
            }
        }
        if (!mine.empty()) {
            timeText(a, sizeof(a), theoretical);
            row("Your theoretical best", "Your best sectors added up: the time you could reach.", a, nullptr);
        }
        if (comparable) {
            timeText(a, sizeof(a), combined);
            snprintf(b, sizeof(b), "%s (%d of %zu yours)", a, beaten, mine.size());
            const ImVec4 blue = deltaColour(-1);
            row("Best combined", "Per sector the faster of yours and the reference's, added up. Yours: sectors where you beat the reference.", b,
                beaten ? &blue : nullptr);
        }
        ImGui::EndTable();
    }
    if (g.finishes.size() >= 2) {
        std::vector<float> values;
        for (int v : g.finishes) values.push_back(static_cast<float>(v));
        const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
        ImGui::PlotLines("##times", values.data(), static_cast<int>(values.size()), 0, nullptr, *lo - 100.0f, *hi + 100.0f, ImVec2(-1.0f, 28.0f * k));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Your finish times in this session, oldest on the left (lower is faster)");
    }
    // The last finished run, sector by sector.
    const Run& run = g.lastFinished;
    if (!run.cps.empty()) {
        ImGui::TextDisabled("Last run, sector by sector");
        if (ImGui::BeginTable("sectors", 5, ImGuiTableFlags_SizingStretchProp)) {
            // Short heads: the column is narrow in a minimap-wide panel ("Top km/h" was cut off).
            static const char* const kNames[] = {"Sector", "Time", "vs best", "Top", "Avg"};
            static const char* const kTips[] = {"From one checkpoint to the next", "Your time in this sector",
                                                "Against your best time in this sector (plus: slower)", "Your top speed in this sector, km/h",
                                                "Your average speed in this sector, km/h"};
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            for (int c = 0; c < 5; c++) {
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", kNames[c]);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kTips[c]);
            }
            for (size_t i = 0; i < run.cps.size() && i < kMaxRows; i++) {
                const int sector = run.cps[i].time - (i ? run.cps[i - 1].time : 0);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (i + 1 == run.cps.size()) ImGui::TextDisabled("Finish");
                else ImGui::TextDisabled("%zu", i + 1);
                ImGui::TableNextColumn();
                timeText(a, sizeof(a), sector);
                ImGui::TextUnformatted(a);
                ImGui::TableNextColumn();
                if (i < mine.size()) {
                    deltaText(a, sizeof(a), sector - mine[i]);
                    ImGui::TextColored(deltaColour(sector - mine[i]), "%s", a);
                }
                const SectorSpeed sp = i < run.sectors.size() ? run.sectors[i] : SectorSpeed();
                ImGui::TableNextColumn();
                if (sp.seconds > 0.0f) ImGui::Text("%.0f", sp.top * 3.6f);
                ImGui::TableNextColumn();
                if (sp.seconds > 0.0f) ImGui::Text("%.0f", sp.sum / sp.seconds * 3.6f);
            }
            ImGui::EndTable();
        }
    }
    ImGui::SetWindowFontScale(1.0f);
    s_place.end();
    ImGui::End();
}

} // namespace

void draw(const Frame& frame) {
    ImVec2 pos, size;
    drawSplitPanel(frame, pos, size);
    if (settings().session && (g.attempts > 0 || !g.finishes.empty())) drawSessionPanel(frame, ImVec2(pos.x, pos.y + size.y), size.x);
}

void drawPage() {
    Settings& s = settings();
    bool changed = false;
    static const char* const kCompare[] = {"Record (TMX, auto)", "Ghost in the race", "My best", "Replay file"};
    changed |= ImGui::Combo("Compare with", &s.splitCompare, kCompare, 4);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Record: the map's world record from TMX, downloaded once per map.\n"
                          "Ghost: the fastest ghost you race against.\nWithout one: your best.");
    }
    if (g.uid.empty()) {
        ImGui::TextDisabled("Open a map.");
        if (changed) settingsChanged();
        return;
    }
    char text[64];
    if (g.best.empty()) {
        ImGui::TextDisabled("My best: none yet");
    } else {
        timeText(text, sizeof(text), g.best.back());
        ImGui::TextDisabled("My best: %s", text);
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) {
            g.best.clear();
            g.bestSpeeds.clear();
            DeleteFileW(bestFile(g.uid).c_str());
        }
    }
    const tmx::Status t = tmx::status();
    if (s.splitCompare == kCompareRecord) {
        if (!g.record.checkpoints.empty()) ImGui::TextDisabled("Record: %s", g.recordNote.c_str());
        else if (!g.recordNote.empty()) ImGui::TextDisabled("Record: %s", g.recordNote.c_str()); // downloaded, but not readable
        else if (tmx::knownMissing(g.uid)) ImGui::TextDisabled("Record: not on TMX");
        else if (t.uid == g.uid && !t.message.empty()) ImGui::TextDisabled("Record: %s", t.message.c_str());
    }
    if (s.splitCompare == kCompareReplay) {
        if (!g.fileNote.empty()) ImGui::TextDisabled("Replay: %s", g.fileNote.c_str());
        // Your replays of this map.
        if (!g_scanned) startScan();
        std::vector<ReplayFile> mine;
        {
            std::lock_guard<std::mutex> lock(g_listLock);
            for (const ReplayFile& f : g_list) {
                if (f.uid == g.uid) mine.push_back(f);
            }
        }
        std::sort(mine.begin(), mine.end(), [](const ReplayFile& a, const ReplayFile& b) { return a.time < b.time; });
        if (g_scanning) ImGui::TextDisabled("Searching...");
        else ImGui::TextDisabled("%zu replays of this map", mine.size());
        ImGui::SameLine();
        if (!g_scanning && ImGui::SmallButton("Reload")) {
            g_scanned = false;
            g_jobUid.clear(); // your replays' sectors are read again too
            startScan();
        }
        if (!mine.empty() && ImGui::BeginListBox("##replays", ImVec2(-1.0f, std::min<float>(static_cast<float>(mine.size()), 6.0f) *
                                                                              ImGui::GetTextLineHeightWithSpacing() + 6.0f))) {
            for (const ReplayFile& f : mine) {
                timeText(text, sizeof(text), f.time);
                const size_t slash = f.path.find_last_of(L"\\/");
                const std::string name = utf8(slash == std::wstring::npos ? f.path : f.path.substr(slash + 1));
                char line[300];
                snprintf(line, sizeof(line), "%s  %s##%s", text, f.driver.empty() ? name.c_str() : f.driver.c_str(), name.c_str());
                if (ImGui::Selectable(line, f.path == g.filePath)) loadReplay(f.path, false);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", name.c_str());
            }
            ImGui::EndListBox();
        }
    }
    if (changed) settingsChanged();
}

} // namespace splits
} // namespace misc
} // namespace tmshaders
