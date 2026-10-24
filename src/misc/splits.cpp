#include "misc/internal.h"
#include "engine.h"
#include "log.h"
#include "menu.h"
#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>

// Split times: at each checkpoint, in the moment the game registers it (the race's own list of
// checkpoint times, engine::raceTimes), the time against your best run on this map or against a
// replay (its ghost's checkpoint times; a local file or the map's record from TMX). Under it a
// sector clock: how much of the reference's time for the sector running now has passed. After the
// finish, the time won or lost in each sector. Nothing says where a checkpoint is.
namespace tmshaders {
namespace misc {
namespace splits {
namespace {

constexpr DWORD kDeltaShownMs = 4000; // the delta after a checkpoint
constexpr size_t kMaxRows = 24;      // sectors in the table after the finish

struct ReplayFile {
    std::wstring path;
    std::string uid, driver;
    int time = -1;
};

struct State {
    const void* map = nullptr;
    std::string uid;
    // Your best run on this map: race time at each checkpoint, the finish last (from its file).
    std::vector<int> best;
    // The loaded replay (its ghost's checkpoint times).
    Replay replay;
    std::wstring replayPath;
    std::string replayNote;
    // The run being driven.
    const void* race = nullptr;
    int lastPassed = 0, lastTime = 0;
    std::vector<int> run; // race time at each checkpoint passed, in order
    bool finished = false;
    std::vector<int> finishedRun, finishedReference; // the table after the finish
    bool finishedIsBest = false;
    // The last checkpoint's delta.
    bool haveDelta = false;
    int delta = 0, deltaIndex = 0;
    DWORD deltaAt = 0;
    // For the finish effect.
    bool finishPending = false;
    int finishTime = 0;
    bool finishBest = false;
    // Speed from the car's positions.
    float speed = 0.0f, lastPos[3] = {};
    LARGE_INTEGER lastPosAt{};
    bool havePos = false;
    // The race times read this frame (for the sector clock).
    engine::RaceTimes times;
    bool timesValid = false;
};
State g;

// Local replays: the folders read on a thread (thousands of files: only their headers).
std::mutex g_listLock;
std::vector<ReplayFile> g_list;
std::atomic<bool> g_scanning{false};
bool g_scanned = false;

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

void loadBest() {
    g.best.clear();
    if (g.uid.empty()) return;
    FILE* f = _wfopen(bestFile(g.uid).c_str(), L"r");
    if (!f) return;
    int v = 0;
    while (g.best.size() < 4096 && fscanf(f, "%d", &v) == 1) g.best.push_back(v);
    fclose(f);
}

void saveBest() {
    FILE* f = _wfopen(bestFile(g.uid).c_str(), L"w");
    if (!f) return;
    for (size_t i = 0; i < g.best.size(); i++) fprintf(f, "%s%d", i ? " " : "", g.best[i]);
    fprintf(f, "\n");
    fclose(f);
}

bool loadReplay(const std::wstring& path) {
    Replay r;
    std::string error;
    if (!readReplay(path, r, error)) {
        g.replayNote = "Can't read it: " + error;
        return false;
    }
    if (r.mapUid != g.uid) {
        g.replayNote = "That replay is of another map.";
        return false;
    }
    g.replay = r;
    g.replayPath = path;
    char note[160];
    snprintf(note, sizeof(note), "%s, %d.%03d s, %zu checkpoints", r.driver.empty() ? "unknown driver" : r.driver.c_str(), r.raceTime / 1000,
             r.raceTime % 1000, r.checkpoints.size());
    g.replayNote = note;
    TMVS_LOG("misc: split times: replay loaded (%s)", note);
    return true;
}

// The times compared with: the replay's when chosen and loaded, else your best.
const std::vector<int>& reference(const char** label) {
    if (settings().splitReference == 1 && !g.replay.checkpoints.empty()) {
        if (label) *label = "replay";
        return g.replay.checkpoints;
    }
    if (label) *label = "your best";
    return g.best;
}

void newMap(const void* map) {
    g.map = map;
    g.uid = map ? engine::currentMapUid() : std::string();
    g.replay = Replay();
    g.replayPath.clear();
    g.replayNote.clear();
    g.race = nullptr;
    g.run.clear();
    g.finished = false;
    g.finishedRun.clear();
    g.haveDelta = false;
    loadBest();
    // The record downloaded before from TMX for this map comes back by itself (no request).
    const std::wstring cached = tmx::cached(g.uid);
    if (!cached.empty()) loadReplay(cached);
}

void newRun() {
    g.run.clear();
    g.finished = false;
    g.haveDelta = false;
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

void measureSpeed() {
    engine::CarPose pose;
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    if (!engine::carPose(pose)) {
        g.havePos = false;
        return;
    }
    if (g.havePos) {
        const double dt = static_cast<double>(now.QuadPart - g.lastPosAt.QuadPart) / static_cast<double>(frequency.QuadPart);
        if (dt > 0.02) {
            const float d[3] = {pose.position[0] - g.lastPos[0], pose.position[1] - g.lastPos[1], pose.position[2] - g.lastPos[2]};
            const float v = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / static_cast<float>(dt);
            g.speed = v < 300.0f ? g.speed * 0.6f + v * 0.4f : g.speed; // a respawn's jump is no speed
        } else {
            return; // too close for a speed: measured over a few frames
        }
    }
    memcpy(g.lastPos, pose.position, sizeof(g.lastPos));
    g.lastPosAt = now;
    g.havePos = true;
}

} // namespace

void update() {
    const void* map = engine::currentMap();
    if (map != g.map) newMap(map);
    g.timesValid = map && engine::raceTimes(g.times);
    if (!g.timesValid) return;
    const engine::RaceTimes& rt = g.times;
    // A restart (the clock or the count went back) or another race: a new run.
    if (rt.race != g.race || rt.passed < g.lastPassed || rt.time + 500 < g.lastTime) {
        if (rt.race != g.race) g.finishedRun.clear();
        g.race = rt.race;
        newRun();
        g.lastPassed = rt.passed;
    }
    const char* label = nullptr;
    const std::vector<int>& ref = reference(&label);
    while (g.lastPassed < rt.passed && !g.finished) {
        const int k = g.lastPassed++;
        int t = rt.count > 0 ? rt.times[k % rt.count] : -1;
        if (t < 0 || t > rt.time + 50) t = rt.time;
        g.run.push_back(t);
        if (static_cast<size_t>(k) < ref.size()) {
            g.haveDelta = true;
            g.delta = t - ref[k];
            g.deltaIndex = k;
            g.deltaAt = GetTickCount();
        }
    }
    g.lastPassed = rt.passed;
    if (rt.finished && !g.finished && !g.run.empty()) {
        g.finished = true;
        if (g.run.back() != rt.time) g.run.push_back(rt.time);
        const bool better = g.best.empty() || rt.time < g.best.back();
        g.finishedRun = g.run;
        g.finishedReference = ref;
        g.finishedIsBest = better;
        if (better && !g.uid.empty()) {
            g.best = g.run;
            saveBest();
        }
        g.finishPending = true;
        g.finishTime = rt.time;
        g.finishBest = better;
        char t[32];
        timeText(t, sizeof(t), rt.time);
        TMVS_LOG("misc: split times: finished in %s after %zu checkpoints%s", t, g.run.size(), better ? " (your best)" : "");
    }
    g.lastTime = rt.time;
    if (settings().splits) measureSpeed();
}

bool takeFinish(int& time, bool& best) {
    if (!g.finishPending) return false;
    g.finishPending = false;
    time = g.finishTime;
    best = g.finishBest;
    return true;
}

bool wanted() {
    return settings().splits && g.timesValid;
}

void draw(const Frame& frame) {
    Settings& s = settings();
    const float scale = menu::scale();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    static bool s_wasOpen = false;
    const bool place = !frame.menuOpen || !s_wasOpen;
    s_wasOpen = frame.menuOpen;
    if (place) {
        const ImVec2 pos(s.splitX >= 0.0f ? s.splitX : display.x * 0.5f, s.splitY >= 0.0f ? s.splitY : 90.0f * scale);
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always, s.splitX >= 0.0f ? ImVec2(0, 0) : ImVec2(0.5f, 0.0f));
    }
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (!frame.menuOpen) flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    ImGui::Begin("Split times##misc", nullptr, flags);
    if (frame.menuOpen && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // Moved by hand: from then on its own place (top left corner) instead of the top centre.
        const ImVec2 pos = ImGui::GetWindowPos();
        if (!place && (fabsf(pos.x - s.splitX) > 0.5f || fabsf(pos.y - s.splitY) > 0.5f)) {
            s.splitX = pos.x;
            s.splitY = pos.y;
            settingsChanged();
        }
    }
    const char* label = nullptr;
    const std::vector<int>& ref = reference(&label);
    const engine::RaceTimes& rt = g.times;
    char text[64];
    const bool recent = g.haveDelta && GetTickCount() - g.deltaAt < kDeltaShownMs;
    if (g.finished && !g.finishedRun.empty()) {
        // The finish: the run's time against the reference, then each sector.
        const std::vector<int>& run = g.finishedRun;
        const std::vector<int>& r = g.finishedReference;
        timeText(text, sizeof(text), run.back());
        ImGui::Text("Finish %s%s", text, g.finishedIsBest ? "  (your best)" : "");
        if (r.size() >= run.size() && !r.empty()) {
            const int total = run.back() - r[run.size() - 1];
            deltaText(text, sizeof(text), total);
            ImGui::SameLine();
            ImGui::TextColored(deltaColour(total), "%s vs %s", text, label);
        }
        if (ImGui::BeginTable("sectors", 3, ImGuiTableFlags_SizingFixedFit)) {
            for (size_t i = 0; i < run.size() && i < kMaxRows; i++) {
                const int mine = run[i] - (i ? run[i - 1] : 0);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled(i + 1 == run.size() ? "Finish" : "Sector %zu", i + 1);
                ImGui::TableNextColumn();
                timeText(text, sizeof(text), mine);
                ImGui::TextUnformatted(text);
                ImGui::TableNextColumn();
                if (i < r.size()) {
                    const int theirs = r[i] - (i ? r[i - 1] : 0);
                    deltaText(text, sizeof(text), mine - theirs);
                    ImGui::TextColored(deltaColour(mine - theirs), "%s", text);
                }
            }
            ImGui::EndTable();
        }
        if (run.size() > kMaxRows) ImGui::TextDisabled("... %zu sectors", run.size());
    } else {
        // The last checkpoint's delta, big, for a few seconds.
        if (recent) {
            deltaText(text, sizeof(text), g.delta);
            ImGui::SetWindowFontScale(1.6f);
            ImGui::TextColored(deltaColour(g.delta), "%s", text);
            ImGui::SetWindowFontScale(1.0f);
            ImGui::SameLine();
            ImGui::TextDisabled("CP %d vs %s", g.deltaIndex + 1, label);
        } else if (ref.empty()) {
            ImGui::TextDisabled(settings().splitReference == 1 ? "No replay loaded (Misc page)" : "No best run on this map yet");
        } else {
            ImGui::TextDisabled("vs %s %s", label, (timeText(text, sizeof(text), ref.back()), text));
        }
        // The sector clock: the time since the last checkpoint against the reference's sector.
        const size_t k = g.run.size();
        if (s.splitBar && rt.started && !rt.finished && k < ref.size()) {
            const int start = k ? g.run[k - 1] : 0, refSector = ref[k] - (k ? ref[k - 1] : 0);
            const int elapsed = std::max(0, rt.time - start);
            const float fraction = refSector > 0 ? static_cast<float>(elapsed) / static_cast<float>(refSector) : 0.0f;
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, fraction <= 1.0f ? ImVec4(0.30f, 0.70f, 1.0f, 0.9f) : ImVec4(1.0f, 0.35f, 0.30f, 0.9f));
            char overlay[48];
            snprintf(overlay, sizeof(overlay), "%.1f / %.1f s", elapsed / 1000.0f, refSector / 1000.0f);
            ImGui::ProgressBar(std::min(fraction, 1.0f), ImVec2(220.0f * scale, 0.0f), overlay);
            ImGui::PopStyleColor();
        }
        if (g.havePos) ImGui::TextDisabled("%.0f km/h", g.speed * 3.6f);
    }
    ImGui::End();
}

void drawPage() {
    Settings& s = settings();
    bool changed = false;
    if (g.uid.empty()) {
        ImGui::TextDisabled("Open a map to choose what to compare with.");
        return;
    }
    char text[64];
    ImGui::TextUnformatted("Compare with");
    if (g.best.empty()) {
        changed |= ImGui::RadioButton("Your best run (none yet on this map)", &s.splitReference, 0);
    } else {
        timeText(text, sizeof(text), g.best.back());
        char line[128];
        snprintf(line, sizeof(line), "Your best run (%s)", text);
        changed |= ImGui::RadioButton(line, &s.splitReference, 0);
        ImGui::SameLine();
        if (ImGui::SmallButton("Forget")) {
            g.best.clear();
            DeleteFileW(bestFile(g.uid).c_str());
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget your best run on this map (the next finish becomes it).");
    }
    changed |= ImGui::RadioButton("A replay of this map", &s.splitReference, 1);
    if (!g.replayNote.empty()) ImGui::TextDisabled("  %s", g.replayNote.c_str());
    if (s.splitReference == 1) {
        // The map's record from TMX: one request when pressed, kept on disk, never asked twice.
        const tmx::Status t = tmx::status();
        if (t.state == tmx::State::Busy) {
            ImGui::TextDisabled("  TMX: %s", t.message.c_str());
        } else {
            if (ImGui::Button("Get the record from TMX")) tmx::request(g.uid);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Asks tmnf.exchange (then tmuf.exchange) for this map's world record replay and keeps it in\n"
                                  "Documents\\TrackMania\\TMVS\\Replays. Only its checkpoint times are used.");
            }
            if (!t.message.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", t.message.c_str());
            }
            static std::wstring s_taken;
            if (t.state == tmx::State::Done && !t.file.empty() && t.file != s_taken) {
                s_taken = t.file;
                loadReplay(t.file);
            }
        }
        // Your own replays of this map.
        if (!g_scanned) startScan();
        std::vector<ReplayFile> mine;
        {
            std::lock_guard<std::mutex> lock(g_listLock);
            for (const ReplayFile& f : g_list) {
                if (f.uid == g.uid) mine.push_back(f);
            }
        }
        std::sort(mine.begin(), mine.end(), [](const ReplayFile& a, const ReplayFile& b) { return a.time < b.time; });
        if (g_scanning) ImGui::TextDisabled("  Looking for your replays...");
        else ImGui::TextDisabled("  %zu replays of this map in Tracks\\Replays", mine.size());
        ImGui::SameLine();
        if (!g_scanning && ImGui::SmallButton("Look again")) {
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
                if (ImGui::Selectable(line, f.path == g.replayPath)) loadReplay(f.path);
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
