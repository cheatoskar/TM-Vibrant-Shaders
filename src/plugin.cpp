#include "plugin.h"
#include "audio.h"
#include "capture.h"
#include "capwriter.h"
#include "casters.h"
#include "perflog.h"
#include "config.h"
#include "depth.h"
#include "engine.h"
#include "framecap.h"
#include "gameshaders.h"
#include "gfx.h"
#include "hook.h"
#include "keys.h"
#include "log.h"
#include "lut.h"
#include "overlay.h"
#include "pipeline.h"
#include "autoquality.h"
#include "blockorder.h"
#include "photo.h"
#include "planeread.h"
#include "thumbnails.h"
#include "tracer.h"
#include "videofile.h"
#include "soundcapture.h"
#include "videoencoder.h"
#include "tm_shaders_version.h"
#include <d3d9.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <numeric>
#include <utility>
#include <vector>

namespace tmshaders {
namespace plugin {
namespace {

HMODULE g_module = nullptr;
std::wstring g_moduleDir;
IDirect3DDevice9* g_device = nullptr;
unsigned g_frame = 0;
int g_cameraIndex = 0;
LARGE_INTEGER g_startTime{};
LARGE_INTEGER g_frequency{};

// Development automation, read from <Documents>\TrackMania\TMVS\dev.ini [dev].
// Times are seconds since the plugin started.
struct DevOptions {
    float traceAt = 0.0f;        // write a frame trace (0 = off)
    int traceVideoFrame = 0;     // trace the video export's n-th frame on its clock (0 = off)
    float captureAt = 0.0f;      // start writing .tmcap captures (0 = off)
    float captureInterval = 2.0f;
    int captureCount = 1;
    bool fakeBlockRank = false;  // "FakeBlockRank=1": OverlapFix takes a rank from the vertex buffer's address (0..7) instead of blockorder, to test the bias without the engine side
    float exitAt = 0.0f;         // terminate the game (0 = off)
    float hardExitAt = 0.0f;     // terminate even if no gameplay camera appeared (absolute time)
    // "Keys=4:13,7:a13,9:min,12:restore": post VK 13 at 4 s, Alt+Enter at 7 s, minimize / restore.
    // Photo mode: "photomode" (like F6), "photo" / "photohires" (like F12 / Shift+F12 in it).
    std::vector<std::pair<float, std::wstring>> keys;
    std::vector<std::pair<float, std::wstring>> sceneKeys; // "SceneKeys=": the same, timed from the first gameplay frame
    std::wstring shaderDir;      // load shaders from here (hot reload with F9)
} g_dev;
float g_nextCaptureAt = 0.0f;
float g_sceneStart = -1.0f; // first frame with a gameplay camera (near plane < 1 m)
size_t g_nextKey = 0;
size_t g_nextSceneKey = 0;

// Main-camera state for the frame being rendered.
struct SceneState {
    bool inMainCamera = false;
    bool haveView = false;
    bool haveProjection = false;
    float view[16] = {};
    float projection[16] = {};
    float lightDirection[3] = {};
    float lightColor[3] = {1.0f, 1.0f, 1.0f};
    bool haveLight = false;
    // Replay camera blends render two main cameras per frame (one into a texture that is
    // blended over the other). Both get the shaders, or the blend shows a "ghost" of the
    // unprocessed image.
    int processedCount = 0;                  // main cameras processed this frame
    IDirect3DSurface9* processedTarget = nullptr; // identity only, not referenced
    int lastFrameCount = 0;
    bool loggedMultiCamera = false;
    // The matrices the engine computed for this main camera before it began
    // (engine::cameraMatrices), for the video export's samples.
    float engineView[16] = {};
    float engineProjection[16] = {};
    bool haveEngine = false;
    int mainRenders = 0;  // main camera renders this frame (shaded or not)
    int otherRenders = 0; // other cameras into a target of the video's size this frame (export only)
} g_scene;
float g_depthUlp = 1.0f / 16777216.0f; // step of the main camera's depth buffer (OverlapFix), set at its begin

bool g_captureRequested = false;
bool g_capturePending = false; // F12 pressed, taken when the writer is free (g_captureRequested)
// F12 with the material masks on: the next main camera tags every depth-writing draw with
// its material's own id (1..255) instead of its class, and that frame is captured with the
// ids (capture_NNN.mat): which material drew each pixel, by name.
bool g_idFrame = false;
struct IdMaterial {
    const void* shader;
    int draws;
    bool depthWrite; // tagged in the stencil (else: drawn without depth writes, no pixels)
    bool additive;   // one-one blending (glows, headlight pools)
};
std::vector<IdMaterial> g_idMaterials; // id = index + 1
int g_capturesWritten = 0;
Pipeline g_pipeline;
AutoQuality g_autoQuality;
bool g_shadedThisFrame = false;
bool g_gameplayThisFrame = false; // a gameplay camera (not a menu background) was shaded
bool g_gameplayLastFrame = false; // and in the frame before (at the next frame's start)
float g_jitterUV[2] = {};         // TAA jitter of this frame (uv shift of the image)
UINT g_frameWidth = 0, g_frameHeight = 0;
bool g_pipelineFailed = false; // don't retry a failed compile every frame; F9 retries
gfx::Target g_sceneCopy;
// Photo mode's compare split with a preset on the other side: that look from a pipeline of its
// own. Its TAA, exposure, bounce light, focus and sky history stay its own: one pipeline's
// history shared by two looks would blend each into the other. Only while it is shown (it
// doubles the effects' cost and memory).
Pipeline g_pipelineB;
bool g_pipelineBFailed = false;
gfx::Target g_splitOut;  // the preset's side, the whole frame
float g_casterSun[3] = {}; // the light the caster maps were drawn for this frame (side A)
bool g_casterSunSet = false;
// A camera that draws into part of the screen (the MediaTracker editor's 3D view between its
// panels): its depth and the shaded image, the size of that part.
gfx::Target g_partDepth;
gfx::Target g_partOut;
// GameHdr's float scene and its added light cut to that part, so the part is shaded from the
// game's float image like a full-screen camera (EngineLights). Without them the replay after a
// race (its 3D view is such a part, between the menu's bars) fell back to the 8-bit guesses:
// under Real night A01's replay came out lit golden while the race was at night (capture_125).
gfx::Target g_partHdr;
gfx::Target g_partAdded;
// The game's colour clears of this frame that reached the top and the bottom row of their
// target (only compared, never used): rows it cleared itself are no leftovers (see fillBars).
struct ClearedRows {
    IDirect3DSurface9* target = nullptr;
    unsigned frame = ~0u;
    // What the log needs to tell a real clear from a wrong guess: the rows it covered (viewport,
    // rects and scissor rect), its colour, the scissor test.
    LONG top = 0, bottom = 0;
    D3DCOLOR color = 0;
    DWORD rects = 0;
    bool scissor = false;
};
ClearedRows g_clearedTop, g_clearedBottom;
// GameHdr: the main camera draws into a float target instead of the game's 8-bit one, so the
// light the game computes above 1.0 isn't cut (captures for working out the tone curve). At
// the camera's end it is resolved into the game's target, cut to 0..1 (the 8-bit copy the
// guesses, the HUD and the photo split use); the lighting pass takes the float image itself.
gfx::Target g_hdrScene;
IDirect3DSurface9* g_hdrGame = nullptr;     // the game's target while the float one stands in (AddRef'd)
IDirect3DSurface9* g_hdrLastGame = nullptr; // the same after the camera (not held): handed back if the game binds the float one
bool g_hdrValid = false;                    // g_hdrScene holds this main camera's image
int g_hdrSupported = -1;                    // float render target with blending: -1 not checked yet
// The light the game adds on top: its one-one blended draws (TAdd glows over whole blocks, the
// lamps' halos, headlight pools) go into a float target of their own during the camera, and it
// is added back into g_hdrScene at the end (addition doesn't care about order: the image stays
// the same). What remains is pixel-exact: where a glow's quad adds nothing, it is 0 (the stencil
// bit marked the whole quad). The lighting pass can take light sources from it instead of
// guessing them in the image.
gfx::Target g_hdrAdded;
bool g_hdrAddedActive = false; // cleared at this camera's start, takes the additive draws
bool g_hdrAddedBound = false;  // the render target during the current (additive) draw
bool g_hdrAddedValid = false;  // holds the last main camera's added light
bool g_castersOn = false;      // CasterShadows this frame: the main camera's meshes are recorded (casters.cpp)
bool g_casterAlphaTest = false; // CasterAlphaTest this frame: the alpha-tested ones too
// GameGBuffer: the game's patched pixel shaders (gameshaders.h) also write the world normal and the
// distance from the eye of what they draw into COLOR1. During the main camera this target is the
// second render target for exactly those draws: opaque ones (depth writes, no blending: the blend
// would act on the normals too) into the float scene. Every other draw (other shaders, glows, the
// game's effect passes) finds no second target bound. The game's clears and target switches unbind
// it first (a Clear would wipe it). Cleared to 0 (no normal) at the camera's start; the pipeline takes
// a normal only where its distance matches the depth buffer, so a surface drawn later by a shader
// without normals (a road over a block) keeps the normal from the depth.
gfx::Target g_gbuf;            // A16B16G16R16F: world normal, distance from the eye (0: none)
bool g_gbufActive = false;     // this main camera: cleared, takes the patched opaque draws
bool g_gbufBound = false;      // bound as render target 1 right now
bool g_gbufValid = false;      // holds the last main camera's normals
bool g_gameMrt = false;        // the game binds a second target itself: hands off
int g_mrtSupported = -1;       // two render targets at once: -1 not checked yet
int g_mrtCount = 0;            // how many at once (GameLight needs three)
// GameLight: the light the patched shaders lit each surface with (COLOR2), the third target for
// the same draws as the normals (every shader patched for it writes both; it is bound after the
// normal target and unbound before it, so the targets never leave a gap). Its own distance in w:
// the pipeline takes the light only where it belongs to the surface the depth shows.
gfx::Target g_gbufLight;       // A16B16G16R16F: light rgb, distance from the eye (0: none)
bool g_gbufLightActive = false; // this main camera: cleared, takes the light of the patched draws
bool g_gbufLightBound = false; // bound as render target 2 right now
bool g_gbufLightValid = false; // holds the last main camera's light
// The last main camera's opaque draws (depth writes, no blending), for the log.
struct GBufferDraws {
    unsigned opaque = 0, written = 0, otherShader = 0, noShader = 0, otherTarget = 0, binds = 0, light = 0;
} g_gbufDraws, g_gbufCounting;

float seconds() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<float>(static_cast<double>(now.QuadPart - g_startTime.QuadPart) / static_cast<double>(g_frequency.QuadPart));
}

// The game's video export (engine::videoExport), with VideoExportBest on; looked at once per
// frame. Its clock runs at a fixed step: a frame of the video may take seconds to render, and
// on the real clock the exposure adapted, the focus pulled, rain fell and the clouds drifted
// that much faster in the video (and unevenly: as fast as each frame happened to render).
// While it steps, the effects run on the video's clock: one step per rendered frame. Without
// an export it is the real clock; after one it goes on from where the video ended (no jump).
bool g_exporting = false;
engine::VideoExport g_export;
double g_videoClock = -1.0;  // >= 0: the export's clock (s), while it runs at a fixed step
float g_clockOffset = 0.0f;  // added to the real clock after an export (0 until the first one)
struct ExportStats {
    unsigned frames = 0;      // rendered frames of the export
    unsigned stepped = 0;     // of them on the video's clock
    unsigned before = 0;      // before it stepped (its dialog, the first frame)
    unsigned after = 0;       // after it stepped: the game plays the clip again in real time for its sound
    double clockStart = 0.0;  // the effects' clock at its start
    double gameStart = 0.0;   // the game's clock at its start
    double clockEnd = 0.0, gameEnd = 0.0; // both at its last stepped frame
    // Over the frames on the video's clock only (the sound pass renders one camera a frame).
    int samplesSeen = 0;      // most main cameras shaded in one frame
    int samplesLeast = -1;    // fewest
    int rendersSeen = 0;      // most main camera renders in one frame
    int othersSeen = 0;       // most other cameras into a target of the video's size in one frame
    unsigned engineMatrices = 0; // main cameras shaded with the engine's matrices
    unsigned noneSet = 0;        // of them with no view or projection set within the camera
    unsigned floatCopies = 0;    // copied from a float target
    D3DFORMAT targetFormat = D3DFMT_UNKNOWN; // the last shaded target
    UINT targetWidth = 0, targetHeight = 0;
} g_exportStats;
bool g_frameStepped = false; // the frame being rendered is on the video's clock

// The video export's own frames (VideoOwnFrames, src/videofile.h). Every sample of a video frame
// (the game's supersamples inside one engine frame, or the sub-frames of its motion blur, one per
// engine frame) is lit by the pipeline and added to a float sum; the frame's last sample grades
// the average into `frame` and converts it into the encoder's planes on the GPU. The game's next
// write of a video frame hands those planes to the encoder (ownDeliver), so the video has exactly
// the game's frames and stays in sync with its sound. The game's own target only gets the last
// finished frame, as the preview on screen. The planes reach the encoder a write or two later
// (PlaneReadback: the read-back no longer waits for the GPU), in order; before the game's own
// image goes in, and when the frames on the video's clock end, the ones on their way go first.
struct OwnExport {
    int framesSinceWrite = 0;  // sub-frame mode: frames on the video's clock since the game's last write
    int subIndex = 0;          // this frame's sub-frame within its video frame
    int lensSamples = 0;       // samples over the lens this video frame (accumulation depth of field), 0 = none
    float lensFocus = 0.0f;    // the auto focus's distance (m), read back with each frame handed over
    bool lensSet = false;      // the engine has a lens offset set
    unsigned videoFrame = 0;   // frames finished (turns the lens pattern between frames)
    bool fresh = false;        // a finished frame waits in the planes for the game's write
    bool haveFrame = false;    // a frame was finished in this export
    bool tenBit = false;       // the planes hold P010
    int tenBitOk = -1;         // 16-bit unorm targets (P010 planes): -1 not asked yet
    gfx::Target frame;         // the finished frame (float, the shaded size)
    gfx::Target luma, chroma;  // its planes
    PlaneReadback read;        // their way into system memory (lives through a device reset)
    // For the log: frames finished, handed over (fresh), handed over again (no new one since the
    // game's last write), finished and replaced before a write, sub-frames past the shutter,
    // samples summed, handed over but lost on the way (a failed lock, the encoder gone); the time
    // the game spent handing frames over and the time between two finished frames.
    unsigned finished = 0, delivered = 0, repeated = 0, dropped = 0, skipped = 0, samples = 0, lensFrames = 0, lost = 0;
    double readMs = 0.0, betweenMs = 0.0;
    LARGE_INTEGER lastFinish{};
} g_own;

// A device reset takes the render targets (and the read-back's queries); the frames on their way
// stay in system memory. The export's end takes everything.
void releaseOwnExport(bool exportOver) {
    g_own.frame.destroy();
    g_own.luma.destroy();
    g_own.chroma.destroy();
    g_own.read.releaseQueries();
    if (exportOver) {
        if (g_own.read.pending() > 0) {
            g_own.lost += static_cast<unsigned>(g_own.read.pending());
            TMVS_LOG("video export: %d of the mod's frames were still on their way to the encoder when it ended: lost", g_own.read.pending());
        }
        g_own.read.release();
    }
    g_own.fresh = false;
    g_own.haveFrame = false;
}

// What this sample of the export does for the mod's own frame (Pipeline::Inputs::accumulate).
struct OwnSample {
    int accumulate = 0;  // 0 not the mod's frame, 1 summed, 2 summed and the frame finished
    bool first = false;  // the video frame's first sample
    bool skip = false;   // a sub-frame past the shutter: nothing to do
    int index = 0;       // which sample of the frame
    int count = 1;       // samples summed per frame
};
OwnSample ownSample(int sampleIndex, bool part) {
    OwnSample o;
    const Settings& s = Config::get().settings;
    videofile::OwnFormat format;
    if (!g_exporting || !g_frameStepped || part || !s.enabled || !s.videoOwnFrames || !videofile::ownFormat(format)) return o;
    if (g_export.samplesPerFrame > 1) {
        // The supersamples, all in this engine frame.
        o.count = g_export.samplesPerFrame;
        o.index = std::min(sampleIndex, o.count - 1);
        o.first = sampleIndex == 0;
        o.accumulate = sampleIndex >= o.count - 1 ? 2 : 1;
    } else if (g_export.subFrames && g_export.samplesPerVideoFrame > 1) {
        // The motion blur's sub-frames, one per engine frame; the shutter keeps the first part of
        // the frame's time (180 degrees: half of them) and the rest is skipped.
        const int n = g_export.samplesPerVideoFrame;
        const int shutter = std::clamp(static_cast<int>(lroundf(n * s.videoShutter / 360.0f)), 1, n);
        o.index = g_own.subIndex % n;
        o.count = shutter;
        if (o.index >= shutter) {
            o.skip = true;
            return o;
        }
        o.first = o.index == 0;
        o.accumulate = o.index == shutter - 1 ? 2 : 1;
    } else {
        o.first = true;
        o.accumulate = 2;
    }
    return o;
}

// The finished frame (or the last one) onto the game's target: the preview on screen.
void showOwnFrame(IDirect3DDevice9* device, IDirect3DSurface9* target, UINT width, UINT height) {
    if (!g_own.haveFrame || !g_own.frame.valid() || g_own.frame.width != width || g_own.frame.height != height) return;
    const RECT all = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    videofile::OwnFormat format;
    if (videofile::ownFormat(format) && format.hdrWhite > 0.0f) {
        g_pipeline.showLinear(device, g_own.frame.texture, target, all);
    } else {
        g_pipeline.resolveScene(device, g_own.frame.texture, target, all);
    }
}

// The frame's last sample is shaded into g_own.frame: preview, then the encoder's planes.
void finishOwnFrame(IDirect3DDevice9* device, IDirect3DSurface9* target, UINT width, UINT height) {
    videofile::OwnFormat format;
    if (!videofile::ownFormat(format)) return;
    if (g_own.tenBitOk < 0) g_own.tenBitOk = Pipeline::tenBitPlanesSupported(device) ? 1 : 0;
    // P010 wanted but no 16-bit target: NV12 planes, the encoder takes those too.
    const bool tenBit = format.tenBit && g_own.tenBitOk == 1;
    // Half a step of dither: enough to break the bands of a dark sky, while a full step of noise
    // on every pixel cost the encoder bits everywhere.
    if (!g_pipeline.exportPlanes(device, g_own.frame.texture, static_cast<UINT>(format.width), static_cast<UINT>(format.height), format.crop, tenBit,
                                 0.5f, tenBit ? format.hdrWhite : 0.0f, g_own.luma, g_own.chroma)) {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            TMVS_LOG("video export: the frame's planes could not be made (%dx%d): the game's image goes in", format.width, format.height);
        }
        return;
    }
    if (g_own.fresh) g_own.dropped++;
    g_own.fresh = true;
    g_own.tenBit = tenBit;
    g_own.finished++;
    g_own.videoFrame++;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_own.haveFrame) g_own.betweenMs += static_cast<double>(now.QuadPart - g_own.lastFinish.QuadPart) * 1000.0 / static_cast<double>(g_frequency.QuadPart);
    g_own.lastFinish = now;
    g_own.haveFrame = true;
    showOwnFrame(device, target, width, height);
}

// The oldest frame on its way back into the encoder (waits for its copy if need be). Every frame
// handed over comes out here exactly once, so the video keeps the game's frame count.
void ownSubmitOldest(venc::Encoder& encoder, const videofile::OwnFormat& format) {
    bool copied = false;
    const bool locked = g_own.read.pop([&](const PlaneReadback::Frame& frame) {
        if (static_cast<int>(frame.width) * 4 != format.width || static_cast<int>(frame.height) != format.height) return;
        uint8_t* out = encoder.acquire(frame.tenBit ? venc::Pixels::P010 : venc::Pixels::NV12);
        if (!out) return;
        PlaneReadback::pack(frame, out);
        encoder.submit();
        copied = true;
        // The auto focus for a coming frame's lens (copied back with these planes).
        if (frame.focus > 0.0f) g_own.lensFocus = frame.focus;
    });
    if (!copied) {
        if (g_own.lost++ == 0)
            TMVS_LOG("video export: one of the mod's frames was lost on its way to the encoder (%s)",
                     locked ? "the encoder took no frame, or the size changed" : "its system memory copy could not be locked");
    }
}

// Every frame on its way, in order (before the game's own image goes in, and at the end).
void ownFlush(venc::Encoder& encoder, const videofile::OwnFormat& format) {
    while (g_own.read.pending() > 0) ownSubmitOldest(encoder, format);
}

// The planes finished last start their way back (or the frame queued last once more).
bool ownQueue(venc::Encoder& encoder, const videofile::OwnFormat& format) {
    IDirect3DDevice9* device = g_device;
    if (!device || !g_own.haveFrame || !g_own.luma.valid() || !g_own.chroma.valid()) return false;
    if (static_cast<int>(g_own.luma.width) * 4 != format.width || static_cast<int>(g_own.luma.height) != format.height) return false;
    if (g_own.fresh) {
        // Every slot taken: the oldest goes in first (the GPU is a frame or more past it).
        while (!g_own.read.canQueue() && g_own.read.pending() > 0) ownSubmitOldest(encoder, format);
        if (!g_own.read.queue(device, g_own.luma, g_own.chroma, g_pipeline.focusTarget(), g_own.tenBit)) return false;
    } else if (!g_own.read.queueAgain()) {
        return false;
    }
    (g_own.fresh ? g_own.delivered : g_own.repeated)++;
    g_own.fresh = false;
    return true;
}

// videofile: the game writes a video frame; the planes finished last go to the encoder. They are
// copied to system memory without waiting, and the frames whose copies are done (normally the
// one before) go into the encoder. False: the game's image goes in, after every frame on its way.
bool ownDeliver(venc::Encoder& encoder, const videofile::OwnFormat& format) {
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    const bool queued = ownQueue(encoder, format);
    if (queued) {
        while (g_own.read.pending() > 0 && g_own.read.oldestDone()) ownSubmitOldest(encoder, format);
    } else {
        ownFlush(encoder, format);
    }
    QueryPerformanceCounter(&end);
    g_own.readMs += static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(g_frequency.QuadPart);
    return queued;
}

// videofile: the export's file closes, or the frames on the video's clock ended.
void ownFlushAll(venc::Encoder& encoder, const videofile::OwnFormat& format) {
    if (g_own.read.pending() <= 0) return;
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    ownFlush(encoder, format);
    QueryPerformanceCounter(&end);
    g_own.readMs += static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(g_frequency.QuadPart);
}

// videofile: will ownDeliver have a frame for the game's coming write? (Its own checks, before
// the read-backs; then the game needn't read its image back.)
bool ownReady(const videofile::OwnFormat& format) {
    return g_device && g_own.haveFrame && g_own.luma.valid() && g_own.chroma.valid() && static_cast<int>(g_own.luma.width) * 4 == format.width &&
           static_cast<int>(g_own.luma.height) == format.height;
}

// videofile: the game wrote a video frame; the next sub-frame starts the next one.
void ownWritten() {
    g_own.framesSinceWrite = 0;
}

// Accumulation depth of field: each sample of a video frame sees the scene from another point of
// the lens (a disc in the camera's plane), sheared so the focus plane stays put: what is out of
// focus lands somewhere else in each sample and the sum blurs it, with the lens's own bokeh and no
// halos at edges. With few samples the copies would stay apart: 16 at least, else the usual blur.
// The engine makes a camera's projection before the camera begins, so the offsets go to it as a
// table at the start of each video frame (engine::setLensSamples), indexed by the game's own
// sample number (docs/engine-video-export.md). Called once a frame, after updateVideoExport.
void updateLens() {
    const Settings& s = Config::get().settings;
    const OwnSample sample = ownSample(0, false); // supersampling: this frame starts a video frame
    const auto off = [] {
        if (g_own.lensSet) engine::setLensSamples(nullptr, 0, 0.0f);
        g_own.lensSet = false;
        g_own.lensSamples = 0;
    };
    if (sample.accumulate == 0 && !sample.skip) {
        off();
        return;
    }
    if (!sample.first) return; // the video frame's table is set
    const float focus = s.focusDistance > 0.0f ? s.focusDistance : g_own.lensFocus;
    const float p11 = g_scene.haveEngine ? g_scene.engineProjection[5] : 0.0f;
    if (!s.videoLensDof || s.depthOfField <= 0.0f || sample.count < 16 || focus <= 0.1f || p11 <= 0.0f) {
        off();
        return;
    }
    // The lens's radius: a point at infinity blurs as far as the usual depth of field takes it
    // (circleOfConfusion in the shader: BokehSize px at 1080p, times DepthOfField x 2.5 up to 1).
    // Its image moves P11 a / F in NDC, half the screen's height per unit.
    const float blurPx = std::min(s.depthOfField * 2.5f, 1.0f) * s.bokehSize;
    const float radius = blurPx / 1080.0f * 2.0f * focus / p11;
    // One entry per sample of the game's video frame; the ones past the shutter are never summed.
    // The game's sub-pixel raster runs column by column: the k-th sample takes the lens point
    // k x stride (a stride coprime to the count), so lens and raster don't move in step. The points
    // lie on a Vogel spiral (even over the disc for any count), turned from frame to frame.
    const int total = std::min(g_export.samplesPerFrame > 1 ? g_export.samplesPerFrame : g_export.samplesPerVideoFrame, 128);
    const int used = std::min(sample.count, total);
    int stride = std::max(1, static_cast<int>(used * 0.618034f));
    while (std::gcd(stride, used) != 1) stride++;
    float table[2 * 128] = {};
    const float turn = static_cast<float>(g_own.videoFrame) * 0.618034f * 6.2831853f;
    for (int j = 0; j < used; j++) {
        const int k = static_cast<int>((static_cast<long long>(j) * stride) % used);
        const float r = radius * sqrtf((k + 0.5f) / used);
        const float angle = k * 2.39996323f + turn;
        table[2 * j] = r * cosf(angle);
        table[2 * j + 1] = r * sinf(angle);
    }
    engine::setLensSamples(table, total, focus);
    g_own.lensSet = true;
    g_own.lensSamples = used;
    g_own.lensFrames++;
}

