#include "perflog.h"
#include "gfx.h"
#include "log.h"
#include "pipeline.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tmshaders {
namespace perflog {
namespace {

constexpr int kSets = 4;            // frames in flight (the game lets the GPU run up to 3 behind)
constexpr unsigned kWindow = 2000;  // shaded frames per log line
constexpr int kStamps = static_cast<int>(Mark::Count) + 1; // the marks, then Present
constexpr int kPresent = static_cast<int>(Mark::Count);
constexpr int kTopPasses = 8;

struct QuerySet {
    IDirect3DQuery9* disjoint = nullptr;
    IDirect3DQuery9* frequency = nullptr;
    IDirect3DQuery9* stamps[kStamps] = {};
    bool issued[kStamps] = {};
    bool pending = false;
    unsigned frame = 0;
};

QuerySet g_sets[kSets];
int g_current = -1;       // the set this frame's marks go to, -1 none
int g_next = 0;
unsigned g_frame = 0;
bool g_failed = false;     // no timestamp queries on this device
UINT64 g_lastPresent = 0;  // the last resolved frame's Present stamp
unsigned g_lastPresentFrame = 0;
bool g_havePresent = false;

struct Window {
    unsigned shaded = 0;
    double frameMs = 0.0;            // CPU: Present to Present, shaded frames
    double gameMs = 0.0, modMs = 0.0, gpuFrameMs = 0.0;
    unsigned game = 0, mod = 0, gpuFrames = 0;
    double cpuMs = 0.0;              // the pipeline's render calls
    double castersFrom = -1.0;       // casters::cpuMs at the window's start
    double pipelineMs = 0.0;
    std::vector<double> passes;
} g_window;
LARGE_INTEGER g_lastFrame{};
double g_toMs = 0.0;
double g_frameCpu = 0.0; // this frame's addCpu

void releaseSet(QuerySet& s) {
    gfx::release(s.disjoint);
    gfx::release(s.frequency);
    for (auto& q : s.stamps) gfx::release(q);
    s.pending = false;
}

bool ensureSet(IDirect3DDevice9* device, QuerySet& s) {
    if (s.disjoint) return true;
    bool ok = SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &s.disjoint)) &&
              SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &s.frequency));
    for (auto& q : s.stamps) ok = ok && SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q));
    if (!ok) releaseSet(s);
    return ok;
}

// The set's results if they are there (never waits).
void resolve(QuerySet& s) {
    if (!s.pending) return;
    BOOL disjoint = TRUE;
    UINT64 frequency = 0;
    UINT64 t[kStamps] = {};
    if (s.disjoint->GetData(&disjoint, sizeof(disjoint), 0) != S_OK) return;
    if (s.frequency->GetData(&frequency, sizeof(frequency), 0) != S_OK) return;
    for (int i = 0; i < kStamps; i++) {
        if (s.issued[i] && s.stamps[i]->GetData(&t[i], sizeof(UINT64), 0) != S_OK) return;
    }
    s.pending = false;
    if (disjoint || frequency == 0) {
        g_havePresent = false;
        return;
    }
    const double toMs = 1000.0 / static_cast<double>(frequency);
    const int begin = static_cast<int>(Mark::GameBegin), gameEnd = static_cast<int>(Mark::GameEnd), modEnd = static_cast<int>(Mark::ModEnd);
    if (s.issued[begin] && s.issued[gameEnd] && t[gameEnd] >= t[begin]) {
        g_window.gameMs += static_cast<double>(t[gameEnd] - t[begin]) * toMs;
        g_window.game++;
    }
    if (s.issued[gameEnd] && s.issued[modEnd] && t[modEnd] >= t[gameEnd]) {
        g_window.modMs += static_cast<double>(t[modEnd] - t[gameEnd]) * toMs;
        g_window.mod++;
    }
    if (s.issued[kPresent]) {
        if (g_havePresent && s.frame == g_lastPresentFrame + 1 && t[kPresent] > g_lastPresent && s.issued[begin]) {
            g_window.gpuFrameMs += static_cast<double>(t[kPresent] - g_lastPresent) * toMs;
            g_window.gpuFrames++;
        }
        g_lastPresent = t[kPresent];
        g_lastPresentFrame = s.frame;
        g_havePresent = true;
    }
}

