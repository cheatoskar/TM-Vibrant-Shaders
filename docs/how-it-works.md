# How it works

[Back to the README](../README.md)

TrackMania Forever renders with Direct3D 9. The mod sits between the game and Direct3D, takes the finished 3D image and the depth buffer at the end of each camera, lights the scene again in a chain of pixel shaders, and hands the result back before the game draws its HUD.

## Loading

- **ModLoader:** the ModLoader starts the game and loads `TMVibrantShaders.dll` into it.
- **Game folder:** the DLL is named `d3d9.dll`. Windows loads it instead of the system `d3d9.dll`; the mod loads the real one from the system folder and forwards every export to it (`src/proxy.cpp`, `src/exports.def`).

Either way the mod patches `Direct3DCreate9` and the device's virtual function table (`src/hook.cpp`), so it sees device creation, resets, render target and depth changes, transforms and lights.

## Depth

The effects need to read the scene depth. Direct3D 9 can't read a normal depth buffer, so the mod creates an `INTZ` texture (a depth format that can be sampled, supported by practically all GPUs) and makes the game render into it instead of its own depth buffer (`src/depth.cpp`). That only works without multisampling, which is why the game's antialiasing is switched off.

## Engine hooks

To draw before the HUD, and to tell driving from replays, the mod hooks a few functions inside the game itself (`src/engine.cpp`):

| Hook | Used for |
|---|---|
| `CVisionViewportDx9` render frame / camera begin / camera end / overlay | Where the pipeline runs (end of the 3D camera, before the HUD) |
| Driver view matrix and projection | The exact camera; a sub-pixel offset for jittered TAA |
| MediaTracker clip player, clip viewer, video shooter | Replays, intros and the video export (for motion blur and depth of field) |
| Race reset, respawn | Clearing the neon trail and the long-shadow height map |
| Zone water height, water reflection plane | The sea level of the map |
| Loading a map's decoration | The environment of the map (United Forever) |

The function addresses come from the game's symbol map. Nations Forever and United Forever ship different builds of `TmForever.exe` with the same code at shifted addresses, so the mod carries one address table per build and checks which one matches before it patches anything. With an unknown executable it runs without the engine hooks.

## The pipeline

Everything is Shader Model 3.0 (`shaders/tmvs.hlsl`, compiled when the mod is built, so the game doesn't stall at start). Per main camera, in this order:

1. **Geometry:** linear depth, normals, half-resolution copies.
2. **Height map:** depth samples are splatted into a top-down height map of the track around the camera, kept from frame to frame. Long-range shadows, volumetric light and rain splashes use it. The player's car is kept out of it.
3. **Occlusion and shadows:** ambient occlusion, sun shadows traced in screen space and through the height map, then a depth-aware blur.
4. **Volumetric light and bounce light:** shadowed haze along each view ray; one bounce of coloured light at quarter resolution, accumulated over frames.
5. **Sky:** the game sky, or one of the custom skies, and volumetric clouds.
6. **Reflections and neon light:** screen-space reflections for wet and polished surfaces and water; light spilled from coloured light sources.
7. **Lighting:** sun, sky ambient, AO, shadows, bounce light, wetness, puddles, water, grass and fog combined in HDR.
8. **Neon trail:** points recorded on the GPU, drawn as a glowing ribbon.
9. **Camera effects:** depth of field and motion blur (replays), light shafts, bloom, auto exposure, tone mapping and grading.
10. **Anti-aliasing:** FXAA, TAA, CAS sharpening, drops on the lens.
11. **Particles:** rain, splashes and spray, drawn on the finished image.

The *Performance* panel in the menu times each of these groups on the GPU with timestamp queries.

## Water

Stadium's water blocks always sit at the same height (8.00 m), and the game reports the sea level of the map when it loads it and the plane it renders water reflections for. The shader treats a surface as water only when it lies at one of those heights. In United Forever the Stadium height is used only on Stadium maps.

## Sound

Rain and thunder are CC0 recordings embedded in the DLL as MP3, decoded with minimp3 and mixed on their own thread through the Windows `waveOut` API. The thunder for each lightning strike is chosen from three distance groups, never the same recording twice in a row, and pitched, filtered and panned per strike.

## Settings and presets

`src/config.cpp` holds the field table that drives the INI files, the menu and the presets. Auto quality (`src/autoquality.cpp`) works on a copy of the settings, so it never changes what you set.
