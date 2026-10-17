# Changelog

## 1.4.0 (2026-10-18)

The first release for TrackMania 2 (ManiaPlanet 4) as well, with a new menu and the game's own light. It replaces 1.4.0-preview (2026-10-12); this list covers everything since 1.3.0.

**New**
- **TrackMania 2 (ManiaPlanet 4)** support (see the end of this list). One zip and one setup for both games.
- **New `F8` menu:** every preset as a card with a picture, a line about it and its GPU load. Point at one and the game shows it, click to use it, right click to use it on day, sunset or night maps. Pages *Shaders*, *Customize* (the everyday settings), *Studio* (every setting), *Maps*, *Performance*, *Settings* and *About*. Short labels with the details in tooltips, the mod's icon in the header, sized for the screen. Your own presets get a picture of the game when you save them.
- Menu: Studio's settings sorted into clearer groups (*Image* split into *Image quality* and *Game integration*, the latter last; *Haze and light shafts* out of *Sky*), the main sliders first in each group; *Customize* gets a *Colour* panel and the same order as Studio; every setting has a tooltip.
- **Share a look as text:** *Copy share code* / *Paste share code* on the *Shaders* page put a look on the clipboard as one line (`TMVS1:...`) for a chat. The same code works in TrackMania Nations/United and TrackMania 2.
- **Hotkeys can be changed** (*Settings* page, *Hotkeys*). *Reload shaders* is now `Ctrl+F9`, so the mod works next to the 100% TMX + Bingo mod: the menu under the pointer gets the mouse.
- **Photo mode** (`F6`, on by itself with the free camera): no HUD, full quality, effects reach three times as far. `F12` saves a photo without the HUD, `Shift+F12` a hi-res photo; middle click sets the focus, `Shift`+wheel zooms. Works with the effects switched off too.
- **Compare split** (`Shift+F6`): the game's image or any preset on the other side of a line, on the same frame and in photos, with both names at the line.
- **Light from the game itself:** *Game HDR* gives the mod the game's image with light brighter than white, and *Game light sources* makes lamps, floodlights, start lights and neon borders glow as the game draws them instead of guessing lamps from bright pixels: no more glowing grass, banners or car paint at night. *Neon strip glow*, *Floodlights*, *Lamp glare* and *Lamp haze glow* tune them.
- **Real night under night skies** (*Real night*): with a night sky or the black hole over a day or sunset map, the game lights the map at midnight itself (its moon, no baked sun). The map, its replays and validation stay as they are. *Game moonlight* lets the game's moon shine from the black hole, or turns it off.
- **Long mesh shadows:** long shadows from the game's own meshes, from the sun or the black hole, without popping; with *Fence shadows* fences and grids cast their lattice.
- **Render scale with AMD FSR 1** (*Image quality*, off by default): the game and the effects at a fraction of the screen's resolution, scaled up before the HUD, which stays sharp. At 0.67 the effects cost about half.
- **MP4 video export:** *Shoot video* in the replay editor writes an MP4 (H.264 or HEVC, sound as AAC), encoded by the graphics card, no codec dialog. The effects run on the video's own clock at full quality, also with the export's supersampling.
- **Colour looks from `.cube` files** (*Studio*, *Colour look (LUT)*) on top of every preset.
- **Focus on the car:** *Focus on* → *Car* keeps your car sharp in replays and photo mode.
- **Neon trail** from the car's real position in the game: no flicker at speed, no gaps, it lies along banked roads, walls and loops and keeps going through the map's clips. *Earlier runs kept* shows the last runs, dimmer.
- **Rain and snow stay outside:** nothing falls under roofs, bridges and in buildings, and the lens dries off under a wide roof.
- **Update check:** the menu says when a new release is out (nothing is downloaded; it can be switched off).
- The snowstorm's wind is a real recording.
- New settings *Haze distance*, *Highlight roll-off*, *Floodlit surfaces* and *Game sunset clouds*.
- *Game normals* (on): the game's own shaders hand the mod their normals (about 1 ms; off on a slow graphics card). *Night on game light* (their light) is not in the menu: `GameLight=1` in `settings.ini`.
- `tmvs.log` writes a performance line every 2000 frames ([performance](docs/performance.md)).
- **SMAA** edge smoothing (*Image quality*, *SMAA instead of FXAA*, on in every preset but *Performance*): smoother long edges and sharper textures than FXAA for about 0.2 ms more at 1440p. Older `settings.ini` and preset files keep FXAA.

