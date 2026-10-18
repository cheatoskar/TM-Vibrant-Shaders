# Performance

[Back to the README](../README.md)

The mod lights every pixel again, so it costs real GPU time. TrackMania itself is light: a card that runs the bare game
at 240 FPS lands at roughly a third of that with the shaders. All numbers here are for TrackMania Nations/United 1.4.1.

## Measured: GTX 1080 Ti at 2560×1440

Ryzen 5 3600, GTX 1080 Ti, the game's settings at maximum, in a race on Stadium:

| | FPS | ms a frame |
|---|---|---|
| The game alone | ~240 | ~4.2 |
| With the shaders (Vibrant and similar) | 70 – 90 | 11 – 14 |
| With *Render scale (FSR)* 0.85 | ~85 | ~12 |

Where the frame goes, from the `perf` line in `tmvs.log`: the mod's passes ~6 ms, the game drawing its 3D scene ~2.5 – 3 ms,
the rest (the game's shadow maps, the HUD, waiting for the CPU) ~3.3 ms.

## Presets

GPU time of the mod's passes per preset, GTX 1080 Ti at 2560×1440 (previewer, two of my race captures, 200 frames,
best of two runs). In the game add about 6 ms for the game itself and the copies around the passes:

| Preset | Passes | FPS in a race (about) |
|---|---|---|
| Performance | 3.3 ms | ~105 |
| Competition | 4.8 ms | ~90 |
| Realistic, Dreamy, Neon | 5.5 – 5.6 ms | ~85 |
| Vibrant, Snowstorm | 5.7 – 6.2 ms | ~80 |
| Golden Hour, Rainy, Aurora, Storm | 6.3 – 6.9 ms | ~75 – 80 |
| Replay Cinema (replays) | 7.0 – 7.2 ms | ~75 |
| Horizon | 6.4 – 8.4 ms (more with the black hole on screen) | ~70 – 80 |

The menu's three dots on each preset card follow this: one dot under 5 ms, two under 6.3 ms, three above.

## Other graphics cards

Not measured, worked out from the 1080 Ti: the mod's time grows with the pixel count (1080p is 56 % of 1440p) and with
how much slower the card is. Expect ±20 %; the CPU, the map and the preset move it.

| GPU | 1080p, Vibrant | 1440p, Vibrant | 1080p, Performance |
|---|---|---|---|
| Intel Iris Xe (laptop) | ~25 | | ~35 |
| GTX 1050, Radeon 780M, Intel Arc 140V | 40 – 55 | | 55 – 70 |
| GTX 1060 6 GB, RX 580 | ~70 | ~45 | ~90 |
| RTX 3060 | ~105 | ~75 | ~130 |
| GTX 1080 Ti | ~120 | **70 – 90 (measured)** | ~145 |
| RTX 4070 | ~145 | ~110 | ~170 |

At 4K expect about 2.2 times the mod's time of 1440p. *Render scale (FSR)* at 0.67 roughly halves it (see below).

## Hardware

| | GPUs | Settings |
|---|---|---|
| Minimum | GTX 1050, Radeon 780M, Intel Arc 140V | 1080p, *Performance* preset, *Render scale (FSR)* 0.67 |
| 60 FPS and more at 1080p | GTX 1060, RX 580 | Any preset |
| 1440p | GTX 1080 Ti, RTX 3060 (70 – 90 FPS) | Any preset |
| 120 FPS and more at 1440p | RTX 4070 Ti and faster | Any preset |

Any GPU with Shader Model 3.0 runs the mod; below the minimum it starts, but the frame rate is low.

## Where the time goes

The passes of Vibrant at 2560×1440 on the GTX 1080 Ti (6.1 ms in all):

| Effect | Cost | Note |
|---|---|---|
| Lighting (sun, shadows, neon light, the look) | 1.3 ms | 2.1 ms with rain and wet roads (Storm) |
| Ambient occlusion and contact shadows | 0.9 ms | fewer samples with *Effect quality* Low |
| Neon glow and the light it throws on walls | 0.9 ms | |
| Reflections | 0.5 ms | 0.7 ms on a wet track |
| Bounce light | 0.45 ms | |
| Colour grade, bloom, tone curve | 0.4 ms | |
| Edge smoothing (SMAA) and TAA | 0.5 ms | |
| Clouds | 0.3 ms | volumetric clouds more |
| Volumetric light | 0.25 ms | |
| Light shafts | up to 0.5 ms | only with the sun on screen |
| Depth of field | ~2 ms | replays and photo mode by default |

Your own numbers are in the menu: the **Performance** page shows the time of every effect on your GPU, measured live, and
how many FPS you would gain by switching it off.

## Single settings

What single settings add on the GTX 1080 Ti at 1440p (Golden Hour):

| Setting | GPU | Note |
|---|---|---|
| GameGBuffer (*Game normals*, off by default) | +0.9 ms | reflections on the real normals; plus a second render target for every game draw (in game only) |
| EngineLights | +0.25 ms | the neon spill from the game's light sources |
| LongShadowQuality 1 (gap-free long shadows) | +0.1–0.2 ms | |
| GameHdr | +0.1 ms in the passes | plus the game drawing into a float target (in game only) |
| CleanWater | ≤ 0.1 ms | |
| CasterShadows | ~0 ms to sample | drawing the maps: in game only, see the log line below |
| CasterAlphaTest | ~0 ms to sample | drawing fences and grids into the maps: ~1 µs of GPU and ~0.3 µs of CPU more per mesh than a solid one (`tmvs_castertest --rebuild`) |

