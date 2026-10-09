# Changelog

## 1.3.1 (not released yet)

- New `F8` menu. **Shaders:** every preset as a card with a picture, a line about it and its GPU load; rest the pointer on one and the game shows that look, click to use it, right click for day, sunset or night maps. Filters for day, night, weather and light presets. Your own presets get a picture of the game when you save them, and a line about them if you like (right click: edit description, update picture, delete). A preset is its .ini and .jpg in the presets folder (*Open the presets folder*): share both; files put there, and pictures replaced there, show up while the game runs. Pages for *Customize* (the main settings), *Studio* (every setting), *Maps* (preset per map mood, Author's Shader) and *Performance*. Segoe UI instead of the pixel font, sized for the screen. The menu opens big, leaving the right part of the game in view for the live preview (one button puts it over the whole screen); on a wide menu the settings stand in panels side by side. Deleting a preset and resetting your changes ask first.
- Settings page: *Preset per map type*, *Picked preset applies to all maps of this type* (off: picking a preset no longer changes which preset that kind of map gets), Author's Shaders, live preview, menu size, update check, the game options and the folders. Every on/off setting is a switch now.
- Studio shows which preset you are editing and whether it is saved; *Save changes* writes your changes into your own preset. Its groups start closed (a dot marks groups with changes), *Expand all* opens them.
- Text fields in the menu take typing (the game never turned key presses into characters: preset names could not be entered).
- Update check: when the game starts the mod asks GitHub for the newest release; a new version shows in the menu with its notes and a link to its release page (*About* page, can be switched off). Nothing is downloaded by itself.
- Rain and snow stay outside (`WeatherShelter`, on by default): every drop and flake is traced back along its way, against the wind. Under roofs and bridges and behind walls nothing falls, around them it keeps falling; under a high roof a side wind still blows rain in. Under a wide roof the lens dries off (a pipe or a narrow bridge above doesn't). It knows the roofs the camera has seen, from above or from below.
- Snowstorm: no snow on the ground by default (it lay across markings, kerbs and edges; *Snow on the ground* still turns it on, a snow texture pack looks better on the road). Thicker haze that closes in nearer (`FogDensity` 6, new `HazeDistance` 0.5).
- Auto quality reworked: it measures every effect on your GPU and, below the target FPS, turns off just enough of them in one step (the ones that cost the most for the least look first). Before, it went down in fixed levels and gave up for two minutes when one level won less than 4 % - on many GPUs it never did anything. The light shafts, FXAA and sharpening can go now too; the menu shows what is off.
- The log says why a screen-sized camera isn't shaded (size or depth buffer), and when a capture's depth is empty.
- New setting *Haze distance* (`HazeDistance`): below 1 the haze closes in nearer the camera.
- Snow: cars keep their paint with every camera (replays, outside and TV cameras, ghosts), not only behind the car.
- Snow: the car on the start podium no longer turns white, and driving down from it leaves no bare rectangle in the snow.
- TAA: the car no longer leaves see-through copies behind it while driving (the road just behind the car passed the depth check), nor its roof against the sky.
- Light sources: your car (or anything else in front) no longer counts as dark surroundings for the surface behind it: a white barrier next to the car got a glowing echo of the car, which drove along like a ghost. Large lit faces (signs, light blocks) get less of the boost. *Light source intensity* is higher by default now (5, night presets 5.5-6).
- Horizon: more bloom (0.15).
- Bounce light (GI): your car's coloured bounce light no longer stays on the road behind it as a trail.
- Dry reflections: dirt roads only get a faint sheen (they mirrored the banners like a polished floor).

**Known issues**
- TrackMania's own FX post-processing (launcher, *Advanced*) can interfere with the shaders. If something looks overexposed, smeared or doubled, turn it off.

## 1.3.0

**Snow**
- Falling snow (`Snow`, 2 = blizzard): flakes of different sizes that streak past the camera and drift with the wind, blurred close to the lens. Flakes also land on the lens (with *Drops and flakes on the lens*). A snow veil far away, blizzard haze.
- Snow on the ground (`SnowCover`): on grass and everything that faces up. Below full cover the wind heaps it into piles and drifts with bare ground between them (0.5 is about half the ground). It has relief, sparkles in the sun, keeps the game's shadows, stays thin on painted surfaces, gathers as streaky crusts on walls facing the wind, and stays off the water.
- Your car keeps its paint: it is found in the depth buffer, and the road around it keeps its snow.
- Powder snow thrown up behind the car (`Spray`).
- Snow storm wind: a synthesised wind sound that grows with the snowfall and the wind.
- New preset **Snowstorm**.

**Author's Shader**
- A look stored in a map's comments. Map authors set it from the menu (*Advanced → Author's Shader → Set as Author's Shader*) into the map open in the editor and save the map. Everyone with the mod gets that look on that map; on the next map your own look is back, and `settings.ini` is never touched.
- Packed into a short code (about 2 bytes per changed setting), so it fits next to the author's own text. *Copy code* puts it on the clipboard; a readable `[TMVS] Preset=Snowstorm Snow=1.5` line typed by hand works too.
- *Load Author's Shaders* switches it off. The comments are read from the map in memory, so campaign and online maps work too.

**Sky**
- The black hole lights the space skies: light shafts, shadows, lens flare and glow come from it instead of the sun.
- Black hole direction and height (both space skies), the planet's position in the black hole sky; size 0 turns the black hole or the planet off.
- Ring World: seamless planets, sharper rings, planet size in every view, a *None* planet.
- Clouds move with the wind; aurora movement speed (`AuroraSpeed`).

**Presets**
- Presets tuned. *Rainy Day* is now **Rainy** (old settings keep working). **Horizon** uses the black hole sky.
- Switching presets glides over about a second instead of jumping.

**Menu**
- Search box in the advanced view: finds any setting across all sections.
- A reset arrow next to every setting you changed: back to the preset's value.
- Coloured section headers.

**Light and surfaces**
- Long-range shadows: animated, even noise instead of a fixed pattern (it blurred into blotches that slid with the camera), and more margin under a low sun or moon.
- Reflection blur (`ReflectionBlur`): the dry track becomes a glossy sheen instead of a mirror.
- Lens flare only from distant lights. Neon light spill without blocky edges. Grass detail with tufts (off by default). Haze up to 6.
- TAA is off by default.

**Fixes**
- Water: no more stripes on pools and no "box" around the car where the water looked different from further away. Positions rebuilt from the depth were off by half a pixel (Direct3D 9 draws pixel centres there), which tilted every view ray: flat ground sank by up to 35 cm at 400 m, and far water fell out of the water mask. The water level is 8.00 m (it was measured as 7.94 with the tilted rays), the depth clean-up no longer lifts flat ground near the camera, reflections no longer hit the water itself, and waves fade their small ripples with distance instead of flickering into stripes.
- Water is at full strength (`WaterSurfaces` 1) in every preset.
- Shadows no longer pop up for a frame near walls and buildings: every ~40 m the long-shadow map moves with the car, and for one frame the shadows read it at the old place.
- Snowstorm: the fog no longer comes and goes between walls and inside buildings. Its colour comes from the sky on screen, which jumped when only a few sky pixels showed; it is now eased over time.
- Snow: no more black rectangles in the snow around the car (next to pools, on grass edges, in banked turns or with replay cameras).
- The mod no longer switches itself off on many GPUs (for example NVIDIA): two shaders had grown past the 4096 instructions those drivers accept ("shader creation failed" in the log). The build now checks every shader against that limit, and the log names the failing shader and the driver's limit.
- Other mods that create their own Direct3D device (for example Twinkie) no longer take the mod's hooks away from the game.
- Frame captures (`F12`) on maps with water saved an empty depth buffer. Captures never overwrite earlier ones.

**Known issues**
- Snow is work in progress: the snow on the ground doesn't follow the game's textures and surfaces well yet. On the start podium the car can turn white, and driving down from it can leave a rectangle in the snow.

## 1.2.1

- Storm: the dry track also reflects (1.3), so the wet road and the stadium mirror more clearly.
- Simple menu: the Sky section and the Sky choice inside it no longer share an ID (ImGui showed a conflict warning).
- Log: when the game fails to create a texture or buffer, `tmvs.log` says so, with the error (process address space or video memory) and the memory left. The free memory is also logged at every map load and device reset. TmForever.exe is 32-bit and has 2 GB of address space; when that runs out, the game draws its surfaces untextured (white).

## 1.2.0

**Game support**
- Setup: every TrackMania found (Nations and United Forever) gets its own install button, next to the ModLoader and "another game folder".
- TrackMania United Forever: its TmForever.exe is a different build than Nations Forever's, and the engine hooks only knew Nations. Both builds now have their own address table and the mod picks the one that matches. In United, the map's environment is read at load: the Stadium water heights only apply on Stadium maps, the island, bay and coast seas come from the game.
- The replay camera hook (UpdateCams) also works when the game is loaded at another base address (it never did with the ModLoader).

**Presets**
- New **Realistic** (replaces Cinematic, which looked almost like Vibrant): TrackMania's own colours and sky, no extra saturation or colour grading; only the light is new (shadows, ambient occlusion, bounce light, a little haze in the colour of the game's sun).
- Shorter names: Neon Night is now **Neon**, Event Horizon **Horizon**, Thunderstorm **Storm**. Saved settings with the old names keep working (Cinematic becomes Realistic).
- Night presets (Neon, Horizon, Aurora) show every star (3). Horizon turns the sky so Saturn and the black hole (up close, size 6) are in view from the start screen.

**Menu**
- The simple menu's sections (Sky, Look, Weather, Performance) and the preset-per-mood choice start collapsed; what you open stays open.

**Lighting**
- Volumetric light: sun shafts with real shadows in the haze, traced through the long-range height map, also with the sun off screen (~0.3 ms).
- Bounce light: one-bounce screen-space global illumination at quarter resolution (~0.25 ms).

**Weather and water**
- Lightning bolts in the sky with branches, near where you look; thunder follows after a delay that matches the distance.
- Rain and thunder sound from real CC0 recordings (freesound.org): a seamless rain loop that follows the rain amount, ten thunders in three distance groups (never the same one twice in a row, pitch, loudness and dullness vary per strike, panned to the side of the flash). Volume up to 2, default 0.45. The log shows the output device and level.
- Rain on every surface: streams running down steep wet surfaces, splash rings on the car and barrier tops.
- Rain drops on the lens: clear drops that bend the image, small ones that dry off, big ones that run down. On/off in the simple menu.
- Spray behind the car on wet roads: its own setting, off by default (0.25 in Storm). It only flies while the tyres touch the road (checked in the depth buffer beside the rear tyres, so slopes keep it and jumps stop it) and varies in gusts per tyre.
- Water: found by height instead of colour (water blocks always sit at 7.94 m; the sea level comes from the map load), so pools and rivers (and the TMUF sea) get waves, refraction and reflections and nothing else does. On by default.

**Neon trail**
- A glowing light trail behind the car (advanced menu, off by default): two lines from the rear tyres or one from the middle, colour, width, whole run or fading. Recorded on the GPU (the car is found in the depth buffer, 30 points a second, 4.5 minutes); a restart clears it, a respawn starts a new line. Not recorded in replays.

**Camera**
- Jittered TAA: the game's projection gets a sub-pixel offset each frame, TAA supersamples the edges.
- Motion blur and depth of field only in replays, intros and the video export (detected from the game), not while driving.

**Performance**
- Sky shader split per sky mode; the aurora's curtains render at half resolution with FSR 1 EASU upscaling (aurora sky about 3x cheaper).

**Fixes**
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
- Previewer: `--time`, `--water`, `--drive` (moving camera, for the trail), `--sound out.wav` (the rain and thunder sound as a file), `--play seconds` (plays it live), `--decode in.mp3 out.wav`.

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
