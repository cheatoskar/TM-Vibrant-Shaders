// perfcpu test: the perf line's CPU split (src/perfcpu.h) from synthetic frame timelines. One tick
// is one millisecond here; each case drives the tracker the way the hooks do in game and checks the
// sums per phase, the mod's share, the blocked time and which cycles count.
//
//   tmvs_perfcputest      (exit code 0 = passed)
#include "perfcpu.h"
#include <cmath>
#include <cstdio>

using namespace tmshaders::perfcpu;

namespace {

int g_failed = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        g_failed++;
        printf("FAIL: %s\n", what);
    }
}

bool near(double a, double b) {
    return std::fabs(a - b) < 1e-9;
}

double phase(const Totals& t, Phase p) {
    return t.phase[static_cast<int>(p)];
}
double mod(const Totals& t, Phase p) {
    return t.mod[static_cast<int>(p)];
}

// One frame of the game with the engine's calls, starting at `t`: begin 1 ms (of it the mod 0.5 at
// the start), rendering 6 ms (the mod's pipeline 2 ms in it, 0.25 of it blocked), end 9 ms (the mod's
// Present 1 ms, the driver's Present 0.5), logic 4 ms. Returns the next frame's start.
int64_t frame(Tracker& tr, int64_t t, bool shaded, bool presentCall = true) {
    tr.point(Point::FrameBeginEnter, t);
    tr.modEnter(t);
    tr.modLeave(t); // frameBeginTimed leaves the detour's scope at once (0 ms) ...
    tr.point(Point::FrameBeginDone, t + 1);
    tr.modEnter(t + 1); // ... and enters it again after RenderFrameBegin
    tr.modLeave(t + 1);
    // Rendering: the camera's end runs the pipeline, nested scopes count once.
    tr.modEnter(t + 3);
    tr.modEnter(t + 3);
    tr.blocked(0.25);
    tr.modLeave(t + 4);
    tr.modLeave(t + 5);
    tr.point(Point::FrameEndEnter, t + 7);
    tr.modEnter(t + 8);
    tr.present(t + 8, shaded);
    tr.modLeave(t + 9);
    if (presentCall) tr.presentCall(0.5);
    tr.point(Point::FrameEndDone, t + 16);
    return t + 20;
}

void engineFrames() {
    Tracker tr;
    int64_t t = 1000;
    for (int i = 0; i < 10; i++) t = frame(tr, t, true);
    tr.point(Point::FrameBeginEnter, t); // closes the tenth
    const Totals& s = tr.totals();
    // The first frame call starts the counting: cycles 1..9 closed by frames 2..10, the tenth by the last call.
    expect(s.cycles == 10, "ten cycles closed");
    expect(s.marks, "with the engine's calls");
    expect(near(phase(s, Phase::Begin), 10.0), "frame start 1 ms each");
    expect(near(phase(s, Phase::Render), 60.0), "rendering 6 ms each");
    expect(near(phase(s, Phase::End), 90.0), "frame end 9 ms each");
    expect(near(phase(s, Phase::Logic), 40.0), "logic 4 ms each");
    expect(near(s.total(), 200.0), "the phases add up to the frame time");
    expect(near(mod(s, Phase::Render), 20.0), "the pipeline's scope once (nested)");
    expect(near(mod(s, Phase::End), 10.0), "the mod's Present in the frame end");
    expect(near(mod(s, Phase::Begin), 0.0), "RenderFrameBegin itself is not the mod's");
    expect(near(s.blocked, 2.5), "blocked 0.25 each");
    expect(s.presentCalls == 10 && near(s.presentCall, 5.0), "the driver's Present 0.5 each");
}

void unshadedLeftOut() {
    Tracker tr;
    int64_t t = 0;
    t = frame(tr, t, true);
    t = frame(tr, t, false); // a menu frame
    t = frame(tr, t, true);
    tr.point(Point::FrameBeginEnter, t);
    expect(tr.totals().cycles == 2, "the unshaded frame is left out");
    expect(near(tr.totals().total(), 40.0), "only the shaded cycles' time");
}

void longGapLeftOut() {
    Tracker tr;
    int64_t t = 0;
    t = frame(tr, t, true);
    // A loading screen: 2 s between this frame's end and the next frame.
    t = frame(tr, t, true) + 2000;
    t = frame(tr, t, true);
    tr.point(Point::FrameBeginEnter, t);
    expect(tr.totals().cycles == 2, "the cycle with the 2 s gap is dropped");
}

void hostedNoPresentCall() {
    Tracker tr;
    int64_t t = 0;
    for (int i = 0; i < 3; i++) t = frame(tr, t, true, false);
    tr.point(Point::FrameBeginEnter, t);
    expect(tr.totals().presentCalls == 0, "no driver Present seen (Horizon Host calls it)");
    expect(tr.totals().cycles == 3, "the cycles count anyway");
}

void withoutEngineCalls() {
    Tracker tr;
    // Present to Present every 16 ms, the mod 3 ms of it, 1 ms blocked.
    int64_t t = 500;
    tr.present(t, true); // the first only starts
    for (int i = 0; i < 5; i++) {
        tr.modEnter(t + 2);
        tr.blocked(1.0);
        tr.modLeave(t + 5);
        t += 16;
        tr.present(t, true);
    }
    const Totals& s = tr.totals();
    expect(s.cycles == 5, "five Present-to-Present cycles");
    expect(!s.marks, "no engine calls");
    expect(near(phase(s, Phase::Logic), 80.0), "all of it in logic");
    expect(near(mod(s, Phase::Logic), 15.0), "the mod's share");
    expect(near(s.blocked, 5.0), "blocked");
}

void switchToEngineCalls() {
    // Present-only cycles first (a menu before the hooks' first frame), then the engine's calls:
    // nothing is counted twice across the switch.
    Tracker tr;
    int64_t t = 0;
    tr.present(t, true);
    tr.present(t + 16, true);
    t = 32;
    t = frame(tr, t, true);
    t = frame(tr, t, true);
    tr.point(Point::FrameBeginEnter, t);
    expect(tr.totals().cycles == 3, "one Present cycle, then two frame cycles");
    expect(near(tr.totals().total(), 16.0 + 40.0), "their time once");
}

void resetTotals() {
    Tracker tr;
    int64_t t = 0;
    t = frame(tr, t, true);
    t = frame(tr, t, true);
    tr.resetTotals();
    t = frame(tr, t, true);
    tr.point(Point::FrameBeginEnter, t);
    expect(tr.totals().cycles == 2, "the totals start again (the cycle open at the reset counts after it)");
}

} // namespace

int main() {
    engineFrames();
    unshadedLeftOut();
    longGapLeftOut();
    hostedNoPresentCall();
    withoutEngineCalls();
    switchToEngineCalls();
    resetTotals();
    // The global tracker runs on the real clock.
    expect(msPerTick() > 0.0, "QueryPerformanceFrequency");
    {
        ModScope scope;
        BlockedScope blocked;
    }
    if (g_failed) {
        printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    printf("perfcputest: all checks passed\n");
    return 0;
}
