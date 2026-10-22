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

`tmvs_loopbacktest` (tools/loopbacktest.cpp) plays a tone with waveOut and records it back. On my
PC (2026-10-21, then still a 440 Hz tone, audible) process loopback 44.1 kHz 16-bit, the tone 46 ms
after it was played, no gaps; the playback device's loopback directly in 44.1 kHz and, with `--mix`,
from its 48 kHz float mix through the converter, both with the pitch exact. A 440 Hz tone heard in
headphones now and then turned out to be this test: it now plays 19 kHz at -40 dB (like the latency
measurement), found with a Goertzel filter, and is not run without asking first.

### Sound and picture in step

- **The clip's start.** The pass plays the clip from 0, the frames start at the clip's start
  (`VideoExport::clipStart`, shooter +0x30): the first `clipStart` seconds of the pass's sound are
  left out (`loopback::Capture::start(skipFrames)`).
- **The output latency.** OpenAL mixes ahead into its output buffer: what the game starts at clip
  time t reaches the loopback at t + latency. Measured once per export, right after its .avi is made
  (the clip is stopped, DoShoot waits on the main thread, where the game's own OpenAL calls come
  from too): a 50 ms tone at 19 kHz and -54 dB (at a mixing rate below 47.5 kHz: 0.4 of it; below 42.5 kHz no measurement, the tone would be audible) on a
  source of the game's own OpenAL context, found again in the loopback with a Goertzel filter, the
  same way the pass is timed (frame 0 = the capture's start). The latency is left out at the start
  as well, and at the game's `alcCaptureStop` the capture runs that much longer (at most 0.25 s), so
  the sound keeps the clip's length. Not heard (sound volume 0, no free source, implausible): not
  compensated, logged. The log line: "the game's sound output latency x ms (measured)".
- **The first packet.** A loopback takes a moment for its first packet after `Start`. That packet
  goes onto the clock by its own time stamp (`GetBuffer`'s QPC position): frame 0 is the start's
  moment, and the measurement and the pass line up however long each start took. In my test (three
  runs) the tone was found 49..58 ms after `waveOutWrite`, against 46 ms before the time stamp
  was used (the first packet then landed early by its delay).
- **The mod's weather sound** (src/audio.cpp, waveOut in the same process, so in the process
  loopback) follows the effects' clock now (`effectTime`, before: the real clock, off by the
  offset an export leaves). In the pass the plugin hands it the video's clock at the clip time
  played: the effects' clock at the first stepped frame + the pass's time - the clip's start
  (`soundcapture::passClock`). The strikes are a function of the one-second slot and the amount
  (`weather::lightningStrike`), so the pass hears the thunder of exactly the flashes in the frames,
  0.4 s + 5 s per distance after them. A clock that jumps back (into the pass) forgets the thunders
  still waiting, or they would come twice. During an export the rain is not faded out when the
  game is in the background (`audio::setAlwaysInFront`).

### Offline sound: what it would take (plan, not built)

Today the sound pass costs the clip's length once more in real time and the sound is a recording.
An offline mix would render the game's sound in step with the fixed clock while the frames are
made, exactly `step x 44100` samples per stepped frame. What the game asks of OpenAL (its imports
and the strings of `COalAudioPort`): sources and buffers (`alGenSources`, `alBufferData`, static
sounds and streams through `alSourceQueueBuffers` / `alSourceUnqueueBuffers`: music and the
engine), 3D (`alSource3f`, distance model, Doppler, `AL_SOURCE_RELATIVE`), pitch (engine sounds
change pitch every frame), gain, looping, and when `ALC_EXT_EFX` is there its EAX reverb
(`alGenEffects`, `AL_EAXREVERB_*`, an auxiliary slot, sends through `alSource3i`: tunnels, inside
the car, outside) plus the HRTF choices of its settings (`|Audio|HrtfFull`). No listener calls in the
imports (they may come through `alGetProcAddress`).

The installed OpenAL (Creative's router, 2007; "Generic Software" on today's Windows) has no
`ALC_SOFT_loopback`, so nothing in it can render on demand. Three ways:

