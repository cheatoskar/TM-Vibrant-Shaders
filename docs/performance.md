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

Your own numbers are in the menu: **Advanced → Performance** shows the time of every effect on your GPU, measured live, and how many FPS you would gain by switching it off.

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
