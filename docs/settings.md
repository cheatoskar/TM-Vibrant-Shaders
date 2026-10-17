# Settings

[Back to the README](../README.md)

Open the menu with `F8`. Everything you change applies at once and is saved automatically.

## The menu

The menu opens over most of the screen's height and the left part of its width: the game stays in view on the right, for the live preview. The button next to the close cross puts it over the whole screen and back. On a wide menu the settings stand in panels side by side.

**Top:** the shaders on or off (`F7`), the frame rate, and *Update: x.y.z* when a new version is out. On the left, the pages:

- **Shaders:** every preset as a card with its picture, a line about it and its GPU load (one to three dots, measured). Rest the pointer on a card and the game shows that look (*Live preview*, switch it off at the top right); click to use it. Right click: use it on day, sunset or night maps. The filters show only day, night, weather or light presets. *Your presets* follow, with the *+* card to save the current look under a name and, if you like, a line about it: the game behind the menu becomes its picture (right click: *Edit description*, *Update picture*, *Delete*). Their GPU load is estimated from the effects they use.

  A preset is `<name>.ini` (its values, and its line under `[Info] Description=`) and `<name>.jpg` (its picture, 16:9 JPEG) in `Documents\TrackMania\TMVS\presets`. *Open the presets folder* opens it. To share a preset, pass on both files; presets put into the folder, and pictures replaced there, show up while the game runs. Without a picture the card shows the preset's sky and sun colours.