// The time the effects see (s).
float effectTime() {
    return g_videoClock >= 0.0 ? static_cast<float>(g_videoClock) : seconds() + g_clockOffset;
}

// CPU time the mod's own rendering takes (D3D calls of the pipeline, not the GPU work):
// logged now and then for the first frames, to see whether the CPU side is worth working on.
struct CpuStats {
    double total = 0.0, worst = 0.0;
    unsigned frames = 0;
} g_cpuStats;

// Hangs: a frame the mod held up for long (logged with the time, so a sound glitch or stutter the
// player noticed can be matched to it), and long gaps between two shaded frames in a race (the
// game or the driver stood still: loading is no race).
void logHangs(double ms, double captureMs, bool racing) {
    static int s_logged = 0;
    static float s_last = -1.0f;
    static int s_maps = -1;
    const float now = seconds();
    // A map loaded or closed in between: that wait is the loading screen.
    const int maps = engine::mapLoads() + engine::mapCloses();
    if (maps != s_maps) s_last = -1.0f;
    s_maps = maps;
    if (s_logged < 60) {
        if (ms > 100.0) {
            s_logged++;
            TMVS_LOG("cpu: one frame of the mod took %.0f ms (%.0f ms of it reading back the F12 capture)", ms, captureMs);
        }
        if (racing && s_last >= 0.0f && now - s_last > 0.25f) {
            s_logged++;
            TMVS_LOG("frame: %.0f ms since the last shaded frame in a race", (now - s_last) * 1000.0f);
        }
    }
    s_last = racing ? now : -1.0f;
}

void countPipelineCpu(double ms) {
    perflog::addCpu(ms);
    if (g_cpuStats.frames >= 20000) return;
    g_cpuStats.total += ms;
    g_cpuStats.worst = ms > g_cpuStats.worst ? ms : g_cpuStats.worst;
    if (++g_cpuStats.frames % 2000 == 0) {
        TMVS_LOG("cpu: the mod's rendering takes %.2f ms per shaded frame on the CPU (worst %.1f ms, %u frames)", g_cpuStats.total / g_cpuStats.frames,
                 g_cpuStats.worst, g_cpuStats.frames);
        g_cpuStats.worst = 0.0;
    }
}

float iniFloat(const wchar_t* ini, const wchar_t* key, float fallback) {
    wchar_t value[64] = {};
    if (!GetPrivateProfileStringW(L"dev", key, L"", value, 64, ini)) return fallback;
    return static_cast<float>(_wtof(value));
}

void loadDevOptions() {
    std::wstring path = log::dataDir() + L"\\dev.ini";
    const wchar_t* ini = path.c_str();
    g_dev.traceAt = iniFloat(ini, L"TraceAt", 0.0f);
    g_dev.traceVideoFrame = GetPrivateProfileIntW(L"dev", L"TraceVideoFrame", 0, ini);
    g_dev.captureAt = iniFloat(ini, L"CaptureAt", 0.0f);
    g_dev.captureInterval = iniFloat(ini, L"CaptureInterval", 2.0f);
    g_dev.captureCount = GetPrivateProfileIntW(L"dev", L"CaptureCount", 1, ini);
    g_dev.exitAt = iniFloat(ini, L"ExitAt", 0.0f);
    g_dev.fakeBlockRank = GetPrivateProfileIntW(L"dev", L"FakeBlockRank", 0, ini) != 0;
    g_dev.hardExitAt = iniFloat(ini, L"HardExitAt", 0.0f);
    wchar_t buffer[MAX_PATH] = {};
    GetPrivateProfileStringW(L"dev", L"ShaderDir", L"", buffer, MAX_PATH, ini);
    g_dev.shaderDir = buffer;
    const auto readKeys = [&](const wchar_t* name, std::vector<std::pair<float, std::wstring>>& keys) {
        GetPrivateProfileStringW(L"dev", name, L"", buffer, MAX_PATH, ini);
        for (wchar_t* token = wcstok(buffer, L","); token; token = wcstok(nullptr, L",")) {
            float at = 0.0f;
            wchar_t action[32] = {};
            if (swscanf(token, L"%f:%31ls", &at, action) == 2) keys.emplace_back(at, action);
        }
    };
    readKeys(L"Keys", g_dev.keys);
    readKeys(L"SceneKeys", g_dev.sceneKeys);
    g_nextCaptureAt = g_dev.captureAt;
    engine::forceShadows(GetPrivateProfileIntW(L"dev", L"ForceShadows", -1, ini));
    engine::shaderCensus(GetPrivateProfileIntW(L"dev", L"LogShaders", 0, ini) != 0);
}

void runDevAutomation(IDirect3DDevice9* device) {
    // Trace/capture times count from the first gameplay frame, so loading and menus don't matter.
    const float now = seconds();
    const float sceneNow = g_sceneStart >= 0.0f ? now - g_sceneStart : -1.0f;
    if (g_dev.traceAt > 0.0f && sceneNow >= g_dev.traceAt) {
        g_dev.traceAt = 0.0f;
        tracer::arm();
    }
    if (g_dev.captureAt > 0.0f && sceneNow >= g_nextCaptureAt && g_capturesWritten < g_dev.captureCount) {
        g_nextCaptureAt = sceneNow + g_dev.captureInterval;
        g_capturePending = true;
    }
    const std::wstring* action = nullptr;
    if (g_nextKey < g_dev.keys.size() && now >= g_dev.keys[g_nextKey].first) {
        action = &g_dev.keys[g_nextKey++].second;
    } else if (g_nextSceneKey < g_dev.sceneKeys.size() && sceneNow >= g_dev.sceneKeys[g_nextSceneKey].first) {
        action = &g_dev.sceneKeys[g_nextSceneKey++].second;
    }
    if (action) {
        D3DDEVICE_CREATION_PARAMETERS cp{};
        device->GetCreationParameters(&cp);
        HWND window = cp.hFocusWindow;
        if (*action == L"photomode") {
            photo::toggle();
        } else if (*action == L"photo" || *action == L"photohires") {
            photo::request(*action == L"photohires");
        } else if (*action == L"min") {
            ShowWindow(window, SW_MINIMIZE);
        } else if (*action == L"restore") {
            ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
        } else if ((*action)[0] == L'a') {
            const WPARAM vk = static_cast<WPARAM>(_wtoi(action->c_str() + 1));
            PostMessageW(window, WM_SYSKEYDOWN, vk, 1 | (1 << 29));
            PostMessageW(window, WM_SYSKEYUP, vk, 0xE0000001);
        } else {
            const WPARAM vk = static_cast<WPARAM>(_wtoi(action->c_str()));
            PostMessageW(window, WM_KEYDOWN, vk, 1);
            PostMessageW(window, WM_KEYUP, vk, 0xC0000001);
        }
        TMVS_LOG("dev: action %ls", action->c_str());
    }
    if ((g_dev.exitAt > 0.0f && sceneNow >= g_dev.exitAt) || (g_dev.hardExitAt > 0.0f && now >= g_dev.hardExitAt)) {
        TMVS_LOG("dev: ExitAt reached, terminating");
        TerminateProcess(GetCurrentProcess(), 0);
    }
}

void traceMatrix(const char* label, const float* m) {
    tracer::event("    %s [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]", label, m[0], m[1],
                  m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
}

// The main 3D camera renders at full back-buffer size with the shadowed depth buffer bound.
// In races that target is the back buffer; with the engine's own camera effects (menus,
// replays) it is an offscreen texture that is composited afterwards.
bool isMainTarget(IDirect3DDevice9* device, IDirect3DSurface9* rt) {
    if (!rt) return false;
    IDirect3DSurface9* ds = nullptr;
    device->GetDepthStencilSurface(&ds);
    IDirect3DTexture9* readable = depth::textureFor(ds);
    D3DSURFACE_DESC rtDesc{}, depthDesc{};
    if (ds) {
        ds->GetDesc(&depthDesc);
        ds->Release();
    }
    if (!readable) return false;
    rt->GetDesc(&rtDesc);
    return rtDesc.Width == depthDesc.Width && rtDesc.Height == depthDesc.Height && rtDesc.Width >= 320;
}

// World-space direction towards the sun: the look's own (SunElevation) or the game's light.
bool sunDirection(const Settings& s, float out[3]) {
    if (s.sunElevationOverride >= 0.0f) {
        const float el = s.sunElevationOverride * 3.14159265f / 180.0f;
        const float az = s.sunAzimuthOverride * 3.14159265f / 180.0f;
        out[0] = cosf(el) * cosf(az);
        out[1] = sinf(el);
        out[2] = cosf(el) * sinf(az);
        return true;
    }
    if (!g_scene.haveLight) return false;
    // D3D light direction is the direction the light travels.
    out[0] = -g_scene.lightDirection[0];
    out[1] = -g_scene.lightDirection[1];
    out[2] = -g_scene.lightDirection[2];
    return true;
}

bool materialsWanted(); // the game's material masks this frame (below, with the draw tagging)

// The colour look-up table picked in the menu: loaded when the choice changes, a broken
// file is reported once and left off. Returns how much of it to use (0 = none).
float updateLut() {
    static std::string loaded;
    const Config& config = Config::get();
    if (config.lutFile != loaded) {
        loaded = config.lutFile;
        lut::Table table;
        std::string error;
        if (!loaded.empty() && !lut::loadCube(lut::path(loaded), table, error)) {
            TMVS_LOG("lut: %s: %s", loaded.c_str(), error.c_str());
        } else if (!loaded.empty()) {
            TMVS_LOG("lut: %s (%d^3)", loaded.c_str(), table.size);
        }
        g_pipeline.setLut(table.texels, table.size);
        g_pipelineB.setLut(table.texels, table.size); // the compare split's preset side (also before it starts)
    }
    return loaded.empty() ? 0.0f : config.lutAmount;
}

// CasterShadows for the main pipeline: also notes the light (see castersSideB).
bool castersSideA(IDirect3DDevice9* device, const float sunWorld[3], const float camera[3], casters::Maps& out) {
    memcpy(g_casterSun, sunWorld, sizeof(g_casterSun));
    g_casterSunSet = true;
    return casters::update(device, sunWorld, camera, out);
}

// The compare split's preset side: the same maps when its light comes from where side A's does,
// or its own when side A drew none this frame (your look a night sky without sun, the preset a
// day look: before, it got none and its sun cast only the height map's coarse shadows).
// Another sun than side A's gets none, the height map's shadows stand in: the maps are drawn for
// one light, a second one would redraw them twice a frame.
bool castersSideB(IDirect3DDevice9* device, const float sunWorld[3], const float camera[3], casters::Maps& out) {
    const float d = sunWorld[0] * g_casterSun[0] + sunWorld[1] * g_casterSun[1] + sunWorld[2] * g_casterSun[2];
    if (g_casterSunSet && d < 0.999999f) return false;
    return casters::update(device, sunWorld, camera, out);
}

void releaseSideB() {
    if (g_pipelineB.ready()) TMVS_LOG("photo: compare split with a preset off, its pipeline released");
    g_pipelineB.release();
    g_splitOut.destroy();
    g_pipelineBFailed = false;
}

// The compare split's other side as a preset's look (photo::splitPreset()), from the same frame
// and inputs as side A; or the game's image through the same lens as side A when its aperture
// blurs (a fair reference: the same focus and blur on both sides). Null: the game's image as it
// came in stays there (no depth of field, or it failed).
IDirect3DSurface9* renderSideB(IDirect3DDevice9* device, const Pipeline::Inputs& inputsA, const Settings& mine, const Settings& shownA) {
    const std::string& name = photo::splitPreset();
    // Kept while the split is on, also while it shows the game's image without depth of field
    // (the aperture goes up and down on the wheel: no new pipeline at every step).
    if (!photo::splitOn() || !photo::active()) {
        if (g_pipelineB.ready() || g_splitOut.texture) releaseSideB();
        return nullptr;
    }
    if (!photo::splitShown()) return nullptr;
    const bool game = name.empty();
    if (game && shownA.depthOfField <= 0.0f) return nullptr;
    // Your machine settings (the game's own changes are made once, for both sides), the preset's look.
    Settings b = mine;
    if (game) {
        // Pipeline::render's vanilla photo path (as with the shaders off): the depth of field on
        // the game's image and nothing else of the mod, not even the sharpening.
        b.enabled = false;
        b.sharpen = 0.0f;
    } else {
        // Your own preset is a file read field by field (about 100 GetPrivateProfileString calls,
        // several ms): read once, again when another preset is picked, its file is written or
        // your settings change (the machine settings are yours), not every frame.
        static std::string s_name;
        static unsigned long long s_written = 0;
        static Settings s_mine, s_look;
        static bool s_known = false;
        const unsigned long long written = Config::get().presetWritten(name);
        if (s_name != name || s_written != written || memcmp(&s_mine, &mine, sizeof(Settings)) != 0) {
            s_name = name;
            s_written = written;
            memcpy(&s_mine, &mine, sizeof(Settings));
            s_look = mine;
            s_known = Config::get().presetSettings(name, s_look);
        }
        if (!s_known) return nullptr;
        b = s_look;
        b.enabled = true; // with the shaders off: the preset next to the game's image with photo mode's lens
    }
    b.debugView = shownA.debugView;
    photo::adjust(b);
    // The lens is the camera's, not the look's: the same aperture, focus and largest blur as side A
    // (a preset's own BokehSize made the same f-stop blur more or less on its side).
    b.depthOfField = shownA.depthOfField;
    b.focusDistance = shownA.focusDistance;
    b.dofAutofocus = shownA.dofAutofocus;
    b.bokehSize = shownA.bokehSize;
    // Another preset picked (the bar at the bottom switches it in a click): no exposure, TAA or
    // sky history of the last one blending into the first frames of this one.
    static std::string s_shown;
    const bool switched = s_shown != name;
    if (switched) s_shown = name;
    const char* shown = game ? "the game's image (depth of field)" : name.c_str();
    if (!g_pipelineB.ready()) {
        if (g_pipelineBFailed || !g_pipelineB.init(device, g_dev.shaderDir)) {
            if (!g_pipelineBFailed) TMVS_LOG("photo: compare split with %s: its pipeline failed (%s)", shown, g_pipelineB.lastError().c_str());
            g_pipelineBFailed = true;
            return nullptr;
        }
        TMVS_LOG("photo: compare split with %s%s (a second pipeline)", game ? "" : "the preset ", shown);
    } else if (switched) {
        g_pipelineB.resetHistory();
        TMVS_LOG("photo: compare split now with %s%s", game ? "" : "the preset ", shown);
    }
    if (switched && !game) {
        // The frame is lit for your look; say once when the preset would have been lit otherwise.
        const std::string note = photo::splitLightNote();
        if (!note.empty()) TMVS_LOG("photo: compare split: %s", note.c_str());
    }
    if (!g_splitOut.create(device, inputsA.width, inputsA.height, D3DFMT_A8R8G8B8)) return nullptr;
    Pipeline::Inputs in = inputsA;
    in.materialClasses = nullptr; // the F12 capture is side A's
    if (game) {
        in.materials = false; // the vanilla path needs the depth only
        in.casterMap = nullptr;
    } else {
        // The preset's own sun position (SunElevation), not yours; without one the game's light.
        in.sunKnown = sunDirection(b, in.sunDirection);
        if (in.casterMap) in.casterMap = castersSideB;
    }
    // Your side's neon trail on the preset's side too (the trail is a machine setting, the same on both).
    g_pipelineB.shareTrail(game ? nullptr : &g_pipeline);
    g_pipelineB.render(device, in, b, g_splitOut.surface);
    return g_splitOut.surface;
}

float halfToFloat(uint16_t h) {
    const unsigned sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
    float value;
    if (exponent == 0) value = ldexpf(static_cast<float>(mantissa), -24);
    else if (exponent == 31) value = 65504.0f; // inf / nan: as bright as it gets
    else value = ldexpf(static_cast<float>(mantissa | 1024), static_cast<int>(exponent) - 25);
    return sign ? -value : value;
}

// How much of the game's image lies above 1.0, i.e. what its own 8-bit target cut away.
// `keep` receives the half floats (RGBA, rows top-down) when given.
// The numbers alone (any thread: the F12 capture's are counted on its writer).
void hdrStats(const std::vector<uint16_t>& half, const char* when) {
    const size_t count = half.size() / 4;
    size_t above1 = 0, above125 = 0, above2 = 0, negative = 0;
    float brightest = 0.0f;
    std::vector<size_t> bins(129, 0); // brightest channel in steps of 1/16 up to 8
    for (size_t i = 0; i < count; i++) {
        const float r = halfToFloat(half[i * 4]), g = halfToFloat(half[i * 4 + 1]), b = halfToFloat(half[i * 4 + 2]);
        const float m = std::max(r, std::max(g, b));
        above1 += m > 1.0f + 1.0f / 255.0f;
        above125 += m > 1.25f;
        above2 += m > 2.0f;
        negative += std::min(r, std::min(g, b)) < 0.0f;
        brightest = std::max(brightest, m);
        bins[static_cast<size_t>(std::min(std::max(m, 0.0f), 8.0f) * 16.0f)]++;
    }
    size_t seen = 0, p999 = 0;
    for (size_t i = 0; i < bins.size(); i++) {
        seen += bins[i];
        if (seen >= count - count / 1000) {
            p999 = i;
            break;
        }
    }
    const double n = count ? static_cast<double>(count) / 100.0 : 1.0;
    TMVS_LOG("hdr (%s): %.2f%% of the image above 1.0, %.2f%% above 1.25, %.2f%% above 2.0; brightest %.2f, 99.9%% at most %.2f; %.2f%% negative",
             when, above1 / n, above125 / n, above2 / n, brightest, (p999 + 1) / 16.0, negative / n);
}

void logHdrStats(IDirect3DDevice9* device, const char* when) {
    std::vector<uint16_t> half;
    if (!g_pipeline.readHalfColor(device, g_hdrScene.surface, half)) {
        TMVS_LOG("hdr: readback failed");
        return;
    }
    hdrStats(half, when);
}

// RenderScale: the main camera draws into GameHdr's float target made smaller, through a viewport
// and scissor rect scaled to it. The game's projection stays as it is, so its whole 3D image lands
// in the smaller target at the lower resolution; the depth buffer stays the game's (a larger one
// is allowed), the image in its top left corner. The effects run at that size and FSR 1 brings the
// shaded image up into the game's target before the HUD, which the game then draws at full size.
// The game reads back its own numbers (GetViewport, GetScissorRect). Draws in screen pixels
// (pre-transformed vertices, no viewport applies: the sun's sprite, lens flares) get a vertex
// shader that scales them (scaleScreenDraw). The game's passes into full-size targets in the same
// camera (Stadium: the depth for its shadow mask) share the depth buffer with the scaled image:
// its depth clears are repeated at the full size, and the scaled image's corner of the depth is
// cleared before the scaled drawing starts on it (scaledDepthBeforeDraw). A second camera into
// the same target in a frame (a small 3D view in a corner) is drawn at full size straight into the
// game's target. What the scaling still can't follow falls back to the full size: draws in screen
// pixels it can't scale and depth written at both sizes for the rest of the map, the first camera
// into part of the screen (the MediaTracker editor, the replay menu's band) for a few seconds, and
// photo mode, captures and the video export always.
struct RenderScaleState {
    bool active = false;      // the main camera being drawn is scaled (beginHdr .. endHdr)
    bool pending = false;     // the last one was and nothing has filled the game's target yet
    bool sceneScaled = false; // g_hdrScene has the scaled size (its rects scale in StretchRect)
    UINT fullWidth = 0, fullHeight = 0; // the game's target
    UINT width = 0, height = 0;         // the scaled float target
    D3DVIEWPORT9 logical{};   // the game's viewport in its own numbers
    D3DVIEWPORT9 device{};    // ... and what was set for it
    RECT logicalScissor{};
    RECT deviceScissor{};
    RECT area{};              // the camera's viewport in the scaled target (at its end)
    int offMap = -1;          // engine::mapLoads() of a map it was turned off for
    const char* offWhy = nullptr; // ... and why (for the log)
    DWORD partUntil = 0;      // a camera into part of the screen was seen: full size until then
    float biasSaved[16] = {}; // the samplers' mip bias before the camera (DWORDs as floats)
    // The game's depth buffer during the scaled camera (only compared, not held).
    IDirect3DSurface9* depth = nullptr;
    bool fullDepth = false;   // depth written through a full-size target since the scaled corner was clean
    bool scaledDepth = false; // depth written by the scaled image since its corner was cleared
    float clearZ = 1.0f;      // the game's last depth clear value
    // What the game did with the scaled target (this map; the log tells once, a few seconds in).
    struct Counts {
        unsigned cameras = 0, viewports = 0, scissors = 0, readBacks = 0, rectClears = 0, rectStretches = 0, sized = 0,
                 pretransformed = 0, fallbacks = 0, screenScaled = 0, fullClears = 0, cornerClears = 0, secondCameras = 0;
    } counts;
    unsigned cameraPretransformed = 0; // in the camera being drawn: draws in screen pixels left unscaled
    const char* cameraPretransformedWhy = nullptr; // ... why
    unsigned cameraMixed = 0;          // ... depth written at the full size after the scaled image's
} g_rs;

// The scale for the main camera starting now: 1 = the full size.
float renderScaleNow(const Settings& s, const char** why) {
    *why = nullptr;
    if (s.renderScale >= 0.99f) return 1.0f;
    if (photo::active()) *why = "photo mode";
    else if (g_exporting) *why = "the video export";
    else if (g_captureRequested) *why = "a capture (F12)";
    else if (g_rs.offMap == engine::mapLoads()) *why = g_rs.offWhy ? g_rs.offWhy : "off for this map (see above)";
    else if (g_rs.partUntil && static_cast<LONG>(GetTickCount() - g_rs.partUntil) < 0) *why = "a camera into part of the screen";
    if (*why) return 1.0f;
    return s.renderScale < 0.5f ? 0.5f : s.renderScale;
}

// From the game's numbers to the scaled target's: the edges are scaled, so the full size maps to
// exactly the scaled one.
LONG scaleX(LONG x) {
    const LONG v = static_cast<LONG>(static_cast<double>(x) * g_rs.width / g_rs.fullWidth + 0.5);
    return v < 0 ? 0 : (v > static_cast<LONG>(g_rs.width) ? static_cast<LONG>(g_rs.width) : v);
}
LONG scaleY(LONG y) {
    const LONG v = static_cast<LONG>(static_cast<double>(y) * g_rs.height / g_rs.fullHeight + 0.5);
    return v < 0 ? 0 : (v > static_cast<LONG>(g_rs.height) ? static_cast<LONG>(g_rs.height) : v);
}
LONG unscaleX(LONG x) { return static_cast<LONG>(static_cast<double>(x) * g_rs.fullWidth / g_rs.width + 0.5); }
LONG unscaleY(LONG y) { return static_cast<LONG>(static_cast<double>(y) * g_rs.fullHeight / g_rs.height + 0.5); }

D3DVIEWPORT9 scaledViewport(const D3DVIEWPORT9& v) {
    const LONG x0 = scaleX(static_cast<LONG>(v.X)), y0 = scaleY(static_cast<LONG>(v.Y));
    LONG x1 = scaleX(static_cast<LONG>(v.X + v.Width)), y1 = scaleY(static_cast<LONG>(v.Y + v.Height));
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    return {static_cast<DWORD>(x0), static_cast<DWORD>(y0), static_cast<DWORD>(x1 - x0), static_cast<DWORD>(y1 - y0), v.MinZ, v.MaxZ};
}

RECT scaledRect(const RECT& r) {
    return {scaleX(r.left), scaleY(r.top), scaleX(r.right), scaleY(r.bottom)};
}

bool sameViewport(const D3DVIEWPORT9& a, const D3DVIEWPORT9& b) {
    return a.X == b.X && a.Y == b.Y && a.Width == b.Width && a.Height == b.Height;
}

// The game draws into the scaled target right now.
bool onScaledTarget(IDirect3DDevice9* device) {
    if (!g_rs.active || !g_hdrScene.surface) return false;
    IDirect3DSurface9* rt = nullptr;
    device->GetRenderTarget(0, &rt);
    if (rt) rt->Release();
    return rt == g_hdrScene.surface;
}

const D3DVIEWPORT9* onSetViewport(IDirect3DDevice9* device, const D3DVIEWPORT9* vp) {
    if (!onScaledTarget(device)) return vp;
    g_rs.logical = *vp;
    // The scaled target's own size: the game sized it from the target it was handed (GetDesc).
    // Scaled again, its image would shrink into a corner.
    if (vp->X == 0 && vp->Y == 0 && vp->Width == g_rs.width && vp->Height == g_rs.height) {
        g_rs.counts.sized++;
        g_rs.device = *vp;
        return vp;
    }
    g_rs.counts.viewports++;
    g_rs.device = scaledViewport(*vp);
    return &g_rs.device;
}

void onGetViewport(IDirect3DDevice9* device, D3DVIEWPORT9* vp) {
    if (!onScaledTarget(device)) return;
    g_rs.counts.readBacks++;
    if (sameViewport(*vp, g_rs.device)) {
        *vp = g_rs.logical;
        return;
    }
    // Set by the device itself (SetRenderTarget resets it to the whole target).
    const LONG x0 = unscaleX(static_cast<LONG>(vp->X)), y0 = unscaleY(static_cast<LONG>(vp->Y));
    const LONG x1 = unscaleX(static_cast<LONG>(vp->X + vp->Width)), y1 = unscaleY(static_cast<LONG>(vp->Y + vp->Height));
    vp->X = static_cast<DWORD>(x0);
    vp->Y = static_cast<DWORD>(y0);
    vp->Width = static_cast<DWORD>(x1 - x0);
    vp->Height = static_cast<DWORD>(y1 - y0);
}

const RECT* onSetScissorRect(IDirect3DDevice9* device, const RECT* rect) {
    if (!onScaledTarget(device)) return rect;
    g_rs.counts.scissors++;
    g_rs.logicalScissor = *rect;
    g_rs.deviceScissor = scaledRect(*rect);
    return &g_rs.deviceScissor;
}

void onGetScissorRect(IDirect3DDevice9* device, RECT* rect) {
    if (!onScaledTarget(device)) return;
    g_rs.counts.readBacks++;
    if (EqualRect(rect, &g_rs.deviceScissor)) {
        *rect = g_rs.logicalScissor;
        return;
    }
    *rect = {unscaleX(rect->left), unscaleY(rect->top), unscaleX(rect->right), unscaleY(rect->bottom)};
}

// A depth or stencil clear of the game's whole screen while the scaled target is bound: D3D9 clears
// the viewport, which can't be larger than the bound target, so only the scaled image's corner of
// the depth buffer would be cleared. The game's passes into full-size targets in this camera use
// the rest (Stadium: the depth for its shadow mask, drawn right after such a clear; with last
// frame's depth around the corner its shadows would break up): the same clear over the whole
// buffer, through the game's own target.
void clearDepthFullSize(IDirect3DDevice9* device, DWORD flags, float z, DWORD stencil) {
    hook::InternalScope internal;
    IDirect3DSurface9* ds = nullptr;
    device->GetDepthStencilSurface(&ds);
    if (!ds) return;
    D3DSURFACE_DESC desc{};
    ds->GetDesc(&desc);
    ds->Release();
    if (!g_hdrGame || desc.Width < g_rs.fullWidth || desc.Height < g_rs.fullHeight) return;
    IDirect3DSurface9* rt = nullptr;
    device->GetRenderTarget(0, &rt);
    if (!rt) return;
    D3DVIEWPORT9 viewport{};
    RECT scissor{};
    DWORD scissorTest = FALSE;
    device->GetViewport(&viewport);
    device->GetScissorRect(&scissor);
    device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorTest);
    if (SUCCEEDED(device->SetRenderTarget(0, g_hdrGame))) {
        const D3DVIEWPORT9 all = {0, 0, g_rs.fullWidth, g_rs.fullHeight, 0.0f, 1.0f};
        device->SetViewport(&all);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        if (SUCCEEDED(device->Clear(0, nullptr, flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL), 0, z, stencil))) g_rs.counts.fullClears++;
        device->SetRenderTarget(0, rt);
        device->SetViewport(&viewport);
        device->SetScissorRect(&scissor);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, scissorTest);
    }
    rt->Release();
}