void logWindow(const Pipeline& pipeline, double castersCpuMs) {
    Window& w = g_window;
    const double n = w.shaded ? static_cast<double>(w.shaded) : 1.0;
    const double frameMs = w.frameMs / n;
    std::string top;
    std::vector<int> order;
    for (int p = 0; p < static_cast<int>(w.passes.size()); p++) {
        if (strcmp(Pipeline::passName(p), "Casters") != 0) order.push_back(p);
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) { return w.passes[a] > w.passes[b]; });
    char item[64];
    for (int i = 0; i < kTopPasses && i < static_cast<int>(order.size()) && w.passes[order[i]] / n >= 0.005; i++) {
        snprintf(item, sizeof(item), "%s%s %.2f", top.empty() ? "" : ", ", Pipeline::passName(order[i]), w.passes[order[i]] / n);
        top += item;
    }
    double casters = 0.0;
    for (int p = 0; p < static_cast<int>(w.passes.size()); p++) {
        if (!strcmp(Pipeline::passName(p), "Casters")) casters = w.passes[p] / n;
    }
    const double game = w.game ? w.gameMs / w.game : 0.0, mod = w.mod ? w.modMs / w.mod : 0.0;
    const double gpuFrame = w.gpuFrames ? w.gpuFrameMs / w.gpuFrames : 0.0;
    const double recording = w.castersFrom >= 0.0 ? (castersCpuMs - w.castersFrom) / n : 0.0;
    if (w.game == 0 && w.mod == 0) {
        TMVS_LOG("perf: %.0f FPS (%.2f ms a frame) over %u shaded frames; no GPU timestamps; CPU ms a frame: mod %.2f, caster recording %.2f",
                 frameMs > 0.0 ? 1000.0 / frameMs : 0.0, frameMs, w.shaded, w.cpuMs / n, recording);
        return;
    }
    TMVS_LOG("perf: %.0f FPS (%.2f ms a frame) over %u shaded frames; GPU ms a frame: game 3D %.2f, mod %.2f (passes %.2f: %s; "
             "caster maps %.2f), rest of the GPU frame %.2f; CPU ms a frame: mod %.2f, caster recording %.2f",
             frameMs > 0.0 ? 1000.0 / frameMs : 0.0, frameMs, w.shaded, game, mod, w.pipelineMs / n, top.c_str(), casters,
             gpuFrame > 0.0 ? std::max(0.0, gpuFrame - game - mod) : 0.0, w.cpuMs / n, recording);
}

} // namespace

void mark(IDirect3DDevice9*, Mark which) {
    if (g_current < 0) return;
    QuerySet& s = g_sets[g_current];
    const int i = static_cast<int>(which);
    if (s.issued[i]) return; // the first main camera of the frame only
    // Marks out of order (a second camera) don't count.
    if (which == Mark::GameEnd && !s.issued[static_cast<int>(Mark::GameBegin)]) return;
    if (which == Mark::ModEnd && !s.issued[static_cast<int>(Mark::GameEnd)]) return;
    s.stamps[i]->Issue(D3DISSUE_END);
    s.issued[i] = true;
}

void addCpu(double ms) {
    g_frameCpu += ms;
}

void frame(IDirect3DDevice9* device, const Pipeline& pipeline, bool shaded, double castersCpuMs) {
    if (g_toMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_toMs = 1000.0 / static_cast<double>(f.QuadPart);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double frameMs = g_lastFrame.QuadPart ? static_cast<double>(now.QuadPart - g_lastFrame.QuadPart) * g_toMs : 0.0;
    g_lastFrame = now;

    // This frame's set: Present closes it.
    if (g_current >= 0) {
        QuerySet& s = g_sets[g_current];
        s.stamps[kPresent]->Issue(D3DISSUE_END);
        s.issued[kPresent] = true;
        s.disjoint->Issue(D3DISSUE_END);
        s.frequency->Issue(D3DISSUE_END);
        s.pending = true;
        g_current = -1;
    }
    for (int k = 0; k < kSets; k++) resolve(g_sets[(g_next + k) % kSets]);

    // A shaded frame (loading screens and long stalls left out) counts towards the window.
    if (shaded && frameMs > 0.0 && frameMs < 250.0) {
        Window& w = g_window;
        if (w.castersFrom < 0.0) w.castersFrom = castersCpuMs;
        w.shaded++;
        w.frameMs += frameMs;
        w.cpuMs += g_frameCpu;
        w.pipelineMs += pipeline.totalTime();
        w.passes.resize(static_cast<size_t>(pipeline.passCount()), 0.0);
        for (int p = 0; p < pipeline.passCount(); p++) w.passes[p] += pipeline.passTime(p);
        if (w.shaded >= kWindow) {
            logWindow(pipeline, castersCpuMs);
            g_window = Window();
        }
    }
    g_frameCpu = 0.0;

    // The next frame's set, unless the GPU is that far behind (then this frame isn't measured).
    if (g_failed) return;
    QuerySet& next = g_sets[g_next];
    if (next.pending) return;
    if (!ensureSet(device, next)) {
        g_failed = true;
        TMVS_LOG("perf: no GPU timestamp queries, the log's GPU times stay empty");
        return;
    }
    memset(next.issued, 0, sizeof(next.issued));
    next.frame = ++g_frame;
    next.disjoint->Issue(D3DISSUE_BEGIN);
    g_current = g_next;
    g_next = (g_next + 1) % kSets;
}

void release() {
    for (auto& s : g_sets) releaseSet(s);
    g_current = -1;
    g_havePresent = false;
    g_failed = false; // a new device may have the queries (CreateQuery failed during a reset)
}

bool bracketOpen() {
    return g_current >= 0;
}

} // namespace perflog
} // namespace tmshaders
