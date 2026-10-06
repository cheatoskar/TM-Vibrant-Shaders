# Changelog

## Unreleased (1.2)

**Presets**
- Shorter names: Neon Night is now **Neon**, Event Horizon **Horizon**, Thunderstorm **Storm**. Saved settings with the old names keep working.

**Lighting**
- Volumetric light: sun shafts with real shadows in the haze, traced through the long-range height map, also with the sun off screen (~0.3 ms).
- Bounce light: one-bounce screen-space global illumination at quarter resolution (~0.25 ms).

**Weather and water**
- Lightning bolts in the sky with branches, near where you look; thunder follows after a delay that matches the distance.
- Rain and thunder sound, synthesised on the fly, with a volume setting (up to 2). The log shows the output device and level.
- Rain on every surface: streams running down steep wet surfaces, splash rings on the car and barrier tops.
- Rain drops on the lens: clear drops that bend the image, small ones that dry off, big ones that run down. On/off in the simple menu.
- Spray behind the car on wet roads.
- Water: found by height instead of colour (water blocks always sit at 7.94 m; the sea level comes from the map load), so pools and rivers (and the TMUF sea) get waves, refraction and reflections and nothing else does. On by default.

**Neon trail**
- A glowing light trail behind the car (advanced menu, off by default): two lines from the rear tyres or one from the middle, colour, width, whole run or fading. Recorded on the GPU (the car is found in the depth buffer, 30 points a second, 4.5 minutes); a restart clears it, a respawn starts a new line. Not recorded in replays.

**Camera**
- Jittered TAA: the game's projection gets a sub-pixel offset each frame, TAA supersamples the edges.
- Motion blur and depth of field only in replays, intros and the video export (detected from the game), not while driving.

**Performance**
- Sky shader split per sky mode; the aurora's curtains render at half resolution with FSR 1 EASU upscaling (aurora sky about 3x cheaper).

**Fixes**
- Spray no longer appears when climbing a quarter pipe, flying or falling.
- Aurora: no moiré (grid and ring patterns) near the horizon; the curtains soften where they get smaller than a pixel.
- Advanced menu: Lighting and Sky had a second, duplicate section.
- TAA: 3x3 neighbourhood and a looser history clamp while the image stands still. The sub-pixel jitter is off by default (fine grates shimmered with it).
- Bounce light (GI) has its own history: no more blotches on the road, also with TAA off.
- Long-range shadows: the player's car no longer goes into the height map (it left blotchy shadows behind and below the car). The map also starts over on a restart or respawn: the respawn camera saw the car from outside the excluded area.
- Reflections on wet and polished tracks are softly blurred instead of speckled.

**Reflections**
- *Track reflections (dry)* goes up to 2: above 1 the dry track is polished, checkpoints and objects mirror clearly. Also in the simple menu.
- Night presets (Neon, Horizon, Aurora) mirror at 1.3; auto quality steps back to 1 first when the frame rate drops.

**Tools**
- Previewer: `--time`, `--water`, `--drive` (moving camera, for the trail), `--sound out.wav` (the rain and thunder sound as a file), `--play seconds` (plays it live).

**License**
- MIT.

## 1.1.0

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

## 1.0.0-beta.1

First public beta. This is a complete rewrite of the earlier colour-filter prototype as a real lighting pipeline.

- Engine-level injection before the HUD, readable scene depth (INTZ), camera and sun taken from the game
- Ambient occlusion, screen-space sun shadows, sun/sky relighting, light shafts, height fog
- HDR reconstruction of light sources, bloom, auto exposure, filmic tone mapping, FXAA, CAS sharpening
- Custom skies: clear sky with clouds, starry night, ray-traced black hole with ringed planet, aurora
- 10 presets, plus your own named presets
- Automatic preset per map mood (day / sunset / night)
- Every setting is saved automatically
- Effects also apply in replays and video export
- Setup.exe: install for the TrackMania ModLoader or straight into the game folder (d3d9.dll, no ModLoader needed), update, uninstall