CPU: CasterShadows looked at every draw of the main camera every frame, 1–11 ms of the game's render thread
(10.7 ms on a big Stadium map with 8000 draws a frame). Since 1.4.0 it checks at most ~700 draws a frame and
nothing while long shadows and volumetric light are off: ≤ 0.5 ms.

## Render scale (FSR 1)

*Render scale* (`RenderScale`, Performance page, off by default) draws the game's 3D image smaller and every effect
with it, then AMD FSR 1 brings the shaded image back up to the screen before the HUD. It helps where the graphics card is
the limit (the *rest of the GPU frame* in the perf line near 0); where the CPU is, it changes nothing.

GPU time of the mod's passes at 2560×1440 with the scale (GTX 1080 Ti, previewer, 200 frames, median of 3; FSR's own two
passes included: 0.24 ms to upscale, 0.08 ms to sharpen):

| Render scale | Scene size | Golden Hour (day) | Horizon (night) | My settings (night) |
|---|---|---|---|---|
| 1 (off) | 2560×1440 | 6.24 ms | 6.68 ms | 3.95 ms |
| 0.77 | 1971×1109 | 4.04 ms | 4.13 ms | 2.54 ms |
| 0.67 | 1715×965 | 3.13 ms | 3.23 ms | 2.00 ms |
| 0.5 | 1280×720 | 1.98 ms | 2.03 ms | 1.31 ms |

The game's own drawing gets cheaper too, but less than in proportion: its pixels shrink with the scale (0.67: 45 % of
them), its draw calls, vertices and shadow maps don't, nor (Stadium by day) the full-size depth pass for its shadow mask.
In a race on the GTX 1080 Ti at 1440p, 0.85 gave ~84 FPS
against ~81 at full size: the GPU saves about 1 ms, but the game's own drawing calls (~3.4 ms of CPU) set a floor, so the
frame rate rises less than the GPU time falls. Lower scales help more where the graphics card is clearly the limit.

What it costs in the picture: fine detail, text on signs and thin lines get softer (0.77 little, 0.5 clearly); small
lights far away can lose some of their glow. *FSR sharpening* (`FsrSharpness`) sets how much is sharpened back.

## The perf line in the log

Every 2000 frames in a race `tmvs.log` gets a line like

    perf: 80 FPS (12.50 ms a frame) over 2000 shaded frames; GPU ms a frame: game 3D 4.10, mod 6.80 (passes 6.50:
    LightingEngine 1.43, Reflect 1.41, ...; caster maps 0.05), rest of the GPU frame 1.20; CPU ms a frame: mod 0.40,
    caster recording 0.45, game 3D drawing calls 3.20

- **game 3D**: the game drawing its scene (with GameHdr and GameGBuffer: into the mod's targets).
- **mod**: from the end of the scene to the shaded image, the copies around the passes included; **passes**: the
  most expensive passes by name; **caster maps**: drawing the CasterShadows maps.
- **rest of the GPU frame**: shadow maps of the game, HUD, menus and idle time. Near 0: the GPU is the limit.
  Large: the CPU is (then GPU times may include waiting for it).
- **CPU**: the mod's work on the game's render thread; **game 3D drawing calls**: how long the game took to issue its scene's
  drawing (the mod's hooks in it included). About as long as *game 3D* on the GPU: the GPU was waiting for the calls, a
  render scale can't make that part faster. With a render scale the line ends with how many frames were drawn scaled.

## Auto quality

On by default. When the frame rate stays below the target FPS, it measures what each effect costs on your GPU and turns
off just enough of them, the ones that cost the most for the least look first (volumetric light, bounce light, samples,
reflections, clouds, TAA, edge smoothing, depth of field, lens flare, sharpening). The light shafts, the neon light and the
long shadows always stay. An effect stays off at least 20 s and comes back once there is room for it again.

It never changes your settings or presets, it decides nothing in the first 20 s after a map start, a loading screen,
alt-tab or photo mode, and it stays out of the way when the effects aren't the limit (CPU, vsync). The menu shows which
effects are off right now. Details: [settings.md](settings.md) (*Auto quality*).

## Getting FPS back

- Pick the **Performance** preset, or set **Effect quality** to Low.
- Set **Render scale** below 1 (0.77, 0.67): the game and the effects at a lower resolution, FSR 1 up to the screen
  (see above). Costs some sharpness, keeps every effect.
- Switch off volumetric clouds and light shafts first, then long-range shadows and TAA.
- In the game's own options: antialiasing off (the mod does it anyway), FX post-processing off.
- Keep the **target FPS** of auto quality at a rate your PC reaches with the shaders (e.g. 75 on a GTX 1080 Ti
  at 1440p). A target it can't reach even with everything off (240) does nothing: auto quality only acts where it can get there.
- *Game normals* (GameGBuffer) costs ~1 ms and more in the game itself: leave it off unless you want its look.