const D3DRECT* onClearRects(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects, DWORD flags, float z, DWORD stencil) {
    if (!onScaledTarget(device)) return rects;
    if (flags & D3DCLEAR_ZBUFFER) g_rs.clearZ = z;
    const D3DVIEWPORT9& v = g_rs.logical;
    bool wholeScreen = v.X == 0 && v.Y == 0 && v.Width >= g_rs.fullWidth && v.Height >= g_rs.fullHeight;
    DWORD scissorTest = FALSE;
    device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorTest);
    const RECT& r = g_rs.logicalScissor;
    if (scissorTest && (r.left > 0 || r.top > 0 || r.right < static_cast<LONG>(g_rs.fullWidth) || r.bottom < static_cast<LONG>(g_rs.fullHeight))) {
        wholeScreen = false;
    }
    if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && !count && wholeScreen) {
        clearDepthFullSize(device, flags, z, stencil);
        // The whole depth is clean again: neither size has drawn into it.
        if (flags & D3DCLEAR_ZBUFFER) g_rs.scaledDepth = g_rs.fullDepth = false;
    }
    if (!count || !rects) return rects;
    static std::vector<D3DRECT> s_scaled;
    s_scaled.assign(rects, rects + count);
    for (D3DRECT& r : s_scaled) r = {scaleX(r.x1), scaleY(r.y1), scaleX(r.x2), scaleY(r.y2)};
    g_rs.counts.rectClears++;
    return s_scaled.data();
}

// A copy from or into the scaled target with rects in the game's numbers (a whole-surface copy
// stretches by itself).
void onStretchRects(IDirect3DSurface9* src, const RECT** srcRect, IDirect3DSurface9* dst, const RECT** dstRect) {
    if (!g_rs.sceneScaled || !g_hdrScene.surface || (src != g_hdrScene.surface && dst != g_hdrScene.surface)) return;
    static RECT s_src, s_dst;
    if (src == g_hdrScene.surface && *srcRect) {
        s_src = scaledRect(**srcRect);
        *srcRect = &s_src;
        g_rs.counts.rectStretches++;
    }
    if (dst == g_hdrScene.surface && *dstRect) {
        s_dst = scaledRect(**dstRect);
        *dstRect = &s_dst;
        g_rs.counts.rectStretches++;
    }
}

// RenderScale: draws in screen pixels. Pre-transformed vertices (a declaration with POSITIONT, or
// the FVF XYZRHW) take no viewport: as they come they land at their full-size spot in the smaller
// image. In the Stadium's main camera these are the sun's sprite, the lens flares and, at the
// camera's end, two quads that mark the sky in the alpha channel (CVisionViewportDx9::RasterizeQuads:
// quads in pixels of the full size the engine knows). For that one draw a vertex shader of the mod
// takes over: the same vertices read as POSITION, their pixels scaled into the smaller image, 1/rhw
// back as w (the interpolation stays perspective-correct), colours and texture coordinates passed on.
struct ScreenLayout {
    std::vector<D3DVERTEXELEMENT9> elements;     // the game's vertices (with POSITIONT)
    IDirect3DVertexDeclaration9* decl = nullptr; // the same read as POSITION
    IDirect3DVertexShader9* vs = nullptr;
    const char* failed = nullptr;                // why its draws can't be scaled
};
std::vector<ScreenLayout> g_screenLayouts;

// What scaleScreenDraw changed for the draw, given back after it (screenDrawDone).
struct ScreenDrawSaved {
    bool active = false;
    IDirect3DVertexDeclaration9* decl = nullptr; // held
    DWORD fvf = 0;
    float constants[8] = {};
    DWORD clipPlanes = 0;
} g_screenSaved;

void releaseScreenLayouts() {
    for (ScreenLayout& l : g_screenLayouts) {
        gfx::release(l.decl);
        gfx::release(l.vs);
    }
    g_screenLayouts.clear();
}

// The vertices of the draw about to happen, if they are in screen pixels.
bool screenElements(IDirect3DDevice9* device, std::vector<D3DVERTEXELEMENT9>& elements) {
    elements.clear();
    IDirect3DVertexDeclaration9* decl = nullptr;
    device->GetVertexDeclaration(&decl);
    if (decl) {
        D3DVERTEXELEMENT9 e[MAXD3DDECLLENGTH + 1] = {};
        UINT n = MAXD3DDECLLENGTH + 1;
        if (SUCCEEDED(decl->GetDeclaration(e, &n))) {
            for (UINT i = 0; i < n && e[i].Stream != 0xFF; i++) elements.push_back(e[i]);
        }
        decl->Release();
    } else {
        // An FVF and no declaration to read: its layout, as D3D9 defines it.
        DWORD fvf = 0;
        device->GetFVF(&fvf);
        if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW) return false;
        WORD offset = 0;
        auto add = [&](BYTE type, BYTE usage, BYTE index, WORD size) {
            elements.push_back({0, offset, type, static_cast<BYTE>(D3DDECLMETHOD_DEFAULT), usage, index});
            offset = static_cast<WORD>(offset + size);
        };
        add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0, 16);
        if (fvf & D3DFVF_PSIZE) add(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0, 4);
        if (fvf & D3DFVF_DIFFUSE) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0, 4);
        if (fvf & D3DFVF_SPECULAR) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1, 4);
        const DWORD count = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
        for (DWORD i = 0; i < count && i < 8; i++) {
            // D3DFVF_TEXCOORDSIZEn: 0 = 2 floats, 1 = 3, 2 = 4, 3 = 1.
            static const BYTE kTypes[4] = {D3DDECLTYPE_FLOAT2, D3DDECLTYPE_FLOAT3, D3DDECLTYPE_FLOAT4, D3DDECLTYPE_FLOAT1};
            static const WORD kSizes[4] = {8, 12, 16, 4};
            const DWORD format = (fvf >> (16 + i * 2)) & 3;
            add(kTypes[format], D3DDECLUSAGE_TEXCOORD, static_cast<BYTE>(i), kSizes[format]);
        }
    }
    for (const D3DVERTEXELEMENT9& e : elements) {
        if (e.Usage == D3DDECLUSAGE_POSITIONT) return true;
    }
    return false;
}

// The declaration and vertex shader for one layout of screen-pixel vertices (made once).
ScreenLayout& screenLayout(IDirect3DDevice9* device, const std::vector<D3DVERTEXELEMENT9>& elements) {
    for (ScreenLayout& l : g_screenLayouts) {
        if (l.elements.size() == elements.size() && !memcmp(l.elements.data(), elements.data(), elements.size() * sizeof(D3DVERTEXELEMENT9))) {
            return l;
        }
    }
    g_screenLayouts.push_back(ScreenLayout());
    ScreenLayout& l = g_screenLayouts.back();
    l.elements = elements;
    std::vector<D3DVERTEXELEMENT9> mine = elements;
    std::string inputs, outputs, copies, fog = "    o.fog = 1.0;\n";
    char line[160];
    for (size_t i = 0; i < mine.size() && !l.failed; i++) {
        D3DVERTEXELEMENT9& e = mine[i];
        if (e.Usage == D3DDECLUSAGE_POSITIONT) {
            if (e.Type != D3DDECLTYPE_FLOAT4 || e.UsageIndex != 0) l.failed = "their pixel positions aren't 4 floats";
            e.Usage = D3DDECLUSAGE_POSITION;
            continue;
        }
        if (e.Usage == D3DDECLUSAGE_PSIZE && e.UsageIndex == 0) {
            // Point sprites: their size in pixels scales too.
            inputs += "    float size : PSIZE;\n";
            outputs += "    float size : PSIZE;\n";
            copies += "    o.size = v.size * u_Depth.z;\n";
            continue;
        }
        const char* semantic = (e.Usage == D3DDECLUSAGE_COLOR && e.UsageIndex < 2)      ? "COLOR"
                               : (e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex < 8) ? "TEXCOORD"
                                                                                        : nullptr;
        if (!semantic) {
            l.failed = "their vertices carry more than colours and texture coordinates";
            break;
        }
        snprintf(line, sizeof(line), "    float4 a%u : %s%u;\n", static_cast<unsigned>(i), semantic, static_cast<unsigned>(e.UsageIndex));
        inputs += line;
        outputs += line;
        snprintf(line, sizeof(line), "    o.a%u = v.a%u;\n", static_cast<unsigned>(i), static_cast<unsigned>(i));
        copies += line;
        // Vertex fog of pre-transformed vertices is the specular colour's alpha.
        if (e.Usage == D3DDECLUSAGE_COLOR && e.UsageIndex == 1) {
            snprintf(line, sizeof(line), "    o.fog = v.a%u.a;\n", static_cast<unsigned>(i));
            fog = line;
        }
    }
    if (l.failed) return l;
    mine.push_back(D3DDECL_END());
    if (FAILED(device->CreateVertexDeclaration(mine.data(), &l.decl)) || !l.decl) {
        l.failed = "no vertex declaration for them";
        return l;
    }
    const std::string source = std::string(
                                   "// RenderScale: the game's vertices in pixels of the full size, into the scaled image.\n"
                                   "float4 u_Pixels : register(c0); // pixels -> clip space: x, y scale, x, y offset\n"
                                   "float4 u_Depth : register(c1);  // depth in the viewport's range: scale, offset; point size scale\n"
                                   "struct VSIn {\n"
                                   "    float4 pos : POSITION0;\n") +
                               inputs + "};\nstruct VSOut {\n    float4 pos : POSITION;\n" + outputs +
                               "    float fog : FOG;\n};\n"
                               "VSOut main(VSIn v) {\n"
                               "    VSOut o;\n"
                               "    float w = v.pos.w > 0.0 ? 1.0 / v.pos.w : 1.0;\n"
                               "    o.pos = float4((v.pos.x * u_Pixels.x + u_Pixels.z) * w, (v.pos.y * u_Pixels.y + u_Pixels.w) * w,\n"
                               "                   (v.pos.z * u_Depth.x + u_Depth.y) * w, w);\n" +
                               copies + fog + "    return o;\n}\n";
    // vs_2_0: with the fixed-function pixel stage or the game's ps_2_x shaders.
    l.vs = gfx::compileVertexShader(device, source, "main", "RenderScaleScreenVS", nullptr, "vs_2_0");
    if (!l.vs) l.failed = "their vertex shader didn't compile (see above)";
    static int s_lines = 0;
    if (s_lines++ < 8) {
        TMVS_LOG("render scale: draws in screen pixels (%u vertex elements): %s", static_cast<unsigned>(elements.size()),
                 l.failed ? l.failed : "scaled into the smaller image by a vertex shader of the mod");
    }
    return l;
}

// The major version of the bound pixel shader (0: none, the fixed-function stage).
int pixelShaderMajor(IDirect3DDevice9* device) {
    IDirect3DPixelShader9* ps = nullptr;
    device->GetPixelShader(&ps);
    if (!ps) return 0;
    static IDirect3DPixelShader9* s_last = nullptr;
    static int s_major = 0;
    if (ps != s_last) {
        s_last = ps;
        s_major = 0;
        UINT size = 0;
        if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= sizeof(DWORD)) {
            std::vector<DWORD> code(size / sizeof(DWORD) + 1);
            if (SUCCEEDED(ps->GetFunction(code.data(), &size))) s_major = static_cast<int>((code[0] >> 8) & 0xFF);
        }
    }
    ps->Release();
    return s_major;
}

// Before a draw into the scaled image: screen-pixel vertices get the mod's vertex shader for it.
void scaleScreenDraw(IDirect3DDevice9* device) {
    IDirect3DVertexShader9* vs = nullptr;
    device->GetVertexShader(&vs);
    if (vs) {
        vs->Release();
        return;
    }
    static std::vector<D3DVERTEXELEMENT9> s_elements;
    if (!screenElements(device, s_elements)) return;
    const char* why = nullptr;
    ScreenLayout* layout = nullptr;
    // A vs_2_0 shader works in hardware vertex processing and with pixel shaders before ps_3_0.
    if (device->GetSoftwareVertexProcessing()) {
        why = "software vertex processing";
    } else if (pixelShaderMajor(device) >= 3) {
        why = "a ps_3_0 pixel shader";
    } else {
        layout = &screenLayout(device, s_elements);
        why = layout->failed;
    }
    if (why) {
        g_rs.cameraPretransformed++;
        g_rs.cameraPretransformedWhy = why;
        return;
    }
    hook::InternalScope internal;
    D3DVIEWPORT9 vp{};
    device->GetViewport(&vp); // the scaled one (the game's numbers are only handed to the game)
    ScreenDrawSaved& s = g_screenSaved;
    device->GetVertexDeclaration(&s.decl);
    device->GetFVF(&s.fvf);
    device->GetVertexShaderConstantF(0, s.constants, 2);
    device->GetRenderState(D3DRS_CLIPPLANEENABLE, &s.clipPlanes);
    // A pixel x of the full size is (x + 0.5) * sx - 0.5 in the scaled image (pixel edges meet);
    // D3D9 puts clip space -1 on the viewport's first pixel centre: ndc = (x - vp.X) * 2 / vp.Width - 1.
    const float sx = static_cast<float>(g_rs.width) / static_cast<float>(g_rs.fullWidth);
    const float sy = static_cast<float>(g_rs.height) / static_cast<float>(g_rs.fullHeight);
    const float w = vp.Width ? static_cast<float>(vp.Width) : 1.0f, h = vp.Height ? static_cast<float>(vp.Height) : 1.0f;
    const float range = vp.MaxZ - vp.MinZ;
    const bool flat = fabsf(range) < 1e-6f;
    const float constants[8] = {2.0f * sx / w,
                                -2.0f * sy / h,
                                (0.5f * sx - 0.5f - static_cast<float>(vp.X)) * 2.0f / w - 1.0f,
                                1.0f - (0.5f * sy - 0.5f - static_cast<float>(vp.Y)) * 2.0f / h,
                                flat ? 1.0f : 1.0f / range,
                                flat ? 0.0f : -vp.MinZ / range,
                                sx,
                                0.0f};
    device->SetVertexDeclaration(layout->decl);
    device->SetVertexShader(layout->vs);
    device->SetVertexShaderConstantF(0, constants, 2);
    // User clip planes apply to transformed vertices only (in clip space): not to these.
    if (s.clipPlanes) device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    s.active = true;
    g_rs.counts.screenScaled++;
}

// After the draw: the game's vertex state as it was (its state caches stay true).
void screenDrawDone(IDirect3DDevice9* device) {
    ScreenDrawSaved& s = g_screenSaved;
    if (!s.active) return;
    s.active = false;
    hook::InternalScope internal;
    device->SetVertexShader(nullptr);
    if (s.fvf) device->SetFVF(s.fvf);
    else device->SetVertexDeclaration(s.decl);
    gfx::release(s.decl);
    device->SetVertexShaderConstantF(0, s.constants, 2);
    if (s.clipPlanes) device->SetRenderState(D3DRS_CLIPPLANEENABLE, s.clipPlanes);
}

// Before the scaled image draws over depth the game wrote at the full size: its corner of the depth
// buffer cleared (and of the stencil, while the material masks are the mod's).
void clearScaledCorner(IDirect3DDevice9* device) {
    hook::InternalScope internal;
    D3DVIEWPORT9 viewport{};
    DWORD scissorTest = FALSE;
    device->GetViewport(&viewport);
    device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorTest);
    const D3DVIEWPORT9 corner = {0, 0, g_rs.width, g_rs.height, 0.0f, 1.0f};
    device->SetViewport(&corner);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    bool ok = materialsWanted() && SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, g_rs.clearZ, 0));
    if (!ok) ok = SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, g_rs.clearZ, 0)); // no stencil in the format
    if (ok) g_rs.counts.cornerClears++;
    device->SetViewport(&viewport);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, scissorTest);
}

// RenderScale and the game's depth buffer. The scaled image uses its top left corner; the game's
// passes into full-size targets in the same camera write the whole buffer at the full size
// (Stadium, by day: the depth for its shadow mask, which the scene's drawing then tests against, as
// a depth pre-pass). There the full-size depth belongs to other pixels and would cut holes into the
// scaled scene, so the corner is cleared before the scaled image draws over it. Depth written at the
// full size after the scaled image's own can't be undone: counted, that map goes to the full size.
void scaledDepthBeforeDraw(IDirect3DDevice9* device, bool scaledTarget) {
    DWORD z = FALSE, zwrite = FALSE;
    device->GetRenderState(D3DRS_ZENABLE, &z);
    device->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
    const bool writes = z != FALSE && zwrite != FALSE;
    if (!scaledTarget) {
        if (writes && !g_rs.fullDepth) {
            IDirect3DSurface9* ds = nullptr;
            device->GetDepthStencilSurface(&ds);
            if (ds) ds->Release();
            g_rs.fullDepth = ds && ds == g_rs.depth;
        }
        return;
    }
    if (g_rs.fullDepth) {
        g_rs.fullDepth = false;
        if (g_rs.scaledDepth) g_rs.cameraMixed++;
        else clearScaledCorner(device);
    }
    if (writes) g_rs.scaledDepth = true;
}

// Every draw of the scaled camera (before the game's draw).
void onDrawScaled(IDirect3DDevice9* device) {
    if (!g_rs.active || !g_scene.inMainCamera) return;
    IDirect3DSurface9* rt = nullptr;
    device->GetRenderTarget(0, &rt);
    if (rt) rt->Release();
    const bool scaledTarget = rt && (rt == g_hdrScene.surface || rt == g_hdrAdded.surface);
    scaledDepthBeforeDraw(device, scaledTarget);
    // Screen pixels into a full-size target in the camera are right as they are.
    if (scaledTarget) scaleScreenDraw(device);
}

// Float targets with blending: the game blends glows and particles into the scene.
bool hdrSupported(IDirect3DDevice9* device) {
    if (g_hdrSupported < 0) {
        g_hdrSupported = 0;
        IDirect3D9* d3d = nullptr;
        D3DDEVICE_CREATION_PARAMETERS params{};
        D3DDISPLAYMODE mode{};
        if (SUCCEEDED(device->GetDirect3D(&d3d)) && d3d) {
            if (SUCCEEDED(device->GetCreationParameters(&params)) && SUCCEEDED(device->GetDisplayMode(0, &mode))) {
                g_hdrSupported = SUCCEEDED(d3d->CheckDeviceFormat(params.AdapterOrdinal, params.DeviceType, mode.Format,
                                                                  D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING,
                                                                  D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F))
                                     ? 1
                                     : 0;
            }
            d3d->Release();
        }
        TMVS_LOG("hdr: float render target with blending %s", g_hdrSupported ? "supported" : "not supported (GameHdr stays off)");
    }
    return g_hdrSupported == 1;
}

bool mrtSupported(IDirect3DDevice9* device) {
    if (g_mrtSupported < 0) {
        D3DCAPS9 caps{};
        g_mrtCount = SUCCEEDED(device->GetDeviceCaps(&caps)) ? static_cast<int>(caps.NumSimultaneousRTs) : 0;
        g_mrtSupported = g_mrtCount >= 2 ? 1 : 0;
        if (!g_mrtSupported) TMVS_LOG("gbuffer: the GPU draws into one target at a time: no normals from the game's shaders");
        else if (g_mrtCount < 3 && gameshaders::lightPatching()) TMVS_LOG("gbuffer: the GPU draws into two targets at a time: no light from the game's shaders");
    }
    return g_mrtSupported == 1;
}

// GameGBuffer at the main camera's start (from beginHdr, which keeps the game's viewport): the
// normals start empty. Needs the float scene: two targets must have the same bit depth. GameLight
// takes the same draws (it needs their distance too) and a third target.
void beginGBuffer(IDirect3DDevice9* device, UINT width, UINT height) {
    g_gbufActive = false;
    g_gbufBound = false;
    g_gbufLightActive = false;
    g_gbufLightBound = false;
    g_gbufCounting = GBufferDraws();
    const Settings& s = Config::get().settings;
    if (!s.enabled || !gameshaders::patching() || g_gameMrt || !mrtSupported(device)) return;
    const bool light = s.gameLight && gameshaders::lightPatching() && g_mrtCount >= 3;
    if (!s.gameGBuffer && !light) return;
    if (!g_gbuf.create(device, width, height, D3DFMT_A16B16G16R16F) || FAILED(device->SetRenderTarget(0, g_gbuf.surface))) return;
    DWORD scissorTest = FALSE;
    device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorTest);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_gbufActive = SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0));
    if (g_gbufActive && light && g_gbufLight.create(device, width, height, D3DFMT_A16B16G16R16F) &&
        SUCCEEDED(device->SetRenderTarget(0, g_gbufLight.surface))) {
        g_gbufLightActive = SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0));
    }
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, scissorTest);
    static bool s_logged = false, s_lightLogged = false;
    if (g_gbufActive && !s_logged) {
        s_logged = true;
        TMVS_LOG("gbuffer: the patched shaders' normals go into a second target (%ux%u)", width, height);
    }
    if (g_gbufLightActive && !s_lightLogged) {
        s_lightLogged = true;
        TMVS_LOG("gbuffer: the patched shaders' light goes into a third target (%ux%u)", width, height);
    }
}

