#include "splits11.h"
#include "game.h"
#include "log.h"
#include "menu.h"
#include "options.h"
#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace tmvs_mp {
namespace splits {
namespace {

constexpr int kRows = 5;            // checkpoint rows shown (the newest ones)
constexpr double kDeltaSeconds = 3; // the big delta stays this long after a checkpoint

struct Run {
    std::vector<int> times;  // race time at each checkpoint (ms), the finish last when finished
    std::vector<int> speeds; // km/h at each
    bool started = false, finished = false;
    int respawnsAtStart = 0, respawns = 0;
};

struct Best {
    std::vector<int> times;   // your best finished run's checkpoint times (the finish last)
    std::vector<int> sectors; // your best time of each sector (any finished run)
};

struct Session {
    int attempts = 0, finishes = 0;
    int best = -1, last = -1;
    long long sum = 0; // of the finished times (the average)
};

struct State {
    std::string mapName, mapKey;
    Best best;
    Run run;
    Session session;
    int lastState = -1, lastCount = 0;
    bool present = false;    // a run of yours this frame
    double cpAt = -1.0;      // when the last checkpoint was passed (s)
    int cpDelta = 0;         // its delta against your best (ms)
    bool cpHasDelta = false;
    bool newBest = false;    // the last finish was a new best
    int mapCheck = 0;
} g;

double now() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

std::wstring file() {
    return tmshaders::log::dataDir() + L"\\splits.ini";
}

// The map's section: a hash of its key (author and name).
std::wstring section(const std::string& key) {
    unsigned long long h = 1469598103934665603ull;
    for (unsigned char c : key) {
        h ^= c;
        h *= 1099511628211ull;
    }
    wchar_t s[32];
    swprintf(s, 32, L"map_%016llx", h);
    return s;
}

std::vector<int> readList(const std::wstring& sec, const wchar_t* key) {
    wchar_t text[4096] = {};
    GetPrivateProfileStringW(sec.c_str(), key, L"", text, 4096, file().c_str());
    std::vector<int> out;
    for (const wchar_t* p = text; *p;) {
        wchar_t* end = nullptr;
        const long v = wcstol(p, &end, 10);
        if (end == p) break;
        out.push_back(static_cast<int>(v));
        p = *end == L',' ? end + 1 : end;
    }
    return out;
}

void writeList(const std::wstring& sec, const wchar_t* key, const std::vector<int>& list) {
    std::wstring text;
    for (size_t i = 0; i < list.size(); i++) {
        if (i) text += L",";
        text += std::to_wstring(list[i]);
    }
    WritePrivateProfileStringW(sec.c_str(), key, list.empty() ? nullptr : text.c_str(), file().c_str());
}

void loadBest() {
    g.best = Best{};
    if (g.mapKey.empty()) return;
    const std::wstring sec = section(g.mapKey);
    g.best.times = readList(sec, L"Times");
    g.best.sectors = readList(sec, L"Sectors");
}

void saveBest() {
    if (g.mapKey.empty()) return;
    const std::wstring sec = section(g.mapKey);
    const int n = MultiByteToWideChar(CP_UTF8, 0, g.mapName.c_str(), static_cast<int>(g.mapName.size()), nullptr, 0);
    std::wstring name(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, g.mapName.c_str(), static_cast<int>(g.mapName.size()), &name[0], n);
    WritePrivateProfileStringW(sec.c_str(), L"Map", name.c_str(), file().c_str());
    writeList(sec, L"Times", g.best.times);
    writeList(sec, L"Sectors", g.best.sectors);
}

std::string timeText(int ms) {
    if (ms < 0) return "-:--.---";
    char s[32];
    snprintf(s, sizeof(s), "%d:%02d.%03d", ms / 60000, (ms / 1000) % 60, ms % 1000);
    return s;
}

std::string deltaText(int ms) {
    char s[32];
    const int a = ms < 0 ? -ms : ms;
    snprintf(s, sizeof(s), "%c%d.%03d", ms < 0 ? '-' : '+', a / 1000, a % 1000);
    return s;
}

ImVec4 deltaColour(int ms) {
    return ms < 0 ? ImVec4(0.45f, 0.75f, 1.0f, 1.0f) : (ms > 0 ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
}

int theoreticalBest() {
    if (g.best.sectors.empty() || g.best.times.empty() || g.best.sectors.size() != g.best.times.size()) return -1;
    int sum = 0;
    for (int s : g.best.sectors) sum += s;
    return sum;
}

void newRun(const game::Race& r) {
    g.run = Run{};
    g.run.respawnsAtStart = r.respawns;
    g.cpAt = -1.0;
    g.cpHasDelta = false;
    g.newBest = false;
}

// ---- finish effects: confetti (and fireworks for a new best, Big) ----

struct Piece {
    float x, y, vx, vy, angle, spin, size, life, age;
    ImU32 colour;
    bool spark;
};
std::vector<Piece> g_pieces;
unsigned g_seed = 12345;
double g_effectLast = -1.0;

float random01() {
    g_seed = g_seed * 1664525u + 1013904223u;
    return static_cast<float>(g_seed >> 8) / 16777216.0f;
}

ImU32 randomColour() {
    static const ImU32 kColours[] = {IM_COL32(255, 80, 80, 255), IM_COL32(255, 200, 60, 255), IM_COL32(90, 220, 120, 255),
                                     IM_COL32(80, 170, 255, 255), IM_COL32(200, 110, 255, 255), IM_COL32(255, 255, 255, 255)};
    return kColours[static_cast<int>(random01() * 6.0f) % 6];
}

void startEffect(bool big) {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float w = size.x > 0 ? size.x : 1920.0f, h = size.y > 0 ? size.y : 1080.0f;
    const int pieces = big ? 260 : 90;
    for (int i = 0; i < pieces; i++) {
        Piece p{};
        p.x = random01() * w;
        p.y = -random01() * h * 0.3f;
        p.vx = (random01() - 0.5f) * w * 0.08f;
        p.vy = h * (0.15f + random01() * 0.2f);
        p.angle = random01() * 6.28f;
        p.spin = (random01() - 0.5f) * 10.0f;
        p.size = h * (0.006f + random01() * 0.006f);
        p.life = 2.5f + random01() * 1.5f;
        p.colour = randomColour();
        g_pieces.push_back(p);
    }
    if (big) {
        // Fireworks: three bursts of sparks in the upper half.
        for (int b = 0; b < 3; b++) {
            const float cx = w * (0.25f + 0.25f * b), cy = h * (0.2f + random01() * 0.15f);
            const ImU32 colour = randomColour();
            for (int i = 0; i < 70; i++) {
                const float a = random01() * 6.28f, v = h * (0.1f + random01() * 0.25f);
                Piece p{};
                p.x = cx;
                p.y = cy;
                p.vx = cosf(a) * v;
                p.vy = sinf(a) * v;
                p.size = h * 0.004f;
                p.life = 1.2f + random01() * 0.8f;
                p.age = -0.4f * b; // one after the other
                p.colour = colour;
                p.spark = true;
                g_pieces.push_back(p);
            }
        }
    }
}

void drawEffect() {
    const double t = now();
    const float dt = g_effectLast < 0.0 ? 0.0f : static_cast<float>(std::min(t - g_effectLast, 0.1));
    g_effectLast = g_pieces.empty() ? -1.0 : t;
    if (g_pieces.empty()) return;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const float h = ImGui::GetIO().DisplaySize.y;
    for (Piece& p : g_pieces) {
        p.age += dt;
        if (p.age < 0.0f) continue;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.vy += h * (p.spark ? 0.25f : 0.05f) * dt;
        p.angle += p.spin * dt;
        const float fade = std::clamp((p.life - p.age) / 0.6f, 0.0f, 1.0f);
        const ImU32 c = (p.colour & 0x00FFFFFF) | (static_cast<ImU32>(fade * 255.0f) << 24);
        if (p.spark) {
            draw->AddCircleFilled(ImVec2(p.x, p.y), p.size, c);
        } else {
            const float cs = cosf(p.angle) * p.size, sn = sinf(p.angle) * p.size * 0.5f;
            draw->AddQuadFilled(ImVec2(p.x - cs, p.y - sn), ImVec2(p.x + sn, p.y - cs), ImVec2(p.x + cs, p.y + sn), ImVec2(p.x - sn, p.y + cs), c);
        }
    }
    g_pieces.erase(std::remove_if(g_pieces.begin(), g_pieces.end(), [](const Piece& p) { return p.age >= p.life; }), g_pieces.end());
}

void finish(int time) {
    Run& run = g.run;
    run.finished = true;
    Session& s = g.session;
    s.finishes++;
    s.last = time;
    s.sum += time;
    if (s.best < 0 || time < s.best) s.best = time;
    const int bestTime = g.best.times.empty() ? -1 : g.best.times.back();
    g.newBest = bestTime < 0 || time < bestTime;
    // Your best sectors: from every finished run (a run with as many checkpoints as the best's).
    if (g.best.sectors.size() != run.times.size()) g.best.sectors.assign(run.times.size(), 0x7FFFFFFF);
    for (size_t i = 0; i < run.times.size(); i++) {
        const int sector = run.times[i] - (i ? run.times[i - 1] : 0);
        if (sector > 0 && sector < g.best.sectors[i]) g.best.sectors[i] = sector;
    }
    if (g.newBest) g.best.times = run.times;
    saveBest();
    TMVS_LOG("splits: finish %s%s (%d checkpoints, %d respawns)", timeText(time).c_str(), g.newBest ? ", your new best on this map" : "",
             static_cast<int>(run.times.size()), run.respawns);
    const int effect = options::current().finishEffect;
    if (effect > 0) startEffect(effect == 2 && g.newBest);
}

} // namespace

void tick() {
    const Options& o = options::current();
    if (!o.splits && o.finishEffect == 0) {
        g.present = false;
        return;
    }
    // The map: every half second (a new one loads your best on it).
    if (g.mapCheck-- <= 0) {
        g.mapCheck = 30;
        std::string name, key;
        if (!game::currentMap(name, key)) key.clear();
        if (key != g.mapKey) {
            g.mapKey = key;
            g.mapName = name;
            g.session = Session{};
            loadBest();
            g.run = Run{};
            g.lastCount = 0;
            if (!key.empty())
                TMVS_LOG("splits: map %s, your best %s (%d checkpoints)", name.c_str(),
                         timeText(g.best.times.empty() ? -1 : g.best.times.back()).c_str(), static_cast<int>(g.best.times.size()));
        }
    }
    game::Race r;
    g.present = !g.mapKey.empty() && game::readRace(r);
    // Where reading the run ends, once per change (to learn the game's other modes from the log).
    static std::string s_status;
    if (!g.mapKey.empty() && s_status != game::raceStatus()) {
        s_status = game::raceStatus();
        static int lines = 0;
        if (lines++ < 20) TMVS_LOG("splits: your run: %s", s_status.c_str());
    }
    if (!g.present) return;
    Run& run = g.run;
    // A new run: the race's checkpoints went back (a restart), or it runs again after a finish.
    if (r.count < g.lastCount || (run.finished && r.state != 2)) newRun(r);
    if (!run.started && r.state == 1) {
        run.started = true;
        g.session.attempts++;
    }
    for (int i = static_cast<int>(run.times.size()); i < r.count; i++) {
        run.times.push_back(r.times[i]);
        run.speeds.push_back(r.speed);
        g.cpAt = now();
        g.cpHasDelta = i < static_cast<int>(g.best.times.size()) && g.best.times.size() > 0;
        if (g.cpHasDelta) g.cpDelta = r.times[i] - g.best.times[i];
    }
    run.respawns = r.respawns - run.respawnsAtStart;
    if (r.state == 2 && !run.finished && r.count > 0 && static_cast<int>(run.times.size()) == r.count) finish(r.times[r.count - 1]);
    if (g.lastState != r.state && g.lastState >= 0) {
        static int lines = 0;
        if (lines++ < 20) TMVS_LOG("splits: race state %d -> %d (%d checkpoints)", g.lastState, r.state, r.count);
    }
    g.lastState = r.state;
    g.lastCount = r.count;
}

bool wantsOverlay() {
    const Options& o = options::current();
    return (o.splits && g.present) || !g_pieces.empty();
}

void drawOverlay(bool menuOpen, bool hide) {
    if (hide) {
        g_pieces.clear();
        return;
    }
    drawEffect();
    const Options& o = options::current();
    if (!o.splits || !g.present) return;
    const float scale = tmshaders::menu::scale();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float width = 250.0f * scale;
    ImGui::SetNextWindowPos(ImVec2(display.x - width - 16.0f * scale, 120.0f * scale), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.5f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    (void)menuOpen;
    if (ImGui::Begin("##tmvs_splits", nullptr, flags)) {
        const Run& run = g.run;
        const int n = static_cast<int>(run.times.size());
        if (!n && g.best.times.empty()) ImGui::TextDisabled("Splits: no best run yet");
        else if (!n) ImGui::TextDisabled("Splits: your best %s", timeText(g.best.times.back()).c_str());
        for (int i = std::max(0, n - kRows); i < n; i++) {
            const bool last = run.finished && i == n - 1;
            if (last) ImGui::Text("Finish");
            else ImGui::Text("CP %d", i + 1);
            ImGui::SameLine(width * 0.3f);
            ImGui::TextUnformatted(timeText(run.times[i]).c_str());
            if (i < static_cast<int>(g.best.times.size())) {
                const int d = run.times[i] - g.best.times[i];
                ImGui::SameLine(width * 0.68f);
                ImGui::TextColored(deltaColour(d), "%s", deltaText(d).c_str());
            }
        }
        // The newest delta, big, for a few seconds.
        if (g.cpHasDelta && g.cpAt >= 0.0 && now() - g.cpAt < kDeltaSeconds) {
            ImGui::SetWindowFontScale(1.5f);
            ImGui::TextColored(deltaColour(g.cpDelta), "%s", deltaText(g.cpDelta).c_str());
            ImGui::SetWindowFontScale(1.0f);
        }
        if (run.finished && g.newBest) ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Your new best");
        if (o.session) {
            const Session& s = g.session;
            ImGui::Separator();
            ImGui::Text("Session: %d runs, %d finishes", s.attempts, s.finishes);
            ImGui::Text("Best %s   Last %s", timeText(s.best).c_str(), timeText(s.last).c_str());
            if (s.finishes) ImGui::Text("Average %s", timeText(static_cast<int>(s.sum / s.finishes)).c_str());
            const int ideal = theoreticalBest();
            ImGui::Text("Your best %s", timeText(g.best.times.empty() ? -1 : g.best.times.back()).c_str());
            if (ideal > 0) ImGui::Text("Best sectors %s", timeText(ideal).c_str());
            if (run.started) ImGui::TextDisabled("Respawns %d", run.respawns);
        }
    }
    ImGui::End();
}

void drawPage() {
    Options& o = options::current();
    bool changed = ImGui::Checkbox("Split times", &o.splits);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("At each checkpoint: your time and the gap to your best run on this map.\nRead from the game; allowed online.");
    ImGui::BeginDisabled(!o.splits);
    changed |= ImGui::Checkbox("Session panel", &o.session);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Runs, finishes, best, last and average of this session, under the split times.");
    ImGui::EndDisabled();
    static const char* kEffects[] = {"Off", "Subtle", "Big"};
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
    changed |= ImGui::Combo("Finish effect", &o.finishEffect, kEffects, IM_ARRAYSIZE(kEffects));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Confetti at the finish. Big: fireworks for a new best.");
    if (changed) options::save();
    ImGui::Separator();
    if (g.mapKey.empty()) {
        ImGui::TextDisabled("No map loaded");
    } else {
        ImGui::Text("Map: %s", g.mapName.c_str());
        ImGui::Text("Your best: %s (%d checkpoints)", timeText(g.best.times.empty() ? -1 : g.best.times.back()).c_str(),
                    static_cast<int>(g.best.times.size()));
        if (!g.best.times.empty() || !g.best.sectors.empty()) {
            if (ImGui::Button("Forget best")) {
                g.best = Best{};
                saveBest();
                TMVS_LOG("splits: your best on %s forgotten", g.mapName.c_str());
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Your best run and sectors on this map start over.");
        }
        if (!g.present && o.splits) ImGui::TextDisabled("No run of yours now: %s", game::raceStatus());
    }
}

} // namespace splits
} // namespace tmvs_mp