1. **OpenAL Soft in the mod, for the whole game.** Load a bundled 32-bit OpenAL Soft (soft_oal.dll)
   and point all 38 OpenAL imports of the exe at it (the IAT, as the capture hooks do now) before
   the game opens its device; open that device as a loopback device (`alcLoopbackOpenDeviceSOFT`,
   44.1 kHz stereo float) and play its output through the mod's own WASAPI thread
   (`alcRenderSamplesSOFT` pulled by the playback clock). During an export the mod stops the
   playback and pulls `step x 44100` samples per stepped frame straight into the encoder: the sound
   is mixed in step with the frames, no pass after them, exact sync, also with the motion blur's
   sub-frames. EFX reverb and HRTF are in OpenAL Soft. Licence: LGPL 2.1; fine as a separate,
   unmodified DLL beside the mod, with its licence text and a pointer to its source in the release
   (THIRD_PARTY_NOTICES.md), no static linking. Risk for the normal game sound: high, it changes
   for everyone: another mixer (its own resampler, panning and reverb sound a little different from
   Creative's), the mod's thread becomes the sound output (device changes, headphones plugged in,
   latency, a stall of the mod = silence or crackle), and the import switch must happen before the
   game opens its device (the mod boots on its own thread from DllMain, a race to check). Behind a
   setting, off by default at first. Effort: about one agent day (the redirect, loopback device and
   output thread 2-3 h, the export side 2 h, a standalone test with OpenAL Soft 1-2 h, review), plus
   my own tests in the game: menus, races, replays, the export, Alt+Tab, device change.
2. **Own mixer, beside Creative's.** Hook every OpenAL call, pass it on, and keep a copy of the
   state (buffers' PCM, sources' parameters and queues); during an export mix that state in the mod
   in step with the clock. The normal sound stays untouched (only bookkeeping: buffer copies in
   memory, a hook on every call). But the mix has to be written: 3D panning and distance rolloff as
   OpenAL's model, Doppler, pitch resampling, streams, looping, and an EAX reverb good enough not to
   sound dry next to the game's. Effort: two to three agent days, much of it in matching OpenAL's
   behaviour; risk for the normal sound low, risk that the video's sound sounds different from the
   game's medium to high.
3. **Keep the real-time pass** (now: process loopback, synced, measured latency). Costs the clip's
   length once more (the frames take far longer anyway) and records whatever the game does in real
   time; its sync is as good as the latency measurement (a few ms).

My recommendation: stay with 3 for 1.4.2. If the pass's extra time or a sync complaint comes up, 1
is the clean way, built as an option and tested on its own first (a test program that plays through
the redirected OpenAL Soft and renders offline).

## If the mod writes its own frames

Already the case: `aviVideoOpenDetour` leaves the game's stream closed and `aviVideoWriteDetour`
returns 1. What still happens or matters:

- The game read every flagged frame back (`GetRenderTargetData`, a GPU stall) and converted it, even
  though the pixels went nowhere: skipped now when the mod's frame is ready (next section).
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

## Skipping the game's read-back (src/engine.cpp, `skipGameReadback`)

`RenderFrameEnd` (TMNF 009a4070, TMUF 009a3df0, the same 614 instructions with the same offsets)
reads every flagged frame back: `GetRenderTargetData` of its colour surface into a system memory
surface (the CPU waits until the GPU has finished the whole frame, then 4 bytes a pixel cross the
bus, 33 MB at 4K), `CopySurfaceToImage` (lock, then every pixel to bottom-up BGR on the CPU, 25 MB
at 4K), then `VideoStreamWrite`. With the mod's own frames (`VideoOwnFrames`, the MP4) those pixels
go nowhere. The function has a spread-out mode: with viewport +0x150 set, a global phase (TMNF
00d77c78, TMUF 00d792d8) does one step per flagged frame (0 read back, 1 convert, 2 write). For a
frame whose own frame is ready (`videofile`'s `ownFrameReady`, the plugin's `ownReady`: a finished
frame whose planes have the encoder's size, the same checks `ownDeliver` makes first), +0x150 = 1 and
phase 2 are set for that one call: the read-back and the conversion are skipped, `VideoStreamWrite`
still runs (the mod's frame goes in, the game's sound timing and frame count stay), and both values
are put back after. Only when the stream is open, the frame flagged, no .dds mode, +0x150 and the
phase 0; the phase's address is checked at install (RenderFrameEnd + 0x322 writes it: `mov [phase],
ebp`). Present is not affected (its condition is the flag). If the mod's frame fails after all, the
last image the game did read goes in (logged, counted in the MP4 line). Logged once: "the game's
read-back of its video frames skipped".

What it saves, from the code (not measured): one full-frame GPU-to-CPU copy with its stall and one
CPU conversion of every pixel per written frame. The mod's own frame still reads its NV12 / P010
planes back (`ownDeliver`: 1.5 or 3 bytes a pixel instead of 4, and no conversion), a frame later
(next section). With the supersampling inside a frame (n x n renders) the frame is long and the
saving small; with the motion blur's sub-frames a written frame has one render, there the
read-back was a large part.

## The mod's read-back, a frame later (src/planeread.cpp, `PlaneReadback`)

Until 1.4.2's tests `ownDeliver` read the planes back at the game's write: `GetRenderTargetData`
into system memory, `LockRect`, copy into the encoder's buffer. The lock waited until the GPU had
finished the whole frame (my export at 1440p, 2 × 2 samples: 20.7 of 38.8 ms a frame "reading
back"), and while the CPU then worked out the game's next frame the GPU had nothing to do.

What the driver does (GTX 1080 Ti, driver 582.78, measured in `tmvs_preview`, 9Ex and `--plain`
alike): `GetRenderTargetData` into a system memory surface returns at once, the copy runs when the
GPU gets to it in its queue, and `LockRect` is where the CPU waits, for the work queued before the
copy only. An event query's `GetData` with `D3DGETDATA_FLUSH` waited for the GPU too (23 ms), without
the flag it returns at once.

So each write starts the copy of the frame's planes (and the 1x1 auto focus, for the lens depth of
field) into one of three system memory slots and puts the slot in a queue; the frames whose copies
are done (event query, no flush) go into the encoder in order, normally the one before. When all
three slots are taken the oldest is locked anyway (it waits, but the GPU is a frame past it). A
write without a new frame queues the last slot again. Before the game's own image goes in (the
mod's frame failed), when the frames on the video's clock end, and when the file closes, every
frame on its way goes in first (`videofile::flushOwnFrames`, `FrameSource::flush`), so the frame
count and order stay the game's. A device reset keeps the system memory slots (their queries go,
a slot without one counts as done); the export's end frees them. Address space: three slots of
5.5 MB at 1440p NV12, 25 MB at 4K P010. The auto focus for the lens now comes one or two frames
later than before.

`tmvs_preview <cap> out.bmp --video --own [hdr] --repeat 3 --export 60 --cpu 15 [--plain]` renders 60
export frames and reads them back both ways, with 15 ms of busy CPU a frame standing in for the
game's own work. My numbers (capture_134, 2560 × 1440, two runs each):

| Samples | CPU a frame | Straight (as before) | A frame later |
|---|---|---|---|
| 2 × 2 | 0 ms | 25.3 ms a frame | 25.3 (the GPU alone is the limit) |
| 2 × 2 | 15 ms | 40.8 ms, reading back 25.5 | 24.9 ms, reading back 9.7 (waiting in the lock) |
| 1 | 10 ms | 20.2 ms, reading back 10.0 | 11.5 ms, reading back 1.0 |
| 2 × 2, HDR10 (P010) | 15 ms | 42.5 ms, reading back 27.3 | 26.5 ms, reading back 11.0 |
| 3 × 3 | 15 ms | 67.5 ms, reading back 52.0 | 50.6 ms, reading back 35.2 |

A frame now takes about the longer of the CPU's and the GPU's time instead of their sum. In the
game the GPU also draws the game's own scene for every sample, so the gain depends on how much of
a frame is the CPU's; the log line "the mod's own frames: ... ms a frame, reading back ... ms a
frame (... of it waiting for the copy ...)" shows it. `tmvs_planereadtest` checks that every frame
comes out once and in order through repeats, flushes and resets.

## The NVENC stream's write thread

The NVENC path writes its packets into the temporary `.video.tmp.h264` / `.hevc`. That `WriteFile`
ran on the encoder's thread; in my export it took 318 ms twice and the game waited for it. Now a
thread of its own writes them (`StreamWriter` in src/videoencoder.cpp): up to 64 MB of packets
wait in memory (about 150 frames at 1440p even near lossless), beyond that the encoder waits,
nothing is dropped. A failed write fails the next packet's encode and the export's report as
before. The log: "venc: NVENC slow: the stream's write thread took ... ms" for writes over 250 ms,
and once per export "venc: NVENC stream: n writes, ... MB, slowest ... ms, at most ... MB waiting;
the encoder waited for room ... times".

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

## The encoders (`src/videoencoder.cpp`, `src/nvenc.cpp`)

Which one writes the MP4:

| *Video encoder* | Card | Encoder |
|---|---|---|
| *Graphics card* | NVIDIA, driver R570 or newer | **NVENC driven directly**: the driver's `nvEncodeAPI.dll` (SysWOW64), API 13.0, its own D3D11 device, H.264 High or HEVC Main / Main10. Constant quality (CQ) with a cap on the bitrate, P7, no B-frames, no AQ, no lookahead (the measurements below say why). HDR10: BT.2020 / PQ in the bitstream and mastering display and content light SEI. |
| *Graphics card* | NVIDIA with an older driver, AMD, Intel; or NVENC refused | The encoder Windows offers through Media Foundation (NVIDIA's, AMD's or Intel's MFT), at an average bitrate with room for peaks; last resort Microsoft's software H.264. |
| *Graphics card*, AV1 | a card whose NVENC has AV1 (RTX 40 and newer) | NVIDIA's AV1 MFT (AV1 is not driven directly). Without AV1 the chain goes on to HEVC, then H.264. |
| *ffmpeg* | any | x264, x265 or SVT-AV1 through your own `ffmpeg.exe`; without it the graphics card. |

The log says what happened: `venc: NVENC: <card>, API 13.0 (the driver offers 13.0); H.264: B-frames 4 ...` once with the card's capabilities, or `venc: NVENC not used: <why>`, and per export `venc: NVENC direct: <why>; Media Foundation's encoder instead` when it fell back.

With NVENC the packets go into a temporary elementary stream next to the video (`VideoNN.video.tmp.h264` / `.hevc`) while the export runs; at the end Media Foundation's sink writer copies them into the MP4 with the sound as AAC (nothing is encoded twice). Should that fail, the stream stays as `VideoNN.h264` (VLC and ffmpeg play it; `ffmpeg -i VideoNN.h264 -c copy x.mp4` wraps it) and the sound as `.wav`. NVENC runs in asynchronous mode: each frame's output buffer has an event the driver sets when it is done, so no fixed delay is trusted (the driver holds frames longer than its B-frames and lookahead say).

### My measurement (2026-10-22)

8 s (480 frames) of a 1440p60 replay over Stadium's grid with film grain, rendered by the mod with 2 × 2 samples and exported near-lossless (275 Mbit/s); every encoder got the same decoded frames and was scored with VMAF (ffmpeg's libvmaf, model 0.6.1) against them. GTX 1080 Ti (Pascal), driver 582.78, a 12-thread CPU. Encoder fps is the encoder's own throughput; the export renders at about 19 fps, so anything above that costs no time. The CPU speeds (x264) were noisy: other work ran on the machine, the quiet-run figures are given.

| Variant | Mbit/s | Encoder fps | VMAF mean / worst frame |
|---|---|---|---|
| NVENC MFT (the old path), quality 80 (0.2 bit per pixel) | 45.9 | 205 | 91.37 / 80.6 |
| NVENC MFT, 61.6 / 82.0 / 102.8 Mbit/s | 61.6 / 82.0 / 102.8 | 200 / 194 / 97 | 95.15 / 97.76 / 99.09 |
| NVENC direct H.264, P7, no B-frames, no AQ, CQ 21 / 22 / 23 / 24 | 93.1 / 84.6 / 75.3 / 66.5 | 110 – 117 | 99.16 / 98.70 / 97.95 / 97.01 |
| the same, CQ 26 / 28 / 30 | 51.4 / 39.8 / 31.2 | 124 – 131 | 94.28 / 90.75 / 86.92 |
| NVENC direct H.264 CQ 23, three B-frames | 64.1 | 81 | 95.83 / 85.5 |
| NVENC direct H.264 CQ 21, three B-frames | 81.6 | 80 | 97.68 / 88.8 |
| NVENC direct H.264 CQ 23, spatial AQ / temporal AQ | 74.1 / 75.4 | 113 / 118 | 96.90 / 97.98 |
| NVENC direct H.264 CQ 23, lookahead 0 / 8 / 24 | 74.2 / 75.3 / 75.5 | 129 / 117 / 122 | 97.89 / 97.95 / 97.96 |
| NVENC direct H.264 CQ 23, two passes (quarter resolution) | 75.1 | 163 | 97.78 / 92.8 |
| NVENC direct H.264 CQ 23, P4 / P1 | 76.1 / 79.1 | 249 / 275 | 97.77 / 97.01 |
| NVENC direct H.264, everything on (3 B, spatial + temporal AQ, lookahead 24, two passes), CQ 21 / 23 / 25 | 81.7 / 64.3 / 49.9 | 142 | 96.83 / 94.61 / 91.66 |
| NVENC direct HEVC 8-bit CQ 24 / 22 | 63.8 / 81.8 | 85 / 79 | 96.97 / 98.67 |
| NVENC MFT HEVC, 45.6 / 60.7 Mbit/s | 45.6 / 60.7 | 177 / 173 | 92.60 / 95.71 |
| x264 medium CRF 18 / 20 / 21 / 22 / 24 / 27 | 100.2 / 75.3 / 64.6 / 55.1 / 39.6 / 24.0 | 18 – 23 | 98.80 / 97.09 / 95.81 / 94.23 / 90.23 / 82.25 |
| x264 CRF 20: veryfast / faster / slow | 74.6 / 74.8 / 75.7 | 65 / 39 / 12 | 93.78 / 96.47 / 97.22 |
| x264 medium CRF 21: aq-mode 3 / -tune film / psy-rd 0 | 67.8 / 66.6 / 59.1 | 22 – 27 | 95.72 / 96.24 / 95.30 |
| x265 fast CRF 22 / 24 | 63.7 / 44.7 | 13 | 90.02 / 85.72 |
| SVT-AV1 preset 7 CRF 32 / 36 | 84.8 / 57.8 | 15 – 21 | 96.66 / 93.86 |

What I take from it:
- On this card NVENC driven plainly beats x264 medium in VMAF at the same rate (75.3 Mbit/s: 97.95 against 97.09) and is six times faster. The old MFT path needed 82 Mbit/s for what NVENC direct gives at 70.
- B-frames and AQ lose VMAF here (the grain makes B-frames expensive; AQ moves bits to where VMAF does not look). Whether spatial AQ looks better in dark flat areas, I have not judged by eye; `Options::nvencAQ` is there to try.
- Lookahead gained nothing and costs a buffer pair per frame in the game's 2 GB address space (10 MB a frame at 1440p: the MFT path took 222 MB of address space, NVENC direct about 360 without lookahead and 570 with 24).
- HEVC on Pascal: about the same as H.264 at the same rate, slower. x265's and SVT-AV1's defaults lose in VMAF on this footage.
- 10-bit encodes were scored after rounding to 8 bits and came out lower (NVENC HEVC 10-bit CQ 25: 55.9 Mbit/s, 93.65); I do not trust that comparison.

### The quality slider

Calibrated on the measurement above (1440p60): 80 (the default) ≈ VMAF 96 or more for as few bits as possible, 92 (*Editing*) ≈ 98, 62 (*Discord*) small. Checked through the slider:

| Quality | NVENC direct H.264 | Media Foundation (MFT) | x264 (ffmpeg, medium) |
|---|---|---|---|
| 62 | CQ 30.8: 27.4 Mbit/s, VMAF 84.6 | 0.10 bit/px: 23.7 Mbit/s, 79.5 | CRF 26.8: about 25 Mbit/s, 83 |
| 80 | CQ 24.5: 61.3 Mbit/s, 96.26 | 0.31 bit/px: 70.5 Mbit/s, 96.51 | CRF 20.5: 69.8 Mbit/s, 96.47 |
| 92 | CQ 22.5: 78.6 Mbit/s, 98.26 | 0.39 bit/px: 89.1 Mbit/s, 98.28 | CRF 18.7: 90.9 Mbit/s, 98.32 |

NVENC's cap is twice the Media Foundation rate (137 Mbit/s at 80, 1440p60), only against bursts. HEVC: NVENC the same CQ (80: 60.6 Mbit/s, 96.53), the MFT 0.9 of the rate, x265 CRF 19 at 80, SVT-AV1 CRF 33. AMD's and Intel's MFTs are not measured and get NVIDIA's rates.

To measure again: `build/Release/tmvs_encbench.exe <reference.mp4> <outdir> --ffmpeg <ffmpeg.exe> --batch variants.txt`, one variant per line (`name quality=80 codec=hevc nvenc=0 bframes=3 aq=1 lookahead=8 q=23 backend=ffmpeg extra="-preset slow"` ...); it prints and appends to `<outdir>\results.tsv`.
