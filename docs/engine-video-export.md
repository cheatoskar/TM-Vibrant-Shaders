# The game's video export, seen from the engine

[Back to the README](../README.md)

Notes from static analysis of TMNF's `TmForever.exe` (timestamp 4d431494, with its map file) in
Ghidra, 2026-10-21. TMUF runs the same code at shifted addresses. Addresses below are TMNF link-time
addresses. "Confirmed" means read in the decompiled or disassembled code; "inferred" means it follows
from that code but I did not see it run.

## The functions

| Address | Function | Role |
|---|---|---|
| 006f4510 | `CGameCtnMediaVideoShooter::DoShoot` | The export, a fiber resumed once per game update (from `CGameCtnApp::UpdateGame` / `RunEditor`) |
| 007ed660 | `CSceneFxSuperSample::FxRender` | Renders the camera's samples and averages them |
| 007ecd20 | `CSceneFxSuperSample::Init(nx, ny, hdrAccum, softShadows, ?)` | DoShoot calls `Init(n, n, 0, 0, 0)` |
| 007ec950 | `CSceneFxSuperSample::EnableExternalSampleLoop` | +0x78 = 0 means one sample per frame (sub-frames) |
| 006f4320 | `CSceneFxSuperSample::IsLastSampleInLoop` | True with the internal loop, or when the next sample is the last (+0x84 = nx - 1, +0x88 = ny - 1) |
| 007ec640 | `CSceneFxSuperSample::CreateBitmaps` | "FxSS Color", "FxSS Accum1" |
| 0053fc80 | `CHmsCamera::ZClipCompute` | Shifts the render frustum by the sample's sub-pixel offset |
| 0095d530 | `CVisionViewportDx9::ComputeDriverProjection` | Projection from that frustum (`projectionDetour`) |
| 0052f6b0 | `CHmsViewport::RenderCameraNormal` | Camera to the screen |
| 0098e8f0 | `CVisionViewportDx9::TransformStackCameraPush` | Camera into a texture (the export's samples) |
| 0052bfe0 | `CHmsViewport::VideoAddCurrentFrame` | Sets viewport +0x54c: write this frame |
| 009a4070 | `CVisionViewportDx9::RenderFrameEnd` | Opens the stream, reads the frame back, writes it, presents |
| 007a6530 | `COalAudioPort::SetIsCapturing` | The sound pass records with OpenAL capture |

## One frame of the export

DoShoot, first call after the dialog's OK (state 0): makes the .avi, sets the viewport's forced size
(+0x13c = 1, +0x140 width, +0x144 height), `HighQualityEnable`, `VideoOpenOutput` (+0x548),
`CSceneFxSuperSample::Init(n, n, 0, 0, 0)` with n = HqSampleSize (at most 11), the external loop
when HqMB is on and n n >= 9, starts the clip and `EnableFixedTickFrequency(1, fps x k)` with
k = n n in the sub-frame mode, else 1. In the same call it goes on as every later call does:

1. clip time = game tick (integer ms) - tick at the start + clip start; `ClipViewer::TimeSet`.
2. `IsLastSampleInLoop`: if true (always without the sub-frames), `VideoAddCurrentFrame`.
3. The next call first checks that the stream opened (viewport +0x554, else "Could not initialize
   the video codec"), ends the export when the previous frame's time reached the clip's end, and
   otherwise repeats 1-2.

The render that follows (confirmed):

- `CSceneFxSuperSample::FxRender` sets on the CHmsCamera +0x180 = 1 (on), +0x18c / +0x190 = n, n, and
  for each sample +0x184 / +0x188 = its column and row. It renders the camera into "FxSS Color"
  (`CameraOrSceneFxToTexture` -> `TexRender_RenderTexture` -> `TransformStackCameraPush` ->
  `ZClipCompute`, `ComputeDriverViewMatrix`, `ComputeDriverProjection`, then `RenderCameraBegin`
  ... `RenderCameraEnd`) and blends it into "FxSS Accum1". Internal loop: n n samples in this one
  frame, indices from 0. External loop: one sample, then the indices step on.
- When the indices wrap to (0, 0) (the last sample was just added) the accumulator is drawn to the
  viewport (`Discard_ShaderToViewportOrTexture` with its shader 1, 6 on the path without blending). Otherwise the viewport gets nothing
  from it (a null shader).
- `RenderFrameEnd`: my `frameEnd` callback runs first (engine.cpp calls it before the original). Then
  the game: on the first frame after `VideoOpenOutput`, `VideoStreamOpen` with the size of the
  viewport's colour surface (with +0x13c it first rounds +0x140 / +0x144 up to 8 for the next
  frames). The open fails if the size it hands back differs from the surface's. If the frame is
  flagged (+0x54c): `StretchRect` (only when multisampled), `GetRenderTargetData` into a system memory
  surface, `CopySurfaceToImage` (bottom-up BGR), `VideoStreamWrite`. Then `EndScene`, then `Present`,
  but only if no stream is open or this frame was flagged.

### Answers

**Write and last sample, same frame (confirmed).** The flag set by DoShoot is consumed by the next
`RenderFrameEnd`, and the `FxRender` of that same render adds the last sample and resolves the
average. So the write comes after `RenderCameraEnd` of the last sample, after the resolve and the
overlay, before `EndScene` and `Present`, in the same engine frame. This holds whichever way the game
orders its update and render inside a loop iteration: DoShoot reads the indices the previous render
left, and the next render renders exactly that sample.

**Which sub-frames form a video frame (confirmed).** Sub-frame sample j of video frame f is the render
with clip time start + (f n n + j) / (fps n n), j = 0 .. n n - 1, in the order of the camera's +0x184 /
+0x188 (column first). The written image is the box average of those n n renders: the shutter opens
at the frame's own time and runs forward (not centred). `Init` resets the indices, so the very first
stepped frame is sample 0 of video frame 0. There is no warm-up and no discarded sample at the start.
At the end the export stops after the first render whose time reached the clip's end; a sub-frame
group in progress then is never written (the mod must drop a partial last group too).

**Time resolution (confirmed).** The clip time is computed from the game's tick in whole ms
(`CMwTimer::GetTickTime`). With a step below 1 ms (60 fps x 121 samples = 0.14 ms) many sub-frames get
the same clip time: the motion blur is quantised to whole ms.

**Present (confirmed).** While the stream is open, `RenderFrameEnd` presents only flagged frames. In
the sub-frame mode only one engine frame in n n reaches `Present`; `EndScene` still runs every frame.
Anything in the mod that counts frames or works at Present sees one Present per video frame there.

**Sub-pixel offset (confirmed).** Not a viewport offset and not a matrix patch: `ZClipCompute` moves
the camera's render frustum by (i - (n - 1) / 2) / (n x viewport pixels x rect width / 2) of its width
(a regular n x n grid of pixel fractions), and `ComputeDriverProjection` builds the projection from
that frustum, so the offset arrives in P20 / P21 of the matrix `projectionDetour` sees. Per render,
exactly one `ComputeDriverProjection` gets the main camera: from `RenderCameraNormal` or
`TransformStackCameraPush`, both BEFORE `RenderCameraBegin`. Every other caller (water reflection,
cube maps, shadows, projectors, overlay, light maps, pickers) passes a null camera. Internal loop:
n n projections of the main camera per frame, each with its own offset. Sub-frame mode: one main
camera render (and projection) per engine frame. The water reflection uses
`GetRenderFrustum` without the offset, so the game's samples don't jitter it.

**"FxSS Color" / "FxSS Accum1" (confirmed, the format names inferred).** `CheckSupportedFormats` sets
viewport +0x2ec when A16B16G16R16F (113) renders with blending, +0x2f0 for A32B32G32R32F (116). With
+0x2ec (any recent GPU): "FxSS Color" is a float render target (`GenRenderF`, colour depth 1),
"FxSS Accum1" a float render target with blending, colour depth 1; depth 2 is only asked with the
HDR accumulation DoShoot leaves off (and that needs +0x2f0, so depth 1 = 16-bit float, depth 2 =
32-bit float). The average is formed on the GPU: the first sample clears Accum1, each sample is
added with a blend shader whose constant is 1 / (n n) (`OnNewSampleCount`), the resolve draws Accum1
to the viewport's 8-bit surface. Without float blending (+0x2ec off) it ping-pongs Accum1 and a copy of it with other shaders instead of blending.

**What the AVI write gets (confirmed).** The viewport's colour surface (the forced export size)
read back with `GetRenderTargetData`, converted to bottom-up BGR (3 bytes a pixel) into a buffer of
`GetImageByteSize` bytes: the resolved average plus whatever the overlay pass drew over it, 8 bits a channel. Its size is
the surface's, which must equal what `VideoStreamOpen` returns (rounded to 8), so the written image
always has the video's size. A size that isn't a multiple of 8 on the first frame would fail the
open (inferred from the compare).

## The sound pass

After the stepped frames (state 0x162) DoShoot disables the high quality, restores the size and, when
the dialog's `IsAudioStream` (params +0x34) is set and a video is written: shows "Shooting audio,
Please wait...", sets the audio port's +0x18 to 10 (restored afterwards), restarts the clip in real
time (the fixed tick is off), waits 1000 ms, then `SetIsCapturing(1)`:
`alcCaptureOpenDevice(NULL, 44100, AL_FORMAT_STEREO16, 88200)` - the system's DEFAULT RECORDING
device - and `AudioStreamOpen` with 44.1 kHz stereo 16-bit. Every frame it pops the captured buffers
(`CaptureDoCapture` -> `alcCaptureSamples`) and writes them (`AudioStreamWrite`), clip time = real time
since the capture started, until the clip's end; then capture off, the rest written, the audio stream
closed.

So the game does not tap its own mix: it records whatever the default recording device hears while
the clip plays through the speakers. That is why the pass runs in real time, and why a PC whose
default recording device is a microphone puts the room into the video. There is no flag that makes
it faster: the OpenAL mixer plays at the sound card's rate and the clip follows the real clock.

### When there is no sound at all (confirmed)

- `IsAudioStream` is 0 in a new `CGameCtnMediaVideoParams` (its constructor) and only the dialog's
  "ButtonShootAudio" sets it (`CGameDialogShootVideo`'s constructor binds the two). Off, state 0x162
  goes straight to closing the .avi and the "Video captured to" message: no sound pass, no
  `AudioStreamOpen`, the file has no sound stream. DoShoot keeps running until that message is
  confirmed (when DoShoot shows it as a message with OK), so the frames rendered after the video in
  my log (291 at 1440p60, about 5 s) were most likely the time until OK, not a sound pass.
- Aborted with Esc (context +0x3c set), or a run without video output: no sound pass either.
- A failing capture device does NOT skip the pass: `AudioStreamOpen` comes right after
  `SetIsCapturing(1)` whatever it did. `SetIsCapturing` opens the device only when the game's sound
  is on (`COalAudioPort::GetIsEnabled`, vtable +0x88); if `alcCaptureOpenDevice` fails the capture
  stays off and the stream gets no data (and a format the port never set). The log would show the
  sound line then, with no sound in it.
- The 1 s wait compares the tick with `GetElapsedTimeSinceInit` + 1000. Harmless: without the fixed
  step `CMwTimer::Tick` sets the tick to the elapsed time, so it is a real second.
- The sound pass sets the clip's time from 0 (tick minus the tick at the capture's start), the frames
  from the clip's start: with a clip that starts later than 0 the sound begins before the video.
  `VideoExport::clipStart` has the start; the log warns.
- The game pulls the capture once a second (`CaptureDoCapture` takes samples only when 44100 or more
  are there, at most 44100; at the end everything left).

### The mod's capture device (src/soundcapture.cpp, src/loopback.cpp)

The exe imports the capture functions from OpenAL32.dll (TMNF IAT 00b28384..00b28400, TMUF the
same names). `soundcapture::install` patches `alcCaptureOpenDevice`, `alcCaptureCloseDevice`,
`alcCaptureStart`, `alcCaptureStop`, `alcCaptureSamples`, `alcGetIntegerv` and `alcGetError` (all
or none). While `engine::videoExport` says an export runs, the open returns a device of the mod's
own (a static address the real OpenAL never sees); the other six answer for it and pass every
other device through. `alcGetIntegerv(ALC_CAPTURE_SAMPLES)` reports what came in,
`alcCaptureSamples` copies it out (silence where a read asks for more), `alcGetError` says no error.

The sound comes from WASAPI loopback on its own thread: first process loopback of the game's
process tree (Windows 10 2004 and newer: the game's sound and the mod's weather sound, nothing else),
asked for in the game's format, converted by the audio engine; else the loopback of the default
playback device (everything the PC plays, in its mix format or converted to the game's: float or
PCM, any rate, the first two channels); else the game's own capture device as before. Each step is
logged. The loopback is activated when the export's .avi is made (`videofile` opened), so it is
ready long before the sound pass; when the dialog's sound is off the log says so once.

A playback device that plays nothing sends no packets: the capture thread then puts in silence by
the clock (more than 0.1 s behind), so the sound keeps its length; a source that runs ahead (more
than 0.5 s) loses its oldest frames. At the capture's close the log has the seconds handed to the
game, from the loopback, filled, missing at reads, dropped, and the glitches the audio engine
reported. The game still decides when the pass starts, its 1 s wait and its length.

`tmvs_loopbacktest` (tools/loopbacktest.cpp) plays a 440 Hz tone with waveOut and records it back:
on my PC (2026-10-21) process loopback 44.1 kHz 16-bit, the tone 46 ms after it was played, no
gaps; the playback device's loopback directly in 44.1 kHz and, with `--mix`, from its 48 kHz float
mix through the converter, both with the pitch exact (440 Hz).

Not done, for later: an offline mix (an OpenAL Soft loopback device, `ALC_SOFT_loopback`, stepped
with the fixed clock during the frames) would need the game's OpenAL replaced. High risk.

## If the mod writes its own frames

Already the case: `aviVideoOpenDetour` leaves the game's stream closed and `aviVideoWriteDetour`
returns 1. What still happens or matters:

- The game still reads every flagged frame back (`GetRenderTargetData`, a GPU stall) and converts it,
  even though the pixels go nowhere. Only time, but at 4K it is a fair part of the frame.
- `VideoStreamClose` / `Close` cope with the null video stream; the game's .avi then holds the sound
  only. The return value of the write is ignored by `RenderFrameEnd`.
- The size the open hands back must stay the surface's (rounded to 8 as now), or the export fails
  with the codec message.
- The mod's frame is complete when `aviVideoWriteDetour` runs: in the sub-frame mode the call marks
  the end of a group of n n samples, inside the frame of its last sample.
- With viewport +0x558 (the "alpha" argument of the open; I found no writer that sets it for the
  export) the game saves .dds files instead and never calls `VideoStreamWrite`.
- Viewport +0x150 (0 from the constructor, no other writer found) would spread read-back, copy and
  write over three flagged frames, writing only every third. Not the case in a normal export.

## Accumulation depth of field: `setLensOffset` / `setLensSamples`

The projection of a render is computed before its `RenderCameraBegin`, so `cameraBegin` is too late to
give that render its own lens offset. `engine::setLensSamples(xy, count, focus)` takes a table
instead: while the game supersamples the main camera, the render of sample k (the index FxRender
wrote into the camera, the same in the internal loop and in the sub-frames) gets entry k % count;
without the game's samples the entries go in turn, one per main camera projection.
`engine::setLensOffset(x, y, focus)` is the one-entry case. `engine::mainCameraSample` tells in
`cameraBegin` which game sample and which entry the render that begins got.

The offset is applied in `projectionDetour` after zoom, tiles and jitter, to the main camera's
perspective projection only (`camera == g_mainCamera`, P23 = 1 in both copies): with the eye moved by
a in view space and the plane z = F held, x -> x + a.x (z / F - 1), y likewise, so row 2 of the D3D
matrix gains (a.x row 0 + a.y row 1) / F and row 3 loses a.x row 0 + a.y row 1 (all four columns),
and the engine's transposed GmMat4 at the start of the struct gets the same. View space is metres
(the view matrix comes from a rigid GmIso4) with z forward (w = z), the same depth the shaders use
for the focus (`PS_Focus`: `car.z`).

`g_lastProjection`, `cameraMatrices()` and what `SetTransform` gets all hold the offset matrix: depth
unprojected with it matches what was rasterised. Positions reconstructed with it are off from the
unmoved camera by at most a (z / F - 1), the aperture's size; for screen-space effects that is the
right matrix. Temporal histories (TAA is off in the export, but other histories) would see the lens
moving like a camera shake.

Notes for the plugin: the table should not run in the same order as the game's sub-pixel grid
(column first), or each lens position always lands on the same sub-pixel; the focus has to be known
on the CPU before the frame's renders (the auto focus lives in a 1x1 texture); with n = 2 there are
only 4 lens positions; the engine culls with the unmoved frustum, so something very near the camera
at the screen's edge could be culled a little early.
