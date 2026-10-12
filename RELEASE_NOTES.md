## TM Vibrant Shaders 1.4.1-preview

# New: TrackMania 2 (ManiaPlanet)

**TM Vibrant Shaders now runs in ManiaPlanet 4: TrackMania² Stadium, Canyon, Valley and Lagoon**, natively on the game's Direct3D 11 renderer. It is the same mod with the same `F8` menu, presets and settings as in Nations and United Forever, built on what ManiaPlanet's engine knows: its real surface normals, material colours, sun shadow maps, motion vectors and HDR image.

![TrackMania 2 Stadium in photo mode's compare split: left the game, right the mod with the black hole sky](https://raw.githubusercontent.com/cheatoskar/TM-Vibrant-Shaders/v1.4.1-preview/docs/images/tm2-black-hole-split.jpg)

*TrackMania 2 Stadium, photo mode's compare split: left the game as it is, right with the mod (black hole sky).*

This is a **preview**: tested on Stadium with one graphics card. Please report what looks wrong (`F12` there, then an [issue](https://github.com/cheatoskar/TM-Vibrant-Shaders/issues) with the screenshot and the capture from `Documents\ManiaPlanet\TMVS\captures`).

**What it does in TrackMania 2**
- **Skies:** clear sky, starry night, black hole, aurora, ring world, with the map darkened to a real night beneath them (*Real night under night skies*): the engine's sky light comes down to moonlight, lamps and neon stay lit.
- **Light:** ambient occlusion and contact shadows, bounce light (GI) and neon light on the surroundings, from the engine's own normals. The engine's light is taken apart into sun, sky light and light sources, so night presets put out the sun without turning shadows bright.
- **Atmosphere:** haze and fog, light shafts, volumetric light through the engine's own shadow maps, volumetric clouds, lens flare.
- **Weather:** rain, snow, wet roads, puddles with reflections, lightning, drops on the lens, rain and storm sound.
- **Cinema:** depth of field, motion blur and TAA with the engine's motion vectors; photo mode (`F6`) with the compare split (`Shift+F6`).
- **Neon trail** from the car's real position in the engine.
- **Author's Shader:** looks stored in a map's comments are applied when the map loads; *Set as Author's Shader* works in the map editor.
- The interface stays sharp: the mod draws before the HUD and its own tone map replaces the engine's.

**Install (TrackMania 2)**
1. Download **`TM-Vibrant-Shaders-ManiaPlanet.zip`** and extract it.
2. Close ManiaPlanet and double-click **`Install.cmd`**. It finds ManiaPlanet (also in Steam) and puts `d3d11.dll` next to `ManiaPlanet.exe` (Windows asks for administrator rights under Program Files).
3. Start ManiaPlanet **64-bit**. `F8` opens the menu, **`F5`** switches the effects off and on (`F7` is ManiaPlanet's friends list), `F12` captures a frame.

`Uninstall.cmd` removes it again. If another `d3d11.dll` is already there (ReShade, another mod), the installer stops and changes nothing.

**Not yet in TrackMania 2:** the spray behind the car, hi-res tiled photos and zoom in photo mode, water surfaces, grass detail.

## TrackMania Nations and United Forever

Download **`TM-Vibrant-Shaders.zip`** and run **`TM-Vibrant-Shaders-Setup.exe`** as before; it replaces 1.4.0-preview.

- **Neon trail:** a clean, unbroken ribbon. Each point takes the line's direction from its neighbours, so the pieces meet without gaps at the joints, and the tyre lines no longer fan out around a car that stands or shakes after a crash. It fades out right in front of the camera, and it is denser at speed (60 points a second or one every metre).
- **Snowstorm:** the wind is a real recording of a howling winter storm now, instead of synthesised noise.

Full details in the [changelog](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/main/CHANGELOG.md).