void unbindGBuffer(IDirect3DDevice9* device) {
    if (!device || (!g_gbufBound && !g_gbufLightBound)) return;
    hook::InternalScope internal;
    // The light target first: target 1 never goes while target 2 is bound.
    if (g_gbufLightBound) device->SetRenderTarget(2, nullptr);
    g_gbufLightBound = false;
    if (g_gbufBound) device->SetRenderTarget(1, nullptr);
    g_gbufBound = false;
}

// Before each of the game's draws in the main camera: the normal target is bound for an opaque
// draw with a patched shader into the float scene, and for no other (unbound lazily: draws of
// one kind come in runs). SetRenderTarget(1, ...) keeps the game's viewport and scissor rect.
void onDrawGBuffer(IDirect3DDevice9* device) {
    if (!g_gbufActive) return;
    bool want = false;
    if (g_scene.inMainCamera) {
        DWORD ztest = FALSE, zwrite = FALSE, blend = TRUE;
        device->GetRenderState(D3DRS_ZENABLE, &ztest);
        device->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
        device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
        if (ztest && zwrite && !blend) {
            g_gbufCounting.opaque++;
            IDirect3DPixelShader9* ps = nullptr;
            device->GetPixelShader(&ps);
            const bool patched = gameshaders::writesNormal(ps);
            if (ps) ps->Release();
            bool onScene = false;
            if (patched) {
                IDirect3DSurface9* rt = nullptr;
                device->GetRenderTarget(0, &rt);
                onScene = rt && rt == g_hdrScene.surface;
                if (rt) rt->Release();
            }
            want = patched && onScene;
            if (!ps) g_gbufCounting.noShader++;
            else if (!patched) g_gbufCounting.otherShader++;
            else if (!onScene) g_gbufCounting.otherTarget++;
            else g_gbufCounting.written++;
            if (want && g_gbufLightActive && gameshaders::writesLight(ps)) g_gbufCounting.light++;
        }
    }
    // GameLight: every shader patched for it writes COLOR2 too (0 where it has no light).
    const bool wantLight = want && g_gbufLightActive;
    if (want == g_gbufBound && wantLight == g_gbufLightBound) return;
    // F11 traces show where the normal target comes and goes (the mod's own calls aren't traced);
    // the line follows the draw it applies to (the trace writes a draw before its callbacks run).
    if (tracer::tracing()) tracer::event("  gbuffer: normal target %s%s from the draw above on", want ? "bound" : "unbound", wantLight ? " (and the light's)" : "");
    if (!want) {
        unbindGBuffer(device);
        return;
    }
    hook::InternalScope internal;
    if (!g_gbufBound) {
        g_gbufBound = SUCCEEDED(device->SetRenderTarget(1, g_gbuf.surface));
        if (g_gbufBound) g_gbufCounting.binds++;
    }
    if (wantLight && g_gbufBound && !g_gbufLightBound) g_gbufLightBound = SUCCEEDED(device->SetRenderTarget(2, g_gbufLight.surface));
}

// At the main camera's end, before the image is resolved and shaded.
void endGBuffer(IDirect3DDevice9* device) {
    unbindGBuffer(device);
    g_gbufValid = g_gbufActive;
    g_gbufLightValid = g_gbufLightActive;
    if (g_gbufActive) g_gbufDraws = g_gbufCounting;
    g_gbufActive = false;
    g_gbufLightActive = false;
    // Once per map, a few seconds into the driving: how many of the game's opaque draws wrote normals.
    static int s_map = -1;
    static DWORD s_since = 0;
    if (g_gbufValid && g_scene.projection[14] > -1.0f && engine::mapLoads() != s_map) {
        if (!s_since) {
            s_since = GetTickCount();
        } else if (GetTickCount() - s_since > 8000) {
            s_map = engine::mapLoads();
            s_since = 0;
            const GBufferDraws& d = g_gbufDraws;
            TMVS_LOG("gbuffer: %u opaque draws in the main camera, %u wrote normals, %u with shaders that have none, %u without a "
                     "pixel shader, %u into another target; %u binds; %d shaders patched so far",
                     d.opaque, d.written, d.otherShader, d.noShader, d.otherTarget, d.binds, gameshaders::patchedCount());
            if (g_gbufLightValid) TMVS_LOG("gbuffer: %u of those draws wrote their light (GameLight)", d.light);
        }
    }
}

// At the main camera's start: the float target stands in for the game's.
void beginHdr(IDirect3DDevice9* device, IDirect3DSurface9* rt) {
    g_hdrValid = false;
    g_hdrAddedValid = false;
    g_hdrAddedActive = false;
    g_hdrAddedBound = false;
    g_gbufValid = false;
    g_gbufActive = false;
    g_gbufLightValid = false;
    g_gbufLightActive = false;
    g_rs.active = false;
    const Settings& s = Config::get().settings;
    if (!s.enabled || !s.gameHdr || g_hdrGame || !rt || !g_pipeline.ready() || !hdrSupported(device)) return;
    // RenderScale: a second camera into the same target in this frame (at a map's start a small 3D
    // view in the corner, 480x272 at 2039,1138 on a 1440p screen) draws straight into the game's
    // target at full size: the first camera's image is already shaded there (the effects don't run
    // twice on one target), and the float target keeps the scaled size instead of switching back
    // and forth (which turned the scaling off for seconds at every map start).
    if (s.renderScale < 0.99f && g_rs.sceneScaled && g_scene.mainRenders > 1 && rt == g_hdrLastGame) {
        g_rs.counts.secondCameras++;
        static int s_map = -1;
        if (s_map != engine::mapLoads()) {
            s_map = engine::mapLoads();
            TMVS_LOG("render scale: a second camera into the screen in the same frame: drawn at full size straight into the game's image, the "
                     "main camera stays scaled");
        }
        return;
    }
    D3DSURFACE_DESC desc{};
    rt->GetDesc(&desc);
    // RenderScale: the float target at the scaled size (at least 320 x 200, as a main camera).
    const char* fullWhy = nullptr;
    const float scale = renderScaleNow(s, &fullWhy);
    UINT width = desc.Width, height = desc.Height;
    if (scale < 1.0f) {
        width = static_cast<UINT>(desc.Width * scale + 0.5f);
        height = static_cast<UINT>(desc.Height * scale + 0.5f);
        width = width < 320 ? 320 : (width > desc.Width ? desc.Width : width);
        height = height < 200 ? 200 : (height > desc.Height ? desc.Height : height);
    }
    const bool scaled = width != desc.Width || height != desc.Height;
    {
        // Each change of the size, and why it is the full size (a limited number of lines; nothing
        // while the render scale stays 1).
        static UINT s_lastWidth = 0;
        static const char* s_lastWhy = nullptr;
        static int s_lines = 0;
        const bool changed = scaled ? width != s_lastWidth : (fullWhy != s_lastWhy || s_lastWidth != 0);
        if (changed && s_lines < 40) {
            s_lines++;
            if (scaled) TMVS_LOG("render scale: the 3D image at %ux%u (%.2f of %ux%u), FSR 1 up to the screen", width, height, scale, desc.Width, desc.Height);
            else TMVS_LOG("render scale: full size (%s)", fullWhy ? fullWhy : "render scale 1");
        }
        s_lastWidth = scaled ? width : 0;
        s_lastWhy = scaled ? nullptr : fullWhy;
    }
    if (!g_hdrScene.create(device, width, height, D3DFMT_A16B16G16R16F)) return;
    g_rs.sceneScaled = scaled;
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        TMVS_LOG("hdr: the main camera draws into a float target (%ux%u)", width, height);
    }
    hook::InternalScope internal;
    // SetRenderTarget resets the viewport and the scissor rect: the game's stay.
    D3DVIEWPORT9 viewport{};
    RECT scissor{};
    device->GetViewport(&viewport);
    device->GetScissorRect(&scissor);
    // The added light starts at 0 (the whole target: no scissor test for the Clear).
    if (g_hdrAdded.create(device, width, height, D3DFMT_A16B16G16R16F) && SUCCEEDED(device->SetRenderTarget(0, g_hdrAdded.surface))) {
        DWORD scissorTest = FALSE;
        device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorTest);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        g_hdrAddedActive = SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0));
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, scissorTest);
    }
    beginGBuffer(device, width, height);
    if (FAILED(device->SetRenderTarget(0, g_hdrScene.surface))) {
        device->SetRenderTarget(0, rt);
        device->SetViewport(&viewport);
        device->SetScissorRect(&scissor);
        g_hdrAddedActive = false;
        g_gbufActive = false;
        g_gbufLightActive = false;
        return;
    }
    // The float target starts the camera as the game's own did. The game clears its target before
    // the camera begins (opaque black), so that clear never reached the float one, which kept the
    // last frames where nothing opaque is drawn: the sky. Some skies (B13-Obstacle, my test
    // 2026-10-17) blend over what is there by its alpha, and the float target doesn't cut the alpha
    // at 1 as the 8-bit one does: the sky added up frame after frame into the hundreds (its alpha
    // counted the frames since the map loaded) and Bloom made white blobs of it. Without a clear of
    // the game's this frame: opaque black, as the game's.
    {
        const bool cleared = g_clearedTop.target == rt && g_clearedTop.frame == g_frame;
        DWORD scissorTest = FALSE;
        device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorTest);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->Clear(0, nullptr, D3DCLEAR_TARGET, cleared ? g_clearedTop.color : D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, scissorTest);
    }
    if (scaled) {
        g_rs.active = true;
        g_rs.fullWidth = desc.Width;
        g_rs.fullHeight = desc.Height;
        g_rs.width = width;
        g_rs.height = height;
        g_rs.logical = viewport;
        g_rs.device = scaledViewport(viewport);
        g_rs.logicalScissor = scissor;
        g_rs.deviceScissor = scaledRect(scissor);
        g_rs.cameraPretransformed = 0;
        g_rs.cameraPretransformedWhy = nullptr;
        g_rs.cameraMixed = 0;
        IDirect3DSurface9* ds = nullptr;
        device->GetDepthStencilSurface(&ds);
        if (ds) ds->Release();
        g_rs.depth = ds;
        g_rs.fullDepth = false;
        g_rs.scaledDepth = false;
        g_rs.counts.cameras++;
        if (g_scene.mainRenders == 1) perflog::renderScaled(static_cast<float>(width) / desc.Width);
        device->SetViewport(&g_rs.device);
        device->SetScissorRect(&g_rs.deviceScissor);
        // Textures as sharp as at the full size: the GPU picks mip levels by the pixels' size on
        // screen, which grew by 1 / scale (AMD's advice for FSR 1: a bias of log2(scale)).
        const float bias = log2f(static_cast<float>(width) / desc.Width);
        for (DWORD i = 0; i < 16; i++) {
            DWORD saved = 0;
            device->GetSamplerState(i, D3DSAMP_MIPMAPLODBIAS, &saved);
            memcpy(&g_rs.biasSaved[i], &saved, sizeof(saved));
            DWORD value = 0;
            memcpy(&value, &bias, sizeof(value));
            device->SetSamplerState(i, D3DSAMP_MIPMAPLODBIAS, value);
        }
    } else {
        device->SetViewport(&viewport);
        device->SetScissorRect(&scissor);
    }
    rt->AddRef();
    g_hdrGame = rt;
    g_hdrLastGame = rt;
}

// RenderScale's fallback: the scaled camera's image stretched into the game's target where the
// camera's viewport is (bilinear, unshaded, cut to 0..1 as GameHdr's resolve). The effects then run
// on it at the full size (a camera into part of the screen), or nothing does (the shaders off, the
// same target twice in a frame, a failure): the screen is never left without the camera's image.
void fillFromScaled(IDirect3DDevice9* device, IDirect3DSurface9* target) {
    if (!g_rs.pending || !target || !g_hdrScene.texture) return;
    g_rs.pending = false;
    g_rs.counts.fallbacks++;
    hook::InternalScope internal;
    const D3DVIEWPORT9& v = g_rs.logical;
    const RECT to = {static_cast<LONG>(v.X), static_cast<LONG>(v.Y), static_cast<LONG>(v.X + v.Width), static_cast<LONG>(v.Y + v.Height)};
    g_pipeline.beginStateSave(device);
    g_pipeline.stretchScene(device, g_hdrScene.texture, g_rs.area, target, to);
    g_pipeline.endStateSave(device);
    device->SetRenderTarget(0, target);
    device->SetViewport(&g_rs.logical);
    device->SetScissorRect(&g_rs.logicalScissor);
}

// RenderScale, at a scaled camera's end (its image in g_hdrScene, the game's target bound): what it
// couldn't follow, and the log.
void endScaledCamera(IDirect3DDevice9* device) {
    static int s_map = -1;
    static DWORD s_since = 0;
    static bool s_summed = false;
    if (engine::mapLoads() != s_map) {
        s_map = engine::mapLoads();
        g_rs.counts = RenderScaleState::Counts();
        s_since = GetTickCount();
        s_summed = false;
    }
    static int s_lines = 0;
    if (g_rs.cameraPretransformed > 0) {
        g_rs.counts.pretransformed += g_rs.cameraPretransformed;
        g_rs.offMap = engine::mapLoads();
        g_rs.offWhy = "off for this map: draws in screen pixels it can't scale";
        if (s_lines < 20) {
            s_lines++;
            TMVS_LOG("render scale: the game drew %u draws in screen pixels in the main camera that can't be scaled (%s): full size for the "
                     "rest of this map",
                     g_rs.cameraPretransformed, g_rs.cameraPretransformedWhy ? g_rs.cameraPretransformedWhy : "?");
        }
    }
    if (g_rs.cameraMixed > 0) {
        g_rs.offMap = engine::mapLoads();
        g_rs.offWhy = "off for this map: the game's depth written at both sizes";
        if (s_lines < 20) {
            s_lines++;
            TMVS_LOG("render scale: the game wrote its depth through a full-size target after the scaled image's own (%u times in the camera): "
                     "full size for the rest of this map",
                     g_rs.cameraMixed);
        }
    }
    // A camera into part of the screen: as processScene's test, a part is under 95% of the target.
    const D3DVIEWPORT9& v = g_rs.logical;
    if (v.Width < g_rs.fullWidth * 0.95f || v.Height < g_rs.fullHeight * 0.95f) {
        if (!g_rs.partUntil && s_lines < 20) {
            s_lines++;
            TMVS_LOG("render scale: a camera into part of the screen (%lux%lu at %lu,%lu): full size while there are such", v.Width, v.Height, v.X, v.Y);
        }
        g_rs.partUntil = GetTickCount() + 3000;
        IDirect3DSurface9* target = nullptr;
        device->GetRenderTarget(0, &target);
        fillFromScaled(device, target);
        if (target) target->Release();
    } else if (g_rs.partUntil && static_cast<LONG>(GetTickCount() - g_rs.partUntil) >= 0) {
        g_rs.partUntil = 0;
    }
    // Once per map, a few seconds in: what the game did with the scaled target (my log).
    if (!s_summed && GetTickCount() - s_since > 8000) {
        s_summed = true;
        const RenderScaleState::Counts& c = g_rs.counts;
        TMVS_LOG("render scale (map): %ux%u of %ux%u in %u cameras; the game set %u viewports, %u scissor rects (scaled), read them back %u "
                 "times, %u viewports of the scaled target's own size; %u clears and %u copies with rects; %u draws in screen pixels scaled, "
                 "%u not; its depth cleared %u times at the full size too, the scaled corner %u times before drawing over full-size depth; "
                 "%u second cameras at full size; %u fallbacks",
                 g_rs.width, g_rs.height, g_rs.fullWidth, g_rs.fullHeight, c.cameras, c.viewports, c.scissors, c.readBacks, c.sized,
                 c.rectClears, c.rectStretches, c.screenScaled, c.pretransformed, c.fullClears, c.cornerClears, c.secondCameras, c.fallbacks);
    }
}

// At the main camera's end, before the pipeline: the image goes into the game's target
// (cut to 0..1, as the game would have drawn it), which is bound again.
void endHdr(IDirect3DDevice9* device) {
    if (!g_hdrGame) return;
    hook::InternalScope internal;
    D3DVIEWPORT9 viewport{};
    RECT scissor{};
    device->GetViewport(&viewport);
    device->GetScissorRect(&scissor);
    const RECT area = {static_cast<LONG>(viewport.X), static_cast<LONG>(viewport.Y), static_cast<LONG>(viewport.X + viewport.Width),
                       static_cast<LONG>(viewport.Y + viewport.Height)};
    g_pipeline.beginStateSave(device);
    // The added light back into the scene (the image is the game's again), then the scene
    // into the game's target. RenderScale: processScene brings it up there (or fillFromScaled).
    g_hdrAddedValid = g_hdrAddedActive && g_pipeline.addScene(device, g_hdrAdded.texture, g_hdrScene.surface, area);
    g_hdrAddedActive = false;
    // Nothing above a ceiling the game's real light never reaches (Pipeline::capScene): what
    // went wrong in the float target can't blow up into Bloom, the light shafts and the exposure.
    g_pipeline.capScene(device, g_hdrScene.surface, area);
    const bool scaled = g_rs.active;
    g_hdrValid = scaled || g_pipeline.resolveScene(device, g_hdrScene.texture, g_hdrGame, area);
    g_pipeline.endStateSave(device);
    device->SetRenderTarget(0, g_hdrGame);
    // RenderScale: the game's own viewport and scissor rect on its target again, the samplers' mip
    // bias as it was.
    device->SetViewport(scaled ? &g_rs.logical : &viewport);
    device->SetScissorRect(scaled ? &g_rs.logicalScissor : &scissor);
    if (scaled) {
        // Only where the bias is still the mod's: the game sets it with its materials, through a
        // cache of what it set last (CDx9StateBlock::FilterSamplerState); a value it set itself in
        // the camera stays, or its cache and the device would disagree after the camera.
        const float bias = log2f(static_cast<float>(g_rs.width) / static_cast<float>(g_rs.fullWidth));
        DWORD ours = 0;
        memcpy(&ours, &bias, sizeof(ours));
        for (DWORD i = 0; i < 16; i++) {
            DWORD now = 0, saved = 0;
            device->GetSamplerState(i, D3DSAMP_MIPMAPLODBIAS, &now);
            if (now != ours) continue;
            memcpy(&saved, &g_rs.biasSaved[i], sizeof(saved));
            device->SetSamplerState(i, D3DSAMP_MIPMAPLODBIAS, saved);
        }
        g_rs.active = false;
        g_rs.pending = true;
        g_rs.area = area;
        endScaledCamera(device);
    }
    g_hdrGame->Release();
    g_hdrGame = nullptr;
    // Once per map, a few seconds into the driving: how much light the float target kept.
    static int s_map = -1;
    static DWORD s_since = 0;
    if (g_hdrValid && g_scene.projection[14] > -1.0f && engine::mapLoads() != s_map) {
        if (!s_since) {
            s_since = GetTickCount();
        } else if (GetTickCount() - s_since > 8000) {
            s_map = engine::mapLoads();
            s_since = 0;
            logHdrStats(device, "map");
        }
    }
}

IDirect3DSurface9* onSetRenderTarget(DWORD index, IDirect3DSurface9* surface) {
    // GameGBuffer: the normal target never stays bound under a target the game picks.
    unbindGBuffer(g_device);
    if (index > 0 && surface && !g_gameMrt && gameshaders::patching()) {
        g_gameMrt = true;
        g_gbufActive = false;
        g_gbufLightActive = false;
        TMVS_LOG("gbuffer: the game draws into a second target itself: no normals from its shaders");
    }
    if (index != 0 || !surface) return surface;
    // The game binds its target again during the camera: the float one stays.
    if (g_hdrGame && surface == g_hdrGame) return g_hdrScene.surface;
    // It asked for the target during the camera (GetRenderTarget gave it the float one) and
    // binds that later, for the HUD: its own target.
    if (!g_hdrGame && surface == g_hdrScene.surface && g_hdrLastGame) return g_hdrLastGame;
    return surface;
}

// Another mod's GetRenderTarget(0) (hook.cpp tells which caller is one): during the camera it is
// told the game's own target, not the float one (or the added light's) standing in for it.
// CenterSpeed recognises the car's draws only while the back buffer is bound. Risk: a mod that
// saves this target and binds it again mid-camera gets the float one back (onSetRenderTarget), but
// one that draws into it with its own device calls (StretchRect, GetRenderTargetData) touches the
// game's 8-bit target, which the camera's end overwrites.
IDirect3DSurface9* onOtherModRenderTarget(IDirect3DSurface9* bound) {
    if (!g_hdrGame || !bound) return nullptr;
    return bound == g_hdrScene.surface || bound == g_hdrAdded.surface ? g_hdrGame : nullptr;
}

// The F12 capture's statistics of the light the game added on top (capture_NNN.add16), counted
// on the capture's writer thread.
void addedStats(const std::vector<uint16_t>& half) {
    const size_t count = half.size() / 4;
    size_t lit = 0, strong = 0;
    float brightest = 0.0f;
    for (size_t i = 0; i < count; i++) {
        const float m = std::max(halfToFloat(half[i * 4]), std::max(halfToFloat(half[i * 4 + 1]), halfToFloat(half[i * 4 + 2])));
        lit += m > 1.0f / 255.0f;
        strong += m > 0.25f;
        brightest = std::max(brightest, m);
    }
    const double n = count ? static_cast<double>(count) / 100.0 : 1.0;
    TMVS_LOG("hdr (capture): added light on %.2f%% of the image, above 0.25 on %.2f%%, brightest %.2f", lit / n, strong / n, brightest);
}

// One-one blending with the colour added (glows, halos, headlight pools) and no depth writes.
bool addsLight(IDirect3DDevice9* device) {
    DWORD blend = FALSE, dest = D3DBLEND_ZERO, op = D3DBLENDOP_ADD, zwrite = TRUE;
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    if (!blend) return false;
    device->GetRenderState(D3DRS_DESTBLEND, &dest);
    device->GetRenderState(D3DRS_BLENDOP, &op);
    device->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
    return dest == D3DBLEND_ONE && op == D3DBLENDOP_ADD && !zwrite;
}

// GameHdr: an additive draw of the main camera goes into g_hdrAdded, for that draw only (it is
// bound again before the game's next call, so the game never finds it: a copy of the scene for
// the water's refraction takes the float scene). SetRenderTarget resets the viewport and the
// scissor rect: the game's stay.
D3DVIEWPORT9 g_addedViewport{};
RECT g_addedScissor{};

void onDrawAdded(IDirect3DDevice9* device) {
    if (!g_hdrAddedActive || !g_hdrGame || !g_scene.inMainCamera || !addsLight(device)) return;
    hook::InternalScope internal;
    IDirect3DSurface9* rt = nullptr;
    device->GetRenderTarget(0, &rt);
    const bool onScene = rt == g_hdrScene.surface;
    if (rt) rt->Release();
    if (!onScene) return;
    device->GetViewport(&g_addedViewport);
    device->GetScissorRect(&g_addedScissor);
    if (FAILED(device->SetRenderTarget(0, g_hdrAdded.surface))) return;
    device->SetViewport(&g_addedViewport);
    device->SetScissorRect(&g_addedScissor);
    g_hdrAddedBound = true;
}

void overlapBiasDone(IDirect3DDevice9* device);

void onDrawDone(IDirect3DDevice9* device) {
    overlapBiasDone(device);
    screenDrawDone(device); // RenderScale: the game's vertex state back after a draw in screen pixels
    if (!g_hdrAddedBound) return;
    hook::InternalScope internal;
    device->SetRenderTarget(0, g_hdrScene.surface);
    device->SetViewport(&g_addedViewport);
    device->SetScissorRect(&g_addedScissor);
    g_hdrAddedBound = false;
}

// GameGBuffer: the statistics of the normals next to the capture (capture_NNN.nrm16, the same
// format as the .hdr16: world normal xyz, distance from the eye in w, 0 where no patched shader
// drew). On the writer thread, with the draw counts of the captured frame.
void normalsStats(const std::vector<uint16_t>& half, const GBufferDraws& d) {
    const size_t count = half.size() / 4;
    size_t written = 0;
    for (size_t i = 0; i < count; i++) written += halfToFloat(half[i * 4 + 3]) > 0.0f;
    TMVS_LOG("gbuffer (capture): normals on %.1f%% of the image; %u of %u opaque draws wrote them (%u with shaders that have none, %u "
             "without a pixel shader)",
             count ? 100.0 * written / count : 0.0, d.written, d.opaque, d.otherShader, d.noShader);
}

