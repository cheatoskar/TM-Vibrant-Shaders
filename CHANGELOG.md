# Changelog

### Unreleased

- *Game HDR*: no more huge white blobs over the sky on some day maps (B13-Obstacle, reported with Realistic). The game clears its picture before drawing a frame, but that clear never reached the float image *Game HDR* has the game draw into, and some skies blend over what is already there: the sky added up frame after frame (to 495 times white after 15 s on the map) and Bloom spread it. The float image now starts each frame as the game's own does, and as a safety net nothing in it goes above 4 times white (the game's real light stays below 3.2 in every capture so far). Other maps look exactly the same.
- New setting *Render scale* (`RenderScale`, *Performance* page, 0.5 – 1, off = 1 by default; TrackMania Nations/United): for graphics cards the shaders push to their limit. The game draws its 3D image at that fraction of the screen's resolution, every effect runs at that size, and AMD FSR 1 (edge-adaptive upscaling and its sharpening, *FSR sharpening* `FsrSharpness`) brings it back up to the screen before the HUD, which stays sharp. At 1440p the effects cost 3.1 ms instead of 6.2 at 0.67 (Golden Hour, GTX 1080 Ti), the game's own drawing less as well. Fine detail and text get somewhat softer. Photo mode, photos, F12 captures and the video export keep the full resolution, so does a camera into part of the screen (MediaTracker, the replay menu); the log says when something the game draws can't be scaled (that map stays at full size). Needs *Game HDR*.
- Neon trail: a frame hitch while driving fast (0.25 s at 900 km/h) no longer counts as a missed restart and wipes the trail.
- Neon trail with *Earlier runs kept*: a second restart signal right after the first no longer pushes the previous run out, and the last metre of each kept run is dimmed like the rest of it.
- Neon trail: the car's up is found again on every map, so a car on its roof when it was first seen no longer turns the trail's up over for the whole session.
- Photo mode's compare split: the preset's side shows your neon trail too, and comparing with one of your own presets no longer reads its file every frame.
- *Performance* page and auto quality: the lighting, neon light, long-shadow (light grid, caster maps) and lamp passes are counted to their effect again (with *Game HDR* on they were left out).
- Lamp glare and haze: if their render targets can't be created, the lamps keep their full light (they came out dimmer) and the log says so once.
- Real night: putting the weather back to midnight happens at most once a second and is counted in the log once per map.
- *The game's moon under the black hole*: a lightmap baked with the changed moon is never saved to the game's cache, also when real night is switched off during the map.
- Neon trail lies along the road on banked curves, walls, loops and over crests (TMNF and TrackMania 2): it was laid out level, so on a tilted road half of it, or one tyre line entirely, went into the road. It now lies across the car's own up, and smoothing can no longer pull it under the road over a crest. The log names the car's tilt once per map.
- Neon trail keeps going while a clip of the map plays (MediaTracker triggers on the track, e.g. a camera in a loop or a clip right after the start; TMNF). It stopped during such clips, and when one ended the car was 130 – 340 m further on: that was taken for a missed restart and the trail started over (the "60 m" lines in the log, each with "cinematic off"), so after a restart it only came 6 s late. A missed restart is now a jump between two frames the car was seen in, or the car back where the run began after a gap. The log says when the trail records nothing for half a second and why, when a map clip plays, and how long after a restart the line begins.
- Share presets as text: *Shaders* page, under *Your presets*, **Copy share code** puts the current look on the clipboard as one line (`TMVS1:...`, 150 to 700 characters) and **Paste share code** loads one from the clipboard, in TrackMania Nations/United and TrackMania 2 alike. The code holds the look's settings that differ from the defaults (not the installation's: auto quality, sound, neon trail ...). A pasted look is yours and unsaved (*Custom* on the closest built-in preset, *Save as preset* keeps it); nothing saved is replaced. Chat text around the code and line breaks in it are fine; unknown keys and unreadable values are ignored and counted, values out of range are clamped, empty, damaged or too long clipboard text is refused with a message (`Loaded 37 settings, 2 unknown ignored`). Tests: `tmvs_sharetest` and `mp_sharetest` (every built-in preset round-trips exactly, garbage is refused, the clipboard).
- New setting *Focus on* (`DofAutofocus`, *Cinematic* panel, *Middle* by default = as before): *Car* keeps your car sharp in replays and the photo mode while the camera moves around it. The depth of field's auto focus follows the car's position from the game (projected onto the screen, the depth of the car's own pixels there) instead of the middle of the screen; off screen it falls back to the middle, a replay camera cut jumps to the new distance at once. TrackMania Nations/United.
- Faster on big maps: *Caster shadows* looked at every draw of the game's camera every frame, up to 11 ms of CPU a frame on a large Stadium map (60 FPS where the graphics card had room for more). It now checks at most ~700 draws a frame (each one still every few frames), cheaper per draw, and none while long shadows and volumetric light are off: about 0.5 ms. The shadows look the same; a newly seen block casts a fraction of a second later.
- Fences and grids cast lattice shadows: new setting *Fence shadows* (`CasterAlphaTest`, *Game* panel, on). *Long shadows from the game's meshes* left out every mesh the game draws with an alpha test (fences, grids, mesh panels, lattice towers: their holes are cut out by the texture's alpha), so they cast no long shadow at all. They are now kept with what the game drew them with (its shaders, that draw's constants and textures, its alpha test) and drawn into the shadow maps the same way, holes included. Only the camera's matrix is swapped for the light's. Maps without such meshes look the same. The log's `casters (...)` lines get a second line: `alpha-tested: N meshes kept (N new, N refused), ...`. Test: `tmvs_castertest --verify` (a striped lattice must leave exactly its stripes in the map, and the state its draws change must be given back).
- No more hang when the shaders start (1.4 s in the account screen), after alt-tab (0.7 s) or when the compare split shows a preset (0.4 – 1.4 s): the shaders the mod compiled while running are compiled ahead when the game starts and kept. What remains on the first frame is the driver preparing the shaders once.
- Stadium maps: the water level measurement no longer stops the CPU until the graphics card catches up, four times a second.
- `tmvs.log` gets a *perf* line every 2000 frames in a race: FPS, the graphics card's time for the game's scene and for the shaders (their most expensive passes by name, the caster maps), the CPU time of the mod. See [docs/performance.md](docs/performance.md).
- Neon trail: a restart the game's hooks didn't report (the car more than 60 m from its last point in one frame without a respawn) now starts the trail over too; the log says when. New setting *Earlier runs kept* (`TrailKeepRuns`, 0 – 5, default 0 = as before): the trails of that many earlier runs stay after a restart, dimmer the older they are.
- Replay menu bars (`ShadeMenuBars`): a clear of the whole screen that the game limits to the camera's band with its scissor rect no longer counts as the game clearing the rows under the bars itself (the leftover strips came back). The log names every change of what happens to those rows, and which clear it was.
- **TrackMania 2:** photo mode's *Compare split* can put a preset on the other side too (*Photo mode* panel, *Other side*), as in TMNF: your look on one side of the line, the preset's on the other, on the same frame. The preset side has a renderer of its own (its own TAA, exposure, bounce light, focus and sky history) and each look draws only its side; it costs about as much again, only while a preset is shown. The light is the mod's in each look (real night, the moon, sun and sky), so the preset side shows its own. Photos of the split get both names at the line (`SplitPhotoLabels`, also with the game's image on the other side). With *The game's image* nothing changes. `mp_preview --compare <preset> [--split x] [--swap] [--labels "left|right"]` renders it offline.
- **TrackMania 2:** new option *Black hole moon* (`BlackHoleMoon` in `maniaplanet.ini`, Settings page, panel "ManiaPlanet", 0 – 2, off by default), the counterpart of `EngineMoon` *from the black hole*: on night maps under the black hole the game's moon (its blue glint on chrome and the ring sculptures, and its light) is taken out of the picture and the black hole lights the scene instead, warm, as bright as the moon was at 1. The moon's glint is rebuilt with the engine's own lighting formula and constants (its specular exponent and scale from the lighting pass's constants, each material's specular from the G-buffer) and drawn again from the black hole's direction. Shaders only: nothing in the game's memory changes. Shadows: the game's moon shadows where the black hole stands near the moon (Horizon), else only short contact shadows. Off: the picture is unchanged to the pixel. New debug view *The moon's glint*.
- Real night: when the weather's clock leaves the forced midnight during a map (reported: the sun came back in the end-of-race replay), it is logged and put back before the frame is drawn. The log also names changes of the game's light during a map and counts the game's sun flares per cinematic switch.

