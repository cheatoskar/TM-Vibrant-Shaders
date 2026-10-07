# Settings

[Back to the README](../README.md)

Open the menu with `F8`. Everything you change applies at once and is saved automatically.

## The menu

**Top:** the shaders on or off (`F7`), *Simple* or *Advanced* view, the preset, and *Preset per map mood*.

**Simple view.** The everyday settings, in four sections that start collapsed. A section you open stays open.

| Section | Settings |
|---|---|
| Sky | Sky, sky rotation, stars, aurora movement, planet, planet size, view, planet direction and height, black hole size, direction and height (only what the chosen sky uses) |
| Look | Shadows, light shafts, glow, neon light, brightness, colour, motion blur |
| Weather | Rain, snow, snow on the ground, wet roads, clouds, lightning, drops on the lens, spray, weather sound, water, reflections, reflection blur |
| Performance | Auto quality, target FPS, effect quality |

**Advanced view.** Every setting below, grouped as in this page, plus *Save as preset*, *Delete*, *Reset preset*, the measured cost of each effect (*Performance*) and the [Author's Shader](#authors-shader). Settings that do nothing with your current choices are hidden; for example, the planet settings only appear with the Ring World sky. The search box at the top finds any setting across all sections.

A small arrow appears next to every value you changed: click it to set that value back to the preset's. Switching presets glides over about a second.

## Presets per map mood

The mod reads the colour of the game's sun when a map loads and sorts the map into day, sunset (and sunrise) or night. With *Preset per map mood* ticked, it switches to the preset you chose for that mood. The defaults are **Vibrant** for day, **Golden Hour** for sunset and **Horizon** for night. Change them under *Which preset for which maps*, or pick a preset on a map of that kind: it becomes the preset for that mood.

While your preset is *Custom* (you changed a value and didn't save it), a map change never replaces your tweaks. A map with an [Author's Shader](#authors-shader) wins over the mood preset.

## Your own presets

1. Start from the preset closest to what you want.
2. Switch to *Advanced* and change values. `F7` shows the original image for comparison.
3. Type a name under the preset list and press **Save as preset**.

Presets are text files in `Documents\TrackMania\TMVS\presets\`, one `.ini` per preset, with one line per setting (`Key=Value`, the keys are listed below). Send a file to a friend and they drop it into the same folder; it appears in their list on the next game start. Settings that belong to your installation rather than to a look are not part of a preset and stay when you switch: auto quality and target FPS, the sound volume, the neon trail, *Only in replays and video export*, TAA jitter and the MSAA and depth options.

## Author's Shader

A look stored in a map's comments: everyone with the mod sees the map that way, and on the next map their own look is back. Their `settings.ini` is never changed.

- **Load Author's Shaders** (`UseMapLooks` in `settings.ini`): on by default. Untick it to always keep your own look.
- **Set as Author's Shader**: stores the current look in the map open in the editor. Save the map to keep it. Whatever else is in the comments stays; an older code is replaced.
- **Copy code**: puts the code (`[TMVS:...]`) on the clipboard, to paste into the comments yourself.

A readable line typed into the comments works too: `[TMVS] Preset=Snowstorm Snow=1.5 SnowCover=0.8`, with the keys below. Preset names with spaces take an underscore (`Golden_Hour`). The settings that aren't part of a preset (see above) are never taken from a map.

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
| Neon light on surroundings | `NeonLight` | 0 – 2 | How much the coloured borders and signs light up the road and walls around them. |
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
| Light shafts / length | `GodRays`, `GodRayDecay` | 0 – 2, 0.9 – 0.995 | Rays around the visible sun. |
| Volumetric light | `VolumetricLight` | 0 – 2 | Sunlit haze with the shadows of the track's structures in it, also with the sun behind you. |

### Weather and surfaces

| Setting | Key | Range | What it does |
|---|---|---|---|
| Wet roads | `Wetness` | 0 – 1 | Darker, glossy, reflective track. |
| Rain | `Rain` | 0 – 2 | Falling drops, splashes, ripples. 2 = downpour. |
| Snow | `Snow` | 0 – 2 | Falling flakes that drift with the wind, a snow veil in the distance, haze. 2 = blizzard. The storm's wind sound grows with the snowfall and the wind. |
| Snow on the ground | `SnowCover` | 0 – 1 | Snow on everything that faces up: piles and drifts with bare ground between them, about half the ground at 0.5, a closed blanket at 1. Your car keeps its paint. |
| Puddles | `Puddles` | 0 – 1 | Standing water on flat ground (needs wet roads). |
| Lightning | `Lightning` | 0 – 1 | How often lightning strikes: a bolt in the sky, a flash over the track, thunder after a delay that matches the distance. |
| Drops and flakes on the lens | `LensDrops` | on / off | Rain drops that run down the screen, or snow flakes, only while it rains or snows. |
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
| Focus distance (m) | `FocusDistance` | 0 – 200 | 0 = auto focus on the middle of the screen. |
| Max blur (px) | `BokehSize` | 2 – 24 | Largest blur size, at 1080p. |

### Neon trail

| Setting | Key | Range | What it does |
|---|---|---|---|
| Neon trail behind the car | `NeonTrail` | 0 – 3 | Brightness of the light trail. 0 = off. Not recorded in replays. |
| Trail colour | `TrailColor` | colour | |
| Trail width (m) | `TrailWidth` | 0.05 – 1 | |
| Two lines from the rear tyres | `TrailTyres` | on / off | Two lines, or one from the middle of the car. |
| Fade after (s) | `TrailDuration` | 0 – 120 | 0 keeps the trail for the whole run. A restart clears it, a respawn starts a new line. |

### Bloom and lens

| Setting | Key | Range | What it does |
|---|---|---|---|
| Bloom | `Bloom` | 0 – 0.4 | Soft glow around bright parts. |
| Bloom radius | `BloomRadius` | 0.3 – 1.2 | Wider or tighter glow. |
| Light source intensity | `HighlightBoost` | 0 – 8 | How bright lamps and neon become in HDR. |
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
| Temporal anti-aliasing | `TAA` | on / off | Smooths edges and flicker over several frames. A little softness on fast movement. |
| TAA: sub-pixel jitter | `TAAJitter` | on / off | Samples a different spot inside each pixel every frame, so TAA supersamples the edges. Off by default: fine grates can shimmer with it. |
| Sharpening (CAS) | `Sharpen` | 0 – 1 | Brings back detail after anti-aliasing. |
| Effect quality | `Quality` | Low, Medium, High | Samples for AO, shadows, long shadows, clouds, volumetric light, bounce light and the aurora. Low is clearly faster. |
| Auto quality | `AutoQuality` | on / off | Turns the effect quality down when the frame rate drops below the target, and back up when there's room. |
| Target FPS | `TargetFPS` | 30 – 240 | The frame rate auto quality tries to hold. |
| Disable game MSAA | `DisableGameMSAA` | on / off | Needed for the depth effects. Leave it on. Takes effect after a restart. |
| Effects in replay / video export | `ReadableGameDepth` | on / off | Also makes the depth buffers of replays and the video export readable. Takes effect after a restart. |