- **Customize:** the everyday settings below.
- **Studio:** every setting, see below.
- **Maps:** the preset per map mood (day, sunset, night) and the [Author's Shader](#authors-shader).
- **Performance:** auto quality, target FPS, effect quality, and what each effect costs on your GPU.
- **Settings:** options that are not part of a preset: the *Hotkeys* ([below](#hotkeys)), *Preset per map type*, *Picked preset applies to all maps of this type* (off by default; on: picking a preset on a night map makes it the preset for all night maps, the same for day and sunset; off: only the Maps page sets them), *Author's Shaders*, live preview, full-screen menu, update check, motion blur only in replays, TAA, the game's MSAA, effects in replays, and the folders.
- **About:** the version, updates (with the new version's notes and a link to its release page), links to the project page and the issue tracker, the folder with your settings and `tmvs.log`, and the credits.

**Updates.** When the game starts, the mod asks GitHub once for the newest release (`api.github.com`, the public release list; nothing about you is sent). It never downloads or installs anything: a new version shows in the menu, you get it from the release page. Switch it off under *About → Check on startup* or *Settings* (`CheckForUpdates=0` in `settings.ini`).

**Customize.** The everyday settings, in three panels.

| Section | Settings |
|---|---|
| Sky | Sky, sky rotation, stars, aurora movement, planet, planet size, view, planet direction and height, black hole size, direction and height (only what the chosen sky uses) |
| Look | Shadows, light shafts, glow, neon light, brightness, colour, motion blur |
| Weather | Rain, snow, snow on the ground, wet roads, clouds, lightning, drops on the lens, spray, weather sound, water, reflections, reflection blur |

**Studio.** Shows the preset you are editing and whether your changes are saved. Load a preset by clicking it on the *Shaders* page, change it here, then *Save changes* (your own presets) or *Save as new preset*. Every setting below, in a panel per group as in this page (closed at first: click a title to open it, *Expand all* opens every group; a dot marks groups with changes), plus *Save as preset*, *Reset preset* (asks first), the debug views and *Reload shaders*. Settings that do nothing with your current choices are hidden; for example, the planet settings only appear with the Ring World sky. The search box at the top finds any setting across all sections.

A small arrow appears next to every value you changed: click it to set that value back to the preset's. Switching presets glides over about a second.

## Presets per map mood

The mod reads the colour of the game's sun when a map loads and sorts the map into day, sunset (and sunrise) or night. With *Preset per map type* on, it switches to the preset you chose for that kind of map. The defaults are **Vibrant** for day, **Golden Hour** for sunset and **Horizon** for night. Change them on the *Maps* page, With *Picked preset applies to all maps of this type* on (off by default), a preset picked on a map also becomes the preset for that kind of map. With it off, a preset you pick in the menu holds on every map, also on maps of another type (and real night follows it), until you choose a preset for a map type on the *Maps* page or press *Use the presets per map type again* there.

While your preset is *Custom* (you changed a value and didn't save it), a map change never replaces your tweaks. A map with an [Author's Shader](#authors-shader) wins over the mood preset.

## Your own presets

1. Start from the preset closest to what you want.
2. Switch to *Advanced* and change values. `F7` shows the original image for comparison.
3. Type a name under the preset list and press **Save as preset**.

Presets are text files in `Documents\TrackMania\TMVS\presets\`, one `.ini` per preset, with one line per setting (`Key=Value`, the keys are listed below). Send a file to a friend and they drop it into the same folder; it appears in their list on the next game start (or [as a share code](#share-a-preset-as-text), one line of text). Settings that belong to your installation rather than to a look are not part of a preset and stay when you switch: auto quality and target FPS, the sound volume, the neon trail, *Only in replays and video export*, TAA jitter and the MSAA and depth options.

## Share a preset as text

For a chat (Discord and the like) instead of two files: on the *Shaders* page, under *Your presets*, **Copy share code** puts the current look on the clipboard as one line, `TMVS1:` and some letters, usually 150 to 700 characters (a Discord message holds 2000). Someone else presses **Paste share code** and has your look.

- The code holds every setting of the look that differs from the defaults, not the settings of the installation (the same ones a preset leaves out, see above). It reads the same in TrackMania Nations/United and in TrackMania 2: the preset it was made from can differ between the games, the look that arrives does not.
- **Paste share code** reads the clipboard (a chat message with the code in it works, so does a code broken over lines) and makes the look yours, unsaved: the preset becomes *Custom* on top of the built-in preset it is closest to, and *Save as preset* keeps it. Nothing saved is replaced, built-in presets never. A code that is exactly a built-in preset selects that preset.
- The line under the buttons says what happened: `Loaded 37 settings, 2 unknown ignored`. Unknown keys (a newer version's, or ones this game doesn't have) and unreadable values are ignored and counted, values outside a setting's range are moved into it (*out of range*), and a code that is no code (empty clipboard, no text, damaged, a newer code version) says so and changes nothing.
- A code is built against the Settings defaults of its version (`TMVS1`). If a default ever changes, the version goes up and old codes keep reading as before.

## Author's Shader

A look stored in a map's comments: everyone with the mod sees the map that way, and on the next map their own look is back. Their `settings.ini` is never changed.

- **Author's Shaders** (`UseMapLooks` in `settings.ini`, *Settings* page): on by default. Switch it off to always keep your own look.
- **Set as Author's Shader**: stores the current look in the map open in the editor. Save the map to keep it. Whatever else is in the comments stays; an older code is replaced.
- **Copy code**: puts the code (`[TMVS:...]`) on the clipboard, to paste into the comments yourself.

A readable line typed into the comments works too: `[TMVS] Preset=Snowstorm Snow=1.5 SnowCover=0.8`, with the keys below. Preset names with spaces take an underscore (`Golden_Hour`). The settings that aren't part of a preset (see above) are never taken from a map.

## Hotkeys

The *Hotkeys* panel on the *Settings* page lists every key the mod uses, with *Change* and *Reset to defaults*. *Change* waits for the next key press (with its `Shift`, `Ctrl` or `Alt`); `Esc` cancels. The mod keeps that key press from the game's window (a key the game reads straight from the keyboard may still reach it). If the key already belongs to another action of the mod, the two swap keys and the panel says so.

| Action | `settings.ini` key | Default |
|---|---|---|
| Menu | `Menu` | `F8` |
| Effects on / off | `Effects` | `F7` (ManiaPlanet: `F5`, because `F7` is its friends list) |
| Photo mode | `Photo` | `F6` |
| Photo mode: compare split | `PhotoSplit` | `Shift+F6` |
| Frame capture / photo (`Shift`: hi-res photo) | `Capture` | `F12` |
| Reload shaders (TMNF only) | `Reload` | `Ctrl+F9` (was `F9`, which the 100% TMX mod's menu uses; an old `Reload=F9` is moved once) |
| Trace (TMNF only) | `Trace` | `F11` |

They are stored in the `[Keys]` section of `settings.ini` as readable names, for example `Menu=F8` or `PhotoSplit=Ctrl+Shift+F6` (`None` turns an action off; modifiers `Shift`, `Ctrl`, `Alt`; keys `A`-`Z`, `0`-`9`, `F1`-`F24`, `Num0`-`Num9`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `Space`, `Enter`, `Tab`, `Up`, `Down`, `Left`, `Right`, `Pause`, `ScrollLock`, `Minus`, `Equals`, `Comma`, `Period`, `Slash`, `Backslash`, `Semicolon`, `Quote`, `Backquote`, `LBracket`, `RBracket`). A name the mod can't read, or two actions on one key, falls back to the default. Letters, digits and the game's own keys (arrows, `Space`, `Enter`, `Tab`) need `Ctrl` or `Alt`, else they would stop working in the game; `F` keys, the number pad and `Insert` / `Delete` / `Home` / `End` / `PageUp` / `PageDown` work alone. While a text field in the menu has the keyboard, only `F` key hotkeys react. Hotkeys only react while the game's window is in front (an `F` key pressed in another program does nothing), and the modifiers must match exactly (`Ctrl+F9` is not `F9`; the one exception is `Shift` on the frame capture). Together with the 100% TMX mod, whichever menu the pointer is over gets the mouse. The texts in this document name the default keys. Mouse controls (middle click, wheel) are fixed.

## Photo mode

For screenshots of replays. It comes on by itself with the free camera (cam 7 in a replay or in the editor), and `F6` turns it on or off anywhere. While it is on:

- The HUD is gone. The `F8` menu still works.
- Full quality: auto quality pauses, and ambient occlusion, sun shadows, bounce light and reflections reach three times as far. Long shadows cover 960 m instead of 320 m, so far views get them too. Nothing assumes a car in the middle of the screen.
- `F12` saves `photo_NNN.png` in `Documents\TrackMania\TMVS\`: the shaded frame without the HUD, at screen size. (Outside photo mode `F12` saves a capture as before.)
- `Shift+F12` saves a larger photo, put together from tiles (*Hi-res photo*: 2 x 2 to 4 x 4 tiles, about 1.7x to 3.4x the screen size). **Pause the replay** first, because every tile is a separate frame. Exposure, focus and haze colour stay as they were while the tiles are taken. Vignette, film grain, chromatic aberration, lens flare and light shafts are left out of hi-res photos, since they would repeat in every tile.
- Middle click sets the focus at that point (depth of field), `Ctrl` + middle click removes it, and `Ctrl` + mouse wheel sets how strong the blur is.
- `Shift` + mouse wheel zooms in (up to 8x, like a telephoto lens), `Shift` + middle click goes back to 1x. Only in: the game decides what to draw from its own view.
- `Shift+F6` splits the screen to compare (*Compare split*; it turns photo mode on too): on one side of a line the game's image without the mod's effects, on the other the mod's. `Ctrl` + `Shift` + mouse wheel moves the line; the menu has a slider for it and a switch for the sides. Photos (`F12`) keep the split, and so do hi-res photos (`Shift+F12`) in TrackMania Nations/United: the line runs through the whole stitched photo, each tile gets its part of it. Photos of the split get both sides' names burnt in at the bottom, at the line (your preset's name, *Game* or the other preset's; *Photo labels*, `SplitPhotoLabels`, on by default); a side too narrow for its name gets none. The aperture (depth of field) is the same on both sides: the game's image gets photo mode's depth of field with the same focus and blur as your side, and nothing else of the mod (TrackMania Nations/United; without depth of field it is the game's image as it came in). What the mod changes inside the game itself is on both sides: the game's own bloom stays off, real night stays night.
- *Other side* (TrackMania Nations/United) puts a preset there instead of the game's image: your look against that preset's, on the same frame. To compare two presets, use one and pick the other there. The preset side is shaded a second time with its own history (TAA, exposure, bounce light, focus): it costs about as much again, only while the split shows a preset. The game's own changes (real night, the game's sky and sun halo hidden under night skies) follow your look on both sides, so a preset that would light the map otherwise can't show that light there: a day preset next to a night look (real night, the moon off) gets no sun, and mostly its sky, clouds and grade show. The list at the bottom of the screen says when that is the case. In photo mode the list at the line (bottom of the screen) switches the other side without the menu; it is never in the photos.
- *Other side* in TrackMania 2: the same choice in the menu (*Photo mode* panel; no list at the line yet). The preset side runs the effects and the HDR stage a second time, with its own history, and each look draws only its side of the line; it costs about as much again, only while the split shows a preset. In TrackMania 2 the light (real night, the moon, sun and sky) is the mod's own in each look, so a preset side shows its own night or day light; only the engine's soft shadows and light draws (`maniaplanet.ini`) are the same on both sides. The game's image on the other side has no depth of field (as before); a preset side gets the aperture. Photos (`F12`) keep the split and get both names (`SplitPhotoLabels`); no hi-res photos in TrackMania 2.

- The game's adaptive view distance (launcher *ZClip*) is off, so far blocks stay in the picture even when the frame rate drops.

The options are on the *Settings* page (*Photo mode* panel) and in `settings.ini` (`[General]`: `PhotoAuto`, `PhotoTiles`, `PhotoDof`, `SplitPhotoLabels`). They are not part of presets.

## Colour look (LUT)

A colour look-up table changes every colour of the image in one go, the way film looks and colour grades are shared. Any 3D `.cube` file works (exported from DaVinci Resolve, Lightroom, Photoshop, OBS and many free LUT packs), up to 64 x 64 x 64.

1. *Customize* page, *Colour look (LUT)*: *Open the LUT folder* (`Documents\TrackMania\TMVS\luts`) and put the `.cube` files there.
2. Pick one under *Look*. *Amount* mixes it with the image without it (1 = all of it).

The look is applied last, after the preset's own colours, on the image as you see it (sRGB), and stays when you change presets. It is not part of presets or Author's Shaders: `settings.ini` keeps it in `[General]` (`Lut`, `LutAmount`). 1D LUTs and an input range other than 0..1 (`DOMAIN_MIN`/`DOMAIN_MAX`) aren't supported; `tmvs.log` says why a file didn't load.

## All settings

Range is the slider range. Key is the name in `settings.ini` and in preset files.

### Lighting

| Setting | Key | Range | What it does |
|---|---|---|---|
| Ambient occlusion | `AOStrength` | 0 – 2 | Darkens corners and contact areas. |
| AO radius (m) | `AORadius` | 0.3 – 5 | How far around a point occlusion is searched. Large values darken whole areas, small ones only tight corners. |
| Sun shadows | `ShadowStrength` | 0 – 1 | Strength of the sun shadows traced against the depth buffer. 0 = off. |
| Shadow ray length (m) | `ShadowLength` | 1 – 30 | How far those shadow rays travel. |
| Long-range shadows | `LongShadows` | 0 – 1 | Shadows from a height map of the track that builds up while you drive: long evening shadows, also from objects off screen. |
| Long shadow range (m) | `LongShadowRange` | 30 – 250 | How far the long shadows reach. |
| Long shadows: every cell | `LongShadowQuality` | 0 / 1 (1) | 1: every cell of the height map towards the light counts (a grid turned to the light and swept towards it), so thin casters like wall tops cast their whole shadow under a low sun or the black hole, without grain. 0: traced in 24 steps as before (thin casters were hit by some rays and missed by others: weak, grainy shadows that ended in straight lines). |
| Neon light on surroundings | `NeonLight` | 0 – 2 | How much the coloured borders and signs light up the road and walls around them. With *Light sources from the game* the light comes only from what the game draws as light (lamps, glows, start lights) and the neon borders (by *Neon strips glow*), each in its own colour; floodlit grass, banners and car paint no longer light their surroundings. |
| Bounce light | `GlobalIllumination` | 0 – 2 | One bounce of coloured light from surface to surface. |
| Sunlight warmth | `SunLight` | 0 – 1.5 | How strongly sunlit surfaces take on the sun colour. |
| Sky ambient tint | `AmbientTint` | 0 – 1.5 | How strongly shaded surfaces take on the sky colour. |
| Sun colour | `SunColor` | colour | Colour of the sunlight. |
| Use game's sun colour | `GameSunColor` | 0 – 1 | Mixes in the map's own light colour, so sunsets stay orange. 1 = only the game's colour. |
| Sky colour | `SkyColor` | colour | Colour of shade and ambient light. |
| Sun elevation / azimuth override | `SunElevation`, `SunAzimuth` | −1 – 90, 0 – 360 | Moves the sun for the shaders. −1 = the game's sun. |

### Sky and atmosphere

| Setting | Key | Range | What it does |
|---|---|---|---|
| Sky | `SkyMode` | 0 – 5 | Game sky, clear sky, starry night, black hole, aurora, Ring World. |
| Night darkening | `SkyNight` | −1 – 1 | How much the scene darkens under a night sky. −1 = automatic (less on maps that are already night). |
| Sky rotation | `SkyRotation` | 0 – 360 | Turns stars, black hole, planet and aurora around you. |
| Sky brightness | `SkyBrightness` | 0.2 – 3 | Brightness of the custom skies. |
| Clouds (clear sky) | `CloudAmount` | 0 – 1 | The flat clouds of the clear sky. |
| Stars | `StarAmount` | 0 – 3 | Number and brightness of the stars, and how often shooting stars cross. |
| Aurora movement | `AuroraSpeed` | 0 – 4 | How fast the aurora's curtains move. |
| Black hole size | `SkyEffectSize` | 0 – 6 | Above 3 you are right at its edge, the disk sweeping across the sky. 0 = off. In the space skies the black hole is the light: shafts, shadows, flare and glow come from it. |
| Black hole direction / height | `BlackHoleAzimuth`, `BlackHoleElevation` | 0 – 360, −10 – 60 | Where the black hole is in the sky. |
| Ringed planet | `PlanetSize` | 0 – 2.5 | Size of the planet (starry night, black hole, Ring World). 0 = none. |
| Ring world planet | `PlanetType` | 0 – 3 | Saturn, Jupiter, ice giant, exotic. |
| Ring world view | `PlanetView` | 0 – 2 | *Distant*: a ringed planet in the sky. *Next to the rings*: the rings cross the sky in front of the planet. *On the rings*: you float just above them. |
| Planet direction / height | `PlanetAzimuth`, `PlanetElevation` | 0 – 360, −10 – 60 | Where the planet is in the sky. |
| Volumetric clouds | `VolumetricClouds` | 0 – 1 | 3D clouds over any sky, lit by the sun. 0 = off. |
| Cloud coverage | `CloudCoverage` | 0.05 – 1 | How much of the sky they cover. |
| Cloud height (m) | `CloudHeight` | 300 – 4000 | Height of the cloud base. |
| Sky enhancement | `SkyEnhance` | 0 – 1 | Richer colour gradient on the game's own sky. |
| Sun glow | `SunGlow` | 0 – 2 | Glow and disc of the sun. |
| Haze density / height falloff | `FogDensity`, `FogHeightFalloff` | 0 – 6, 0 – 3 | How thick the haze is, and how fast it thins out with height. |
| Haze sun scattering | `FogSunScatter` | 0 – 2 | How much the haze glows around the sun. |
| Haze distance | `HazeDistance` | 0.1 – 1 | Where the haze closes in. 1 = as before; lower brings it nearer the camera (less visibility, *Snowstorm* 0.5). |
| Light shafts / length | `GodRays`, `GodRayDecay` | 0 – 2, 0.9 – 0.995 | Rays around the visible sun. |
| Volumetric light | `VolumetricLight` | 0 – 2 | Sunlit haze with the shadows of the track's structures in it, also with the sun behind you. |

### Weather and surfaces

| Setting | Key | Range | What it does |
|---|---|---|---|
| Wet roads | `Wetness` | 0 – 1 | Darker, glossy, reflective track. |
| Rain | `Rain` | 0 – 2 | Falling drops, splashes, ripples. 2 = downpour. |
| Snow | `Snow` | 0 – 2 | Falling flakes that drift with the wind, a snow veil in the distance, haze. 2 = blizzard. The storm's wind sound grows with the snowfall and the wind. |
| Snow on the ground | `SnowCover` | 0 – 1 | Snow on everything that faces up: piles and drifts with bare ground between them, about half the ground at 0.5, a closed blanket at 1. Cars keep their paint. Off in every preset: it can't follow the game's surfaces well (a snow texture pack looks better on the road). |
| Puddles | `Puddles` | 0 – 1 | Standing water on flat ground (needs wet roads). |
| Lightning | `Lightning` | 0 – 1 | How often lightning strikes: a bolt in the sky, a flash over the track, thunder after a delay that matches the distance. |
| Drops and flakes on the lens | `LensDrops` | on / off | Rain drops that run down the screen, or snow flakes, only while it rains or snows. |
| Dry under roofs and bridges | `WeatherShelter` | on / off | No rain or snow falls under roofs, bridges and in buildings the camera has seen; around them it keeps falling. The lens dries off under a wide roof. On by default. |
| Spray behind the car | `Spray` | 0 – 1 | Water (wet roads) or powder snow (snow on the ground) thrown up by the rear tyres, only while they touch the road. Off by default. |
| Weather sound | `WeatherSound` | 0 – 2 | Volume of rain, thunder and the snow storm's wind, only with rain, lightning or snow. Default 0.45. It is the mod's own sound output: the game's sound and music sliders don't change it, the Windows volume mixer (entry *TmForever*) does. |
| Water | `WaterSurfaces` | 0 – 1 | Waves, refraction and reflections on the game's water. |
| Track reflections (dry) | `Reflections` | 0 – 2 | Up to 1 a glossy sheen; from 1 to 2 polished like a mirror, checkpoints and objects reflect clearly. |
| Reflection blur | `ReflectionBlur` | 0 – 1 | Blurs the dry track's reflection into a soft sheen. |
| Grass detail | `GrassDetail` | 0 – 1 | Grass patches and blades close to the camera. |
| Mowing stripes | `MowingStripes` | 0 – 1 | Stadium-style stripes in the grass. |
| Wind | `Wind` | 0 – 1 | Slant of the rain, movement of grass and clouds. |

### Cinematic

| Setting | Key | Range | What it does |
|---|---|---|---|
| Motion blur | `MotionBlur` | 0 – 1.5 | Blur from camera movement. 1 = one full frame of movement. Your car stays sharp in the chase camera. |
| Depth of field | `DepthOfField` | 0 – 1 | Background and foreground blur. |
| Only in replays and video export | `CinematicOnlyInReplays` | on / off | Motion blur and depth of field switch off while you drive. On by default. |
| Best quality in the video export | `VideoExportBest` | on / off | The replay editor's video export: full effect quality (no auto quality), photo mode's depth of field, and the effects run on the video's own clock (exposure, focus, rain, lightning and clouds move as in the finished video, however slowly its frames render). With the export's *high quality* (supersampling) every sample gets the effects and TAA is off. Off: as before. On by default. |
| Video export file | `VideoCodec` | 0 – 2 | 1 (default): the replay editor's *Shoot video* writes an MP4 with H.264, 2: HEVC (H.265), both with the sound as AAC, next to the game's file (same name, `.mp4`, in `Documents\TrackMania\Video`). The graphics card encodes when its driver offers it (NVIDIA, AMD, Intel), else Windows' own encoder; no codec dialog, nothing to install. 0: the game's AVI with its codec list, as before. If Windows can't encode, the game's AVI is written instead (the log says why). |
| Video export bitrate | `VideoBitrate` | 0 – 200 Mbit/s | 0 = from the size and frame rate (4K at 30 fps: 50 Mbit/s H.264, 30 HEVC; twice that at 60 fps). |
| Focus distance (m) | `FocusDistance` | 0 – 200 | 0 = auto focus (where: *Focus on*). |
| Focus on | `DofAutofocus` | Middle, Car | Where the auto focus sits (with *Focus distance* 0). *Middle* (0, default): what is in the middle of the screen, a little below the centre, as before. *Car* (1): your car, found where the game draws it (TrackMania Nations/United), wherever the replay camera puts it; the focus measures the depth of the car's pixels around that point, and a pillar in front of the car stays blurred. While the car is off screen, behind the camera or its position unknown (the log says so), the middle again. The focus moves there within about a quarter of a second, a camera cut jumps at once. A click in the photo mode still wins. |
| Max blur (px) | `BokehSize` | 2 – 24 | Largest blur size, at 1080p. |

### Neon trail

| Setting | Key | Range | What it does |
|---|---|---|---|
| Neon trail behind the car | `NeonTrail` | 0 – 3 | Brightness of the light trail. 0 = off. Not recorded in replays. |
| Trail colour | `TrailColor` | colour | |
| Trail width (m) | `TrailWidth` | 0.05 – 1 | |
| Two lines from the rear tyres | `TrailTyres` | on / off | Two lines, or one from the middle of the car. |
| Fade after (s) | `TrailDuration` | 0 – 120 | 0 keeps the trail for the whole run. A restart clears it, a respawn starts a new line. |
| Earlier runs kept | `TrailKeepRuns` | 0 – 5 (0) | 0: a restart clears the trail. 1 – 5: the trails of that many earlier runs stay after a restart, dimmer (the last at 60 %, the oldest at 60 % / n); a new map clears them all. |

### Bloom and lens

| Setting | Key | Range | What it does |
|---|---|---|---|
| Bloom | `Bloom` | 0 – 0.4 | Soft glow around bright parts. |
| Bloom radius | `BloomRadius` | 0.3 – 1.2 | Wider or tighter glow. |
| Light source intensity | `HighlightBoost` | 0 – 8 | How bright lamps and neon become in HDR. |
| Neon strips glow | `BorderGlow` | 0 – 5 | With *Light sources from the game*: how much the blue neon of the road borders glows, on top of the light source intensity (1 = as before, 0 = as the game draws it; the night presets Neon, Horizon and Aurora use 3). |
| Floodlights | `Floodlights` | 0 – 3 (1) | With *Light sources from the game*: brightness and glow of stadium floodlights, start lights and the other lamps the game draws as glows, and the light they throw around them (*Neon light spill*), on their own: the neon borders (`BorderGlow`) and *Light source intensity* stay as they are. 1 = as before. |
| Lamp glare | `LampGlare` | 0 – 1 (0.7) | With *Light sources from the game*: this part of the lamps' light (the game's glows: floodlight heads and their halos, start lights, car lights) leaves the bright core, and some of it spreads wide and soft around the lamp. The bulbs of a floodlight panel stay apart instead of merging into a white slab, and the light reaches the air and the frame around it. The neon borders are not touched. 0 = off (as before). |
| Lamp haze glow | `LampHaze` | 0 – 4 (1) | With *Light sources from the game*: the light the haze takes from the lamps on its way to the camera glows in the air around them; more with more haze (*Haze density*) and for far lamps (stadium floodlights). 1 = all of that light, 0 = off (as before). |
| Lamp halos over the sky | `LampSkyHalos` | on / off (off) | Prototype, with *Light sources from the game*, at night: a lamp's halo also shines where the sky is behind it instead of ending at the edge of the frame the lamp sits on. Off = as before. |
| Lens flare | `LensFlare` | 0 – 1.5 | Ghost reflections from the sun. |
| Chromatic aberration | `ChromaticAberration` | 0 – 1 | Colour fringes towards the screen edges. |
| Vignette | `Vignette` | 0 – 1 | Darker corners. |
| Film grain | `FilmGrain` | 0 – 0.15 | Fine film noise. |

### Colour

| Setting | Key | Range | What it does |
|---|---|---|---|
| Exposure (EV) | `Exposure` | −2 – 2 | Overall brightness. Called *Brightness* in the simple view. |
| Auto exposure | `AutoExposure` | 0 – 1 | How much the brightness adapts to the scene, like an eye. |
| Contrast | `Contrast` | 0.7 – 1.5 | |
| Saturation | `Saturation` | 0 – 2 | Called *Colour* in the simple view. |
| Vibrance | `Vibrance` | −0.5 – 1 | Saturates dull colours more than strong ones. |
| Temperature, tint | `Temperature`, `Tint` | −1 – 1 | Warmer or cooler, greener or pinker. |
| Lift, gamma, gain | `Lift`, `Gamma`, `Gain` | | Brightness of shadows, mid-tones and highlights. |
| Split toning | `SplitToning` | 0 – 1 | Cool shadows, warm highlights. |

### Image

| Setting | Key | Range | What it does |
|---|---|---|---|
| FXAA | `FXAA` | on / off | Smooths edges within one frame. |
| Temporal anti-aliasing | `TAA` | on / off | Smooths edges and flicker over several frames, and the steps of long shadows under a low light. On by default (not in *Performance*). A little softness on fast movement. |
| TAA: sub-pixel jitter | `TAAJitter` | on / off | Samples a different spot inside each pixel every frame, so TAA supersamples the edges. Off by default: fine grates can shimmer with it. |
| Sharpening (CAS) | `Sharpen` | 0 – 1 | Brings back detail after anti-aliasing. |
| Effect quality | `Quality` | Low, Medium, High | Samples for AO, shadows, long shadows, clouds, volumetric light, bounce light and the aurora. Low is clearly faster. |
| Auto quality | `AutoQuality` | on / off | Below the target FPS it measures what every effect costs on your GPU and turns off just enough of them to get there, in one step: first what costs the most for the least look (volumetric light, GI, samples, long shadows, reflections, clouds, TAA, FXAA, depth of field, lens flare and sharpening). The light shafts and the neon light always stay: they are the look. It turns off as few as it can, the long shadows late (they pop in visibly). An effect stays off at least 20 s and comes back only after a few seconds with room for it in a row; one that comes back and drops the frame rate again within a minute stays off for 5 minutes. Your settings stay as they are. The menu shows which effects are off right now. It changes nothing if the game itself (CPU, vsync) holds the frame rate back, or if even everything it may turn off couldn't reach the target (a target above what your PC reaches, such as 240 FPS at 83 FPS). It only decides during a race, not in the menus. |
| Target FPS | `TargetFPS` | 30 – 240 | The frame rate auto quality tries to hold. Lighting, shadows and bloom always stay on, so with heavy presets on a slow GPU it can end a little below. |
| Render scale | `RenderScale` | 0.5 – 1 (1 = off) | The game draws its 3D image at this fraction of the screen's resolution (0.67 of 2560×1440 is 1715×965), every effect runs at that size, and AMD FSR 1 (edge-adaptive upscaling, then its sharpening) brings the shaded image back up to the screen before the HUD, which stays sharp. For when the graphics card is the limit: at 0.67 the effects cost about half, the game's own drawing less too (see [performance](performance.md#render-scale-fsr-1)). Costs some sharpness on fine detail and text; 0.77 is close to the full image, 0.5 visibly soft. Photo mode, photos, F12 captures and the video export always use the full resolution; a camera into part of the screen (MediaTracker editor, the replay menu's band) too, and the log says when the game draws something the scaling can't follow (then it stays at full size for that map). Needs *Game HDR*. TrackMania Nations/United. Machine setting, not part of a look. |
| FSR sharpening | `FsrSharpness` | 0 – 1 (0.8) | How much FSR sharpens the image it scaled up (its RCAS): 1 is AMD's strongest, 0 none. While the render scale is below 1 it takes the place of *Sharpening* (sharpened before the upscale, its halos would grow with the image). Machine setting. |
| Disable game MSAA | `DisableGameMSAA` | on / off | Needed for the depth effects. Leave it on. Takes effect after a restart. |
| Effects in replay / video export | `ReadableGameDepth` | on / off | Also makes the depth buffers of replays and the video export readable. Takes effect after a restart. |
| Turn off the game's FX | `DisableGameFX` | on / off | Skips the game's own bloom and colour filter (launcher *Advanced*, FX post-processing) while the mod shades, so nothing glows twice. The launcher setting is left alone. The game asks only when it starts its effects, so a change shows from the next map on. |
| Real night under night skies | `GameNight` | on / off (on) | With a night sky or the black hole over a day or sunset map, the game lights the map at midnight itself: its moon, fog and night sky, nothing of the sun baked into the image. The mod no longer has to take the sun out of a finished picture (where the sun had already turned surfaces white, there was no texture left). The map file, its replays and validation are untouched; only the time of day the game starts the map's lighting at changes. Switched while a map runs, the map is relit at once (if a map doesn't follow, it does from the next map on). |
| The game's moon under the black hole | `EngineMoon` | as the game has it, from the black hole, off (from the black hole) | Night maps (their own night, or lit at midnight by *Real night*) under a sky with the black hole: the game's own moonlight comes from the side (on A01 it is horizontal, opposite the black hole). *From the black hole*: the game takes the black hole's direction for its moon when it builds the map's light at load, so its own shadows, the glints on the blocks and the light on the car come from the black hole, complete and without popping. *Off*: the game's moon has no light; the sky light (ambient) and the lamps stay. Both also tell the mod that nothing of a sun is baked into the image (it doesn't darken the sides facing the black hole). Takes effect from the next map (the light is baked once per map; back to the menu and reload the map). Machine setting, not part of a look. |
| Black hole moon (TrackMania 2) | `BlackHoleMoon` (`maniaplanet.ini`) | 0 – 2 (0 = off) | TrackMania 2's counterpart of *From the black hole*, in the mod's shaders only (ManiaPlanet's moon can't be moved without writing into the game): on night maps under the black hole the game's moon, its blue glint on chrome and the sculptures and its light, is taken out of the picture and the black hole lights the scene instead, warm, with the game's own shine on each material; 1 = as bright as the moon was. Its shadows are the game's moon shadows where the black hole stands near the moon (Horizon), else only short contact shadows. Settings page, panel *ManiaPlanet*. Off by default. |
| Floodlit surfaces at night | `NightSurfaces` | 0.4 - 1 (0.8) | Night maps: what the game drew nearly white keeps at least that brightness. Lamps and neon in full; large lit surfaces (stands, lit roads, white walls) only this part of it, so they keep some structure instead of burning out. 1 = all in full. |
| Highlight roll-off | `ToneKnee` | 0.3 - 0.9 (0.5) | The tone curve is linear up to this value, then rolls off softly. Higher: brighter highlights, more of the game's bright tones unchanged; lower: softer, greyer highlights. |
| Read the game's materials | `GameMaterials` | on / off (on) | While the game draws, the mod notes which material each pixel is (stencil buffer). Used for the TMUF sea (water by material, its height measured from it) and the debug view *Materials*. Not on Stadium (the engine reports its water). |
| Keep the game's clouds under night skies | `GameSkyClouds` | on / off (off) | With a night sky over a sunset map, the game's sunset clouds still show through the stars and its sea still mirrors the sunset. Off: both go. |
| Long shadows from the game's meshes | `CasterShadows` | on / off (on) | The mod keeps every static mesh the game draws (once it stood still for half a second) and draws them from the sun or the black hole into two shadow maps: a fine one around the camera (about 8 cm a texel, drawn again in the background as you drive on) and a coarse one over the whole track. The long shadows and the volumetric light's shafts then come from those maps instead of the height map: they don't pop with the camera, keep the meshes' real shape (overhangs, poles) and reach as far as the maps. Needs *Long-range shadows* (or *Volumetric light*). Where no map is complete yet (the first second of a map, after a light change), the height map's shadows stay. Fences and grids (see-through textures) cast their pattern with *Fence shadows* (below). Removing blocks in the editor starts the maps over. About 192 MB of video memory. |
| Fence shadows | `CasterAlphaTest` | on / off (on) | With *Long shadows from the game's meshes*: fences, grids, mesh panels and lattice towers (meshes the game draws with an alpha test, the holes cut out by their texture's alpha) are kept too and drawn into the shadow maps the way the game draws them: with its own shaders, the constants and textures of that draw and its alpha test. Their shadows have the holes of the mesh instead of none at all. Off: they cast no shadow from the maps (as before). Maps without such meshes look the same either way. Cost: nothing more while recording; drawing them into the maps about 1 µs of the graphics card per mesh (a few hundred meshes each time the map around the camera is drawn again). The log's `casters (...)` lines get a second line `alpha-tested: N meshes kept`. |
| Normals from the game's shaders | `GameGBuffer` | on / off (off) | Experimental. The game's own material shaders also write the normal of every surface they draw (blocks with their bump maps, the car, decorations) into a second target, next to the image. Where it holds the surface that is really seen there, ambient occlusion, sun and long shadows, reflections and the relighting take that normal instead of one guessed from the depth buffer: no stair steps on curved surfaces, the bumps of the blocks catch the light. Roads and flat ground keep the guessed normal (their shaders have none). Needs *Game HDR*. The shaders are changed when the game compiles them, at its start: switch it on, then restart the game; from then on it can be switched off and on while playing (off, the game draws exactly as before). F12 also saves the normals (`capture_NNN.nrm16`, `tools/gbuffer.py` shows them). |
| Night on the game's light | `GameLight` | on / off (off) | Experimental. The game's own material shaders also write the light they lit each surface with (the baked lightmap and the lamps baked into it at night, the ambient light; the car's sun and ambient) into a third target, next to the image, the same way as *Normals from the game's shaders*. Under a night sky, where that light is stronger than the paint alone (above 1: only stacked lamp light gets there at night), the night's darkening and blue tint step back: the asphalt under a lamp and the floodlit stands stay as bright and as warm as the game drew them, everything else gets the night as before. In the image a white wall in the moonlight and dark asphalt under a lamp can be equally bright; their light is not. Only on night maps and maps lit at midnight (*Real night*), not where a sun is baked in. Needs *Game HDR* and *Game light sources*. The shaders are changed when the game compiles them, at its start: switch it on, then restart the game; from then on it can be switched off and on while playing. F12 also saves the light (`capture_NNN.lit16`, `tools/gbuffer.py --light` shows it). |
| Clean water | `CleanWater` | on / off (on) | The game's water (pools, rivers, the sea) mirrors only what it can really see, sharp. Off: from above, the reflection came out from behind the ramps and bridges over the water and mirrored their tops in blotches, softened by the dry track's reflection blur. Needs the water's height from the engine (Stadium, TMUF sea). |
| Shaded image under the replay menu's bars | `ShadeMenuBars` | on / off (on) | At the end of a race the replay shrinks into a band between the menu's half-transparent bars, and the game draws nothing under the bars any more: they showed a stepped, blocky strip of every frame of the shrinking (shaded each on its own). The shaded band, mirrored at its edges, fills them instead. Rows the game clears itself stay as they are. |
