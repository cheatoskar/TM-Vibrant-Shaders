// Tests auto quality's grace (src/autoquality.cpp) on synthetic frame sequences: a map's slow
// first seconds, a hitch and a pause decide no cut; a lasting slow frame rate still does.
// Without GPU timings (a pipeline that never ran), as on a driver without timestamps: one
// effect per decision. Exit code 0 = all passed.
#include "autoquality.h"
#include "log.h"
#include "pipeline.h"
#include <cstdio>

using namespace tmshaders;

static int g_failed = 0;
#define CHECK(...) \
    do { \
        if (!(__VA_ARGS__)) { \
            printf("FAIL line %d: %s\n", __LINE__, #__VA_ARGS__); \
            g_failed++; \
        } \
    } while (0)

static Pipeline g_pipeline; // no device: every pass 0 ms

struct Run {
    AutoQuality aq;
    Settings s;
    double t = 0.0;
    double firstCut = -1.0;
    Run() { s.targetFps = 109.0f; }
    // Frames at fps for seconds (gameplay = a race is shaded).
    void frames(double fps, double seconds, bool gameplay = true) {
        const double end = t + seconds;
        while (t < end) {
            t += 1.0 / fps;
            aq.onFrame(t, gameplay, g_pipeline, s);
            if (firstCut < 0.0 && aq.reducedCount() > 0) firstCut = t;
        }
    }
    void pause(double seconds) { t += seconds; }
};

int main() {
    log::setFileName(L"tmvs_aqtest.log"); // never the game's log
    // The owner's map start (16:58:55 - 16:59:07): menus, the map's first 3D frame, a hitch at
    // 8 s, 76 FPS until 13 s, then the GPU's real 120 FPS. Before: cut at the 8 s grace's end.
    {
        Run r;
        r.frames(60.0, 5.0, false);
        r.frames(76.0, 8.0);
        r.pause(0.28);
        r.frames(76.0, 5.0);
        r.frames(120.0, 60.0);
        printf("map start dip: first cut at %.1f s (none = -1)\n", r.firstCut);
        CHECK(r.firstCut < 0.0);
    }
    // A GPU that is slow for good still gets its cut, once the grace (20 s) and a window are over.
    {
        Run r;
        r.frames(76.0, 60.0);
        printf("lasting 76 FPS: first cut at %.1f s\n", r.firstCut);
        CHECK(r.firstCut >= 22.0 && r.firstCut < 26.0);
    }
    // Mid-race: a hitch (0.5 s) holds decisions 4 s, then a fresh 2 s window.
    {
        Run r;
        r.frames(120.0, 40.0);
        r.pause(0.5);
        const double hitch = r.t;
        r.frames(76.0, 3.9);
        r.frames(120.0, 30.0);
        printf("hitch, 3.9 s at 76 FPS: first cut at %.1f s\n", r.firstCut);
        CHECK(r.firstCut < 0.0);
        Run q;
        q.frames(120.0, 40.0);
        q.pause(0.5);
        q.frames(76.0, 20.0);
        printf("hitch, then 76 FPS: first cut %.1f s after it\n", q.firstCut - hitch);
        CHECK(q.firstCut - hitch >= 6.0 && q.firstCut - hitch < 9.0);
    }
    // Photo mode / alt-tab / device reset: frames stop for seconds, 20 s grace after.
    {
        Run r;
        r.frames(120.0, 40.0);
        r.pause(25.0);
        const double back = r.t;
        r.frames(76.0, 18.0);
        r.frames(120.0, 30.0);
        printf("pause, 18 s at 76 FPS: first cut at %.1f s (none = -1)\n", r.firstCut < 0.0 ? -1.0 : r.firstCut - back);
        CHECK(r.firstCut < 0.0);
    }
    printf(g_failed ? "%d FAILED\n" : "all passed\n", g_failed);
    return g_failed ? 1 : 0;
}