**Changed defaults**
- On: *Game HDR*, *Game light sources*, *Clean highlights*, *Long mesh shadows*, *Fence shadows*, *Gap-free long shadows*, *Clean water*, *Replay menu shading*, *Real night*, *Game moonlight* from the black hole, *Turn off game FX* (the game's own bloom and colour filter, from the next map on), temporal AA (not in *Performance*), MP4 for the video export and *Best export quality*.
- *Lamp glare* 0.7 and *Lamp haze glow* 1. The night presets (Neon, Horizon, Aurora): neon borders glow at 3, the dry track has a sheen instead of a mirror. Neon's bounce light 1, Horizon's bloom 0.15, more neon light spill on Vibrant, Realistic, Competition and Performance. Snowstorm: no snow on the ground, thicker haze.
- *Picked preset applies to all maps of this type* is off: a preset you pick holds on every map until you choose presets per map type again on the *Maps* page.

**Fixed**
- Night maps (*Border glow*, Quality 2): the blue glow under far neon strips no longer pulses while the camera barely moves. The game draws a strip 1-2 pixels high with whole rows coming and going from frame to frame; its glow is now averaged over a few frames (only while the view moves slowly, with no trail behind the car).
- *TAA jitter*: the image no longer trembles with the camera standing still (edges, thin masts, light rows, stars, rain and snow); edges stay smooth.
- *Reflections*: the grass no longer mirrors the stadium like a lake in a low sun (A01 at sunset): the game's grass material is never glossy, whatever colour the light gives it.
- Day maps: the road keeps the game's concrete texture (stains, tyre marks, cracks). Taming the sun glare darkened the bright concrete more than its marks, so the road came out flat grey; it now goes by the brightness around each pixel, and the road is as bright as before. *Clean highlights* off gives the old look.
- Night maps: floodlit white comes out even instead of in frost-like patches; your car no longer counts as a lamp.
- *Game HDR*: no huge white blobs over the sky on some day maps (B13) after a while on the map.
- Water: a clear mirror instead of blotches of the ramps over it, the sky you picked in the reflection, the United Island sea gets waves; roads at Rally's sea level and Stadium dirt at water height are no longer water.
- Water: the stands, floodlight masts, gates and ramps across a pool are mirrored in it (*Far water reflections*). The reflection used to end 40 to 120 m out and showed the plain sky where they stood.
- Long shadows: no bright boxes around the car under a low light, smooth edges, no grain without TAA.
- TAA no longer leaves see-through copies behind the car; the car's bounce light no longer trails behind it.
- Neon light spill: the blue light on a wall next to a far-away neon strip no longer flickers as you drive towards it (TrackMania Nations/United, *Effect quality* high): the strips are now found pixel by pixel.
- *Neon strip glow*: the glow of far-away neon strips (and the blue light it blooms onto the wall between them) no longer pulses as you drive towards them (*Game light sources*, *Effect quality* high): a strip the game blends over two rows of pixels now counts by how much of it each row holds. About 0.25 ms more at 1440p.
- Snow: cars keep their paint with every camera, the car on the start podium no longer turns white.
- Replay menu: the shaded replay under its bars instead of leftover strips. In the replay viewer the menu shows properly and its keys react at once.
- No hang when the effects start, after alt-tab or when the compare split shows a preset; after alt-tab the effects are back sooner.
- Big maps: *Long mesh shadows* cost about 0.5 ms of CPU a frame instead of up to 11 ms.
- Auto quality measures what each effect costs on your GPU and turns off just enough of them, keeps the light shafts, the neon light and the long shadows (cut, a stand's shadow on the road stayed light in play but was dark in photo mode), decides nothing in the first 20 s after a map start, a loading screen, alt-tab or photo mode (a map's first seconds no longer turn effects off for the whole session), and no longer switches effects off and on in a loop.
- Real night follows the look during a map (from a night to a day look and back), and the sun no longer comes back in end-of-race replays.
- Video export with supersampling: every sample gets the effects.
- Photo mode: depth of field blurs the lamps of night maps too.
- Author's Shader: leaving the map brings your own look back at once.
- Text fields in the menu take typing.
- Saving the settings (a moment after a change in the menu, and when a map switches the look) no longer stalls the game for 0.1 to 1.3 s: `settings.ini` is written in one go instead of once per setting, and never left half written.
- F12 frame capture: the game no longer freezes for half a second (on a busy disk for seconds) while the files are written; they are written in the background.

**TrackMania 2 (ManiaPlanet 4)**
- The mod as a 64-bit `d3d11.dll` for ManiaPlanet's Direct3D 11 renderer, with the same `F8` menu, presets and settings ([maniaplanet/README.md](maniaplanet/README.md)). Tested in the game on Stadium. `F5` switches the effects (`F7` is ManiaPlanet's friends list).
- Skies with real night, ambient occlusion, contact shadows, bounce light and neon light from the engine's normals, haze, light shafts and volumetric light through the engine's shadow maps, weather with sound, depth of field, motion blur and TAA with the engine's motion vectors, photo mode with the compare split (also against a preset), the neon trail from the car's position in the engine, Author's Shaders.
- *Soft sun shadows* (on, 1°): sharp where a thing touches the ground, softer the farther they fall. The mod draws the engine's sun shadow pass with its own shader; nothing in the game's memory changes, but whether ManiaPlanet's anti-cheat minds is not known. About 1 ms at 1440p; 0 turns it off.
- *Game light sources* (on, `EngineLightDraws` in `maniaplanet.ini`): lamps, light strips, screens, halos and the car's lights glow as the engine draws them; grass no longer glows.
- *Game moonlight* does what it says in TrackMania 2 (its *Game's* and *Black hole* were the same there): *Game's* keeps the moon, *Black hole* lets the black hole light night maps under it instead of the game's moon (in the mod's shaders only; its brightness, *Black hole light*, in the *ManiaPlanet* panel), *Off* takes the moon's light out. *Black hole* by default, as in TrackMania Nations/United.
- Night maps are recognised by the engine's time of day. Auto quality works as in TrackMania Nations/United and can also turn off the volumetric light.
- Neon light spill is smoothed over a few frames, as in TMNF: the coloured light a neon strip throws on a wall next to it no longer flickers when the car or the camera moves. A still image comes out as before.
- Neon trail: a frame hitch while driving fast no longer breaks the line as if the car had respawned.
- Photo mode's compare split with a preset: the preset's side shows your neon trail too, and comparing with one of your own presets no longer reads its file every frame.
- *Focus on* (`DofAutofocus`, *Cinematic* panel): *Car* keeps your car sharp while the camera moves around it, as in TMNF (the depth of field runs in photo mode in TrackMania 2).
- The preset gallery shows TrackMania 2 pictures (the Stadium start by day, rendered from a capture) instead of the TMNF ones.
- Tooltips say what *Real night*, *Game moonlight* and *Cinematic in replays only* do in TrackMania 2; *Turn off game FX*, which changed nothing there, is no longer shown.
- *Game light sources*: the engine's light draws are drawn again into the mod's light buffer with a depth test that lets their own depth through and writes nothing, so a light drawn with a strict depth test keeps its glow and stencil counts stay the game's (ManiaPlanet's own light draws in capture 034 were not affected).
- *Set as Author's Shader* in the map editor also checks that the editor is the map editor (its class: Free, Puzzle or Simple) before it writes the map's comments; with the MediaTracker or another editor open nothing is written.
- Photo mode's compare split: the list at the line (bottom of the screen) switches the other side between the game's image and any preset without the menu, as in TrackMania Nations/United; never in the photos.
- Neon trail: *Earlier runs kept* (`TrailKeepRuns`) works in TrackMania 2 too: after a restart (the car back at the run's start) the last runs stay, dimmer, as in TrackMania Nations/United; a new map clears them.
- Neon light spill: the light on a wall next to a far-away neon strip no longer pulses as you drive towards it (*Effect quality* high): the strips are now found pixel by pixel, as in TrackMania Nations/United. About 0.15 ms more at 1440p.
- `tmvs-mp.log` gets a *perf* line every 2000 frames in a race: FPS, the shaders' GPU time and their most expensive passes.
- *Best export quality* (on) for the replay editor's *Shoot video* (ManiaPlanet writes the WebM itself): while the game reads its frames back for the video, slower than real time (a recorder such as OBS's memory capture is left alone), the effects run on the video's clock (`VideoFps` in `maniaplanet.ini`, 30 by default: set it to the video's frame rate) at full quality, with depth of field and motion blur as in a replay. Seen from the graphics side only; not yet tested in the game.

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
