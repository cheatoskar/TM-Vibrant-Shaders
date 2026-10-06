## TM Vibrant Shaders 1.2

Real-time lighting, weather and skies for TrackMania Nations Forever and United Forever.

This release brings full TrackMania United Forever support, real rain and thunder sound, volumetric light, bounce light, lightning bolts, water that is found by its height, a neon light trail and the new **Realistic** preset.

![Original and Realistic](https://raw.githubusercontent.com/cheatoskar/TM-Vibrant-Shaders/v1.2.0/docs/images/compare-realistic.jpg)

![Storm: rain on a wet track, the checkpoint mirrored in the road](https://raw.githubusercontent.com/cheatoskar/TM-Vibrant-Shaders/main/docs/images/storm-checkpoint.jpg)

### Download and install

**`TM-Vibrant-Shaders.zip`** is all you need. Unzip it and run **`TM-Vibrant-Shaders-Setup.exe`**. You can install:
- **for the [TrackMania ModLoader](https://tomashu.dev/software/tmloader/)**, then tick the mod in the ModLoader, or
- **into TrackMania Nations Forever or United Forever without the ModLoader**: the setup lists every TrackMania it finds, each with its own button, and adds a `d3d9.dll` next to `TmForever.exe`.

Updating from 1.1: run the new setup, it replaces the old version. Your settings and presets in `Documents\TrackMania\TMVS` stay.

In the game: `F8` opens the menu, `F7` switches the shaders on and off. For the best result set *Antialiasing: off* and *Anisotropic filtering: 16x* in TrackMania's options.

### What's new

**Game support**
- **TrackMania United Forever**: its game executable is a different build than Nations Forever's. The mod now knows both and picks the right one. In United it reads each map's environment, so Stadium's water only shows on Stadium maps and the island, bay and coast seas come from the game.
- The setup finds Nations and United Forever and gives each its own install button.

**Presets**
- New **Realistic** (replaces Cinematic): TrackMania's own colours and sky, no extra saturation or colour grading. Only the light is new: shadows, ambient occlusion, bounce light and a little haze.
- Shorter names: **Neon** (was Neon Night), **Horizon** (was Event Horizon), **Storm** (was Thunderstorm). Saved settings keep working.
- Night presets show every star. Horizon turns the sky so Saturn and the black hole are in view from the start screen.

**Lighting**
- Volumetric light: sun shafts with real shadows in the haze, also with the sun off screen.
- Bounce light: coloured light bouncing off nearby surfaces.

**Weather and water**
- Rain and thunder sound from real recordings: a rain loop that follows the rain amount and ten different thunders (close cracks, rolling and far rumbles). Each strike sounds a little different and comes from the side of the flash.
- Lightning bolts in the sky, near where you look. The thunder follows after a delay that matches the distance.
- Rain runs down steep surfaces and splashes on the car and barrier tops. Rain drops on the lens.
- Spray behind the car on wet roads: its own setting (on in Storm). It only flies while the tyres touch the road.
- Water is found by its height: pools, rivers and the sea get waves, refraction and reflections, and nothing else does.

**Neon trail**
- A glowing light trail behind the car, from the rear tyres or the middle, for the whole run or fading. Advanced menu, off by default. A restart clears it, a respawn starts a new line.

**Reflections**
- *Track reflections (dry)* goes up to 2: above 1 the track is polished and checkpoints and objects mirror clearly. The night presets use it.

**Menu**
- The simple menu's sections start collapsed and remember what you open.

**Fixes**
- Long-range shadows: your own car no longer leaves blotchy shadows behind and below it.
- Aurora: no grid and ring patterns near the horizon.
- The replay camera detection also works with the ModLoader.

Full details in the [README](https://github.com/cheatoskar/TM-Vibrant-Shaders#readme) and the [changelog](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/main/CHANGELOG.md). Sound recordings: CC0 from freesound.org, see [sounds/CREDITS.md](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/main/sounds/CREDITS.md). The mod is now MIT licensed.
