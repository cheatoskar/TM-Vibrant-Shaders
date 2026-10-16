# Performance

[Back to the README](../README.md)

## Hardware

| | GPUs | Settings |
|---|---|---|
| Minimum | Shader Model 3.0: GTX 750, HD 7750, Intel UHD 620 | *Performance* preset, *Effect quality: Low* |
| Recommended | GTX 1060, RX 580, Radeon 780M, Intel Arc 140V | Any preset at 1080p |
| Everything on, 1440p and up | RTX 3060, GTX 1080 Ti and faster | |

## What it costs

Time the shaders add per frame at 1920×1080, looking into the sun (the worst case):

| GPU | Game without shaders | Performance | Vibrant | Replay Cinema |
|---|---|---|---|---|
| Intel Iris Xe (laptop) | ~90 FPS | 10.0 ms, ~45 FPS | 17.0 ms, ~35 FPS | 23.5 ms, ~30 FPS |
| GTX 1050 | ~200 FPS | 8.4 ms, ~75 FPS | 14.3 ms, ~50 FPS | 19.7 ms, ~40 FPS |
| Radeon 780M (laptop) | ~160 FPS | 5.2 ms, ~85 FPS | 8.9 ms, ~65 FPS | 12.3 ms, ~55 FPS |
| **Intel Arc 140V (measured)** | ~115 FPS | 3.7 ms, ~80 FPS | 6.3 ms, ~65 FPS | 8.7 ms, ~55 FPS |
| GTX 1060 6 GB / RX 580 | ~300 FPS | 3.6 ms, ~145 FPS | 6.1 ms, ~105 FPS | 8.4 ms, ~85 FPS |
| RTX 3060 | ~400 FPS | 1.8 ms, ~235 FPS | 3.0 ms, ~180 FPS | 4.1 ms, ~150 FPS |
| GTX 1080 Ti | ~400 FPS | 1.6 ms, ~245 FPS | 2.6 ms, ~195 FPS | 3.7 ms, ~160 FPS |
| RTX 4070 | ~450 FPS | 0.9 ms, ~320 FPS | 1.5 ms, ~270 FPS | 2.1 ms, ~230 FPS |

Only the Arc 140V row is measured. The others are estimates: the measured shader time scaled by each card's relative speed, on top of a typical frame rate for the bare game. CPU, drivers and the map change the numbers. At 1440p expect about 1.8× the shader time, at 4K about 4×.

Most presets cost about as much as Vibrant. Storm costs about 20% more, Golden Hour and Horizon (custom skies) about 40% more, Aurora roughly twice as much.

## Where the time goes

Approximate cost of single settings on the Arc 140V:

| Setting | Cost |
|---|---|
| Volumetric clouds | 1.5 ms |
| Light shafts (sun on screen) | 1.1 ms |
| Long-range shadows | 0.8 ms |
| Temporal anti-aliasing | 0.7 ms |
| Reflections (wet or dry track) | 0.5 – 1 ms |
| Rain (particles and wet surfaces) | 0.5 – 1 ms |
| Volumetric light | 0.3 ms |
| Bounce light | 0.25 ms |
| Depth of field (replays only by default) | 2 ms |
| Effect quality High → Low | saves about 1 ms |

Your own numbers are in the menu: the **Performance** page shows the time of every effect on your GPU, measured live, and how many FPS you would gain by switching it off.

## Measured: GTX 1080 Ti at 2560×1440

Ryzen 5 3600, GTX 1080 Ti, the game's settings at maximum: the game alone runs at ~240 FPS, with the
shaders at 75–80 FPS. The mod is GPU-bound there: its passes take ~6–7 ms, the game's own frame ~4 ms.

GPU time of the mod's passes (previewer, 200 frames, same captures and settings, median of 3 runs):

| Build | Golden Hour (day) | Horizon (night) |
|---|---|---|
| 1.4.0 checkpoint of Oct 16 (4a99b42) | 5.96 ms | 6.22 ms |
| Now, *Normals from the game's shaders* (GameGBuffer) off | 6.29 ms | 6.37 ms |
| Now, GameGBuffer on | 7.21 ms | 6.71 ms |

What single settings cost there (Golden Hour):

| Setting | GPU | Note |
|---|---|---|
| GameGBuffer (*Normals from the game's shaders*, off by default) | +0.9 ms | reflections on the real normals; plus a second render target for every game draw (in game only) |
| EngineLights | +0.25 ms | the neon spill from the game's light sources |
| LongShadowQuality 1 (gap-free long shadows) | +0.1–0.2 ms | |
| GameHdr | +0.1 ms in the passes | plus the game drawing into a float target (in game only) |
| CleanWater | ≤ 0.1 ms | |
| CasterShadows | ~0 ms to sample | drawing the maps: in game only, see the log line below |

The most expensive passes: the lighting (1.4 ms), reflections (0.5–1.4 ms), the custom sky of Golden Hour
or Horizon (0.8–1.0 ms), the final grade (0.4 ms), light shafts (0.5 ms), AO and shadows (0.3–0.6 ms), TAA
(0.3 ms).

CPU: CasterShadows looked at every draw of the main camera every frame, 1–11 ms of the game's render thread
(10.7 ms on a big Stadium map with 8000 draws a frame). Since 1.4.0 it checks at most ~700 draws a frame and
nothing while long shadows and volumetric light are off: ≤ 0.5 ms.

## The perf line in the log

Every 2000 frames in a race `tmvs.log` gets a line like

    perf: 80 FPS (12.50 ms a frame) over 2000 shaded frames; GPU ms a frame: game 3D 4.10, mod 6.80 (passes 6.50:
    LightingEngine 1.43, Reflect 1.41, ...; caster maps 0.05), rest of the GPU frame 1.20; CPU ms a frame: mod 0.40,
    caster recording 0.45

- **game 3D**: the game drawing its scene (with GameHdr and GameGBuffer: into the mod's targets).
- **mod**: from the end of the scene to the shaded image, the copies around the passes included; **passes**: the
  most expensive passes by name; **caster maps**: drawing the CasterShadows maps.
- **rest of the GPU frame**: shadow maps of the game, HUD, menus and idle time. Near 0: the GPU is the limit.
  Large: the CPU is (then GPU times may include waiting for it).
- **CPU**: the mod's work on the game's render thread.

## Auto quality

On by default. It watches the frame rate and, when it stays below the target FPS, steps down in three levels:

1. Fewer samples (AO, shadows, clouds, long shadows); track reflections no higher than 1.
2. Long shadows, grass detail, reflections, volumetric light and bounce light off.
3. TAA, neon light, light shafts, volumetric clouds, depth of field, motion blur, lens flare and sharpening off.

When there's room again it steps back up. It never changes your settings or presets: it works on a copy. It also stays out of the way when the effects aren't the bottleneck (CPU limit, vsync). The current level is shown at the top of the menu.

## Getting FPS back

- Pick the **Performance** preset, or set **Effect quality** to Low.
- Switch off volumetric clouds and light shafts first, then long-range shadows and TAA.
- In the game's own options: antialiasing off (the mod does it anyway), FX post-processing off.
- Keep the **target FPS** of auto quality at a rate your PC reaches with the shaders (e.g. 75 on a GTX 1080 Ti
  at 1440p). A target it never reaches (240) keeps it switching effects off and on every few seconds.
- *Normals from the game's shaders* (GameGBuffer) costs ~1 ms and more in the game itself: leave it off unless you want its look.