- Photo mode's compare split (TrackMania Nations/United): photos of the split carry both sides' names at the bottom, at the line (*Photo labels*, `SplitPhotoLabels`, on by default; the list on the screen stays out of photos). The aperture now blurs both sides alike: the game's image gets the same depth of field (focus, f-stop, blur size) as your side and nothing else of the mod, and a preset on the other side takes your blur size instead of its own. Hi-res photos (`Shift+F12`) keep the split too, the line straight through the stitched photo.
- Works next to the 100% TMX + Bingo mod: *Reload shaders* is now `Ctrl+F9` (`F9` opens that mod's menu; an old `Reload=F9` in `[Keys]` moves to `Ctrl+F9` once, noted in the log), all hotkeys react only while the game window is in front, modifiers must match exactly, and both mods share the mouse through the window property `TMMods.MouseOwner`: the menu under the pointer gets the clicks, the other one stays out, and a release is only swallowed if its press was.
- Hotkeys can be rebound: *Settings* page, *Hotkeys* panel (TMNF and TrackMania 2). *Change* takes the next key press (with `Shift`, `Ctrl`, `Alt`; `Esc` cancels), a key another action has is swapped, *Reset to defaults*. Stored as readable names in the `[Keys]` section of `settings.ini` (`Menu=F8`, `PhotoSplit=Shift+F6` ...). The defaults are the keys of before. The menu's hints show your keys.
- Photo mode's *Compare split* can put a preset on the other side instead of the game's image (*Photo mode* panel, *Other side*): your look on one side of the line, the chosen preset's on the other, on the same frame. To compare two presets, use one and pick the other there. The preset side runs the effects a second time with their own history (TAA, exposure, bounce light, focus), so it costs about as much again, only while it is shown (TrackMania Nations/United; TrackMania 2: its own entry above).
- Photo mode's compare split: a small list at the bottom, at the line, names both looks and switches the other side (the game's image or any preset) without the menu; only while the split and photo mode's line are on screen, never in photos. When the frame can't show the other preset's light it says so there: the game draws one frame, lit for your look (real night, the moon changed or off under the black hole, its sky and sun halo hidden under a night sky), and a day preset next to a night look gets no sun on that frame, so of its effects mostly the sky, the clouds and the grade showed. The preset side now takes its own sun position (`SunElevation`) instead of yours, draws the caster shadow maps for its sun when your look draws none, starts clean when another preset is picked, and its long-shadow map and neon trail start over on respawns and new maps like yours.
- Photo mode with the shaders off (`F7`): the focus follows a middle click and the auto focus again. It stayed where it was first measured when the game started with the shaders off.
- The volumetric light's march range is set before the march (it read the value left over from before: in game whatever the game's own shaders had put there).
- New setting *Normals from the game's shaders* (`GameGBuffer`, *Game* panel, off, experimental): the game's own material shaders also write the normal of what they draw (the blocks' bump maps, the car's real shape) into a second target, and where it belongs to the surface seen, AO, shadows, reflections and the relighting take it instead of the normal guessed from the depth buffer. The game's own image stays exactly the same. Needs *Game HDR*; switch it on, then restart the game. F12 also saves the normals (`capture_NNN.nrm16`).
- New setting *Night on the game's light* (`GameLight`, *Game* panel, off, experimental; G-buffer step 5): the game's own material shaders also write the light they lit each surface with (lightmap, baked lamps, ambient) into a third target, from the same draws as the normals. Under a night sky the darkening and blue tint then step back where the game's lamps light a surface (its light above 1), by that light instead of by the image's brightness: lamp-lit asphalt and floodlit stands keep the game's brightness and warmth, the rest gets the night as before. Needs *Game HDR* and *Game light sources*; switch it on, then restart the game. F12 also saves the light (`capture_NNN.lit16`, `tools/gbuffer.py --light`). Off (the default) the image is exactly as before.
- Video export as MP4: *Shoot video* in the replay editor now writes an MP4 with H.264 (or HEVC, `VideoCodec` 2) and the sound as AAC, next to the game's file in `Documents\TrackMania\Video`. The graphics card encodes (NVIDIA, AMD, Intel; else Windows' own encoder), no codec dialog, nothing to install, 4K and any frame rate. Bitrate automatic or set (`VideoBitrate`). `VideoCodec` 0 keeps the game's AVI with its codec list; if Windows can't encode, that is what you get.
- A preset you pick in the menu holds on every map, also on maps of another type: picking Horizon and then exporting a replay of a day map no longer switched to the day preset and left the map without its real night. *Maps* page: choosing a preset for a map type (or *Use the presets per map type again*) brings the automatic presets back. With *Picked preset applies to all maps of this type* on, nothing changes.
- Video export with the dialog's supersampling (*High quality*, 2x2 and up without its motion blur): every sample now gets the effects. The game renders each sample into a texture and sets its camera before it begins, so the mod saw no camera in samples 2 and up and left them unshaded: averaged, the effects showed at about 1/4 strength (2x2). They now take the camera the game computed, the same for every sample of a frame. The log at the end of an export tells the frames before, during and after the video (the game plays the clip again for its sound) and how many samples were shaded.
- Video export log: its settings are logged when it starts shooting (they were read while its dialog was still open: the dialog's defaults and an unstepped clock).

- Video export (replay editor, *Shoot video*): the effects run on the video's own clock instead of the real time, so exposure, focus, rain, lightning and clouds move in the video as they do in a replay (before, a frame that took 0.2 s to render moved them 0.2 s on: at 30 fps six times too fast, and unevenly). Full effect quality during the export (auto quality no longer drops it because the frames are slow) and photo mode's depth of field. With the dialog's *high quality* (supersampling) every one of the game's samples gets the effects (before, only the first of n x n did: the video showed a faint trace of them) and TAA steps aside for the game's own anti-aliasing. The first frame starts clean, without fading in from the editor's view. Setting *Best quality in the video export* (`VideoExportBest`, on).
- **TrackMania 2:** new setting *Soft sun shadows* (`SoftShadows` in `maniaplanet.ini`, Settings page, panel "ManiaPlanet", off by default): the sun's shadows sharp where a thing touches the ground and wider the farther they fall from it (PCSS on the engine's own shadow maps), the value is the sun's size in the sky (real sun 0.5 degrees). The mod's shader draws the engine's sun shadow pass in its place, for that one draw; nothing in the game's memory changes, but whether the anti-cheat minds is not known. Costs about 1 ms at 1440p. Only the engine's 40 known shadow variants are replaced, else the log says why and the game's shadows stay. F12 captures now include the shadow atlas and face normal (`mp_shadowtest`).
- TrackMania 2, neon trail: its start right behind the car stands still (the points near the car slid along the line every frame and the fade-in behind the car jumped by up to 120 px at 400 km/h), and it lies 10 cm over the road (the car's origin is on the road and dips under it as the car rides). It follows the car the camera follows: after a camera switch next to a ghost it no longer goes over to the ghost.
- New setting *The game's moon under the black hole* (`EngineMoon`, *Game* panel, off by default): on night maps under the black hole the game's own moon (its baked light and shadows, glints, the car's light) shines from the black hole instead of from the side, or is taken out. From the next map on.

### 1.4.0-preview (2026-10-12)

- Replay menu at the end of a race: under its half-transparent bars the replay's own shaded image (mirrored at the band's edges) instead of a stepped, blocky copy (`ShadeMenuBars`, on by default). The game draws the replay into a shrinking band and never draws under the bars again: what showed there were strips left over from every frame of the shrinking.
- Water: a clear mirror instead of blotches (`CleanWater`, on by default). Seen from above, the reflection on pools and rivers mirrored the tops of the ramps and bridges over them in patches that came and went, and took the dry track's reflection blur. Now a reflected ray only stops on a surface that faces it and that it really meets.
- **TrackMania 2 (ManiaPlanet 4):** the mod as a 64-bit `d3d11.dll` for ManiaPlanet's Direct3D 11 renderer, with the same menu, presets and settings ([maniaplanet/README.md](maniaplanet/README.md)). Skies with real night, AO, contact shadows, GI and neon light from the engine's normals, haze, light shafts and volumetric light through the engine's shadow maps, weather with sound, depth of field, motion blur and TAA with the engine's motion vectors, photo mode with the compare split, the neon trail from the car's position in the engine, Author's Shaders (read from the map, set in the editor). `F5` switches the effects (`F7` is ManiaPlanet's friends list). Installed with `Install.cmd` from `TM-Vibrant-Shaders-ManiaPlanet.zip`.
- Neon trail: it follows the car's real position from the game instead of looking for the car in the middle of the screen. No more flicker at speed, no line beside the car in curves or after a crash, and it keeps going while the car is off screen. Each point takes the line's direction from its neighbours (the pieces share their corners: no gaps at the joints, no fan of tyre lines around a car that stands or shakes after a crash); nothing is kept closer than 30 cm to the last point; it fades out right in front of the camera; with the game's car position 60 points a second or one a metre (the ring holds twice as many).
- Snowstorm: the wind is a recording ([Howling winter storm ambient sounds](https://freesound.org/people/DBlover/sounds/505999/) by DBlover, CC0) instead of synthesised noise.
- Photo mode: *Compare split* (`Shift+F6`): one side of a line shows the game's image without the mod's effects, the other the mod's, also in photos (not in hi-res ones). `Ctrl`+`Shift`+wheel moves the line, the menu swaps the sides.
- Photo mode: works even when the shaders are switched off (`F7` / `F5`): aperture (depth of field) and sharpness can be used on the vanilla game look with immediate autofocus on the car.

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
- Light sources: your car (or anything else in front) no longer counts as dark surroundings for the surface behind it: a white barrier next to the car got a glowing echo of the car, which drove along like a ghost.
- Horizon: more bloom (0.15).
- Bounce light (GI): your car's coloured bounce light no longer stays on the road behind it as a trail.
- Dry reflections: dirt roads only get a faint sheen (they mirrored the banners like a polished floor).
- Photo mode (`F6`, on by itself with the free camera, cam 7): no HUD (the `F8` menu stays), full quality (auto quality pauses), ambient occlusion, shadows, bounce light and reflections reach three times as far, and long shadows cover 960 m. `F12` saves `photo_NNN.png` without the HUD; `Shift+F12` saves a hi-res photo from 2 x 2 to 4 x 4 tiles (about 1.7x to 3.4x the screen, pause the replay first). Middle click sets the focus, `Ctrl`+wheel the depth of field, `Shift`+wheel zooms in (up to 8x). Options on the *Settings* page ([details](docs/settings.md#photo-mode)). Photo mode also turns off the game's adaptive view distance (launcher *ZClip*), so far blocks stay in the picture.
- The game's own FX post-processing (bloom and colour filter, launcher *Advanced*) is turned off while the mod shades (`DisableGameFX`, on by default, from the next map on): it glowed over the mod's image a second time (overexposed, smeared). The launcher setting itself is left alone; `F7` brings it back on the next map.
- Night skies (and the black hole) over day maps: the game's sun halo and lens flare fade out.
- Neon, Horizon and Aurora: the dry track has a sheen instead of a mirror (`Reflections` 0.8): every lamp mirrored in the road, brightened, looked overexposed.
- Animated skies: filaments stream round the black hole (the way its bright side says it moves), the gas giants turn in about two minutes and their bands drift against each other.
- Long shadows: no more bright boxes on the road around and ahead of the car under a low light (the black hole, sunsets). Everything close in the middle of the screen was kept out of the height map so the car would cast no shadow; the arch just driven under went too and counted as "nothing here". Only what stands up to a car's height above the road is kept out now.
- Long shadows: smooth edges instead of saw teeth under a low light (the height map is read filtered and dithered over a cell; far ground no longer leaves striped gaps in it), no lit dots inside shadows.
- United Forever: roads at the environment's sea level (Rally) no longer turn into a mirror. Only water the game draws a reflection for counts as sea.
- New setting *Real night under night skies* (`GameNight`, on by default; once, on A01, the game played a loud noise when the map loaded at midnight; not seen again, cause unknown): with a night sky (or the black hole) over a day or sunset map, the game lights the map at midnight itself (its moon, no baked sun to take out: no overexposed white). The map, its replays and validation stay as they are. Switched while a map runs, the map is relit at once.
- Night skies over sunset maps: the sunset clouds of the game's sky no longer show through the stars, and its sea no longer mirrors the sunset. New setting *Keep the game's clouds under night skies* (`GameSkyClouds`, off) keeps both (nice for photos).
- Photo mode: the depth of field blurs the lamps of night maps too (they stayed sharp).
- Long shadows without TAA: smooth edges instead of grain (the jitter was white noise, fixed per pixel; now an even pattern the blur removes).
- Night maps: new setting *Floodlit surfaces at night* (`NightSurfaces`, 0.8): floodlit stands, lit roads and white walls keep this part of the game's white instead of burning out; lamps and neon always keep it in full (1 = as before).
- New setting *Highlight roll-off* (`ToneKnee`, 0.5 = as before): where the tone curve starts to bend; higher keeps more of the game's bright mid-tones, brighter highlights.
- Night sky over a sunset map: the sunset ring TMUF Island paints around the map isn't drawn (it stood like an orange wall at the horizon and the sea mirrored it; `GameSkyClouds` keeps it), the far distance loses its sunset colour.
- Author's Shader: leaving the map (back to the menu) brings your own look back at once; before, the author's look stayed until the next map. The log says why a map's comments gave no look (no code in them, the switch off, a code cut off or broken); a code broken over lines by pasting is read.
- The last run's log stays as `tmvs.prev.log`. Weather sound: a buffer of runaway full-scale noise (never reached by rain and thunder) goes out silent and the mixer starts over, with a line in the log.
- *Picked preset applies to all maps of this type* is off by default: picking a preset on a map no longer changes the preset of every map of that kind unless you switch it on.
- United Forever Island: the sea reports no height; it is measured from the sea's material now, so it gets the exact water plane like the pools (waves, clean reflections).
- Water mirrors the sky you picked (planets, the black hole, stars) where the reflection finds nothing else, also far out on the sea; before it was a flat average colour.
- Stadium: dirt at the height of the water blocks no longer turns into water. Once a map's water was seen as the game's water material, only that is water; before, dark brown ground never is.
- Grass detail: the blades fade out over a longer distance, the mowing stripes have soft edges (hard lines ahead of the car).
- MediaTracker editor detection: replay cameras that leave a few rows out are shaded as before (only real panels count).
- The game's materials are read while it draws (`GameMaterials`, on): the TMUF sea is water by its material (and its height is measured from it). Not on Stadium, where the engine reports the water. Debug view *Materials* shows lamps, cars, glass, grass, water and light added on top.
- The map's mood (day, sunset, night: which preset it gets) comes from the map's own time of day when the engine reports it, not from the colour of its sun (Rally at noon has a warm sun and got the sunset preset).
- Reflections while driving as clean as in photo mode: twice the steps, half the noise (dashed, grainy reflections without TAA).
- MediaTracker editor: only its 3D view is shaded. The panels around it no longer get the preset's sky and colours, and the effects line up with the view.
- Photo mode shows the aperture (f-stop), zoom and focus at the top left (never in the photos).
- Colour looks from LUT files: put 3D `.cube` files (DaVinci Resolve, Lightroom, Photoshop, OBS, ...) into `Documents\TrackMania\TMVS\luts` and pick one on the *Customize* page (*Colour look*), with an amount. It sits on top of every preset ([details](docs/settings.md#colour-look-lut)).

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
