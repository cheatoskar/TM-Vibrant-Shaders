// Main camera test (src/maincamera.h): TrackMania Sunrise's view into a 512x512 texture, drawn before the
// main camera in every frame, must not be the main camera, so the pipeline keeps one size (1.6.0-rc.1
// made all its targets again twice a frame: 2677 "render targets created" lines in 4 minutes).
// The screen's own cameras, the video export's frame and a smaller screen after a reset still count.
// Exit code 0 = all passed.
#include "maincamera.h"
#include <cstdio>

using namespace tmshaders::maincamera;

namespace {

int g_failed = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            std::printf("FAILED line %d: %s\n", __LINE__, #cond); \
            g_failed++;                                            \
        }                                                          \
    } while (0)

Target camera(unsigned w, unsigned h, bool screensDepth) {
    Target t;
    t.width = t.depthWidth = w;
    t.height = t.depthHeight = h;
    t.depthReadable = true;
    t.depthIsScreens = screensDepth;
    return t;
}

// As isMainTarget on the 2006 engine: the size rule, the views, then the history. Counts the
// changes of the shaded size (each one = the pipeline's targets made again).
struct Frames {
    SizeHistory history;
    unsigned frame = 0, lastW = 0, lastH = 0, resizes = 0, shaded = 0, refused = 0;
    bool main(const Target& t, unsigned refW, unsigned refH, bool exporting = false) {
        if (!sameSizeAsDepth(t)) return false;
        if (isTextureView(t, refW, refH)) {
            refused++;
            return false;
        }
        if (!exporting && !history.accept(t.width, t.height, frame)) {
            refused++;
            return false;
        }
        shaded++;
        if (t.width != lastW || t.height != lastH) {
            resizes++;
            lastW = t.width;
            lastH = t.height;
        }
        return true;
    }
};

} // namespace

int main() {
    // The rule as before: a readable depth of the target's size, at least 320 wide.
    CHECK(sameSizeAsDepth(camera(2560, 1440, true)));
    CHECK(sameSizeAsDepth(camera(512, 512, false)));
    CHECK(!sameSizeAsDepth(camera(256, 256, false)));
    {
        Target t = camera(2560, 1440, false);
        t.depthWidth = 1280;
        CHECK(!sameSizeAsDepth(t));
        t = camera(2560, 1440, false);
        t.depthReadable = false;
        CHECK(!sameSizeAsDepth(t));
    }

    // Views into textures: smaller than the screen, their own depth buffer.
    CHECK(isTextureView(camera(512, 512, false), 2560, 1440));
    CHECK(isTextureView(camera(512, 512, false), 640, 480)); // a low resolution: by the area
    CHECK(!isTextureView(camera(2560, 1440, false), 2560, 1440)); // a replay's camera into a screen-sized texture
    CHECK(!isTextureView(camera(3840, 2160, false), 2560, 1440)); // larger than the screen
    CHECK(!isTextureView(camera(512, 512, true), 2560, 1440));    // the screen's own depth never makes a view
    CHECK(!isTextureView(camera(512, 512, false), 0, 0));         // no reference: as before

    // Sunrise at 1440p, the log of 2026-11-13: per frame the 512x512 view, then the screen. One size
    // for the whole race, the view never shaded.
    {
        Frames f;
        for (f.frame = 1; f.frame <= 600; f.frame++) {
            CHECK(!f.main(camera(512, 512, false), 2560, 1440));
            CHECK(f.main(camera(2560, 1440, true), 2560, 1440));
        }
        std::printf("Sunrise 1440p: %u resizes, %u shaded, %u refused in 600 frames\n", f.resizes, f.shaded, f.refused);
        CHECK(f.resizes == 1);
        CHECK(f.shaded == 600);
    }

    // The same view without the view rule (no reference known): the history alone keeps the size.
    // The first frame may take the view (it comes first), from then on the screen.
    {
        Frames f;
        for (f.frame = 1; f.frame <= 600; f.frame++) {
            f.main(camera(512, 512, false), 0, 0);
            CHECK(f.main(camera(2560, 1440, true), 0, 0));
        }
        std::printf("history alone: %u resizes in 600 frames\n", f.resizes);
        CHECK(f.resizes <= 2);
    }

    // Camera blends (two cameras of the screen's size in a frame): both shaded, no resize.
    {
        Frames f;
        for (f.frame = 1; f.frame <= 100; f.frame++) {
            CHECK(f.main(camera(2560, 1440, true), 2560, 1440));
            CHECK(f.main(camera(2560, 1440, false), 2560, 1440));
        }
        CHECK(f.resizes == 1);
        CHECK(f.shaded == 200);
    }

    // A smaller screen after a device reset (the history is reset): shaded at once.
    {
        Frames f;
        for (f.frame = 1; f.frame <= 10; f.frame++) f.main(camera(2560, 1440, true), 2560, 1440);
        f.history.reset();
        CHECK(f.main(camera(1280, 720, true), 1280, 720));
        f.frame++;
        CHECK(f.main(camera(1280, 720, true), 1280, 720));
        CHECK(f.resizes == 2);
    }

    // The larger camera gone for longer than the hold (a menu that draws only a smaller one): the
    // smaller one takes over after kHoldFrames, not before.
    {
        Frames f;
        f.frame = 1;
        f.main(camera(2560, 1440, false), 0, 0);
        unsigned firstShaded = 0;
        for (f.frame = 2; f.frame < 40 && !firstShaded; f.frame++)
            if (f.main(camera(1280, 720, false), 0, 0)) firstShaded = f.frame;
        std::printf("larger camera gone: the smaller one shaded from frame %u\n", firstShaded);
        CHECK(firstShaded == 2 + SizeHistory::kHoldFrames);
    }

    // The video export at 1920x1080 on a 1440p screen: its frame is the reference, its camera counts,
    // Sunrise's view still doesn't, and no history while it runs.
    {
        Frames f;
        for (f.frame = 1; f.frame <= 50; f.frame++) {
            CHECK(!f.main(camera(512, 512, false), 1920, 1080, true));
            CHECK(f.main(camera(1920, 1080, false), 1920, 1080, true));
            CHECK(f.main(camera(2560, 1440, true), 1920, 1080, true)); // a pass at the screen's size
        }
    }

    // Equal area, another shape, while the main size is held: not a second main camera.
    {
        SizeHistory h;
        CHECK(h.accept(1024, 512, 1));
        CHECK(!h.accept(512, 1024, 1));
        CHECK(h.accept(1024, 512, 2));
        CHECK(h.width() == 1024 && h.height() == 512);
    }

    // The frame counter wrapping around.
    {
        SizeHistory h;
        CHECK(h.accept(2560, 1440, 0xFFFFFFFEu));
        CHECK(!h.accept(512, 512, 1u)); // 3 frames later
    }

    if (g_failed) {
        std::printf("%d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("all main camera checks passed\n");
    return 0;
}