// GameLight: the light next to the capture (capture_NNN.lit16, the same format: the light the
// game's shader multiplied the surface's colour with, rgb, and the distance from the eye in w; 0
// where no shader with its light drew). tools/gbuffer.py --light reads it against the depth.
void lightStats(const std::vector<uint16_t>& half, const GBufferDraws& d) {
    const size_t count = half.size() / 4;
    size_t written = 0;
    float brightest = 0.0f;
    for (size_t i = 0; i < count; i++) {
        if (halfToFloat(half[i * 4 + 3]) <= 0.0f) continue;
        written++;
        brightest = std::max(brightest, std::max(halfToFloat(half[i * 4]), std::max(halfToFloat(half[i * 4 + 1]), halfToFloat(half[i * 4 + 2]))));
    }
    TMVS_LOG("gbuffer (capture): light on %.1f%% of the image, brightest %.2f; %u of %u opaque draws wrote it", count ? 100.0 * written / count : 0.0,
             brightest, d.light, d.opaque);
}

// One F12 capture: what the game's thread read back, owned by the writer thread from then on
// (capwriter; D3D9 and the game's memory stay on the game's thread).
struct CaptureJob {
    struct Half {
        UINT width = 0, height = 0;
        std::vector<uint16_t> pixels; // RGBA half floats, rows top-down (empty: not captured)
    };
    std::wstring base; // dataDir\capture_NNN, the extensions are added
    FrameCapture frame;
    Half hdr, added, normals, light;
    GBufferDraws draws;
    // The material ids (capture_NNN.mat): the header and the materials (their names read from the
    // game here), then the ids.
    std::vector<uint8_t> materialTable, ids;
    int materials = 0, lights = 0;
    double readMs = 0.0; // on the game's thread
};

// One float target of the capture; logs as before when it can't be read.
void readHalf(IDirect3DDevice9* device, const gfx::Target& target, CaptureJob::Half& out, const char* failed) {
    if (!g_pipeline.readHalfColor(device, target.surface, out.pixels)) {
        TMVS_LOG("%s", failed);
        out.pixels.clear();
        return;
    }
    out.width = target.width;
    out.height = target.height;
}

// The material ids: "TMM1", width, height, count, then per id (1..count) class (engine::Material),
// flags (1 depth writes, 2 one-one blending, 4 its textures were read: neon by material), draws, the length and UTF-8 bytes of its shader
// file (and " | " its textures' files, "; " between them, when they can be read); then one id
// byte per pixel, rows top-down (0: none).
void readMaterialCapture(IDirect3DDevice9* device, CaptureJob& job) {
    if (!g_pipeline.readMaterialIds(device, job.ids) || job.ids.size() != static_cast<size_t>(g_frameWidth) * g_frameHeight) {
        TMVS_LOG("capture: material ids not read");
        job.ids.clear();
        return;
    }
    std::vector<uint8_t>& out = job.materialTable;
    const auto append = [&out](const void* data, size_t bytes) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        out.insert(out.end(), p, p + bytes);
    };
    const uint32_t header[4] = {0x314D4D54u, g_frameWidth, g_frameHeight, static_cast<uint32_t>(g_idMaterials.size())}; // "TMM1"
    append(header, sizeof(header));
    for (const IdMaterial& m : g_idMaterials) {
        wchar_t file[2048] = {};
        char utf8[4096] = {};
        if (!engine::materialFile(m.shader, file, 512)) swprintf(file, 512, m.shader ? L"(unnamed %p)" : L"(none)", m.shader);
        // Which material is which (the neon strips' textures, the paint's): the shader file is
        // shared by most of them.
        const size_t named = wcslen(file);
        const bool textures = named + 3 < 2048 && engine::materialTextures(m.shader, file + named + 3, 2048 - named - 3);
        if (textures) wmemcpy(file + named, L" | ", 3);
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, file, -1, utf8, sizeof(utf8), nullptr, nullptr);
        const uint8_t kind[2] = {static_cast<uint8_t>(engine::materialClass(m.shader)),
                                 static_cast<uint8_t>((m.depthWrite ? 1 : 0) | (m.additive ? 2 : 0) | (textures ? 4 : 0))};
        const uint32_t draws = static_cast<uint32_t>(m.draws);
        const uint16_t length = static_cast<uint16_t>(bytes > 0 ? bytes - 1 : 0);
        append(kind, sizeof(kind));
        append(&draws, sizeof(draws));
        append(&length, sizeof(length));
        append(utf8, length);
        if (kind[0] == static_cast<uint8_t>(engine::Material::Light)) job.lights++;
    }
    job.materials = static_cast<int>(g_idMaterials.size());
}

// The writer thread's half: the statistics for the log and the files, each under a temporary
// name until it is complete.
void writeCaptureFiles(CaptureJob& job) {
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    static bool s_cleaned = false;
    if (!s_cleaned) {
        s_cleaned = true;
        const int removed = capwriter::removeUnfinished(log::dataDir(), L"capture_*.part") + capwriter::removeUnfinished(log::dataDir(), L"screen_*.part");
        if (removed) TMVS_LOG("capture: removed %d unfinished capture files of an earlier session (the game closed while writing)", removed);
    }
    const size_t slash = job.base.find_last_of(L"\\");
    const std::wstring name = job.base.substr(slash + 1);
    double megabytes = 0.0;
    const auto count = [&megabytes](size_t bytes) { megabytes += static_cast<double>(bytes) / (1024.0 * 1024.0); };
    TMVS_LOG("capture: writing %ls.tmcap", name.c_str());
    const bool written = capwriter::writeVia(job.base + L".tmcap", [&job](const std::wstring& path) { return job.frame.save(path); });
    count(job.frame.color.size() * 4 + job.frame.depth.size() * 4);
    TMVS_LOG("capture: %s %ls.tmcap", written ? "wrote" : "could not write", name.c_str());
    const auto writeHalf = [&](const CaptureJob::Half& half, const wchar_t* extension) {
        capwriter::writeVia(job.base + extension, [&half](const std::wstring& path) { return writeHalfFile(path, half.width, half.height, half.pixels); });
        count(half.pixels.size() * 2);
    };
    // GameHdr: the image before the game's target cut it, next to it (capture_NNN.hdr16).
    if (!job.hdr.pixels.empty()) {
        hdrStats(job.hdr.pixels, "capture");
        writeHalf(job.hdr, L".hdr16");
    }
    // ... and the light the game added on top (capture_NNN.add16).
    if (!job.added.pixels.empty()) {
        addedStats(job.added.pixels);
        writeHalf(job.added, L".add16");
    }
    // GameGBuffer: the game's normals (capture_NNN.nrm16).
    if (!job.normals.pixels.empty()) {
        normalsStats(job.normals.pixels, job.draws);
        writeHalf(job.normals, L".nrm16");
    }
    // GameLight: the light the game's shaders lit each surface with (capture_NNN.lit16).
    if (!job.light.pixels.empty()) {
        lightStats(job.light.pixels, job.draws);
        writeHalf(job.light, L".lit16");
    }
    // Which material drew each pixel (capture_NNN.mat).
    if (!job.ids.empty()) {
        const bool idsWritten = capwriter::writeVia(job.base + L".mat", [&job](const std::wstring& path) {
            FILE* f = _wfopen(path.c_str(), L"wb");
            if (!f) return false;
            fwrite(job.materialTable.data(), 1, job.materialTable.size(), f);
            fwrite(job.ids.data(), 1, job.ids.size(), f);
            fclose(f);
            return true;
        });
        count(job.materialTable.size() + job.ids.size());
        if (idsWritten) TMVS_LOG("capture: material ids of %d materials (%d lights)", job.materials, job.lights);
    }
    QueryPerformanceCounter(&end);
    TMVS_LOG("capture: %ls done, %.0f MB written in %.0f ms on the writer thread (the game's thread spent %.0f ms reading it back)", name.c_str(),
             megabytes, static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(g_frequency.QuadPart), job.readMs);
}

// The screenshot that goes with a capture (screen_NNN.bmp, effects and HUD) is read at the
// frame's Present; -1 = none wanted.
int g_screenshotNumber = -1;

void writeCapture(IDirect3DDevice9* device, const Pipeline::Inputs& inputs, bool materialIds) {
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    auto job = std::make_shared<CaptureJob>();
    FrameCapture& cap = job->frame;
    cap.width = inputs.width;
    cap.height = inputs.height;
    memcpy(cap.view, inputs.view, sizeof(cap.view));
    memcpy(cap.projection, inputs.projection, sizeof(cap.projection));
    memcpy(cap.sunDirection, inputs.sunDirection, sizeof(float) * 3);
    cap.sunDirection[3] = inputs.sunKnown ? 1.0f : 0.0f;
    memcpy(cap.sunColor, inputs.sunColor, sizeof(float) * 3);
    cap.sunColor[3] = inputs.sunColorKnown ? 1.0f : 0.0f;
    cap.time = inputs.time;
    // Only the copies here (the GPU's targets into memory): converting, counting and writing the
    // 100-200 MB is the writer's.
    try {
        if (!g_pipeline.readColor(device, g_sceneCopy.surface, cap.color) || !g_pipeline.readDepth(device, inputs.depth, cap.depth)) {
            TMVS_LOG("capture: readback failed");
            return;
        }
        // A part of the screen (the replay after a race): its cut-out float images.
        if (inputs.hdr && inputs.hdr == g_partHdr.texture)
            readHalf(device, g_partHdr, job->hdr, "hdr: readback failed");
        else if (g_hdrValid && g_hdrScene.width == inputs.width && g_hdrScene.height == inputs.height)
            readHalf(device, g_hdrScene, job->hdr, "hdr: readback failed");
        if (inputs.added && inputs.added == g_partAdded.texture)
            readHalf(device, g_partAdded, job->added, "hdr: added light not read");
        else if (g_hdrAddedValid && g_hdrAdded.width == inputs.width && g_hdrAdded.height == inputs.height)
            readHalf(device, g_hdrAdded, job->added, "hdr: added light not read");
        if (g_gbufValid && g_gbuf.width == inputs.width && g_gbuf.height == inputs.height)
            readHalf(device, g_gbuf, job->normals, "gbuffer: normals not read");
        if (g_gbufLightValid && g_gbufLight.width == inputs.width && g_gbufLight.height == inputs.height)
            readHalf(device, g_gbufLight, job->light, "gbuffer: light not read");
        if (materialIds) readMaterialCapture(device, *job);
    } catch (const std::bad_alloc&) {
        // A 32-bit game: the copies of a large capture may not fit next to it.
        TMVS_LOG("capture: not enough memory for the copies, no capture");
        return;
    }
    job->draws = g_gbufDraws;
    wchar_t name[64];
    // The next free number: captures of earlier sessions are kept. The counter is past every
    // capture of this session, also one the writer has not finished yet.
    for (;;) {
        swprintf(name, 64, L"\\capture_%03d.tmcap", g_capturesWritten);
        if (GetFileAttributesW((log::dataDir() + name).c_str()) == INVALID_FILE_ATTRIBUTES) break;
        g_capturesWritten++;
    }
    g_screenshotNumber = g_capturesWritten;
    g_capturesWritten++;
    job->base = log::dataDir() + name;
    job->base.resize(job->base.size() - 6); // without ".tmcap"
    QueryPerformanceCounter(&end);
    job->readMs = static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(g_frequency.QuadPart);
    capwriter::post([job] { writeCaptureFiles(*job); });
}

// Switching presets (or a map bringing its own look) glides over about a second instead of
// jumping: every number and colour eases toward its new value. Switches (sky, on/off) change
// at once. Angles take the short way round.
Settings g_shown;
float g_shownTime = -1.0f;

void smoothLook(Settings& s, float now) {
    const float dt = now - g_shownTime;
    if (g_shownTime < 0.0f || dt > 0.5f) {
        g_shown = s; // first frame, or after a pause (loading): no glide
        g_shownTime = now;
        return;
    }
    if (dt > 0.0f) { // several cameras in one frame share one step
        g_shownTime = now;
        const float k = 1.0f - expf(-dt / 0.3f);
        char* shown = reinterpret_cast<char*>(&g_shown);
        const char* target = reinterpret_cast<const char*>(&s);
        for (const Field& f : fields()) {
            float* a = reinterpret_cast<float*>(shown + f.offset);
            const float* b = reinterpret_cast<const float*>(target + f.offset);
            if (f.kind == Field::Float || f.kind == Field::Color) {
                const bool angle = strstr(f.key, "Azimuth") || !strcmp(f.key, "SkyRotation");
                for (int i = 0; i < (f.kind == Field::Color ? 3 : 1); i++) {
                    float d = b[i] - a[i];
                    if (angle) d = fmodf(d + 540.0f, 360.0f) - 180.0f;
                    a[i] = fabsf(d) < 1e-4f * (fabsf(b[i]) + 1.0f) ? b[i] : a[i] + d * k;
                    if (angle) a[i] = fmodf(a[i] + 360.0f, 360.0f);
                }
            } else {
                memcpy(shown + f.offset, target + f.offset, f.kind == Field::Bool ? sizeof(bool) : sizeof(int));
            }
        }
    }
    const bool enabled = s.enabled;
    const int debugView = s.debugView;
    s = g_shown;
    s.enabled = enabled;
    s.debugView = debugView;
}

// The replay menu at the end of a race: the replay shrinks into a full-width band between the
// menu's half-transparent bars (2560x1364 at 0,38 down to 2560x1080 at 0,180 within half a
// second) and the game draws nothing above and below it any more. Under the bars stays what the
// shrinking band left there, a strip of each frame: in capture 075 the image jumps exactly at
// rows 38, 56, 72, 88, ... 180 and 1402, 1384, ... 1260, the band's edges frame by frame. A
// stepped, blocky copy, each strip from another frame, scaled differently and shaded on its own
// (the pipeline's targets were new every frame: no history). ShadeMenuBars on: the shaded band,
// mirrored at its edges, fills those rows; off: black, as the game's own bars (under the
// half-transparent bars the mirror reads as the road reflecting the menu: a tester wanted it
// off). Not where the game cleared them itself this frame, not for parts that leave columns free
// (the MediaTracker editor's panels).
void fillMenuBars(IDirect3DDevice9* device, IDirect3DSurface9* target, const D3DSURFACE_DESC& desc, const RECT& area, bool mirrored) {
    // Every change of what happens to the rows goes into the log (a limited number of lines): the
    // replay menu's bars came back with leftovers in a build where the log had only said it once.
    static int s_last = -1, s_lines = 0;
    auto note = [](int what) {
        if (what == s_last || s_lines >= 40) return false;
        s_last = what;
        s_lines++;
        return true;
    };
    if (area.left != 0 || area.right != static_cast<LONG>(desc.Width)) {
        if (note(8)) TMVS_LOG("scene: the camera's part (%ld,%ld .. %ld,%ld) leaves columns free: rows outside it left alone", area.left, area.top, area.right, area.bottom);
        return;
    }
    // Only a clear of the bar rows themselves is the game's own bars. One that also covered the
    // band was the frame's first clear, the band drawn over it after: after an alt-tab the game
    // clears the whole screen black every frame, which left the bars black and unshaded (my test,
    // 2026-10-16: "the mirroring isn't there" after minimising).
    const bool clearedTop = g_clearedTop.target == target && g_clearedTop.frame == g_frame && g_clearedTop.bottom <= area.top;
    const bool clearedBottom = g_clearedBottom.target == target && g_clearedBottom.frame == g_frame && g_clearedBottom.top >= area.bottom;
    const bool top = area.top > 0 && !clearedTop;
    const bool bottom = area.bottom < static_cast<LONG>(desc.Height) && !clearedBottom;
    int state = 0; // 0 left alone (cleared by the game), 1 filled, 2 failed
    if (top || bottom) state = g_pipeline.fillBars(device, mirrored ? g_partOut.texture : nullptr, target, area, top, bottom) ? 1 : 2;
    if (!note(state * 4 + (top ? 1 : 0) + (bottom ? 2 : 0))) return;
    if (state == 1) {
        TMVS_LOG("scene: rows outside the camera's band (%ld above, %ld below; band %ldx%ld at %ld) hold earlier frames: %s",
                 top ? area.top : 0L, bottom ? static_cast<LONG>(desc.Height) - area.bottom : 0L, area.right - area.left, area.bottom - area.top, area.top,
                 mirrored ? "filled with the shaded band, mirrored" : "cleared black");
    } else if (state == 2) {
        TMVS_LOG("scene: filling the rows outside the camera's band (%ld..%ld) failed: left as the game had them", area.top, area.bottom);
    }
    // Rows the game cleared itself this frame (left alone): which clear it was.
    const ClearedRows* clears[2] = {clearedTop && area.top > 0 ? &g_clearedTop : nullptr,
                                    clearedBottom && area.bottom < static_cast<LONG>(desc.Height) ? &g_clearedBottom : nullptr};
    for (int i = 0; i < 2; i++) {
        const ClearedRows* c = clears[i];
        if (!c) continue;
        TMVS_LOG("scene: the game cleared the rows %s the camera's band (%ld..%ld) itself this frame: rows %ld..%ld, colour %08lx, %lu rects, scissor test %s: left alone",
                 i ? "below" : "above", area.top, area.bottom, c->top, c->bottom, c->color, c->rects, c->scissor ? "on" : "off");
    }
}

