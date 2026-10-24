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
- **Resource packs** (TrackMania Nations/United): the game's environment mods on every map, [below](#resource-packs).
- **Video** (TrackMania Nations/United): the replay editor's *Shoot video*. Presets for what the file is for (*YouTube*: H.264 quality 80, grain halved; *Best*: H.264 quality 92, grain halved; *Editing*: H.264 quality 92, full grain; *Small*: HEVC 10-bit quality 70; *Discord*: H.264 at a fixed 8 Mbit/s; *HDR10*: HEVC 10-bit HDR10 quality 85; each of them picks the graphics card's encoder) and for the look (*Replay*: shutter 360, no crop; *Cinema*: shutter 180, lens depth of field, 2.39:1), a few lines on the game's own dialog (size, frame rate, the sound button, *High quality* with its samples: that is where the supersampling and the motion blur are switched on), and every *Video* setting below.
- **Settings:** options that are not part of a preset: the *Hotkeys* ([below](#hotkeys)), *Preset per map type*, *Picked preset applies to all maps of this type* (off by default; on: picking a preset on a night map makes it the preset for all night maps, the same for day and sunset; off: only the Maps page sets them), *Author's Shaders*, live preview, full-screen menu, update check, the video export, *Effects* (motion blur only in replays, weather volume, TAA), *Game integration* (the same switches as in Studio: the game's MSAA, effects in replays, real night, game light sources, ...), and the folders.
- **About:** the version, updates (with the new version's notes and a link to its release page), links to the project page and the issue tracker, the folder with your settings and `tmvs.log`, and the credits.

**Updates.** When the game starts, the mod asks GitHub once for the newest release (`api.github.com`, the public release list; nothing about you is sent). It never downloads or installs anything: a new version shows in the menu, you get it from the release page. Switch it off under *About → Check on startup* or *Settings* (`CheckForUpdates=0` in `settings.ini`).

**Customize.** The everyday settings, in panels (closed at first: click a title to open it). Some have a plainer name there than in Studio: *Brightness* is *Exposure*, *Glow* is *Bloom*, *Neon borders* is *Neon strip glow*. The panels follow the order of Studio's groups.

| Section | Settings |
|---|---|
| Light and shadows | Shadows, long shadows, ambient occlusion, neon light spill, light shafts |
| Sky | Sky, turn the sky, stars, aurora movement, planet, planet size and view, planet direction and height, black hole size, direction and height, clouds (only what the chosen sky uses) |
| Bloom and lens | Glow, glow radius, light intensity, neon borders, floodlights, lens flare, colour fringes, vignette, film grain |
| Colour | Brightness, contrast, saturation |
| Weather | Rain, snow, snow on the ground, wet roads, lightning, dry under roofs, drops on the lens, car spray, weather volume, water, reflections, reflection blur |
| Neon trail | Neon trail, colour, width, tyre lines, trail fade, earlier runs |
| Cinematic | Motion blur, depth of field, focus on, cinematic in replays only |
| Image quality | Temporal AA, edge smoothing (SMAA or FXAA), sharpening, effect quality, auto quality |

**Studio.** Shows the preset you are editing and whether your changes are saved. Load a preset by clicking it on the *Shaders* page, change it here, then *Save changes* (your own presets) or *Save as new preset*. Every setting below, in a panel per group as in this page (*Lighting*, *Sky*, *Haze and light shafts*, *Bloom and lens*, *Colour*, *Weather and surfaces*, *Neon trail*, *Cinematic*, *Image quality* and, last, *Game integration*: how the mod works with the game itself; closed at first: click a title to open it, *Expand all* opens every group; a dot marks groups with changes), plus the *Colour look (LUT)*, *Save as preset*, *Reset preset* (asks first), the debug views and *Reload shaders*. Settings that do nothing with your current choices are hidden; for example, the planet settings only appear with the Ring World sky. The search box at the top finds any setting across all sections.

A small arrow appears next to every value you changed: click it to set that value back to the preset's. Switching presets glides over about a second.

## Presets per map mood

The mod reads the colour of the game's sun when a map loads and sorts the map into day, sunset (and sunrise) or night. With *Preset per map type* on, it switches to the preset you chose for that kind of map. The defaults are **Vibrant** for day, **Golden Hour** for sunset and **Horizon** for night. Change them on the *Maps* page, With *Picked preset applies to all maps of this type* on (off by default), a preset picked on a map also becomes the preset for that kind of map. With it off, a preset you pick in the menu holds on every map, also on maps of another type (and real night follows it), until you choose a preset for a map type on the *Maps* page or press *Use the presets per map type again* there.

While your preset is *Custom* (you changed a value and didn't save it), a map change never replaces your tweaks. A map with an [Author's Shader](#authors-shader) wins over the mood preset.

## Your own presets

1. Start from the preset closest to what you want.
2. Change values on the *Customize* or *Studio* page. `F7` shows the original image for comparison.
3. Press **Save as preset** and type a name.

Presets are text files in `Documents\TrackMania\TMVS\presets\`, one `.ini` per preset, with one line per setting (`Key=Value`, the keys are listed below). Send a file to a friend and they drop it into the same folder; it appears in their list on the next game start (or [as a share code](#share-a-preset-as-text), one line of text). Settings that belong to your installation rather than to a look are not part of a preset and stay when you switch: auto quality and target FPS, render scale and FSR sharpening, the weather volume, the neon trail, the video export, *Cinematic in replays only*, TAA jitter, and the switches for the game itself (the *Game integration* table below).

## Share a preset as text

For a chat (Discord and the like) instead of two files: on the *Shaders* page, under *Your presets*, **Copy share code** puts the current look on the clipboard as one line, `TMVS1:` and some letters, usually 150 to 700 characters (a Discord message holds 2000). Someone else presses **Paste share code** and has your look.

- The code holds every setting of the look that differs from the defaults, not the settings of the installation (the same ones a preset leaves out, see above). It reads the same in TrackMania Nations/United and in TrackMania 2: the preset it was made from can differ between the games, the look that arrives does not.
- **Paste share code** reads the clipboard (a chat message with the code in it works, so does a code broken over lines) and makes the look yours, unsaved: the preset becomes *Custom* on top of the built-in preset it is closest to, and *Save as preset* keeps it. Nothing saved is replaced, built-in presets never. A code that is exactly a built-in preset selects that preset.
- The line under the buttons says what happened: `Loaded 37 settings, 2 unknown ignored`. Unknown keys (a newer version's, or ones this game doesn't have) and unreadable values are ignored and counted, values outside a setting's range are moved into it (*out of range*), and a code that is no code (empty clipboard, no text, damaged, a newer code version) says so and changes nothing.
- A code is built against the Settings defaults of its version (`TMVS1`). When a default changes, old codes keep reading as before: the code keeps the old value as its base (*Sharpening* 0.35, the default before 1.4.1) or the version goes up.

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
| Menu | `Menu` | `F8` (`F5` while the CenterSpeed mod is loaded, see below) |
| Effects on / off | `Effects` | `F7` (ManiaPlanet: `F5`, because `F7` is its friends list) |
| Photo mode | `Photo` | `F6` |
| Photo mode: compare split | `PhotoSplit` | `Shift+F6` |
| Frame capture / photo (`Shift`: hi-res photo) | `Capture` | `F12` |
| Reload shaders (TMNF only) | `Reload` | `Ctrl+F9` (was `F9`, which the 100% TMX mod's menu uses; an old `Reload=F9` is moved once) |
| Trace (TMNF only) | `Trace` | `F11` |

They are stored in the `[Keys]` section of `settings.ini` as readable names, for example `Menu=F8` or `PhotoSplit=Ctrl+Shift+F6` (`None` turns an action off; modifiers `Shift`, `Ctrl`, `Alt`; keys `A`-`Z`, `0`-`9`, `F1`-`F24`, `Num0`-`Num9`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `Space`, `Enter`, `Tab`, `Up`, `Down`, `Left`, `Right`, `Pause`, `ScrollLock`, `Minus`, `Equals`, `Comma`, `Period`, `Slash`, `Backslash`, `Semicolon`, `Quote`, `Backquote`, `LBracket`, `RBracket`). A name the mod can't read, or two actions on one key, falls back to the default. Letters, digits and the game's own keys (arrows, `Space`, `Enter`, `Tab`) need `Ctrl` or `Alt`, else they would stop working in the game; `F` keys, the number pad and `Insert` / `Delete` / `Home` / `End` / `PageUp` / `PageDown` work alone. While a text field in the menu has the keyboard, only `F` key hotkeys react. Hotkeys only react while the game's window is in front (an `F` key pressed in another program does nothing), and the modifiers must match exactly (`Ctrl+F9` is not `F9`; the one exception is `Shift` on the frame capture). Together with the 100% TMX mod, whichever menu the pointer is over gets the mouse. The speedometer mod CenterSpeed opens its own menu with `F8` (with any modifier held): when it is loaded and the menu key is still the default `F8`, the menu moves to `F5` once (logged; `MenuMovedForCenterSpeed=1` in `[Keys]` remembers it, so an `F8` you pick afterwards stays); without CenterSpeed a menu moved that way goes back to `F8`. The texts in this document name the default keys. Mouse controls (middle click, wheel) are fixed.

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
- *Other side* in TrackMania 2: the same choice in the menu (*Photo mode* panel) and, as in TrackMania Nations/United, in the list at the line (bottom of the screen), which switches the other side without the menu and is never in the photos. The preset side runs the effects and the HDR stage a second time, with its own history, and each look draws only its side of the line; it costs about as much again, only while the split shows a preset. In TrackMania 2 the light (real night, the moon, sun and sky) is the mod's own in each look, so a preset side shows its own night or day light; only the engine's soft shadows and light draws (`maniaplanet.ini`) are the same on both sides. The game's image on the other side has no depth of field (as before); a preset side gets the aperture. Photos (`F12`) keep the split and get both names (`SplitPhotoLabels`); no hi-res photos in TrackMania 2.

- The game's adaptive view distance (launcher *ZClip*) is off, so far blocks stay in the picture even when the frame rate drops.
- **The game pauses** (*Pause the game*, `PhotoFreeze`, on; TrackMania Nations/United) in the two places of a race where that cannot help a timed run: after the finish line, while the run is over (the car stops where it is, mid-air included, the race clock stops), and in the editor's test mode. Photo mode asks the game to do what its own `Esc` menu does when it pauses a solo race (stop its clock, pause the race), without the menu on the picture; leaving photo mode, restarting the race or the end-of-race replay starting gives it back. It never pauses **online** (the mod takes anything it can't tell for sure as online), **in a live run** and **in replays**, which have their own pause button. The reason is the leaderboards: a pause with a free camera in the middle of a timed run would let you stop and look at the track ahead, so a live run stays live, and photo mode there keeps the camera the game allows. The game's own cameras and MediaTracker clips are never switched off. While paused the line at the top says *Paused*; whether the free camera (cam 7) still moves while the game is paused depends on the game's own input handling.

The options are on the *Settings* page (*Photo mode* panel) and in `settings.ini` (`[General]`: `PhotoAuto`, `PhotoFreeze`, `PhotoTiles`, `PhotoDof`, `SplitPhotoLabels`). They are not part of presets.

## Colour look (LUT)

A colour look-up table changes every colour of the image in one go, the way film looks and colour grades are shared. Any 3D `.cube` file works (exported from DaVinci Resolve, Lightroom, Photoshop, OBS and many free LUT packs), up to 64 x 64 x 64.

1. *Studio* page, *Colour look (LUT)*: *Open the LUT folder* (`Documents\TrackMania\TMVS\luts`) and put the `.cube` files there.
2. Pick one under *Look*. *Amount* mixes it with the image without it (1 = all of it).

The look is applied last, after the preset's own colours, on the image as you see it (sRGB), and stays when you change presets. It is not part of presets or Author's Shaders: `settings.ini` keeps it in `[General]` (`Lut`, `LutAmount`). 1D LUTs and an input range other than 0..1 (`DOMAIN_MIN`/`DOMAIN_MAX`) aren't supported; `tmvs.log` says why a file didn't load.

## Resource packs

TrackMania Nations/United (not TrackMania 2). The game's own environment mods, zips of textures in `Documents\TrackMania\Skins\<Environment>\Mod` (`Stadium\Mod\DarkBlue.zip` and so on: a texture in the zip replaces the game's texture of the same name), normally only show on a map whose author linked one. The *Resource packs* page uses one on every map you load: races, replays, online, the editor.

- Every pack is a card with its picture (the zip's `Image/LoadScreen.dds`, else a `preview.png`, `.jpg` or `.dds` in it). Click a card to use it for its environment, from the next map you load on, or at once with *Apply now*. *Map's own* (the default) is the game's behaviour; *None* shows the game's own textures, also on maps that bring a mod.
- **Also replace a map's own mod** (on): off, a map that brings its own mod keeps it (also with *None*); your pack then only shows on maps without one.
- **One per map mood** (off): a pack for day, sunset and night maps each (the chips pick which one the cards set). The mood comes from the map's decoration (*Day*; *Sunrise* and *Sunset*; *Night*). When it is something else, *All moods* is used, and the log says so.
- **Apply now** (while a map is loaded, when your choice differs from what it shows; the line next to it says both): switches the map you are on without loading it again. Every texture the environment's mods can replace gets the new pack's file or the game's own, the way the game itself swaps in a map's mod that finishes downloading during a race; it takes a moment (the textures are read from the zip). The line under it and `tmvs.log` say how many were switched, for example `modpack: live: Stadium: DarkBlue.zip -> NEONglowSTICKS.zip: 37 textures swapped, 120 already right, 0 failed`. The next map loads with the choice as before. In the editor, a block you place for the first time after switching may still show the old pack until the next map.
- **Refresh** looks for new zips. The game itself only takes the packs that were there when it started: a zip added later shows *Restart TrackMania to use it*.
- **Open folder** opens that environment's `Mod` folder.

The pack is only in your game: the map, its saved file, replays and ghosts keep the map's own mod, and nothing is sent to the server (also with *Apply now*, which only changes the images of the game's textures). `tmvs.log` says at every map load what was used, for example `modpack: Stadium: DarkBlue.zip (instead of the map's Portal.zip)`, `modpack: Stadium: map's own: none` or `modpack: Stadium: none`. Not part of presets: `settings.ini` keeps it in `[ResourcePacks]` (`ReplaceMapMod`, default 1; per environment `Stadium` for all moods, `Stadium.PerMood`, `Stadium.Day`, `Stadium.Sunset`, `Stadium.Night`; empty = the map's own, `None`, or the zip's file name).

## All settings

Range is the slider range. Key is the name in `settings.ini` and in preset files.

### Lighting

| Setting | Key | Range | What it does |
|---|---|---|---|
| Sun shadows | `ShadowStrength` | 0 – 1 | Strength of the sun shadows traced against the depth buffer. 0 = off. |
| Long shadows | `LongShadows` | 0 – 1 | Shadows from a height map of the track that builds up while you drive: long evening shadows, also from objects off screen. |
| Ambient occlusion | `AOStrength` | 0 – 2 | Darkens corners and contact areas. |
| Neon light spill | `NeonLight` | 0 – 2 | How much the coloured borders and signs light up the road and walls around them. With *Game light sources* the light comes only from what the game draws as light (lamps, glows, start lights) and the neon borders (by *Neon strip glow*), each in its own colour; floodlit grass, banners and car paint no longer light their surroundings. |
| Bounce light | `GlobalIllumination` | 0 – 2 | One bounce of coloured light from surface to surface. |
| Sun tint | `SunLight` | 0 – 1.5 | How strongly sunlit surfaces take on the sun colour. |
| Ambient tint | `AmbientTint` | 0 – 1.5 | How strongly shaded surfaces take on the sky colour. |
| Shadow length (m) | `ShadowLength` | 1 – 30 | How far those shadow rays travel. |
| Long shadow range (m) | `LongShadowRange` | 30 – 250 | How far the long shadows reach. |
| Gap-free long shadows | `LongShadowQuality` | Stepped, Every cell (Every cell) | *Every cell* (1): every cell of the height map towards the light counts (a grid turned to the light and swept towards it), so thin casters like wall tops cast their whole shadow under a low sun or the black hole, without grain. *Stepped* (0): traced in 24 steps as before (thin casters were hit by some rays and missed by others: weak, grainy shadows that ended in straight lines). |
| AO radius (m) | `AORadius` | 0.3 – 5 | How far around a point occlusion is searched. Large values darken whole areas, small ones only tight corners. |
| Sun colour | `SunColor` | colour | Colour of the sunlight. |
| Game sun colour | `GameSunColor` | 0 – 1 | Mixes in the map's own light colour, so sunsets stay orange. 1 = only the game's colour. |
| Sky colour | `SkyColor` | colour | Colour of shade and ambient light. |
| Sun height / Sun direction | `SunElevation`, `SunAzimuth` | −1 – 90, 0 – 360 | Moves the sun for the shaders. −1 = the game's sun. |

### Sky

| Setting | Key | Range | What it does |
|---|---|---|---|
| Sky | `SkyMode` | 0 – 5 | Game sky, clear sky, starry night, black hole, aurora, Ring World. |
| Sky enhancement | `SkyEnhance` | 0 – 1 | Richer colour gradient on the game's own sky. |
| Sky brightness | `SkyBrightness` | 0.2 – 3 | Brightness of the custom skies. |
| Night darkening | `SkyNight` | −1 – 1 | How much the scene darkens under a night sky. −1 = automatic (less on maps that are already night). |
| Sky rotation | `SkyRotation` | 0 – 360 | Turns stars, black hole, planet and aurora around you. |
| Sun glow | `SunGlow` | 0 – 2 | Glow and disc of the sun. |
| Sky clouds | `CloudAmount` | 0 – 1 | The flat clouds of the clear sky. |
| Stars | `StarAmount` | 0 – 3 | Number and brightness of the stars, and how often shooting stars cross. |
| Aurora movement | `AuroraSpeed` | 0 – 4 | How fast the aurora's curtains move. |
| Black hole size | `SkyEffectSize` | 0 – 6 | Above 3 you are right at its edge, the disk sweeping across the sky. 0 = off. In the space skies the black hole is the light: shafts, shadows, flare and glow come from it. |
| Black hole direction / Black hole height | `BlackHoleAzimuth`, `BlackHoleElevation` | 0 – 360, −10 – 60 | Where the black hole is in the sky. |
| Planet | `PlanetType` | 0 – 3 | Saturn, Jupiter, ice giant, exotic. |
| Planet size | `PlanetSize` | 0 – 2.5 | Size of the planet (starry night, black hole, Ring World). 0 = none. |
| Ring world view | `PlanetView` | 0 – 2 | *Distant*: a ringed planet in the sky. *Next to the rings*: the rings cross the sky in front of the planet. *On the rings*: you float just above them. |
| Planet direction / Planet height | `PlanetAzimuth`, `PlanetElevation` | 0 – 360, −10 – 60 | Where the planet is in the sky. |
| Volumetric clouds | `VolumetricClouds` | 0 – 1 | 3D clouds over any sky, lit by the sun. 0 = off. |
| Cloud coverage | `CloudCoverage` | 0.05 – 1 | How much of the sky they cover. |
| Cloud height (m) | `CloudHeight` | 300 – 4000 | Height of the cloud base. |

### Haze and light shafts

| Setting | Key | Range | What it does |
|---|---|---|---|
| Haze density / Haze height falloff | `FogDensity`, `FogHeightFalloff` | 0 – 6, 0 – 3 | How thick the haze is, and how fast it thins out with height. |
| Haze distance | `HazeDistance` | 0.1 – 1 | Where the haze closes in. 1 = as before; lower brings it nearer the camera (less visibility, *Snowstorm* 0.5). |
| Haze sun scattering | `FogSunScatter` | 0 – 2 | How much the haze glows around the sun. |
| Light shafts / Light shaft length | `GodRays`, `GodRayDecay` | 0 – 2, 0.9 – 0.995 | Rays around the visible sun. |
| Volumetric light | `VolumetricLight` | 0 – 2 | Sunlit haze with the shadows of the track's structures in it, also with the sun behind you. |

### Bloom and lens

| Setting | Key | Range | What it does |
|---|---|---|---|
| Bloom | `Bloom` | 0 – 0.4 | Soft glow around bright parts. |
| Bloom radius | `BloomRadius` | 0.3 – 1.2 | Wider or tighter glow. |
| Light intensity | `HighlightBoost` | 0 – 8 | How bright lamps and neon become in HDR. |
| Neon strip glow | `BorderGlow` | 0 – 5 | With *Game light sources*: how much the blue neon of the road borders glows, on top of *Light intensity* (1 = as before, 0 = as the game draws it; the night presets Neon, Horizon and Aurora use 3). |
| Floodlights | `Floodlights` | 0 – 3 (1) | With *Game light sources*: brightness and glow of stadium floodlights, start lights and the other lamps the game draws as glows, and the light they throw around them (*Neon light spill*), on their own: the neon borders (`BorderGlow`) and *Light intensity* stay as they are. 1 = as before. |
| Lamp glare | `LampGlare` | 0 – 1 (0.7) | With *Game light sources*: this part of the lamps' light (the game's glows: floodlight heads and their halos, start lights, car lights) leaves the bright core, and some of it spreads wide and soft around the lamp. The bulbs of a floodlight panel stay apart instead of merging into a white slab, and the light reaches the air and the frame around it. The neon borders are not touched. 0 = off (as before). |
| Lamp haze glow | `LampHaze` | 0 – 4 (1) | With *Game light sources*: the light the haze takes from the lamps on its way to the camera glows in the air around them; more with more haze (*Haze density*) and for far lamps (stadium floodlights). 1 = all of that light, 0 = off (as before). |
| Lamp halos over the sky | `LampSkyHalos` | on / off (off) | Prototype, with *Game light sources*, at night: a lamp's halo also shines where the sky is behind it instead of ending at the edge of the frame the lamp sits on. Off = as before. |
| Lens flare | `LensFlare` | 0 – 1.5 | Ghost reflections from the sun. |
| Chromatic aberration | `ChromaticAberration` | 0 – 1 | Colour fringes towards the screen edges. |
| Vignette | `Vignette` | 0 – 1 | Darker corners. |
| Film grain | `FilmGrain` | 0 – 0.15 | Fine film noise. |

### Colour

| Setting | Key | Range | What it does |
|---|---|---|---|
| Exposure (EV) | `Exposure` | −2 – 2 | Overall brightness. Called *Brightness* on the *Customize* page. |
| Contrast | `Contrast` | 0.7 – 1.5 | |
| Saturation | `Saturation` | 0 – 2 | Strength of all colours. 0 takes them out. |
| Vibrance | `Vibrance` | −0.5 – 1 | Saturates dull colours more than strong ones. |
| Temperature / Tint | `Temperature`, `Tint` | −1 – 1, −1 – 1 | Warmer or cooler, greener or pinker. |
| Auto exposure | `AutoExposure` | 0 – 1 | How much the brightness adapts to the scene, like an eye. |
| Lift / Gamma / Gain | `Lift`, `Gamma`, `Gain` | −0.1 – 0.1, 0.6 – 1.6, 0.6 – 1.6 | Brightness of shadows, mid-tones and highlights. |
| Split toning | `SplitToning` | 0 – 1 | Cool shadows, warm highlights. |
| Highlight roll-off | `ToneKnee` | 0.3 - 0.9 (0.5) | The tone curve is linear up to this value, then rolls off softly. Higher: brighter highlights, more of the game's bright tones unchanged; lower: softer, greyer highlights. |
| Floodlit surfaces | `NightSurfaces` | 0.4 - 1 (0.8) | Night maps: what the game drew nearly white keeps at least that brightness. Lamps and neon in full; large lit surfaces (stands, lit roads, white walls) only this part of it, so they keep some structure instead of burning out. 1 = all in full. |
| Tone curve | `ToneCurve` | 0 – 2 (1) | TrackMania 2 only: how bright colours roll off to white. 0, *The game's*: the engine's own curve; 1, *TMVS (keeps hue)*: the mod's; 2, *ACES (film)*. |

### Weather and surfaces

| Setting | Key | Range | What it does |
|---|---|---|---|
| Rain | `Rain` | 0 – 2 | Falling drops, splashes, ripples. 2 = downpour. |
| Snow | `Snow` | 0 – 2 | Falling flakes that drift with the wind, a snow veil in the distance, haze. 2 = blizzard. The storm's wind sound grows with the snowfall and the wind. |
| Wet roads | `Wetness` | 0 – 1 | Darker, glossy, reflective track. |
| Puddles | `Puddles` | 0 – 1 | Standing water on flat ground (needs wet roads). |
| Snow on the ground | `SnowCover` | 0 – 1 | Snow on everything that faces up: piles and drifts with bare ground between them, about half the ground at 0.5, a closed blanket at 1. Cars keep their paint. Off in every preset: it can't follow the game's surfaces well (a snow texture pack looks better on the road). |
| Lightning | `Lightning` | 0 – 1 | How often lightning strikes: a bolt in the sky, a flash over the track, thunder after a delay that matches the distance. |
| Wind | `Wind` | 0 – 1 | Slant of the rain, movement of grass and clouds. |
| Lens drops | `LensDrops` | on / off | Rain drops that run down the screen, or snow flakes, only while it rains or snows. |
| Dry under roofs | `WeatherShelter` | on / off (on) | No rain or snow falls under roofs, bridges and in buildings the camera has seen; around them it keeps falling. The lens dries off under a wide roof. |
| Car spray | `Spray` | 0 – 1 | Water (wet roads) or powder snow (snow on the ground) thrown up by the tyres, only while they touch the road: from all four tyres of the car the engine draws, along its own axes (also sliding, in replays and in the video export); TrackMania 2 too (the car the camera follows, read from the constants the game draws it with), else by the rear tyres of the car below the screen's centre. Off by default. |
| Weather volume | `WeatherSound` | 0 – 2 (0.45) | Volume of rain, thunder and the snow storm's wind, only with rain, lightning or snow. It is the mod's own sound output: the game's sound and music sliders don't change it, the Windows volume mixer (entry *TmForever*) does. |
| Track reflections | `Reflections` | 0 – 2 | The dry track: up to 1 a glossy sheen; from 1 to 2 polished like a mirror, checkpoints and objects reflect clearly. |
| Reflection blur | `ReflectionBlur` | 0 – 1 | Blurs the dry track's reflection into a soft sheen. |
| Water | `WaterSurfaces` | 0 – 1 | Waves, refraction and reflections on the game's water. |
| Water clarity | `WaterClarity` | 0 – 1 (0.5) | Makes the game's water (pools and the sea) clear instead of murky. You see the bottom through the surface, refracted by the waves, with a few caustics in sunlight; water takes the colour of its depth (shallow is clear and slightly tinted, deep turns darker blue-green; the depth is guessed from the distance to the edge, the mod cannot see the game's bottom); and foam forms where the water meets a wall or the shore. The reflection stays and blends in by viewing angle. 0 is the game's own water. Needs *Water* above 0, the game's materials and the water's height from the engine (Stadium, TMUF sea). Costs one pass of about 0.9 ms at 1440p with a pool on screen (my previewer measurement); auto quality turns it off first with the reflections. |
| Water splash | `WaterSplash` | 0 – 1 (0.6) | The car the camera follows throws up water where it enters a pool or the sea: a fountain of droplets and spray, rings spreading over the surface and foam that fades over a few seconds, all scaled by its speed and the angle it came in at. While it drives through the water it pushes a bow wave and leaves a wake, and when it comes out it drips for a few seconds. Follows the effects' clock, so it is in exported videos too. Costs nothing while no car is in water. The droplet, foam and ripple pictures come from `water_droplet.png`, `water_foam.png` and `water_ripple.png` in `TMVS/textures/water` when they are there (a texture pack can bring its own), else built-in ones. Not in TrackMania 2. |
| Grass detail | `GrassDetail` | 0 – 1 | Grass patches and blades close to the camera. |
| Mowing stripes | `MowingStripes` | 0 – 1 | Stadium-style stripes in the grass. |

### Neon trail

| Setting | Key | Range | What it does |
|---|---|---|---|
| Neon trail | `NeonTrail` | 0 – 3 (0) | Brightness of the light trail. 0 = off. Not recorded in replays. |
| Trail colour | `TrailColor` | colour | |
| Trail width (m) | `TrailWidth` | 0.05 – 1 (0.2) | |
| Tyre lines | `TrailTyres` | on / off (on) | Two lines, or one from the middle of the car. |
| Trail fade (s) | `TrailDuration` | 0 – 120 (0) | 0 keeps the trail for the whole run. A restart clears it, a respawn starts a new line. |
| Earlier runs kept | `TrailKeepRuns` | 0 – 5 (0) | 0: a restart clears the trail. 1 – 5: the trails of that many earlier runs stay after a restart, dimmer (the last at 60 %, the oldest at 60 % / n); a new map clears them all. |

### Cinematic

| Setting | Key | Range | What it does |
|---|---|---|---|
| Motion blur | `MotionBlur` | 0 – 1.5 | Blur from camera movement. 1 = one full frame of movement. Your car stays sharp in the chase camera. |
| Depth of field | `DepthOfField` | 0 – 1 | Background and foreground blur. |
| Focus distance (m) | `FocusDistance` | 0 – 200 | 0 = auto focus (where: *Focus on*). |
| Focus on | `DofAutofocus` | Middle, Car (Middle) | Where the auto focus sits (with *Focus distance* 0). *Middle* (0, default): what is in the middle of the screen, a little below the centre, as before. *Car* (1): your car, found where the game draws it (TrackMania Nations/United), wherever the replay camera puts it; the focus measures the depth of the car's pixels around that point, and a pillar in front of the car stays blurred. While the car is off screen, behind the camera or its position unknown (the log says so), the middle again. The focus moves there within about a quarter of a second, a camera cut jumps at once. A click in the photo mode still wins. |
| Max blur (px) | `BokehSize` | 2 – 24 | Largest blur size, at 1080p. |
| Cinematic in replays only | `CinematicOnlyInReplays` | on / off (on) | Motion blur and depth of field only in replays, intros and the video export, not while you drive. TrackMania 2: only in photo mode (its replays can't be told from driving yet). |
| Video format | `VideoCodec` | 0 – 3 (1) | What the replay editor's *Shoot video* writes. 1, *MP4 (H.264)*: plays and edits everywhere. 2, *MP4 (HEVC)* and 3, *MP4 (AV1)*: the same picture in a smaller file, heavier to edit and decode (AV1 needs an RTX 40, RX 7000, Intel Arc or newer, or ffmpeg). The sound goes in as AAC; the file lands next to the game's (same name, `.mp4`, in `Documents\TrackMania\Video`), no codec dialog. Without an encoder for the choice it falls back AV1 → HEVC → H.264 (the log says so). 0, *Game's AVI*: the game's AVI with its codec list, as before; if the encoder can't start, that is written instead. |
| Video quality | `VideoQuality` | 0 – 100 (80) | How much picture each second gets, calibrated with VMAF on a grainy 1440p60 replay: 80 ≈ 96 (hard to tell from the source), 92 ≈ 98 (for editing), 62 small. NVIDIA cards (driver R570 or newer, NVENC driven directly): a constant quality, CQ 24.5 at 80, about 61 Mbit/s at 1440p60 on that replay (less on calmer ones), with a cap at twice the rate below. Other cards (Media Foundation): an average bitrate with room for peaks, 0.31 bit per pixel and frame at 80, 70 Mbit/s at 1440p60, 39 at 1080p60; HEVC and AV1 0.9 of it. With ffmpeg its constant quality (x264 CRF 20.5 at 80). The log names what the encoder took; [the measurements](engine-video-export.md#the-encoders-srcvideoencodercpp-srcnvenccpp). |
| 10-bit video | `Video10Bit` | on / off (off) | HEVC and AV1 in 10 bits: no bands in skies, fog and glows, and a little smaller at the same quality. H.264 stays 8 bits (graphics cards have no 10-bit H.264); with *Blend samples in HDR* it gets a fine dither from the float image instead. Some editors and older players can't decode 10 bits. |
| Video encoder | `VideoEncoder` | 0 – 1 (0) | 0, *Graphics card*: on NVIDIA cards NVENC driven directly (the driver's own interface: the best quality per megabyte here, faster than the export renders); AMD, Intel or an NVIDIA driver older than R570 through Windows (Media Foundation), else Windows' own encoder; nothing to install. 1, *ffmpeg (software)*: x264, x265 or SVT-AV1 through your own `ffmpeg.exe` (in `Documents\TrackMania\TMVS` or on PATH; the mod brings none); for AMD and Intel cards the better choice, on NVIDIA no better than the card (x264 medium scored a little lower at the same rate). Without ffmpeg the graphics card's encoder is used. |
| ffmpeg preset | `VideoFfmpegPreset` | 0 – 2 (1) | With *ffmpeg*: 0 *Fast*, 1 *Balanced*, 2 *Best (slow)*: more time per frame, more quality in each megabyte (x264 faster, medium, slow: medium about keeps up with an export at 1440p and 2 × 2 samples, slow takes longer than the frames). |
| Video bitrate (Mbit/s) | `VideoBitrate` | 0 – 200 (0) | 0 = from *Video quality* and the video's size (recommended). Above 0: this average bitrate instead. |
| Best export quality | `VideoExportBest` | on / off (on) | The replay editor's video export: full effect quality (no auto quality), photo mode's depth of field, and the effects run on the video's own clock (exposure, focus, rain, lightning and clouds move as in the finished video, however slowly its frames render). With the export's *high quality* (supersampling) every sample gets the effects and TAA is off. Off: as before. TrackMania 2: the export is recognised by its frame read-backs; the video's frame rate is `VideoFps` in `maniaplanet.ini` (30), full quality and depth of field and motion blur as in a replay, untested in the game ([maniaplanet/README.md](../maniaplanet/README.md)). |
| Blend samples in HDR | `VideoOwnFrames` | on / off (on) | On the Video page together with *HDR10 video* as the dropdown *Dynamic range*: *SDR (classic)* is this off, *SDR (HDR blend)* on, *HDR10* on with *HDR10 video*. With an MP4 format the mod renders each video frame itself: every supersample and every motion blur sub-frame of *Shoot video*'s high quality is lit and summed in float, then bloom, exposure and the grade run once on the average, and the frame goes to the encoder in 10 bits or with a fine dither. Bright lights streak like on a real camera instead of a grey smear, no banding, and the effects cost once a frame instead of once a sample. The screen shows the last finished frame meanwhile. Needs *Best export quality*. Off (or with the effects off): the game's own 8-bit average, as before. |
| Shutter angle | `VideoShutter` | 45 – 360 (360) | With *Shoot video*'s motion blur: how much of each frame's time its sub-frames cover. 360 blurs over the whole frame (smoothest, as the game does); 180 is the film look, crisper; the sub-frames past the shutter are skipped, which also saves the mod's work on them. Needs *Blend samples in HDR*. |
| Lens depth of field | `VideoLensDof` | on / off (on) | The export's depth of field from the samples themselves: between them the camera moves over a lens (a disc), sheared so the focus plane stays put, and the sum blurs everything else like a real lens: true bokeh, no halos at edges. As strong as *Depth of field* and *Max blur* set it, focus from *Focus distance* / *Focus on*. Needs 16 or more samples per frame (*Shoot video*'s high quality 4 × 4 and up, or its motion blur); with fewer, the usual blur. The game's clock for its motion blur sub-frames runs in whole milliseconds: at 60 fps more than 4 × 4 samples share moments (the edges and the lens still gain, the motion blur no longer). |
| Video grain | `VideoGrain` | 0 – 1 (0.5) | How much of the look's film grain goes into the video. Grain is noise to the encoder and costs much of its bits; 0.5 keeps the feel at half the cost, 1 is the look as it is. |
| Video crop | `VideoAspect` | 0 – 2 (0) | 0 *As shot*; 1 *2:1*; 2 *2.39:1 (cinema)*: the video is cropped to that picture (the middle of the frame), the file holds only the picture and players add the bars. Set *Shoot video*'s size for the width you want. |
| HDR10 video | `VideoHdr` | on / off (off) | *Dynamic range* *HDR10* on the Video page (it also picks HEVC in 10 bits when H.264 was set). HDR10 (PQ, BT.2020): the tone curve runs on above white up to *HDR peak*, so lamps, the sun and glows stay brighter than the white of the image on an HDR screen. Needs HEVC or AV1 with *10-bit video* and *Blend samples in HDR*; an encoder that falls back to 8 bits writes SDR. YouTube makes an SDR copy for other screens. The LUT, the night lamps and the grain work on the image up to white, the light above it is added after them. The screen shows the frames cut to white during the export. |
| HDR peak (nits) | `VideoHdrPeak` | 400 – 4000 (1000) | Where the HDR tone curve ends and the brightest light the file's metadata names. |
| HDR paper white (nits) | `VideoHdrWhite` | 100 – 400 (203) | How bright the game's white (the SDR image's 1.0) is; 203 is the reference white of BT.2408. Higher brightens the whole picture and leaves less room for lights above it. |

### Image quality

| Setting | Key | Range | What it does |
|---|---|---|---|
| Temporal AA | `TAA` | on / off (on) | Smooths edges and flicker over several frames, and the steps of long shadows under a low light. Off in the *Performance* preset. A little softness on fast movement. |
| TAA jitter | `TAAJitter` | on / off (on) | Samples a different spot inside each pixel every frame, so TAA supersamples the edges. Off: fine grates can shimmer with it. |
| Edge smoothing | `FXAA` | on / off | Smooths edges within one frame, with SMAA or FXAA (below). Off: neither. |
| SMAA instead of FXAA | `SMAA` | on / off (on) | SMAA 1x finds the edges and blends along them: smoother long edges (railings, roofs, the car) and sharper textures than FXAA, about 0.2 ms more at 2560x1440. Off: FXAA, as before 1.4.0, and in the *Performance* preset. A `settings.ini` or preset file from before (with `FXAA`, without `SMAA`) keeps FXAA. Runs before the render scale's upscaling, on the scene's image. |
| Sharpening | `Sharpen` | 0 – 1 (0.8) | Contrast-adaptive sharpening (CAS): brings back detail after anti-aliasing. |
| Effect quality | `Quality` | Low (fast), Medium, High (High) | Samples for AO, shadows, long shadows, clouds, volumetric light, bounce light and the aurora. Low is clearly faster. |
| Auto quality | `AutoQuality` | on / off (on) | Below the target FPS it measures what every effect costs on your GPU and turns off just enough of them to get there, in one step: first what costs the most for the least look (volumetric light, GI, samples, reflections, clouds, TAA, edge smoothing, depth of field, lens flare and sharpening). The light shafts, the neon light and the long shadows always stay: they are the look (without the long shadows a stand's shadow on the road is only a light tint until the car is close, and photo mode, which auto quality never touches, shows it dark). It turns off as few as it can. An effect stays off at least 20 s and comes back only after a few seconds with room for it in a row; one that comes back and drops the frame rate again within a minute stays off for 5 minutes. Your settings stay as they are. The menu shows which effects are off right now. It changes nothing if the game itself (CPU, vsync) holds the frame rate back, or if even everything it may turn off couldn't reach the target (a target above what your PC reaches, such as 240 FPS at 83 FPS). It only decides during a race, not in the menus, and not in the first 20 s after a map start, a loading screen, alt-tab or photo mode (4 s after a hitch): the game is still compiling shaders and loading then. |
| Target FPS | `TargetFPS` | 30 – 240 (60) | The frame rate auto quality tries to hold. Lighting, shadows and bloom always stay on, so with heavy presets on a slow GPU it can end a little below. |
| Render scale (FSR) | `RenderScale` | 0.5 – 1 (1 = off) | The game draws its 3D image at this fraction of the screen's resolution (0.67 of 2560×1440 is 1715×965), every effect runs at that size, and AMD FSR 1 (edge-adaptive upscaling, then its sharpening) brings the shaded image back up to the screen before the HUD, which stays sharp. For when the graphics card is the limit: at 0.67 the effects cost about half, the game's own drawing less too (see [performance](performance.md#render-scale-fsr-1)). Costs some sharpness on fine detail and text; 0.77 is close to the full image, 0.5 visibly soft. Photo mode, photos, F12 captures and the video export always use the full resolution; a camera into part of the screen (MediaTracker editor, the replay menu's band) too. The log sums up once per map how the scaling went (`render scale (map):`) and says when the game draws something the scaling can't follow (then it stays at full size for that map). Needs *Game HDR*. TrackMania Nations/United. Machine setting, not part of a look. |
| FSR sharpening | `FsrSharpness` | 0 – 1 (0.8) | How much FSR sharpens the image it scaled up (its RCAS): 1 is AMD's strongest, 0 none. While the render scale is below 1 it takes the place of *Sharpening* (sharpened before the upscale, its halos would grow with the image). Machine setting. |

### Game integration

| Setting | Key | Range | What it does |
|---|---|---|---|
| Real night | `GameNight` | on / off (on) | With a night sky or the black hole over a day or sunset map, the game lights the map at midnight itself: its moon, fog and night sky, nothing of the sun baked into the image. The mod no longer has to take the sun out of a finished picture (where the sun had already turned surfaces white, there was no texture left). The map file, its replays and validation are untouched; only the time of day the game starts the map's lighting at changes. Switched while a map runs, the map is relit at once (if a map doesn't follow, it does from the next map on). |
| Game moonlight | `EngineMoon` | Game's, Black hole, Off (Black hole) | Night maps (their own night, or lit at midnight by *Real night*) under a sky with the black hole: the game's own moonlight comes from the side (on A01 it is horizontal, opposite the black hole). *Black hole*: the game takes the black hole's direction for its moon when it builds the map's light at load, so its own shadows, the glints on the blocks and the light on the car come from the black hole, complete and without popping. *Off*: the game's moon has no light; the sky light (ambient) and the lamps stay. Both also tell the mod that nothing of a sun is baked into the image (it doesn't darken the sides facing the black hole). Takes effect from the next map (the light is baked once per map; back to the menu and reload the map). Machine setting, not part of a look. TrackMania 2 can't move its moon: there *Game's* keeps it, *Black hole* is the shaders' version (*Black hole light* below), *Off* takes the moon's light and glint out, at once and under any night sky. |
| Black hole light (TrackMania 2) | `BlackHoleMoon` (`maniaplanet.ini`) | 0.25 – 2 (1; 0 = off) | What TrackMania 2's *Game moonlight* → *Black hole* does, in the mod's shaders only (ManiaPlanet's moon can't be moved without writing into the game): on night maps under the black hole the game's moon, its blue glint on chrome and the sculptures and its light, is taken out of the picture and the black hole lights the scene instead, warm, with the game's own shine on each material; 1 = as bright as the moon was. Its shadows are the game's moon shadows where the black hole stands near the moon (Horizon), else only short contact shadows. Chosen with *Game moonlight* (0 = not chosen, the default); its brightness on the Settings page, panel *ManiaPlanet*, while it is chosen. |
| Game sunset clouds | `GameSkyClouds` | on / off (off) | With a night sky over a sunset map, the game's sunset clouds still show through the stars and its sea still mirrors the sunset. Off: both go. |
| Game HDR | `GameHdr` | on / off (on) | The game draws its 3D image into a floating-point image instead of 8 bits a colour, so light brighter than white keeps its texture (car paint in the sun, floodlit concrete) and the mod sees how bright the game's light really is. Needed by *Game light sources*, *Render scale*, *Game normals* and *Night on game light*. F12 also saves it (`capture_NNN.hdr16`). |
| Game light sources | `EngineLights` | on / off (on) | The light sources are the ones the game draws as light (lamp materials, the glows it adds, start lights, the cars' lights, the neon borders) instead of guessed from bright pixels: floodlit grass, banners and car paint no longer glow or light their surroundings. *Neon strip glow*, *Floodlights*, *Lamp glare* and *Lamp haze glow* tune them. Needs *Game HDR*. |
| Clean highlights | `CleanHighlights` | on / off (on) | Night maps: the game's floodlit white comes out even. Off: the look of earlier versions, where the same white of the game became a patchwork of brighter and dimmer spots (frost-like patches on the start ramp). Day maps look the same either way. |
| Long mesh shadows | `CasterShadows` | on / off (on) | The mod keeps every static mesh the game draws (once it stood still for half a second) and draws them from the sun or the black hole into two shadow maps: a fine one around the camera (about 8 cm a texel, drawn again in the background as you drive on) and a coarse one over the whole track. The long shadows and the volumetric light's shafts then come from those maps instead of the height map: they don't pop with the camera, keep the meshes' real shape (overhangs, poles) and reach as far as the maps. Needs *Long shadows* (or *Volumetric light*). Where no map is complete yet (the first second of a map, after a light change), the height map's shadows stay. Fences and grids (see-through textures) cast their pattern with *Fence shadows* (below). Removing blocks in the editor starts the maps over. About 192 MB of video memory. |
| Fence shadows | `CasterAlphaTest` | on / off (on) | With *Long mesh shadows*: fences, grids, mesh panels and lattice towers (meshes the game draws with an alpha test, the holes cut out by their texture's alpha) are kept too and drawn into the shadow maps the way the game draws them: with its own shaders, the constants and textures of that draw and its alpha test. Their shadows have the holes of the mesh instead of none at all. Off: they cast no shadow from the maps (as before). Maps without such meshes look the same either way. Cost: nothing more while recording; drawing them into the maps about 1 µs of the graphics card per mesh (a few hundred meshes each time the map around the camera is drawn again). The log's `casters (...)` lines get a second line `alpha-tested: N meshes kept`. |
| Clean water | `CleanWater` | on / off (on) | The game's water (pools, rivers, the sea) mirrors only what it can really see, sharp. Off: from above, the reflection came out from behind the ramps and bridges over the water and mirrored their tops in blotches, softened by the dry track's reflection blur. Needs the water's height from the engine (Stadium, TMUF sea). |
| Far water reflections | `WaterFarReflections` | on / off (on) | With *Clean water*: the water's reflection is traced up to the screen's edge (up to 1.5 km) in even steps on screen, so the stands, floodlight masts, gates and ramps across a pool or the sea are mirrored in it. Off: as before, the trace ended after 120 m, and after only 40 m on water close to the camera: everything further away showed as plain sky in the water. What is off the screen (behind the camera, above its top edge) still can't be mirrored: the sky takes its place. Cost: about 0.05 ms at 1440p. |
| Overlapping blocks: last placed on top | `OverlapFix` | on / off (on) | TMUnlimiter maps: where two blocks overlap at the same height and have a face in the same plane, action blocks are always shown on top (engine off first, then boosters, then start, finish and checkpoints, so a booster mixed into a start block shows), otherwise the block placed last, instead of stripes of both that flicker as the camera moves. The mod pulls each such block a few steps of the depth buffer toward the camera, the later placed the more (up to 7 steps: under a millimetre at 10 m, centimetres at 100 m), so nothing real is moved. Only the game's own opaque drawing of the main camera is touched; shadows, reflections and glass are not. Works with the shaders switched off as well. Takes effect at once. Cost: none measurable. |
| Effects in replays | `ReadableGameDepth` | on / off (on) | Also makes the depth buffers of replays and the video export readable. Takes effect after a restart. |
| Replay menu shading | `ShadeMenuBars` | on / off (on) | At the end of a race the replay shrinks into a band between the menu's half-transparent bars, and the game draws nothing under the bars any more: they showed a stepped, blocky strip of every frame of the shrinking (shaded each on its own). On: the shaded replay fills them, mirrored at the band's edges. Off: black, as the game's own bars (under the half-transparent bars the mirror can look like the road reflecting the menu). Never leftovers. Rows the game clears itself stay as they are. |
| Turn off game FX | `DisableGameFX` | on / off (on) | Skips the game's own bloom and colour filter (launcher *Advanced*, FX post-processing) while the mod shades, so nothing glows twice. The launcher setting is left alone. The game asks only when it starts its effects, so a change shows from the next map on. |
| Turn off game MSAA | `DisableGameMSAA` | on / off (on) | Needed for the depth effects. Leave it on. Takes effect after a restart. |
| Game materials | `GameMaterials` | on / off (on) | While the game draws, the mod notes which material each pixel is (stencil buffer). Used for the TMUF sea (water by material, its height measured from it), Stadium water (only where the game draws its water, not on dirt at the same height), dirt (no dry-track mirror on it) and the debug view *Materials*. |
| Game normals | `GameGBuffer` | on / off (off) | Experimental. The game's own material shaders also write the normal of every surface they draw (blocks with their bump maps, the car, decorations) into a second target, next to the image. Where it holds the surface that is really seen there, ambient occlusion, sun and long shadows, reflections and the relighting take that normal instead of one guessed from the depth buffer: no stair steps on curved surfaces, the bumps of the blocks catch the light. Roads and flat ground keep the guessed normal (their shaders have none). Needs *Game HDR*. The shaders are changed when the game compiles them, at its start: switch it on, then restart the game; from then on it can be switched off and on while playing (off, the game draws exactly as before). F12 also saves the normals (`capture_NNN.nrm16`, `tools/gbuffer.py` shows them). |
| Night on game light | `GameLight` | on / off (off) | Not in the menu: only `GameLight=1` in `settings.ini` turns it on. Experimental. The game's own material shaders also write the light they lit each surface with (the baked lightmap and the lamps baked into it at night, the ambient light; the car's sun and ambient) into a third target, next to the image, the same way as *Game normals*. Under a night sky, where that light is stronger than the paint alone (above 1: only stacked lamp light gets there at night), the night's darkening and blue tint step back: the asphalt under a lamp and the floodlit stands stay as bright and as warm as the game drew them, everything else gets the night as before. In the image a white wall in the moonlight and dark asphalt under a lamp can be equally bright; their light is not. Only on night maps and maps lit at midnight (*Real night*), not where a sun is baked in. Needs *Game HDR* and *Game light sources*. The shaders are changed when the game compiles them, at its start: switch it on, then restart the game; from then on it can be switched off and on while playing. F12 also saves the light (`capture_NNN.lit16`, `tools/gbuffer.py --light` shows it). |
