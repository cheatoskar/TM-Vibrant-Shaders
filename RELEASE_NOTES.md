## TM Vibrant Shaders 1.1

Real-time lighting, weather and skies for TrackMania Nations Forever and United Forever.

This release adds rain and thunderstorms, long-range shadows, neon light, reflections, volumetric clouds, the Ring World sky and a new two-level menu with auto quality.

![On the rings of a gas giant](https://raw.githubusercontent.com/cheatoskar/tmnf-vibrant-shaders/v1.1.0/docs/images/ring-world.jpg)

### Download and install

**`TM-Vibrant-Shaders.zip`** is all you need. Unzip it and run **`TM-Vibrant-Shaders-Setup.exe`**. You can install:
- **for the [TrackMania ModLoader](https://tomashu.dev/software/tmloader/)**, then tick the mod in the ModLoader, or
- **into the game folder without the ModLoader**: the setup adds a `d3d9.dll` next to `TmForever.exe`.

Updating from 1.0 beta: run the new setup, it replaces the old version. Your settings and presets in `Documents\TrackMania\TMVS` stay.

In the game: `F8` opens the menu, `F7` switches the shaders on and off. For the best result set *Antialiasing: off* and *Anisotropic filtering: 16x* in TrackMania's options.

### What's new

**Lighting**
- Long-range shadows: a height map of the track builds up while you drive, so a low sun throws long shadows, even from objects off screen.
- Neon light: the coloured borders and signs light up the road and walls around them, far down the track too.
- Stronger shadows and light shafts in the day presets; the sun disc no longer floods the screen.

**Weather and surfaces**
- Rain as real particles: drops that streak with the camera's motion, splashes on the track, ripples in puddles. A downpour goes up to rain 2.
- Wet roads and puddles with screen-space reflections of the scenery and the lights.
- Thunderstorms with lightning that lights up the clouds and the track.
- Grass patches, mowing stripes and blades close to the camera.
- Water surfaces for TMUF (experimental).

**Sky**
- Volumetric clouds.
- Ring World sky: Saturn, Jupiter, an ice giant or an exotic planet, seen from far away, from next to the rings or right above them.
- The black hole is a light source now: white-hot disk, sharp photon ring, a wide halo and warm light on the stadium.
- Brighter stars and Milky Way, shooting stars; space skies continue below the horizon.
- Night skies on day maps: the game's baked-in sun (lit sides, glare on roofs, roads and screens) is taken out.

**Camera**
- Temporal anti-aliasing.
- Motion blur and depth of field for replays and the video export.
- Replay camera blends: both cameras of a blend get the shaders.

**Menu and performance**
- New `F8` menu: *Simple* for the everyday settings, *Advanced* for everything. Settings that do nothing with your current sky are hidden.
- Auto quality: holds a target frame rate by turning effects down and back up. It never changes your settings.
- The *Performance* panel shows what each effect costs on your GPU, live.
- Presets per map mood can be picked in the simple menu (defaults: Vibrant for day, Golden Hour for sunset, Event Horizon for night).
- Your own tweaks survive a map change.

**Presets**
- New: Rainy Day, Thunderstorm, Replay Cinema.
- Event Horizon now uses the Ring World sky; Neon Night is brighter with more neon.
- Removed: Balanced. Ring World is now a sky you can use with any preset.

Full details in the [README](https://github.com/cheatoskar/tmnf-vibrant-shaders#readme) and the [changelog](https://github.com/cheatoskar/tmnf-vibrant-shaders/blob/main/CHANGELOG.md).
