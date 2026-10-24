#include "misc/internal.h"
#include "engine.h"
#include "log.h"
#include "menu.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
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

constexpr size_t kRunningRows = 6; // checkpoints listed while driving (the newest)
constexpr size_t kMaxRows = 30;    // in the table after the finish

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

// The file: the times (ms) on the first line, the speeds (km/h x 10, -1 unknown) on the second.
void loadBest() {
    g.best.clear();
    g.bestSpeeds.clear();
    if (g.uid.empty()) return;
    FILE* f = _wfopen(bestFile(g.uid).c_str(), L"r");
    if (!f) return;
    char line[16384];
    for (int row = 0; row < 2 && fgets(line, sizeof(line), f); row++) {
        const char* p = line;
        char* end = nullptr;
        for (long v = strtol(p, &end, 10); end != p && g.best.size() < 4096; v = strtol(p, &end, 10)) {
            if (row == 0) g.best.push_back(static_cast<int>(v));
            else g.bestSpeeds.push_back(v < 0 ? -1.0f : static_cast<float>(v) / 36.0f);
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
        return fromBest(tmx::knownMissing(g.uid) ? "not on TMX" : (tmx::status().state == tmx::State::Busy ? "fetching the record" : "no record yet"));
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

// Your best time for each sector: this session's finished runs and your best run.
std::vector<int> bestSectors() {
    std::vector<int> best = sectorsOf(g.best);
    for (const std::vector<int>& run : g.sectorTimes) {
        if (best.empty()) best = run;
        for (size_t i = 0; i < run.size() && i < best.size(); i++) best[i] = std::min(best[i], run[i]);
    }
    return best;
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

void drawSplitPanel(const Frame& frame, ImVec2& shownPos, ImVec2& shownSize) {
    Settings& s = settings();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float k = screenScale(), width = s.splitWidth * k, margin = 16.0f * k, gap = 8.0f * k;
    ImVec2 mapPos, mapSize;
    ImVec2 pos(display.x - width - margin, margin);
    if (mapview::rect(mapPos, mapSize)) pos = ImVec2(mapPos.x + mapSize.x - width, mapPos.y + mapSize.y + gap);
    if (s.splitX >= 0.0f) pos = ImVec2(s.splitX, s.splitY);
    static WindowPlace s_place;
    s_place.before(frame, pos, ImVec2(width, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("Split times##misc", nullptr, windowFlags(frame, true));
    ImVec2 p, size;
    if (s_place.after(frame, p, size)) {
        s.splitX = p.x;
        s.splitY = p.y;
        s.splitWidth = std::clamp(size.x / k, 160.0f, 800.0f);
        settingsChanged();
    }
    windowChrome(frame, "Split times", true);
    ImGui::SetWindowFontScale(0.9f);
    const Reference ref = g.run.finished ? g.finishedReference : reference();
    const engine::RaceTimes& rt = g.times;
    char text[64], speedText[32];
    // What it is compared with.
    if (ref.times.empty()) {
        ImGui::TextDisabled("No reference yet%s%s", ref.note.empty() ? "" : ": ", ref.note.c_str());
    } else {
        timeText(text, sizeof(text), ref.times.back());
        ImGui::TextDisabled("vs %s %s%s%s", ref.label, text, ref.note.empty() ? "" : " - ", ref.note.c_str());
    }
    const Run& run = g.run;
    if (run.finished) {
        timeText(text, sizeof(text), run.cps.back().time);
        ImGui::Text("Finish %s%s", text, g.finishedIsBest ? "  (best)" : "");
        if (ref.times.size() >= run.cps.size() && !ref.times.empty()) {
            const int total = run.cps.back().time - ref.times[run.cps.size() - 1];
            deltaText(text, sizeof(text), total);
            ImGui::SameLine();
            ImGui::TextColored(deltaColour(total), "%s", text);
        }
    } else if (g.haveDelta) {
        // The last checkpoint's delta, big, until the next one or a restart.
        deltaText(text, sizeof(text), g.delta);
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextColored(deltaColour(g.delta), "%s", text);
        ImGui::SetWindowFontScale(0.9f);
        ImGui::SameLine();
        if (g.haveSpeedDelta) {
            ImGui::TextColored(speedColour(g.speedDelta), "%+.0f km/h", g.speedDelta);
            ImGui::SameLine();
        }
        ImGui::TextDisabled("CP %d", g.deltaIndex + 1);
    }
    // The sector clock: time since the last checkpoint against the reference's sector.
    const size_t passed = run.cps.size();
    if (s.splitBar && g.timesValid && rt.started && !rt.finished && passed < ref.times.size()) {
        const int start = passed ? run.cps[passed - 1].time : 0, refSector = ref.times[passed] - (passed ? ref.times[passed - 1] : 0);
        const int elapsed = std::max(0, rt.time - start);
        const float fraction = refSector > 0 ? static_cast<float>(elapsed) / static_cast<float>(refSector) : 0.0f;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, fraction <= 1.0f ? ImVec4(0.30f, 0.70f, 1.0f, 0.9f) : ImVec4(1.0f, 0.35f, 0.30f, 0.9f));
        snprintf(text, sizeof(text), "%.1f / %.1f s", elapsed / 1000.0f, refSector / 1000.0f);
        ImGui::ProgressBar(std::min(fraction, 1.0f), ImVec2(-1.0f, 0.0f), text);
        ImGui::PopStyleColor();
    }
    // This run's checkpoints (the newest while driving, all after the finish).
    if (!run.cps.empty() && ImGui::BeginTable("cps", 4, ImGuiTableFlags_SizingStretchProp)) {
        const size_t first = run.finished ? 0 : (run.cps.size() > kRunningRows ? run.cps.size() - kRunningRows : 0);
        const size_t last = std::min(run.cps.size(), first + kMaxRows);
        for (size_t i = first; i < last; i++) {
            const Checkpoint& cp = run.cps[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (run.finished && i + 1 == run.cps.size()) ImGui::TextDisabled("Fin");
            else ImGui::TextDisabled("%zu", i + 1);
            ImGui::TableNextColumn();
            timeText(text, sizeof(text), run.finished ? cp.time - (i ? run.cps[i - 1].time : 0) : cp.time);
            ImGui::TextUnformatted(text);
            ImGui::TableNextColumn();
            if (i < ref.times.size()) {
                const int d = run.finished ? (cp.time - (i ? run.cps[i - 1].time : 0)) - (ref.times[i] - (i ? ref.times[i - 1] : 0)) : cp.time - ref.times[i];
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
        ImGui::EndTable();
        if (run.finished) ImGui::TextDisabled("Sector times; km/h at each checkpoint");
    }
    if (run.respawns > 0 && !run.finished) ImGui::TextDisabled("%d respawn%s", run.respawns, run.respawns == 1 ? "" : "s");
    ImGui::SetWindowFontScale(1.0f);
    shownPos = ImGui::GetWindowPos();
    shownSize = ImGui::GetWindowSize();
    s_place.end();
    ImGui::End();
}

void drawSessionPanel(const Frame& frame, ImVec2 under) {
    Settings& s = settings();
    const float k = screenScale(), width = s.splitWidth * k, gap = 8.0f * k;
    ImVec2 pos = s.sessionX >= 0.0f ? ImVec2(s.sessionX, s.sessionY) : ImVec2(under.x, under.y + gap);
    static WindowPlace s_place;
    s_place.before(frame, pos, ImVec2(width, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("Session##misc", nullptr, windowFlags(frame, true));
    ImVec2 p, size;
    if (s_place.after(frame, p, size)) {
        s.sessionX = p.x;
        s.sessionY = p.y;
        s.splitWidth = std::clamp(size.x / k, 160.0f, 800.0f);
        settingsChanged();
    }
    windowChrome(frame, "Session", true);
    ImGui::SetWindowFontScale(0.9f);
    char a[32], b[32], c[32];
    ImGui::Text("%d attempt%s, %zu finished", g.attempts, g.attempts == 1 ? "" : "s", g.finishes.size());
    if (!g.finishes.empty()) {
        timeText(a, sizeof(a), *std::min_element(g.finishes.begin(), g.finishes.end()));
        timeText(b, sizeof(b), median(g.finishes));
        timeText(c, sizeof(c), g.finishes.back());
        ImGui::Text("Best %s  Median %s  Last %s", a, b, c);
        if (g.finishes.size() >= 2) {
            std::vector<float> values;
            for (int v : g.finishes) values.push_back(static_cast<float>(v));
            const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
            ImGui::PlotLines("##times", values.data(), static_cast<int>(values.size()), 0, nullptr, *lo - 100.0f, *hi + 100.0f, ImVec2(-1.0f, 28.0f * k));
        }
    }
    // The theoretical best: your best time of every sector, added up.
    const std::vector<int> best = bestSectors();
    if (!best.empty()) {
        int sum = 0;
        for (int v : best) sum += v;
        timeText(a, sizeof(a), sum);
        if (!g.best.empty()) {
            deltaText(b, sizeof(b), sum - g.best.back());
            ImGui::Text("Theoretical best %s (%s)", a, b);
        } else {
            ImGui::Text("Theoretical best %s", a);
        }
    }
    // Consistency: how much your sector times spread over this session's finished runs.
    if (g.sectorTimes.size() >= 2) {
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
        if (sectors) ImGui::Text("Spread %.3f s per sector", spread / sectors / 1000.0);
    }
    // The last finished run: each sector's top and average speed, and the time lost against your best
    // time of that sector.
    const Run& run = g.lastFinished;
    if (!run.cps.empty()) {
        ImGui::TextDisabled("Last run: %d respawn%s", run.respawns, run.respawns == 1 ? "" : "s");
        if (ImGui::BeginTable("sectors", 4, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("#");
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Top");
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Avg");
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Lost");
            for (size_t i = 0; i < run.cps.size() && i < kMaxRows; i++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%zu", i + 1);
                ImGui::TableNextColumn();
                const SectorSpeed sp = i < run.sectors.size() ? run.sectors[i] : SectorSpeed();
                if (sp.seconds > 0.0f) ImGui::Text("%.0f", sp.top * 3.6f);
                ImGui::TableNextColumn();
                if (sp.seconds > 0.0f) ImGui::Text("%.0f", sp.sum / sp.seconds * 3.6f);
                ImGui::TableNextColumn();
                if (i < best.size()) {
                    const int mine = run.cps[i].time - (i ? run.cps[i - 1].time : 0);
                    deltaText(a, sizeof(a), mine - best[i]);
                    ImGui::TextColored(deltaColour(mine - best[i]), "%s", a);
                }
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
    if (settings().session && (g.attempts > 0 || !g.finishes.empty())) drawSessionPanel(frame, ImVec2(pos.x, pos.y + size.y));
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