// Runs the post pipeline over the main camera's image, before the HUD is drawn.
void processScene(IDirect3DDevice9* device) {
    Settings& settings = Config::get().settings;
    // The video export's supersampling (its dialog's "high quality" without the sub-frames of
    // its motion blur): CSceneFxSuperSample::FxRender renders the camera n x n times a frame,
    // each sample with a sub-pixel offset into the texture "FxSS Color" (CameraToTexture), and
    // adds each into "FxSS Accum1"; the average goes to the screen. Each sample is shaded (the
    // video got 1 / (n n) of the effects otherwise), the first as the frame, the rest repeat it
    // (its motion blur, focus, exposure). Only in the video's frames: the sound pass after them
    // renders without it (CHmsViewport::HighQualityDisableAll), one camera a frame.
    const bool samples = g_exporting && g_frameStepped && g_export.samplesPerFrame > 1;
    // A camera drawn into a texture has its view and projection set before it begins (see
    // engine::cameraMatrices): within it the SetTransform hook sees none, or a shadow's or a
    // projector's. During the export the samples take the engine's (all alike), and so does any
    // main camera without them.
    if (g_exporting && g_scene.haveEngine && (samples || !g_scene.haveView || !g_scene.haveProjection)) {
        if (g_frameStepped) {
            g_exportStats.engineMatrices++;
            if (!g_scene.haveView || !g_scene.haveProjection) g_exportStats.noneSet++;
        }
        memcpy(g_scene.view, g_scene.engineView, sizeof(g_scene.view));
        memcpy(g_scene.projection, g_scene.engineProjection, sizeof(g_scene.projection));
        g_scene.haveView = g_scene.haveProjection = true;
    }
    if (!g_scene.haveView || !g_scene.haveProjection) return;
    if (!settings.enabled && !photo::active() && !g_captureRequested) {
        if (g_pipelineB.ready()) releaseSideB(); // photo mode ended with the shaders off
        return;
    }
    if (g_sceneStart < 0.0f && g_scene.projection[14] > -1.0f) {
        g_sceneStart = seconds();
        TMVS_LOG("scene: first gameplay camera");
    }
    // A new map: its own look (from its comments), before the mood picks a preset.
    {
        static int s_mapLoads = 0;
        if (engine::mapLoads() != s_mapLoads) {
            s_mapLoads = engine::mapLoads();
            Config::get().onMapComments(engine::mapComments());
        }
    }
    // Map mood, only in gameplay: menu backgrounds have their own light. From the map's time of
    // day when the engine told it (a map lit at midnight keeps its own mood; Rally at noon has
    // a sun warm enough for the colour test to call it sunset), else from the sun's colour.
    if (g_scene.haveLight && g_scene.projection[14] > -1.0f) {
        const float dayTime = engine::mapDayTime();
        Config::get().onMoodDetected(dayTime >= 0.0f ? moodOfDayTime(dayTime) : classifyMood(g_scene.lightColor));
        // The game's light changing its kind during a map (the moon of a real night turning into
        // a sun: reported in end-of-race replays), for the log.
        static int s_lightMood = -1, s_lightMap = -1, s_lightLines = 0;
        const int mood = static_cast<int>(classifyMood(g_scene.lightColor));
        if (engine::mapLoads() != s_lightMap) {
            s_lightMap = engine::mapLoads();
            s_lightMood = mood;
        } else if (mood != s_lightMood && s_lightLines < 20) {
            s_lightLines++;
            TMVS_LOG("engine: the game's light changed from mood %d to %d (colour %.2f %.2f %.2f, direction %.2f %.2f %.2f, cinematic %d, real night %d)",
                     s_lightMood, mood, g_scene.lightColor[0], g_scene.lightColor[1], g_scene.lightColor[2], g_scene.lightDirection[0],
                     g_scene.lightDirection[1], g_scene.lightDirection[2], engine::cinematicActive() ? 1 : 0, engine::mapLitAtNight() ? 1 : 0);
            s_lightMood = mood;
        }
    }

    hook::InternalScope internal;
    IDirect3DSurface9* target = nullptr;
    IDirect3DSurface9* savedDepth = nullptr;
    if (FAILED(device->GetRenderTarget(0, &target)) || !target) return;
    const bool repeat = samples && g_scene.processedCount > 0;
    // The same target twice in a frame would be shaded twice.
    if (g_scene.processedCount > 0 && target == g_scene.processedTarget && !repeat) {
        target->Release();
        return;
    }
    // Only the first camera of a single-camera frame continues the temporal history (and the
    // first of the export's samples: every frame has them).
    const bool temporal = g_scene.processedCount == 0 && (g_scene.lastFrameCount <= 1 || samples);
    if (g_scene.processedCount == 1 && !g_scene.loggedMultiCamera && !repeat) {
        g_scene.loggedMultiCamera = true;
        TMVS_LOG("scene: several main cameras in one frame (camera blend) - shading each, temporal effects paused");
    }
    g_scene.processedCount++;
    g_scene.processedTarget = target;
    device->GetDepthStencilSurface(&savedDepth);
    IDirect3DTexture9* sceneDepth = depth::textureFor(savedDepth);
    if (!sceneDepth) {
        target->Release();
        if (savedDepth) savedDepth->Release();
        return;
    }

    D3DSURFACE_DESC desc{};
    target->GetDesc(&desc);
    // The MediaTracker editor draws its 3D view into part of the screen, its panels around it
    // (drawn later, half transparent). Only that part is shaded: shading the whole screen
    // turned the panels' background into sky, and the effects missed the view (the camera's
    // projection belongs to its part of the screen).
    D3DVIEWPORT9 viewport{};
    device->GetViewport(&viewport);
    // Real panels only: replay cameras sometimes leave a few rows out (2560x1433 at 0,3).
    const bool part = viewport.Width >= 320 && viewport.Height >= 200 && viewport.X + viewport.Width <= desc.Width &&
                      viewport.Y + viewport.Height <= desc.Height &&
                      (viewport.Width < desc.Width * 0.95f || viewport.Height < desc.Height * 0.95f);
    const RECT area = {static_cast<LONG>(viewport.X), static_cast<LONG>(viewport.Y), static_cast<LONG>(viewport.X + viewport.Width),
                       static_cast<LONG>(viewport.Y + viewport.Height)};
    // RenderScale: the camera was drawn scaled, its image is in g_hdrScene and not yet in the game's
    // target: the effects run at that size and FSR brings the result up to the target. Anything else
    // gets the scaled image stretched into the target first and is shaded as before.
    const bool scaledPath = g_rs.pending && !part && g_hdrValid;
    if (g_rs.pending && !scaledPath) fillFromScaled(device, target);
    if (part && settings.renderScale < 0.99f) g_rs.partUntil = GetTickCount() + 3000; // see endScaledCamera
    const UINT width = scaledPath ? g_rs.width : (part ? viewport.Width : desc.Width);
    const UINT height = scaledPath ? g_rs.height : (part ? viewport.Height : desc.Height);
    {
        // Each time it starts (a limited number of lines): the replay menu after every race.
        static bool s_wasPart = false;
        static int s_partLines = 0;
        if (part && !s_wasPart && s_partLines < 20) {
            s_partLines++;
            TMVS_LOG("scene: the camera draws into part of the screen (%ux%u at %u,%u): shading that part", width, height, viewport.X, viewport.Y);
        }
        s_wasPart = part;
    }
    if (!g_pipeline.ready() && (g_pipelineFailed || !g_pipeline.init(device, g_dev.shaderDir))) {
        g_pipelineFailed = true;
        target->Release();
        if (savedDepth) savedDepth->Release();
        return;
    }
    // The supersampling's sample texture can be a float one: StretchRect doesn't convert float
    // into 8 bits, the copy takes its format then (export only).
    const bool floatTarget = desc.Format == D3DFMT_A16B16G16R16F || desc.Format == D3DFMT_A32B32G32R32F;
    const D3DFORMAT copyFormat = g_exporting && floatTarget && !part ? desc.Format : D3DFMT_A8R8G8B8;
    if (g_exporting && g_frameStepped) {
        if (copyFormat != D3DFMT_A8R8G8B8) g_exportStats.floatCopies++;
        g_exportStats.targetFormat = desc.Format;
        g_exportStats.targetWidth = desc.Width;
        g_exportStats.targetHeight = desc.Height;
    }
    if (!g_sceneCopy.create(device, width, height, copyFormat) || (part && !g_partOut.create(device, width, height, D3DFMT_A8R8G8B8))) {
        target->Release();
        if (savedDepth) savedDepth->Release();
        return;
    }

    g_pipeline.beginStateSave(device);
    // The 8-bit copy of the scene (RenderScale: GameHdr's resolve at the scaled size, from the float
    // target; the game's target doesn't have the image).
    const RECT scaledArea = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    if (scaledPath) g_pipeline.resolveScene(device, g_hdrScene.texture, g_sceneCopy.surface, scaledArea);
    else device->StretchRect(target, part ? &area : nullptr, g_sceneCopy.surface, nullptr, D3DTEXF_NONE);
    device->SetDepthStencilSurface(nullptr);
    // RenderScale: the depth of the scaled camera is the depth buffer's top left corner; the stencil
    // (the materials) stays the game's buffer, read there by targets of the scaled size.
    IDirect3DTexture9* const stencil = sceneDepth;
    if (part || scaledPath) {
        if (!g_pipeline.copyDepthArea(device, sceneDepth, part ? area : scaledArea, g_partDepth)) {
            device->SetRenderTarget(0, target);
            device->SetDepthStencilSurface(savedDepth);
            g_pipeline.endStateSave(device);
            target->Release();
            if (savedDepth) savedDepth->Release();
            return;
        }
        sceneDepth = g_partDepth.texture;
    }
    // Where the pipeline writes: the screen, or the part's own image (copied into place below).
    IDirect3DSurface9* shaded = part ? g_partOut.surface : target;

    Pipeline::Inputs inputs;
    inputs.color = g_sceneCopy.texture;
    if (g_hdrValid && !part && g_hdrScene.width == width && g_hdrScene.height == height) {
        inputs.hdr = g_hdrScene.texture;
        if (g_hdrAddedValid) inputs.added = g_hdrAdded.texture;
        if (g_gbufValid && g_gbuf.width == width && g_gbuf.height == height) inputs.gbuffer = g_gbuf.texture;
        if (g_gbufLightValid && g_gbufLight.width == width && g_gbufLight.height == height) inputs.gameLight = g_gbufLight.texture;
    } else if (g_hdrValid && part && g_hdrScene.width == desc.Width && g_hdrScene.height == desc.Height) {
        // A part of the screen (the replay after a race, the MediaTracker editor): the float scene
        // has the whole target's size with the camera's image at its place; that part of it, as
        // with the depth. (Not the game's normals and light: experimental, off by default.)
        const bool hdrCut = g_pipeline.copyColorArea(device, g_hdrScene.texture, area, g_partHdr);
        const bool addedCut = hdrCut && g_hdrAddedValid && g_pipeline.copyColorArea(device, g_hdrAdded.texture, area, g_partAdded);
        if (hdrCut) inputs.hdr = g_partHdr.texture;
        if (addedCut) inputs.added = g_partAdded.texture;
        // Each change (a limited number of lines): my log tells which way the replay went.
        static int s_partHdrLines = 0;
        static bool s_partHdrWas = false;
        if (hdrCut != s_partHdrWas && s_partHdrLines < 10) {
            s_partHdrLines++;
            if (hdrCut) TMVS_LOG("scene: the part of the screen is shaded from the game's float image (%ux%u, its added light %s)", width, height,
                                 addedCut ? "too" : "not");
            else TMVS_LOG("scene: the part of the screen: the float image could not be cut out, shading the 8-bit image");
        }
        s_partHdrWas = hdrCut;
    } else if (part) {
        static bool s_logged = false;
        if (!s_logged && settings.gameHdr) {
            s_logged = true;
            TMVS_LOG("scene: the part of the screen is shaded from the 8-bit image (float image valid %d, %ux%u for a %ux%u target)",
                     g_hdrValid ? 1 : 0, g_hdrScene.width, g_hdrScene.height, desc.Width, desc.Height);
        }
    }
    inputs.depth = sceneDepth;
    inputs.width = width;
    inputs.height = height;
    if (scaledPath) {
        inputs.stencil = stencil;
        inputs.outputWidth = desc.Width;
        inputs.outputHeight = desc.Height;
    }
    memcpy(inputs.view, g_scene.view, sizeof(inputs.view));
    memcpy(inputs.projection, g_scene.projection, sizeof(inputs.projection));
    inputs.sunKnown = sunDirection(settings, inputs.sunDirection);
    inputs.sunColorKnown = g_scene.haveLight;
    inputs.moonReplaced = engine::moonReplaced();
    memcpy(inputs.sunColor, g_scene.lightColor, sizeof(inputs.sunColor));
    inputs.time = effectTime();
    inputs.temporal = temporal;
    inputs.video = g_exporting;
    inputs.repeat = repeat;
    {
        float lo = 0.0f, hi = 0.0f;
        if (engine::waterHeights(lo, hi)) {
            // -1e30 = none (TMUF Island reports no sea: 0, the pipeline measures it from the
            // sea's material). Stadium: 2, its water blocks (only their height is known).
            const bool known = lo > -1e29f || hi > -1e29f;
            inputs.water[0] = lo;
            inputs.water[1] = hi;
            inputs.water[2] = !known ? 0.0f : (engine::stadium() ? 2.0f : 1.0f);
        }
    }
    // The export counts as cinematic for as long as it runs (the hooks' 500 ms ran out within
    // one of its frames: a 4K frame with supersampling lost its blur halfway through).
    const bool cinematic = engine::cinematicActive() || g_exporting;
    inputs.driving = !cinematic;
    inputs.carKnown = engine::carLocation(inputs.carPosition, inputs.carUp);
    {
        // The spray's car: its axes and tyres. Once per export the log says how often they were known.
        engine::CarPose pose;
        inputs.carPoseKnown = engine::carPose(pose);
        if (inputs.carPoseKnown) {
            memcpy(inputs.carPosePosition, pose.position, sizeof(inputs.carPosePosition));
            memcpy(inputs.carForward, pose.forward, sizeof(inputs.carForward));
            memcpy(inputs.carRight, pose.right, sizeof(inputs.carRight));
            memcpy(inputs.carPoseUp, pose.up, sizeof(inputs.carPoseUp));
            const float wheels[4] = {pose.frontAxle, pose.rearAxle, pose.halfTrack, pose.contactHeight};
            memcpy(inputs.carWheels, wheels, sizeof(inputs.carWheels));
        }
        static bool s_exporting = false, s_measured = false;
        static unsigned s_frames = 0, s_known = 0, s_onScreen = 0;
        static double s_screen[3] = {};
        if (g_exporting) {
            if (!s_exporting) {
                s_frames = s_known = s_onScreen = 0;
                memset(s_screen, 0, sizeof(s_screen));
                g_pipeline.takeSprayStats();
            }
            s_frames++;
            if (inputs.carPoseKnown) {
                s_known++;
                // Where the engine's car lands on screen (row vectors: p V P): the chase camera keeps
                // the drawn car low in the middle, a few metres ahead.
                const float* p = inputs.carPosePosition;
                const float* V = inputs.view;
                const float* P = inputs.projection;
                float v[4], c[4];
                for (int j = 0; j < 4; j++) v[j] = p[0] * V[j] + p[1] * V[4 + j] + p[2] * V[8 + j] + V[12 + j];
                for (int j = 0; j < 4; j++) c[j] = v[0] * P[j] + v[1] * P[4 + j] + v[2] * P[8 + j] + v[3] * P[12 + j];
                if (c[3] > 0.1f) {
                    const float u = c[0] / c[3] * 0.5f + 0.5f, w = 0.5f - c[1] / c[3] * 0.5f;
                    if (u > -0.5f && u < 1.5f && w > -0.5f && w < 1.5f) {
                        s_onScreen++;
                        s_screen[0] += u;
                        s_screen[1] += w;
                        s_screen[2] += v[2];
                    }
                }
            }
            if (inputs.carPoseKnown && pose.wheelsMeasured) s_measured = true;
        } else if (s_exporting) {
            TMVS_LOG("video export: the spray had the car's axes from the engine in %u of %u shaded cameras%s", s_known, s_frames,
                     s_known == 0 ? " (none: it found the car in the depth buffer, behind the screen's centre)"
                                  : (s_measured ? " (its tyres as measured standing)" : " (its tyres: the Stadium car's estimates)"));
            if (s_onScreen)
                TMVS_LOG("video export: the engine's car on screen in %u of %u cameras, on average at %.2f %.2f (0..1 across, down), %.1f m ahead",
                         s_onScreen, s_known, s_screen[0] / s_onScreen, s_screen[1] / s_onScreen, s_screen[2] / s_onScreen);
            const Pipeline::SprayStats spray = g_pipeline.takeSprayStats();
            if (spray.frames)
                TMVS_LOG("video export: spray in %u of %u frames (speed %.1f m/s on average, at most %.1f; amount at most %.2f)", spray.throwing,
                         spray.frames, spray.speedSum / spray.frames, spray.topSpeed, spray.topAmount);
            s_measured = false;
        }
        s_exporting = g_exporting;
    }
    // A clip of the map (in-game trigger) while the player drives: the trail goes on recording.
    inputs.clipInRace = !g_exporting && engine::clipInRace() && inputs.carKnown;
    {
        // Once the engine has given the car in this session, the game is one whose car the
        // engine gives: a frame without it (a map loading, the start's countdown, a pause) records
        // nothing, the depth search never comes back. With a 5 s window it did after every map
        // load (the load takes longer), recorded guesses on the car in the first seconds of the
        // map (the start camera looks at it) and joined them to the engine's first point; kept
        // as an earlier run (TrailKeepRuns) they floated as short lines through the car at the
        // start (my B13). The first time the engine gives the car, what the depth search
        // recorded before goes (those were guesses, the trail starts over).
        static DWORD s_carSeen = 0;
        if (inputs.carKnown && !s_carSeen) {
            g_pipeline.resetTrail();
            g_pipelineB.resetTrail();
        }
        if (inputs.carKnown) s_carSeen = GetTickCount() | 1;
        inputs.carFromEngine = s_carSeen != 0;
    }
    {
        // DofAutofocus Car: the engine's car; in the photo mode also the last position it gave
        // on this map (the game may stop reporting it while paused, and the car stands still).
        static float s_held[3] = {};
        static bool s_haveHeld = false;
        static int s_heldMap = -1;
        if (engine::mapLoads() != s_heldMap) {
            s_heldMap = engine::mapLoads();
            s_haveHeld = false;
        }
        if (inputs.carKnown) {
            memcpy(s_held, inputs.carPosition, sizeof(s_held));
            s_haveHeld = true;
        }
        inputs.focusCarKnown = inputs.carKnown || (s_haveHeld && photo::active());
        memcpy(inputs.focusCar, inputs.carKnown ? inputs.carPosition : s_held, sizeof(inputs.focusCar));
    }
    {
        // The log says which way the trail finds the car while it records (from the engine, the
        // engine's car lost, in the depth buffer), each change up to 30 times.
        static int s_carSource = -1, s_logged = 0;
        const int source = inputs.carKnown ? 1 : (inputs.carFromEngine ? 2 : 0);
        if ((inputs.driving || inputs.clipInRace) && source != s_carSource && s_logged < 30) {
            s_carSource = source;
            s_logged++;
            static const char* const kSource[] = {"in the depth buffer", "from the engine", "nowhere: the engine lost it (a gap in the line)"};
            TMVS_LOG("pipeline: neon trail follows the car %s", kSource[source]);
        }
        static int s_clip = 0, s_clipLogged = 0;
        if ((inputs.clipInRace ? 1 : 0) != s_clip && s_clipLogged < 30) {
            s_clip = inputs.clipInRace ? 1 : 0;
            s_clipLogged++;
            TMVS_LOG("pipeline: neon trail: %s", s_clip ? "a clip of the map plays while you drive: the trail records on"
                                                          : "the clip of the map is over");
        }
    }
    {
        // Neon trail: a new map starts it over, a restart starts a new run (the earlier ones stay
        // as TrailKeepRuns says), a respawn starts a new line.
        static int s_resets = -1, s_respawns = -1;
        const int resets = engine::raceResets(), respawns = engine::respawns();
        static int s_maps = -1;
        if (engine::mapLoads() != s_maps) {
            s_maps = engine::mapLoads();
            g_pipeline.resetSeaLevel();
            g_pipelineB.resetSeaLevel(); // the compare split's preset side (its history is its own)
            g_pipeline.resetTrail();
            g_pipelineB.resetTrail();
        }
        if (resets != s_resets) {
            if (s_resets >= 0) TMVS_LOG("engine: race reset (trail: new run)");
            s_resets = resets;
            g_pipeline.newTrailRun();
            g_pipeline.resetHeights();
            g_pipeline.resetCeilings();
            g_pipelineB.newTrailRun();
            g_pipelineB.resetHeights();
            g_pipelineB.resetCeilings();
        }
        if (respawns != s_respawns) {
            if (s_respawns >= 0) TMVS_LOG("engine: respawn");
            s_respawns = respawns;
            g_pipeline.breakTrail();
            g_pipeline.resetHeights();
            g_pipelineB.breakTrail();
            g_pipelineB.resetHeights();
        }
    }
    inputs.jitter[0] = g_jitterUV[0];
    inputs.jitter[1] = g_jitterUV[1];
    inputs.photo = photo::active();
    photo::focusPoint(inputs.focusPoint);
    inputs.freeze = photo::tiling();
    inputs.lut = updateLut();
    inputs.materials = materialsWanted();
    g_casterSunSet = false;
    if (g_castersOn) inputs.casterMap = castersSideA;
    g_frameWidth = width;
    g_frameHeight = height;

    // F12: with the material masks on, the capture is the next frame whose draws carried their
    // material ids (the mask is built from them); not of a camera into part of the screen.
    const bool idFrame = g_idFrame && inputs.materials && !part && !scaledPath;
    unsigned char idClasses[256] = {};
    if (idFrame) {
        for (size_t i = 0; i < g_idMaterials.size(); i++) idClasses[i + 1] = static_cast<unsigned char>(engine::materialClass(g_idMaterials[i].shader));
        inputs.materialClasses = idClasses;
    }
    if (g_idFrame && !idFrame) inputs.materials = false; // ids in the stencil, but no capture of them
    g_idFrame = false;
    // The neon strips by their material (its textures) instead of by their colour.
    inputs.neonMaterials = inputs.materials && engine::neonMaterialsKnown();
    // RenderScale: a capture is taken at the full size (the next camera is drawn so).
    const bool capture = g_captureRequested && (idFrame || !inputs.materials || part) && !scaledPath;
    if (capture) g_captureRequested = false;
    if (capture && !settings.enabled && !inputs.photo) writeCapture(device, inputs, false);
    if (settings.enabled || inputs.photo) {
        // Auto quality renders a reduced copy; your settings stay untouched.
        Settings effective = settings;
        // The menu points at a preset: show it (it glides in like a chosen one).
        if (Config::get().previewSettings(effective)) effective.debugView = settings.debugView;
        smoothLook(effective, effectTime());
        // Photo mode and the video export: full quality, whatever the frame rate.
        if (!inputs.photo && !inputs.video) g_autoQuality.apply(effective);
        if (inputs.video) {
            effective.quality = 2;
            // The game's supersampling is the anti-aliasing. TAA would run on the first of its
            // samples in a frame only (the others repeat it), and over its sub-frames it smears.
            if (g_export.samplesPerFrame > 1 || g_export.subFrames) effective.taa = false;
        }
        // Motion blur and depth of field are for watching, not for driving (the auto focus
        // would blur the track ahead of you).
        if (effective.cinematicOnlyInReplays && !cinematic && !inputs.photo) {
            effective.motionBlur = 0.0f;
            effective.depthOfField = 0.0f;
        }
        photo::adjust(effective);
        // The video export's own frames: this sample into the float sum, the last one finishes the
        // frame into g_own.frame (see OwnExport).
        OwnSample own = ownSample(g_scene.processedCount - 1, part || scaledPath);
        // Several samples a frame are the anti-aliasing (FXAA or SMAA over them only softened the
        // textures: my previewer test, capture_134), and the frame stays float to the planes. A
        // single sample keeps SMAA, which needs an 8-bit frame, unless 10 bits are wanted.
        videofile::OwnFormat ownFormat;
        const bool ownFormatKnown = videofile::ownFormat(ownFormat);
        const bool ownFloat = own.count > 1 || (ownFormatKnown && ownFormat.tenBit);
        if (own.accumulate == 2 && !g_own.frame.create(device, width, height, ownFloat ? D3DFMT_A16B16G16R16F : D3DFMT_A8R8G8B8)) own = OwnSample();
        if (own.accumulate) {
            inputs.accumulate = own.accumulate;
            inputs.accumulateFirst = own.first;
            inputs.floatOutput = own.accumulate == 2 && ownFloat;
            // HDR10: P010 planes only (no 16-bit targets: SDR, the frame is cut at white anyway).
            if (ownFormatKnown && ownFormat.hdrWhite > 0.0f) {
                if (g_own.tenBitOk < 0) g_own.tenBitOk = Pipeline::tenBitPlanesSupported(device) ? 1 : 0;
                if (g_own.tenBitOk == 1) inputs.hdrPeak = ownFormat.hdrPeak / ownFormat.hdrWhite;
            }
            if (own.count > 1) effective.fxaa = false;
            inputs.lensDof = g_own.lensSamples > 0;
            effective.filmGrain *= settings.videoGrain;
            // The sub-frames are the motion blur: the screen-space one would blur it twice.
            if (g_export.subFrames) effective.motionBlur = 0.0f;
            g_own.samples++;
        }
        LARGE_INTEGER renderStart, renderEnd;
        QueryPerformanceCounter(&renderStart);
        // RenderScale: switching between the scaled and the full size (photo mode, captures) keeps
        // the height map, the neon trail and the exposure.
        g_pipeline.keepWorldOnResize(settings.renderScale < 0.99f);
        if (own.skip) {
            g_own.skipped++;
            showOwnFrame(device, shaded, width, height);
        } else if (own.accumulate == 2) {
            if (g_pipeline.render(device, inputs, effective, g_own.frame.surface)) finishOwnFrame(device, shaded, width, height);
        } else if (g_pipeline.render(device, inputs, effective, shaded) && scaledPath) {
            g_rs.pending = false;
        }
        // A sample only summed wrote nothing: the screen keeps showing the last finished frame.
        if (own.accumulate == 1) showOwnFrame(device, shaded, width, height);
        LARGE_INTEGER captureStart, captureEnd;
        QueryPerformanceCounter(&captureStart);
        if (capture) writeCapture(device, inputs, idFrame);
        QueryPerformanceCounter(&captureEnd);
        // A preset was just saved: this frame (without HUD and menu) becomes its picture, never
        // with the compare split.
        std::string picture;
        if (temporal && Config::get().takePictureRequest(picture)) {
            thumbnails::savePicture(device, shaded, Config::get().presetPicture(picture));
            Config::get().pictureTaken();
        }
        // Photo mode's compare split: the game's image (copied before the pipeline) on one side,
        // or a preset's look.
        IDirect3DSurface9* otherSide = renderSideB(device, inputs, settings, effective);
        photo::applySplit(device, otherSide ? otherSide : g_sceneCopy.surface, shaded);
        if (part) {
            device->StretchRect(g_partOut.surface, nullptr, target, &area, D3DTEXF_NONE);
            fillMenuBars(device, target, desc, area, settings.shadeMenuBars);
        }
        QueryPerformanceCounter(&renderEnd);
        const double toMs = 1000.0 / static_cast<double>(g_frequency.QuadPart);
        countPipelineCpu(static_cast<double>(renderEnd.QuadPart - renderStart.QuadPart) * toMs);
        logHangs(static_cast<double>(renderEnd.QuadPart - renderStart.QuadPart) * toMs,
                 static_cast<double>(captureEnd.QuadPart - captureStart.QuadPart) * toMs,
                 inputs.driving && !inputs.photo && g_scene.projection[14] > -1.0f); // not a menu background
        g_shadedThisFrame = true;
        // The shaded frame before the HUD: a photo or a hi-res tile.
        if (temporal) photo::afterRender(device, shaded);
        g_gameplayThisFrame = g_gameplayThisFrame || g_scene.projection[14] > -1.0f; // not a menu background
    }

    device->SetRenderTarget(0, target);
    device->SetDepthStencilSurface(savedDepth);
    g_pipeline.endStateSave(device);

    target->Release();
    if (savedDepth) savedDepth->Release();
}

// The video export: its start and end in the log, its clock (effectTime), and a clean start
// (the first frame of the video doesn't fade in from the editor's view).
void updateVideoExport() {
    engine::VideoExport v;
    const bool on = Config::get().settings.videoExportBest && engine::videoExport(v);
    ExportStats& stats = g_exportStats;
    // The frame that just ended, if it was one of the video's.
    if (g_exporting && g_frameStepped) {
        stats.samplesSeen = (std::max)(stats.samplesSeen, g_scene.processedCount);
        stats.samplesLeast = stats.samplesLeast < 0 ? g_scene.processedCount : (std::min)(stats.samplesLeast, g_scene.processedCount);
        stats.rendersSeen = (std::max)(stats.rendersSeen, g_scene.mainRenders);
        stats.othersSeen = (std::max)(stats.othersSeen, g_scene.otherRenders);
    }
    // DoShoot runs from the moment its dialog opens: the dialog's numbers are its defaults
    // (512x384, 24 fps) until OK, and the game's clock only steps once it shoots. The settings
    // are logged when the stepping starts (and again if they change).
    if (on && !g_exporting) {
        stats = ExportStats();
        g_own.framesSinceWrite = g_own.subIndex = 0;
        g_own.fresh = g_own.haveFrame = false;
        g_own.tenBitOk = -1;
        g_own.lensFocus = 0.0f;
        g_own.videoFrame = 0;
        g_own.finished = g_own.delivered = g_own.repeated = g_own.dropped = g_own.skipped = g_own.samples = g_own.lensFrames = g_own.lost = 0;
        g_own.readMs = g_own.betweenMs = 0.0;
        g_own.read.release(); // nothing left from an earlier export (it flushes at its end)
        g_own.read.lockMs = 0.0;
        g_own.read.waited = 0;
        g_own.read.pendingPeak = 0;
        TMVS_LOG("video export: started (its dialog; the settings follow when it shoots)");
    }
    const bool shooting = on && v.step > 0.0;
    if (shooting && (g_videoClock < 0.0 || v.width != g_export.width || v.height != g_export.height || v.fps != g_export.fps ||
                     v.samplesPerFrame != g_export.samplesPerFrame || v.subFrames != g_export.subFrames)) {
        if (g_videoClock < 0.0) {
            // The video's first frame: a clean start (no fade-in from the editor's view).
            stats.clockStart = effectTime();
            stats.gameStart = v.gameClock;
            g_pipeline.resetHistory();
        }
        TMVS_LOG("video export: %dx%d at %.3g fps%s, %s; game step %.3f ms. Full quality, the effects on the video's clock%s", v.width,
                 v.height, v.fps, v.writesVideo ? "" : " (a run without video output)",
                 v.samplesPerFrame > 1 ? "supersampling, every sample shaded"
                 : v.subFrames         ? "supersampling over sub-frames (the game's motion blur)"
                                       : "no supersampling",
                 v.step * 1000.0, v.samplesPerFrame > 1 || v.subFrames ? ", TAA off (the game's samples are the anti-aliasing)" : "");
        if (v.samplesPerFrame > 1 || v.subFrames)
            TMVS_LOG("video export: %d x %d samples (%d each frame, %d per video frame), stereo %d", v.hqSamples, v.hqSamples,
                     v.samplesPerFrame, v.samplesPerVideoFrame, v.stereo);
    }
    if (!on && g_exporting) {
        // The frames still on their way to the encoder (normally gone when the stepping ended).
        videofile::flushOwnFrames();
        // Expected: n x n samples a frame with the supersampling inside a frame, else one (the
        // sub-frames are frames of their own). Frames before and after the video's: its dialog,
        // and DoShoot playing the clip again in real time to record the sound.
        TMVS_LOG("video export ended: %u frames (%u before it shot, %u on the video's clock, %u after: the sound pass), while it shot the "
                 "effects' clock advanced %.2f s, the game's %.2f s; main cameras shaded in a frame of the video %d..%d (expected %d), "
                 "rendered at most %d, other cameras of the video's size at most %d",
                 stats.frames, stats.before, stats.stepped, stats.after, stats.clockEnd - stats.clockStart, stats.gameEnd - stats.gameStart,
                 (std::max)(stats.samplesLeast, 0), stats.samplesSeen, g_export.samplesPerFrame, stats.rendersSeen, stats.othersSeen);
        TMVS_LOG("video export: %u main cameras took the engine's matrices (%u had none set within them), %u copied from a float "
                 "target; the last shaded target %ux%u format %d",
                 stats.engineMatrices, stats.noneSet, stats.floatCopies, stats.targetWidth, stats.targetHeight,
                 static_cast<int>(stats.targetFormat));
        if (g_own.finished > 0) {
            // Reading back: the game's time in handing frames over (the copies start without
            // waiting; locking them a write later, the copy into the encoder's buffer). Of it the
            // wait in the locks: a copy the GPU had not done yet.
            const unsigned handed = g_own.delivered + g_own.repeated;
            TMVS_LOG("video export: the mod's own frames: %u finished from %u samples (%u sub-frames past the shutter skipped), %u handed over, "
                     "%u handed over again, %u replaced before the game wrote them; %u with lens depth of field; %s planes; %.1f ms a frame, "
                     "reading back %.1f ms a frame (%.1f of it waiting for the copy; %u frames found it unfinished, at most %d on the way)%s",
                     g_own.finished, g_own.samples, g_own.skipped, g_own.delivered, g_own.repeated, g_own.dropped, g_own.lensFrames,
                     g_own.tenBit ? "P010" : "NV12", g_own.finished > 1 ? g_own.betweenMs / (g_own.finished - 1) : 0.0,
                     handed > 0 ? g_own.readMs / handed : 0.0, handed > 0 ? g_own.read.lockMs / handed : 0.0, g_own.read.waited,
                     g_own.read.pendingPeak, g_own.lost || g_own.read.pending() > 0
                         ? (", " + std::to_string(g_own.lost + static_cast<unsigned>(g_own.read.pending())) + " lost on the way").c_str()
                         : "");
        }
        if (g_own.lensSet) engine::setLensSamples(nullptr, 0, 0.0f);
        g_own.lensSet = false;
        g_own.lensSamples = 0;
        releaseOwnExport(true); // a 4K frame and its planes: some 100 MB of video memory
    }
    g_exporting = on;
    if (on) {
        g_export = v;
        stats.frames++;
    }
    // The frames on the video's clock ended (the sound pass or the file's close follow): the
    // game wrote its last one, the mod's frames still on their way go into the encoder now.
    if (g_frameStepped && !shooting) videofile::flushOwnFrames();
    g_frameStepped = on && v.step > 0.0;
    if (on && !g_frameStepped) (stats.stepped ? stats.after : stats.before)++;
    if (on && v.step > 0.0) {
        g_videoClock = g_videoClock < 0.0 ? static_cast<double>(effectTime()) : g_videoClock + v.step;
        stats.stepped++;
        g_own.subIndex = g_own.framesSinceWrite++;
        // dev.ini TraceVideoFrame=n: a trace of one frame of the video (written at its Present).
        if (g_dev.traceVideoFrame > 0 && stats.stepped == static_cast<unsigned>(g_dev.traceVideoFrame)) {
            TMVS_LOG("video export: tracing frame %u of the video", stats.stepped);
            tracer::arm();
        }
        stats.clockEnd = g_videoClock;
        stats.gameEnd = v.gameClock;
    } else if (g_videoClock >= 0.0) {
        g_clockOffset = static_cast<float>(g_videoClock - static_cast<double>(seconds()));
        g_videoClock = -1.0;
    }
}

// --- Engine callbacks -------------------------------------------------------

// TAA jitter: a Halton (2, 3) sequence of 8 sub-pixel offsets, one per frame. Only while
// TAA runs: without it the image would visibly shake.

float halton(int index, int base) {
    float f = 1.0f, r = 0.0f;
    for (int i = index; i > 0; i /= base) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(i % base);
    }
    return r;
}

void updateJitter() {
    Settings effective = Config::get().settings;
    if (!photo::active() && !g_exporting) g_autoQuality.apply(effective);
    // Hi-res tiles: no jitter (each tile must line up with its neighbours to the pixel). The
    // export's supersampling: no TAA, and the game shifts its samples itself.
    const bool supersampled = g_exporting && (g_export.samplesPerFrame > 1 || g_export.subFrames);
    // The compare split: its other side (the game's image, or a preset shaded a second time) has
    // no TAA of this frame to take the jitter out again and trembled (my test, 2026-10-17).
    const bool on = effective.enabled && effective.taa && effective.taaJitter && effective.debugView == 0 && g_frameWidth > 0 &&
                    !photo::tiling() && !supersampled && !photo::splitShown();
    if (!on) {
        engine::setProjectionJitter(0.0f, 0.0f);
        g_jitterUV[0] = g_jitterUV[1] = 0.0f;
        return;
    }
    static int s_index = 0;
    s_index = s_index % 8 + 1;
    const float jx = (halton(s_index, 2) - 0.5f) * 2.0f / static_cast<float>(g_frameWidth);  // NDC, +-half a pixel
    const float jy = (halton(s_index, 3) - 0.5f) * 2.0f / static_cast<float>(g_frameHeight);
    engine::setProjectionJitter(jx, jy);
    g_jitterUV[0] = jx * 0.5f;
    g_jitterUV[1] = -jy * 0.5f;
}

void onFrameBegin() {
    tracer::event("## RenderFrameBegin");
    hook::diagFrame();
    photo::beginFrame();
    {
        // The game's bloom and colour filter would work on the image the mod already graded.
        const Settings& s = Config::get().settings;
        engine::skipGameFx(s.enabled && s.disableGameFx);
        // A night sky (or the black hole) has no sun: the game's halo and flare go too.
        engine::hideSunFlare(s.enabled && Pipeline::skyReplacesSun(s));
        engine::hideGameSky(s.enabled && Pipeline::skyReplacesSun(s) && !s.gameSkyClouds);
        // Real night stays midnight: when the weather's clock moved away (logged), it jumps back
        // before this frame is drawn. Only in gameplay (the weather of a map left may be gone).
        if (g_gameplayLastFrame && s.enabled) engine::keepNight(true);
    }
    updateVideoExport();
    updateLens();
    updateJitter();
    g_cameraIndex = 0;
    g_scene.lastFrameCount = g_scene.processedCount;
    g_scene.processedCount = 0;
    g_scene.mainRenders = 0;
    g_scene.otherRenders = 0;
}

void onCameraBegin(void* camera) {
    if (!g_device) return;
    IDirect3DSurface9* rt = nullptr;
    g_device->GetRenderTarget(0, &rt);
    g_scene.inMainCamera = isMainTarget(g_device, rt);
    if (g_scene.inMainCamera) {
        perflog::mark(g_device, perflog::Mark::GameBegin);
        g_scene.haveView = false;
        g_scene.haveProjection = false;
        // The scene's perspective projection (as onSetTransform takes it), not an orthographic one.
        g_scene.haveEngine = engine::cameraMatrices(g_scene.engineView, g_scene.engineProjection) && g_scene.engineProjection[11] == 1.0f &&
                             g_scene.engineProjection[14] != 0.0f;
        g_scene.mainRenders++;
        engine::setMainCamera(camera);
        {
            // The step of its depth buffer (OverlapFix): 2^-16 for 16 bits, else 2^-24 (D24, INTZ becomes D24S8).
            IDirect3DSurface9* ds = nullptr;
            D3DSURFACE_DESC dsDesc{};
            if (SUCCEEDED(g_device->GetDepthStencilSurface(&ds)) && ds) ds->GetDesc(&dsDesc);
            if (ds) ds->Release();
            g_depthUlp = dsDesc.Format == D3DFMT_D16 || dsDesc.Format == D3DFMT_D15S1 ? 1.0f / 65536.0f : 1.0f / 16777216.0f;
        }
        // Material masks: nothing left from the last frame where nothing is drawn (the sky).
        if (materialsWanted()) g_device->Clear(0, nullptr, D3DCLEAR_STENCIL, 0, 1.0f, 0);
        g_idFrame = g_captureRequested && materialsWanted();
        if (g_idFrame) g_idMaterials.clear();
        beginHdr(g_device, rt);
    } else if (rt) {
        if (g_exporting) {
            D3DSURFACE_DESC exportDesc{};
            rt->GetDesc(&exportDesc);
            if (static_cast<int>(exportDesc.Width) == g_export.width && static_cast<int>(exportDesc.Height) == g_export.height) g_scene.otherRenders++;
        }
        // A screen-sized camera that isn't shaded: say why (a player's shaders stayed off in
        // races while they worked in the editor, and the log showed nothing). Each kind once.
        static std::vector<std::array<UINT, 5>> s_logged;
        D3DSURFACE_DESC rtDesc{}, dsDesc{};
        rt->GetDesc(&rtDesc);
        IDirect3DSurface9* ds = nullptr;
        g_device->GetDepthStencilSurface(&ds);
        if (ds) ds->GetDesc(&dsDesc);
        const bool readable = depth::textureFor(ds) != nullptr;
        if (ds) ds->Release();
        const std::array<UINT, 5> kind = {rtDesc.Width, rtDesc.Height, dsDesc.Width, dsDesc.Height, static_cast<UINT>(dsDesc.Format)};
        if (rtDesc.Width >= 640 && rtDesc.Height >= 400 && s_logged.size() < 8 &&
            std::find(s_logged.begin(), s_logged.end(), kind) == s_logged.end()) {
            s_logged.push_back(kind);
            TMVS_LOG("scene: camera into %ux%u not shaded (depth %ux%u format %u, %s)", rtDesc.Width, rtDesc.Height, dsDesc.Width,
                     dsDesc.Height, static_cast<unsigned>(dsDesc.Format),
                     !ds ? "no depth buffer" : (readable ? "size differs from the depth" : "depth not readable"));
        }
    }
    if (tracer::tracing()) {
        IDirect3DSurface9* ds = nullptr;
        g_device->GetDepthStencilSurface(&ds);
        tracer::event("## RenderCameraBegin camera=%p main=%d rt=%s ds=%s", camera, g_scene.inMainCamera, tracer::describe(rt),
                      tracer::describe(ds));
        if (ds) ds->Release();
    }
    if (rt) rt->Release();
}

// The game's materials: every draw of the main camera writes the class of its material
// (engine::currentMaterial: light, car, glass, grass) into the stencil buffer, and the
// pipeline turns that into a mask (lamps glow for real, white walls don't). Draws that don't
// write depth (glows, headlight cones, decals, particles over whole blocks) leave what is
// behind them: they marked whole blocks as lamps. If the game uses the stencil itself (shadow
// volumes) the masks stay off.
bool g_stencilTagged = false;
bool g_gameStencil = false;

bool materialsWanted() {
    const Settings& s = Config::get().settings;
    return s.enabled && s.gameMaterials && !g_gameStencil;
}

// The stencil states the tagging changes, as the game had them (given back when it stops).
constexpr D3DRENDERSTATETYPE kTagStates[] = {D3DRS_STENCILENABLE, D3DRS_TWOSIDEDSTENCILMODE, D3DRS_STENCILFUNC, D3DRS_STENCILPASS,
                                             D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_STENCILWRITEMASK, D3DRS_STENCILREF};
DWORD g_savedStencil[sizeof(kTagStates) / sizeof(kTagStates[0])] = {};

void untagStencil(IDirect3DDevice9* device) {
    if (!g_stencilTagged) return;
    for (size_t i = 0; i < sizeof(kTagStates) / sizeof(kTagStates[0]); i++) device->SetRenderState(kTagStates[i], g_savedStencil[i]);
    g_stencilTagged = false;
}

void tagStencil(IDirect3DDevice9* device, DWORD value, DWORD mask) {
    if (!g_stencilTagged) {
        for (size_t i = 0; i < sizeof(kTagStates) / sizeof(kTagStates[0]); i++) device->GetRenderState(kTagStates[i], &g_savedStencil[i]);
    }
    device->SetRenderState(D3DRS_STENCILENABLE, TRUE);
    device->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
    device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
    device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
    device->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_STENCILWRITEMASK, mask);
    device->SetRenderState(D3DRS_STENCILREF, value);
    g_stencilTagged = true;
}

// F12's frame: the id of a material (1..254; 255 = the 255th and every one after it).
DWORD materialId(const void* shader, bool depthWrite, bool additive) {
    for (size_t i = 0; i < g_idMaterials.size(); i++) {
        IdMaterial& m = g_idMaterials[i];
        if (m.shader == shader) {
            m.draws++;
            m.depthWrite = m.depthWrite || depthWrite;
            m.additive = m.additive || additive;
            return static_cast<DWORD>(i + 1);
        }
    }
    if (g_idMaterials.size() >= 255) return 255;
    g_idMaterials.push_back({shader, 1, depthWrite, additive});
    return static_cast<DWORD>(g_idMaterials.size());
}

// Every draw call (all four kinds: a draw without its own tag must not write the tag of the
// draw before it, the stencil state stays set).
void onDraw(IDirect3DDevice9* device) {
    onDrawGBuffer(device); // first: an additive draw unbinds it before onDrawAdded switches the target
    onDrawAdded(device);
    onDrawScaled(device);
    if (!g_scene.inMainCamera || !materialsWanted()) {
        untagStencil(device);
        return;
    }
    DWORD stencil = FALSE;
    device->GetRenderState(D3DRS_STENCILENABLE, &stencil);
    if (stencil && !g_stencilTagged) {
        g_gameStencil = true;
        TMVS_LOG("materials: the game uses the stencil buffer itself, material masks off");
        return;
    }
    // Only draws with the depth test on: with it off the stencil is still written wherever
    // they draw (the stencil test passes always), and a screen-wide draw without depth (the
    // sky, effects at the end of the camera) wiped every class to "other".
    DWORD zwrite = TRUE, ztest = TRUE;
    device->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
    device->GetRenderState(D3DRS_ZENABLE, &ztest);
    if (!ztest) {
        untagStencil(device);
        return;
    }
    DWORD blend = FALSE, dest = D3DBLEND_ZERO;
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    device->GetRenderState(D3DRS_DESTBLEND, &dest);
    if (g_idFrame) {
        const DWORD id = materialId(engine::currentShader(), zwrite != FALSE, blend && dest == D3DBLEND_ONE);
        if (zwrite) tagStencil(device, id, 0xFF);
        else untagStencil(device);
        return;
    }
    if (zwrite) {
        // Its class, in the low bits; clears the "added light" bit of what it covers.
        tagStencil(device, static_cast<DWORD>(engine::currentMaterial()), 0xFF);
        return;
    }
    // Light the game adds on top of a surface (headlight pools, glows: one-one blending,
    // no depth writes) only sets bit 3 and keeps the class of the surface below.
    if (blend && dest == D3DBLEND_ONE) tagStencil(device, Pipeline::kAddedLightBit, Pipeline::kAddedLightBit);
    else untagStencil(device);
}

// OverlapFix: TMUnlimiter maps put blocks inside each other, and two blocks with a face in the
// same plane z-fight (stripes that flicker with the camera). blockorder gives each block draw a
// rank among the blocks overlapping at its spot (1..7, placed later = higher); that draw gets a
// small depth bias toward the camera, rank times one step, so the block placed last wins.
//
// Where: the game's own opaque draws of the main camera (depth test and depth writes on, no
// blending). Not the shadow and reflection cameras (not the main camera, the shadows of a block
// come from its geometry anyway), not the mod's passes (hook::InternalScope: casters.cpp, the
// light passes, ImGui never reach these callbacks), not blended draws (glass, glows).
//
// The bias is in depth units (0..1) added to the depth the draw writes, so the step is a number
// of depth buffer steps (ULP) at every distance. Two coplanar faces differ by about 1 to 2 ULP
// (each rasterises the plane from its own vertices); one rank step is 4 ULP: 2.4e-7 on a 24 bit
// buffer. In metres that is z^2 * (far - near) / (far * near) * ULP: with near 0.5 m, 0.12 mm per
// step at 10 m, 1.2 cm at 100 m, 12 cm at 1 km, which is where the buffer itself resolves 12 cm
// anyway. The seven steps together (28 ULP) stay below 1 mm at 10 m and 8 cm at 100 m: well
// under a road's decals and the car's gap above the road, and further away the buffer cannot
// tell such gaps apart either. The slope term (0.25 of the depth change over a pixel, per rank)
// covers the interpolation error of tilted faces, which grows with the slope; at most 1.75
// pixels' worth of depth, far less than a block's thickness. LESS and LESSEQUAL both work.
// Sign: negative = toward the camera. The previous values are put back after the draw.
bool g_overlapBiasActive = false;
DWORD g_overlapSavedBias = 0, g_overlapSavedSlope = 0;

constexpr float kOverlapUlpPerRank = 4.0f;
constexpr float kOverlapSlopePerRank = 0.25f;

DWORD floatBits(float v) {
    DWORD bits;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

float bitsFloat(DWORD bits) {
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

bool overlapBiasSupported(IDirect3DDevice9* device) {
    static int s_state = -1; // -1 not asked yet, 0 no, 1 yes
    if (s_state < 0) {
        D3DCAPS9 caps{};
        const bool ok = SUCCEEDED(device->GetDeviceCaps(&caps)) && (caps.RasterCaps & D3DPRASTERCAPS_DEPTHBIAS) &&
                        (caps.RasterCaps & D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS);
        s_state = ok ? 1 : 0;
        if (!ok) TMVS_LOG("overlap fix: the graphics card has no depth bias, OverlapFix stays off");
    }
    return s_state == 1;
}

int overlapRank(IDirect3DDevice9* device) {
    if (!g_dev.fakeBlockRank) return blockorder::currentDrawRank();
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT offset = 0, stride = 0;
    device->GetStreamSource(0, &vb, &offset, &stride);
    if (!vb) return 0;
    const int rank = static_cast<int>((reinterpret_cast<uintptr_t>(vb) >> 4) & 7);
    vb->Release();
    return rank;
}

void overlapBiasBegin(IDirect3DDevice9* device) {
    const Settings& s = Config::get().settings;
    if (!g_scene.inMainCamera || !s.overlapFix || !s.enabled) return;
    const int rank = overlapRank(device);
    if (rank <= 0) return;
    if (!overlapBiasSupported(device)) return;
    DWORD z = FALSE, zwrite = FALSE, blend = FALSE;
    device->GetRenderState(D3DRS_ZENABLE, &z);
    device->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    if (!z || !zwrite || blend) return;
    device->GetRenderState(D3DRS_DEPTHBIAS, &g_overlapSavedBias);
    device->GetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, &g_overlapSavedSlope);
    const int r = rank > 7 ? 7 : rank;
    // On top of what the game set itself (its decals may use a bias of their own).
    hook::InternalScope internal;
    device->SetRenderState(D3DRS_DEPTHBIAS, floatBits(bitsFloat(g_overlapSavedBias) - r * kOverlapUlpPerRank * g_depthUlp));
    device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, floatBits(bitsFloat(g_overlapSavedSlope) - r * kOverlapSlopePerRank));
    g_overlapBiasActive = true;
}

void overlapBiasDone(IDirect3DDevice9* device) {
    if (!g_overlapBiasActive) return;
    g_overlapBiasActive = false;
    hook::InternalScope internal;
    device->SetRenderState(D3DRS_DEPTHBIAS, g_overlapSavedBias);
    device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, g_overlapSavedSlope);
}

// CasterShadows: the main camera's opaque draws of static materials go to casters.cpp (which
// keeps the meshes that stay put). Not the car (it moves), glass or water (light passes).
void onDrawIndexed(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start, UINT count) {
    overlapBiasBegin(device); // blocks are indexed draws; restored in onDrawDone
    if (!g_castersOn || !g_scene.inMainCamera || !g_scene.haveView || !g_scene.haveProjection) return;
    if (!casters::takeTurn()) return; // most draws of a big map wait for a later frame
    DWORD z = FALSE, zwrite = FALSE, blend = FALSE, alphaTest = FALSE, colorWrite = 0;
    device->GetRenderState(D3DRS_ZENABLE, &z);
    device->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    device->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
    device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
    if (!z || !zwrite || blend || !colorWrite) return;
    // Alpha-tested meshes (fences, grids, leaves): with CasterAlphaTest drawn into the maps with
    // the game's alpha test (their holes let the light through), else left out (a solid shadow).
    if (alphaTest && !g_casterAlphaTest) return;
    const engine::Material material = engine::currentMaterial();
    if (material == engine::Material::Car || material == engine::Material::Glass || material == engine::Material::Water) return;
    casters::recordDraw(device, type, base, minIndex, vertices, start, count, g_scene.view, g_scene.projection, alphaTest != FALSE);
}

// Once a frame: the kept meshes belong to the map that is open (the game's buffers are held
// while they are kept), and only while the setting is on.
void castersTick() {
    const Settings& s = Config::get().settings;
    // Not with the effects (F7): switched off to compare, the meshes stay (the recording pauses
    // below, nothing uses the maps) and the shadows are back at once, not rebuilt over seconds.
    const bool on = s.casterShadows && engine::active();
    static int s_loads = -1, s_closes = -1, s_removed = -1;
    if (engine::mapLoads() != s_loads || engine::mapCloses() != s_closes) {
        s_loads = engine::mapLoads();
        s_closes = engine::mapCloses();
        casters::reset("map changed");
    }
    // The editor took blocks out: their shadows go (what is still there comes back once seen).
    if (engine::blocksRemoved() != s_removed) {
        static DWORD s_logged = 0;
        const bool log = GetTickCount() - s_logged > 60000;
        if (log) s_logged = GetTickCount();
        if (s_removed >= 0) casters::reset("blocks removed in the editor", log);
        s_removed = engine::blocksRemoved();
    }
    if (g_castersOn && !on) casters::reset("CasterShadows off");
    g_castersOn = on;
    // CasterAlphaTest off: the alpha-tested meshes kept go (all of them, the rest comes back
    // within seconds). On: they are kept from now on.
    if (g_casterAlphaTest && !s.casterAlphaTest && casters::keptAlphaMeshes() > 0) casters::reset("CasterAlphaTest off");
    g_casterAlphaTest = s.casterAlphaTest;
    // The draws are checked only while a pipeline uses the maps (or did in the last 3 s: auto
    // quality turns the effects off for moments, a preset glides in).
    static DWORD s_wantedAt = 0;
    const bool wanted = g_pipeline.consumeCastersWanted() | g_pipelineB.consumeCastersWanted();
    if (wanted) s_wantedAt = GetTickCount();
    casters::pauseRecording(!wanted && GetTickCount() - s_wantedAt > 3000);
    casters::frameDone();
}

void onCameraEnd(void* camera, const engine::CameraInfo&) {
    if (g_device) untagStencil(g_device);
    if (tracer::tracing()) {
        tracer::event("## RenderCameraEnd camera=%p main=%d", camera, g_scene.inMainCamera);
        if (g_scene.inMainCamera) {
            traceMatrix("scene view", g_scene.view);
            traceMatrix("scene proj", g_scene.projection);
        }
    }
    if (g_scene.inMainCamera && g_device) {
        perflog::mark(g_device, perflog::Mark::GameEnd);
        endGBuffer(g_device);
        endHdr(g_device);
        processScene(g_device);
        // RenderScale: a scaled camera processScene didn't shade still fills the screen.
        if (g_rs.pending) {
            IDirect3DSurface9* target = nullptr;
            g_device->GetRenderTarget(0, &target);
            fillFromScaled(g_device, target);
            if (target) target->Release();
        }
        perflog::mark(g_device, perflog::Mark::ModEnd);
    }
    g_scene.inMainCamera = false;
}

void onOverlayBegin() {
    tracer::event("## RenderOverlayZones begin");
}

void onOverlayEnd() {
    tracer::event("## RenderOverlayZones end");
}

void onFrameEnd() {
    tracer::event("## RenderFrameEnd");
}

// --- Device callbacks -------------------------------------------------------

void onAdjustPresentParams(D3DPRESENT_PARAMETERS* params) {
    // The game rebuilds its present parameters from the bound depth buffer, which is our
    // INTZ texture. A back buffer can't have an INTZ auto depth-stencil: Reset would fail
    // (fullscreen <-> window, Alt+Enter, alt-tab) and the game would wait forever.
    if (params->EnableAutoDepthStencil && params->AutoDepthStencilFormat == depth::kINTZ) {
        TMVS_LOG("present: auto depth-stencil INTZ -> D24S8");
        params->AutoDepthStencilFormat = D3DFMT_D24S8;
    }
    if (Config::get().settings.disableGameMSAA && params->MultiSampleType != D3DMULTISAMPLE_NONE) {
        TMVS_LOG("present: disabling game MSAA x%d (depth effects need a single-sample depth buffer)",
                 static_cast<int>(params->MultiSampleType));
        params->MultiSampleType = D3DMULTISAMPLE_NONE;
        params->MultiSampleQuality = 0;
    }
}

void onDeviceCreated(IDirect3DDevice9* device) {
    g_device = device;
    depth::onDeviceReady(device);
}

void onPreReset() {
    {
        // Who wanted the reset: a lost device (alt-tab, another window in front, a mode switch)
        // or the game on its own with the device fine. And how long since our last hotkey, so a
        // reset after F7 / F8 shows in the log.
        const HRESULT coop = g_device ? g_device->TestCooperativeLevel() : D3D_OK;
        const DWORD key = keys::lastPressAt();
        char since[48] = "no hotkey yet";
        if (key) snprintf(since, sizeof(since), "last hotkey %lu ms ago", GetTickCount() - key);
        TMVS_LOG("device: reset by the game, %s; %s", coop == D3DERR_DEVICENOTRESET ? "the device was lost (focus or mode change)"
                                                     : coop == D3D_OK ? "the device was fine (not lost)" : "the device is still lost",
                 since);
    }
    perflog::release();
    g_pipeline.setProfiling(true, false); // perflog's bracket is gone until the next Present
    g_pipeline.releaseForReset(); // the shaders stay (they live through the reset)
    releaseOwnExport(false);
    releaseSideB();
    g_sceneCopy.destroy();
    g_partDepth.destroy();
    g_partOut.destroy();
    g_partHdr.destroy();
    g_partAdded.destroy();
    if (g_hdrGame) g_hdrGame->Release();
    g_hdrGame = nullptr;
    g_hdrLastGame = nullptr;
    g_hdrValid = false;
    g_hdrScene.destroy();
    g_hdrAddedActive = g_hdrAddedBound = g_hdrAddedValid = false;
    g_hdrAdded.destroy();
    unbindGBuffer(g_device);
    g_gbufActive = g_gbufValid = false;
    g_gbuf.destroy();
    g_gbufLightActive = g_gbufLightValid = false;
    g_gbufLight.destroy();
    g_rs.active = g_rs.pending = g_rs.sceneScaled = false;
    g_rs.depth = nullptr;
    gfx::release(g_screenSaved.decl);
    g_screenSaved.active = false;
    releaseScreenLayouts(); // made again when needed (the shader from the compile cache)
    casters::releaseDevice();
    depth::onPreReset();
    overlay::preReset();
}

DWORD g_resetAt = 0; // a device reset, until the first shaded frame after it (the log)

void onPostReset(IDirect3DDevice9* device) {
    g_resetAt = GetTickCount();
    depth::onDeviceReady(device);
    overlay::postReset();
}

bool onCreateDepthStencil(IDirect3DDevice9* device, UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms,
                          IDirect3DSurface9** out, HRESULT* hr) {
    return Config::get().settings.readableGameDepth && depth::createReadable(device, w, h, format, ms, out, hr);
}

IDirect3DSurface9* onSetDepthStencil(IDirect3DSurface9* surface) {
    return depth::substitute(surface);
}

void onSetTransform(D3DTRANSFORMSTATETYPE type, const D3DMATRIX* m) {
    if (!g_scene.inMainCamera) return;
    if (type == D3DTS_VIEW) {
        memcpy(g_scene.view, m, sizeof(g_scene.view));
        g_scene.haveView = true;
    } else if (type == D3DTS_PROJECTION && m->_34 == 1.0f && m->_43 != 0.0f && !g_scene.haveProjection) {
        // The scene projection (perspective with a near plane). The sky is drawn with
        // a variant whose _43 is 0 (pushed to the far plane); ignore that one.
        memcpy(g_scene.projection, m, sizeof(g_scene.projection));
        g_scene.haveProjection = true;
    }
}

// Notes which game clears of this frame reached the top or bottom row of their target (the
// viewport, or the rects inside it). Only for fillBars: those rows are the game's own then.
void onClear(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color) {
    unbindGBuffer(device); // a Clear would wipe the normals too
    if (!(flags & D3DCLEAR_TARGET)) return;
    IDirect3DSurface9* target = nullptr;
    if (FAILED(device->GetRenderTarget(0, &target)) || !target) return;
    D3DSURFACE_DESC desc{};
    target->GetDesc(&desc);
    target->Release(); // only the pointer is kept, to compare
    D3DVIEWPORT9 viewport{};
    device->GetViewport(&viewport);
    LONG top = static_cast<LONG>(viewport.Y), bottom = static_cast<LONG>(viewport.Y + viewport.Height);
    if (count > 0 && rects) {
        LONG lo = bottom, hi = top;
        for (DWORD i = 0; i < count; i++) {
            lo = rects[i].y1 < lo ? rects[i].y1 : lo;
            hi = rects[i].y2 > hi ? rects[i].y2 : hi;
        }
        top = lo > top ? lo : top;
        bottom = hi < bottom ? hi : bottom;
    }
    // A D3D9 Clear keeps to the scissor rect while the scissor test is on: a clear with the whole
    // screen as its viewport but the camera's band as its scissor rect leaves the rows above and
    // below as they were (it counted as the game's own black bars, the leftovers stayed).
    DWORD scissorTest = FALSE;
    device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorTest);
    if (scissorTest) {
        RECT scissor{};
        device->GetScissorRect(&scissor);
        top = scissor.top > top ? scissor.top : top;
        bottom = scissor.bottom < bottom ? scissor.bottom : bottom;
    }
    if (bottom <= top) return;
    const ClearedRows cleared = {target, g_frame, top, bottom, color, count, scissorTest != FALSE};
    if (top <= 0) g_clearedTop = cleared;
    if (bottom >= static_cast<LONG>(desc.Height)) g_clearedBottom = cleared;
}

void onCreatePixelShader(IDirect3DPixelShader9* shader, const DWORD* code) {
    gameshaders::notePixelShader(shader, code);
}

void onSetLight(DWORD, const D3DLIGHT9* light) {
    if (light->Type != D3DLIGHT_DIRECTIONAL) return;
    g_scene.lightDirection[0] = light->Direction.x;
    g_scene.lightDirection[1] = light->Direction.y;
    g_scene.lightDirection[2] = light->Direction.z;
    g_scene.lightColor[0] = light->Diffuse.r;
    g_scene.lightColor[1] = light->Diffuse.g;
    g_scene.lightColor[2] = light->Diffuse.b;
    g_scene.haveLight = true;
}

void relightOnSwitch(const Settings& s);

// The device was lost (the game resets it later, reloading everything: seconds): which program
// had the front then (the exe name and window class only, no titles), and our last hotkey.
void logDeviceLost(HRESULT coop) {
    static int s_lines = 0;
    if (s_lines >= 50) return;
    s_lines++;
    const HWND front = GetForegroundWindow();
    DWORD pid = 0;
    if (front) GetWindowThreadProcessId(front, &pid);
    wchar_t program[MAX_PATH] = L"nothing";
    if (pid == GetCurrentProcessId()) {
        wcscpy_s(program, L"the game");
    } else if (pid) {
        wcscpy_s(program, L"?");
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t path[MAX_PATH] = {};
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(process, 0, path, &size)) {
                const wchar_t* slash = wcsrchr(path, L'\\');
                wcscpy_s(program, slash ? slash + 1 : path);
            }
            CloseHandle(process);
        }
    }
    wchar_t windowClass[64] = L"";
    if (front) GetClassNameW(front, windowClass, 64);
    const DWORD key = keys::lastPressAt();
    char since[48] = "no hotkey yet";
    if (key) snprintf(since, sizeof(since), "last hotkey %lu ms ago", GetTickCount() - key);
    TMVS_LOG("device: lost (0x%08lx); in front: %ls (window class %ls); %s", static_cast<unsigned long>(coop), program, windowClass, since);
}

void onPresent(IDirect3DDevice9* device) {
    g_device = device;
    g_frame++;
    // CenterSpeed's menu key is F8 too (keys::centerSpeed). The ModLoader has loaded it by the first
    // frame; once more some seconds later, should it come late.
    if (g_frame == 1 || g_frame == 600) keys::centerSpeed(GetModuleHandleW(L"CenterSpeed.dll") != nullptr);
    // Device lost (alt-tab, mode switch): nothing can be drawn until the game resets it.
    static bool s_lost = false;
    const HRESULT coop = device->TestCooperativeLevel();
    if (coop != D3D_OK) {
        if (!s_lost) logDeviceLost(coop);
        s_lost = true;
        return;
    }
    if (s_lost) TMVS_LOG("device: usable again");
    s_lost = false;
    if (g_resetAt && g_shadedThisFrame) {
        // The game's own reload and ours (the passes, the render targets) until the effects run again.
        const DWORD after = GetTickCount() - g_resetAt;
        if (after < 30000) TMVS_LOG("device: first shaded frame %lu ms after the reset", after); // else the effects were off
        g_resetAt = 0;
    }

    // Fallback when the engine hooks are unavailable: process at Present (HUD included).
    if (!engine::active() && g_scene.haveView) processScene(device);

    // GPU timing per pass: auto quality, the overlay and the perf log line use it (a few
    // timestamp queries a frame, never waited for).
    const Settings& settings = Config::get().settings;
    perflog::frame(device, g_pipeline, g_shadedThisFrame, casters::cpuMs());
    // Inside perflog's frame-wide disjoint query (open from here to the next Present) the
    // pipeline opens none of its own: only one at a time.
    g_pipeline.setProfiling(true, perflog::bracketOpen());
    // Paused in photo mode: full quality there must not count as slow gameplay (the gap in
    // frame times when it ends starts a quiet period).
    // The video export likewise: its frames are slow on purpose.
    // Gameplay frames only: the menus' 3D backgrounds are shaded too, but their frame rate says
    // nothing about a race (it cut effects in the main menu before a map was loaded).
    if (!photo::active() && !g_exporting) g_autoQuality.onFrame(seconds(), g_gameplayThisFrame, g_pipeline, settings);
    D3DDEVICE_CREATION_PARAMETERS cp{};
    device->GetCreationParameters(&cp);
    // The weather sound follows the effects' clock, so its thunder follows the flashes on screen
    // (the real clock drifted from them by the export's offset). In the export's sound pass the
    // game replays the clip in real time from 0 for its recording: the video's clock at the clip
    // time it plays, so the thunder in the video's sound comes after the flashes in its frames.
    // During an export the rain is not faded out when the game is in the background (the export
    // runs on, and so does its sound pass).
    {
        double passed = 0.0;
        const bool pass = g_exporting && soundcapture::passClock(passed);
        const float soundTime = pass ? static_cast<float>(g_exportStats.clockStart + passed - g_export.clipStart) : effectTime();
        audio::setAlwaysInFront(g_exporting);
        // The sound pass shades no gameplay frames: counted as a race only by those, the rain
        // faded out at its start (a short hiss at the video's start, my test) and the thunder
        // stayed silent. The whole export counts, so nothing fades between frames and pass.
        audio::update(settings, soundTime, g_gameplayThisFrame || g_exporting, cp.hFocusWindow);
    }
    {
        static float s_waterMin = 1e30f, s_waterMax = 1e30f;
        float lo = 0.0f, hi = 0.0f;
        const bool known = engine::waterHeights(lo, hi);
        if (!known) lo = hi = -1e30f;
        if (fabsf(lo - s_waterMin) > 0.01f || fabsf(hi - s_waterMax) > 0.01f) {
            s_waterMin = lo;
            s_waterMax = hi;
            if (known) TMVS_LOG("engine: water heights %.2f .. %.2f", lo, hi);
        }
    }
    // Replay / intro / video export detection, logged when it changes.
    static int s_cinematic = -1;
    const int cinematic = engine::cinematicActive() ? 1 : 0;
    if (cinematic != s_cinematic) {
        s_cinematic = cinematic;
        // The game's lens flares since the last switch (the sun came back in end-of-race replays).
        unsigned other = 0, sun = 0, hidden = 0;
        engine::flareCounts(other, sun, hidden);
        TMVS_LOG("engine: cinematic %s (sources %u); lens flares since: %u of the sun (%u hidden), %u of other lights", cinematic ? "on" : "off",
                 engine::cinematicSources(), sun, hidden, other);
    }
    relightOnSwitch(settings);
    castersTick();
    unbindGBuffer(device);
    gameshaders::tick();
    g_shadedThisFrame = false;
    g_gameplayLastFrame = g_gameplayThisFrame;
    g_gameplayThisFrame = false;

    overlay::Status status;
    status.pipeline = &g_pipeline;
    status.autoQuality = &g_autoQuality;
    status.depthAvailable = depth::texture() != nullptr;
    status.engineHooks = engine::active();
    status.sunKnown = sunDirection(Config::get().settings, status.sunDirection);
    status.shaderError = g_pipeline.lastError().empty() ? nullptr : g_pipeline.lastError().c_str();
    overlay::setStatus(status);
    overlay::draw(device);
    // overlay::draw polled the hotkeys (keys::poll, once a frame).
    const bool reload = overlay::consumeReloadRequest() | keys::take(keys::Action::Reload);
    if (reload) {
        g_pipelineFailed = false;
        g_pipeline.reloadShaders(device);
        releaseSideB(); // starts again from the reloaded shaders
    }

    if (tracer::tracing() || g_screenshotNumber >= 0) {
        // Screenshot of the final frame (effects + HUD), for checking results in game.
        hook::InternalScope internal;
        IDirect3DSurface9* backBuffer = nullptr;
        if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) && backBuffer) {
            if (g_screenshotNumber >= 0) {
                // The capture's: read back here, written after the capture's files by its writer.
                LARGE_INTEGER start, end;
                QueryPerformanceCounter(&start);
                auto pixels = std::make_shared<capture::Pixels>();
                bool read = false;
                try {
                    read = capture::readSurface(device, backBuffer, *pixels);
                } catch (const std::bad_alloc&) {
                    TMVS_LOG("capture: not enough memory for the screenshot");
                }
                QueryPerformanceCounter(&end);
                if (read) {
                    wchar_t name[64];
                    swprintf(name, 64, L"screen_%03d.bmp", g_screenshotNumber);
                    const std::wstring file = name;
                    const double readMs = static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(g_frequency.QuadPart);
                    capwriter::post([pixels, file, readMs] {
                        LARGE_INTEGER writeStart, writeEnd;
                        QueryPerformanceCounter(&writeStart);
                        const bool ok = capwriter::writeVia(log::dataDir() + L"\\" + file,
                                                            [&pixels](const std::wstring& path) { return capture::writeBmp(*pixels, path); });
                        QueryPerformanceCounter(&writeEnd);
                        TMVS_LOG("capture: %s %ls in %.0f ms on the writer thread (read back in %.0f ms on the game's thread)", ok ? "wrote" : "could not write",
                                 file.c_str(), static_cast<double>(writeEnd.QuadPart - writeStart.QuadPart) * 1000.0 / static_cast<double>(g_frequency.QuadPart),
                                 readMs);
                    });
                }
            } else {
                capture::saveSurface(device, backBuffer, log::dataDir() + L"\\present.bmp");
            }
            backBuffer->Release();
        }
        g_screenshotNumber = -1;
    }
    runDevAutomation(device);
    {
        // Back in the menu: an Author's Shader ends with its map, your own look is back (else
        // it stayed until the next map). Only when no map is open any more: switching maps
        // lets go of the old one too, and the next map's load decides then. The same map open
        // again without a new load (its decoration kept) gets its look back.
        static int s_closes = 0, s_loadsAtClose = -1;
        if (engine::mapCloses() != s_closes) {
            s_closes = engine::mapCloses();
            if (!engine::mapOpen() && s_loadsAtClose < 0) {
                TMVS_LOG("engine: map closed");
                s_loadsAtClose = engine::mapLoads();
                Config::get().onMapComments(std::string());
            }
        }
        if (s_loadsAtClose >= 0 && engine::mapOpen()) {
            if (engine::mapLoads() == s_loadsAtClose) {
                TMVS_LOG("engine: the same map open again");
                Config::get().onMapComments(engine::mapComments());
            }
            s_loadsAtClose = -1;
        }
    }
    Config::get().tick();
    if (keys::take(keys::Action::Trace)) tracer::arm();
    if (keys::take(keys::Action::Capture)) {
        // Photo mode: a photo without the HUD (Shift: hi-res) instead of a .tmcap capture.
        if (photo::active()) photo::request(keys::shiftHeld());
        else g_capturePending = true;
    }
    // One capture in flight (its copies are 100-200 MB): the next is taken once the writer has
    // the last one on disk. Pressed again meanwhile: still one more.
    if (g_capturePending) {
        static bool s_waitLogged = false;
        if (!capwriter::busy()) {
            g_capturePending = false;
            g_captureRequested = true;
            s_waitLogged = false;
        } else if (!s_waitLogged) {
            s_waitLogged = true;
            TMVS_LOG("capture: F12 waits until the last capture is written");
        }
    }
}

} // namespace

void setModule(HMODULE module) {
    g_module = module;
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    g_moduleDir = path;
    size_t slash = g_moduleDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) g_moduleDir.resize(slash);
}

const std::wstring& moduleDir() {
    return g_moduleDir;
}

namespace {

bool g_passive = false;
hook::DeviceCallbacks g_deviceCallbacks;

DWORD WINAPI retryHooks(LPVOID) {
    for (int attempt = 2; attempt <= 20; attempt++) {
        Sleep(1000);
        if (hook::install(g_deviceCallbacks)) return 0;
    }
    TMVS_LOG("hook: failed to hook Direct3D 9");
    return 0;
}

// Map load: light a day or sunset map at midnight when its look will have a night sky (the
// sky replaces the sun anyway; the game's own night has no baked sun to take out).
// The map's mood comes from the same time of day (see processScene), so the look decided here
// is the one the map gets.
bool nightFor(float dayTime, bool log) {
    const Config& config = Config::get();
    const Mood mood = moodOfDayTime(dayTime);
    if (mood == Mood::Night) return false; // a night map already
    const bool night = Pipeline::skyReplacesSun(config.lookForMood(mood));
    if (!night && log) TMVS_LOG("config: real night: the %s look has no night sky", moodName(mood));
    return night;
}

bool nightAtLoad(float dayTime) {
    const Settings& s = Config::get().settings;
    if (!s.enabled || !s.gameNight) {
        TMVS_LOG("config: real night off (setting)");
        return false;
    }
    return nightFor(dayTime, true);
}

// EngineMoon, at map load: on a night map (its own night or lit at midnight) whose look has the
// black hole, the game's own moon (its baked light and shadows, glints, the car) shines from the
// black hole instead of from the side, or is off. Decided with the look of the map's mood, as
// real night is; an Author's Shader read later doesn't change it.
bool moonAtLoad(float dayTime, bool litAtNight, engine::MoonLight& moon) {
    const Settings& s = Config::get().settings;
    if (!s.enabled || s.engineMoon == 0 || dayTime < 0.0f) return false;
    const Mood mood = moodOfDayTime(dayTime);
    if (!litAtNight && mood != Mood::Night) return false; // the game's sun lights this map
    const Settings look = Config::get().lookForMood(mood);
    if (!Pipeline::blackHoleLights(look)) {
        TMVS_LOG("config: engine moon: the %s look has no black hole, the game's moon stays", moodName(mood));
        return false;
    }
    moon.mode = s.engineMoon;
    Pipeline::blackHoleDirection(look, moon.toward);
    // Not from below the horizon (BlackHoleElevation goes down to -10): the ground would shade
    // everything and light the undersides. At least 3 degrees up.
    const float minUp = 0.0523f;
    if (moon.toward[1] < minUp) {
        const float flat = sqrtf(moon.toward[0] * moon.toward[0] + moon.toward[2] * moon.toward[2]);
        const float k = flat > 1e-4f ? sqrtf(1.0f - minUp * minUp) / flat : 0.0f;
        moon.toward[0] *= k;
        moon.toward[2] *= k;
        moon.toward[1] = flat > 1e-4f ? minUp : 1.0f;
    }
    return true;
}

// Real night switched in the menu, or a look picked with or without a night sky while a map
// runs: the map is relit now, not at the next one. It follows the look that is on (also an
// Author's Shader, read after the map's light was set up), once that has held for 1.5 s
// (transitions glide). The menu's hover preview never relights: it only changes the look drawn
// (processScene's `effective`), not Config::settings (a relight is too costly to follow a
// pointer, and the lightmap baked at load stays anyway). Only in gameplay (the
// weather of a map that was left may be gone); F7 (shaders off to compare) doesn't relight.
void relightOnSwitch(const Settings& s) {
    static double s_since = -1.0;
    static bool s_failed = false;
    const float dayTime = engine::mapDayTime();
    if (!g_gameplayThisFrame || dayTime < 0.0f) {
        s_since = -1.0;
        return;
    }
    const bool want = s.gameNight && moodOfDayTime(dayTime) != Mood::Night && Pipeline::skyReplacesSun(s);
    if (want == engine::mapLitAtNight()) {
        s_since = -1.0;
        s_failed = false;
        return;
    }
    if (s_failed) return;
    if (s_since < 0.0) s_since = seconds();
    if (seconds() - s_since < 1.5) return;
    s_since = -1.0;
    // Why the map changes its light now (the engine's line says to which time of day).
    TMVS_LOG("config: real night %s: the look on now (%s) has %s", want ? "on" : "off", Config::get().preset.c_str(),
             want ? "a night sky" : (s.gameNight ? "no night sky" : "real night off"));
    if (!engine::relightNight(want)) {
        s_failed = true;
        TMVS_LOG("config: real night: this map can't be relit now (next map)");
    }
}

void bootOnce() {
    QueryPerformanceFrequency(&g_frequency);
    QueryPerformanceCounter(&g_startTime);
    TMVS_LOG("TM Vibrant Shaders %s starting", TM_SHADERS_VERSION_A);
    loadDevOptions();
    Config::get().load();
    lut::writeBuiltInLooks();
    gameshaders::install();

    engine::Callbacks engineCallbacks;
    engineCallbacks.frameBegin = onFrameBegin;
    engineCallbacks.cameraBegin = onCameraBegin;
    engineCallbacks.cameraEnd = onCameraEnd;
    engineCallbacks.overlayBegin = onOverlayBegin;
    engineCallbacks.overlayEnd = onOverlayEnd;
    engineCallbacks.frameEnd = onFrameEnd;
    engine::install(engineCallbacks);
    engine::setNightAtLoad(nightAtLoad);
    videofile::install();
    videofile::FrameSource frames;
    frames.deliver = ownDeliver;
    frames.written = ownWritten;
    frames.ready = ownReady;
    frames.flush = ownFlushAll;
    videofile::setFrameSource(frames);
    engine::setMoonAtLoad(moonAtLoad);

    hook::DeviceCallbacks deviceCallbacks;
    deviceCallbacks.adjustPresentParams = onAdjustPresentParams;
    deviceCallbacks.deviceCreated = onDeviceCreated;
    deviceCallbacks.present = onPresent;
    deviceCallbacks.preReset = onPreReset;
    deviceCallbacks.postReset = onPostReset;
    deviceCallbacks.setDepthStencil = onSetDepthStencil;
    deviceCallbacks.setRenderTarget = onSetRenderTarget;
    deviceCallbacks.createDepthStencil = onCreateDepthStencil;
    deviceCallbacks.setTransform = onSetTransform;
    deviceCallbacks.setLight = onSetLight;
    deviceCallbacks.clear = onClear;
    deviceCallbacks.createPixelShader = onCreatePixelShader;
    deviceCallbacks.draw = onDraw;
    deviceCallbacks.drawDone = onDrawDone;
    deviceCallbacks.drawIndexed = onDrawIndexed;
    deviceCallbacks.setViewport = onSetViewport;
    deviceCallbacks.getViewport = onGetViewport;
    deviceCallbacks.setScissorRect = onSetScissorRect;
    deviceCallbacks.getScissorRect = onGetScissorRect;
    deviceCallbacks.clearRects = onClearRects;
    deviceCallbacks.stretchRects = onStretchRects;
    deviceCallbacks.otherModRenderTarget = onOtherModRenderTarget;
    g_deviceCallbacks = deviceCallbacks;
    // Never block the caller (possibly the game's main thread) on retries.
    if (!hook::install(deviceCallbacks)) {
        HANDLE thread = CreateThread(nullptr, 0, retryHooks, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
}

} // namespace

bool claimInstance() {
    wchar_t name[64];
    swprintf(name, 64, L"Local\\TMVibrantShaders_%lu", GetCurrentProcessId());
    HANDLE mutex = CreateMutexW(nullptr, FALSE, name); // kept open for the life of the process
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        g_passive = true;
    }
    return !g_passive;
}

bool active() {
    return !g_passive;
}

void ensureBooted() {
    static volatile LONG state = 0; // 0 not started, 1 booting, 2 done
    static DWORD bootThread = 0;
    if (g_passive || state == 2) return;
    if (InterlockedCompareExchange(&state, 1, 0) == 0) {
        bootThread = GetCurrentThreadId();
        bootOnce();
        InterlockedExchange(&state, 2);
        return;
    }
    if (bootThread == GetCurrentThreadId()) return; // re-entered: the boot itself creates Direct3D
    for (int waited = 0; state != 2 && waited < 5000; waited++) Sleep(1);
}

void boot() {
    ensureBooted();
    // The HLSL compiled while the mod runs, compiled here ahead (this thread is the plugin's own;
    // the game is not waiting for it): the first shaded frame took 1.4 s, most of it compiling
    // the trail's and the height map's vertex shaders, and every device reset compiled them again.
    if (g_passive) return;
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    std::vector<gfx::ShaderSource> shaders;
    Pipeline::runtimeShaders(shaders);
    casters::runtimeShaders(shaders);
    gfx::precompile(shaders);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    TMVS_LOG("boot: %u shaders compiled ahead in %.0f ms", static_cast<unsigned>(shaders.size()),
             static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart));
}

void shutdown() {
    capwriter::shutdown();
    overlay::shutdown();
    audio::shutdown();
    hook::remove();
}

} // namespace plugin
} // namespace tmshaders
