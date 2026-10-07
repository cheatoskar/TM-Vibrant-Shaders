# TM Vibrant Shaders

**Real-time lighting, weather and skies for TrackMania Nations Forever and United Forever.**

[![Latest release](https://img.shields.io/github/v/release/cheatoskar/TM-Vibrant-Shaders?label=download)](https://github.com/cheatoskar/TM-Vibrant-Shaders/releases/latest)
[![Build](https://github.com/cheatoskar/TM-Vibrant-Shaders/actions/workflows/build.yml/badge.svg)](https://github.com/cheatoskar/TM-Vibrant-Shaders/actions/workflows/build.yml)
![Game](https://img.shields.io/badge/game-TMNF%20%7C%20TMUF-orange)
![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-blue)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

![Horizon: the black hole lights the stadium through the haze](docs/images/black-hole-loop.jpg)

![Storm: rain on a wet Stadium track, the checkpoint mirrored in the road](docs/images/storm-checkpoint.jpg)

![Realistic: the start line in the late sun, with haze and soft shadows](docs/images/realistic-start.jpg)

A Direct3D 9 mod that lights the game again: shadows, ambient occlusion, light shafts, bounce light, glowing neon, rain with sound and lightning, snow, wet roads with reflections, water, volumetric clouds and new skies. It draws inside the game's renderer, before the HUD, so the interface stays sharp. It works while driving, in replays and in the video export.

**[Download the latest release](https://github.com/cheatoskar/TM-Vibrant-Shaders/releases/latest)** · [Install](#install) · [Features](#features) · [Presets](#presets) · [Author's Shader](#authors-shader) · [Controls](#controls) · [Known issues](#known-issues) · [Documentation](#documentation)

---

## Install

1. Download **`TM-Vibrant-Shaders.zip`** from the [latest release](https://github.com/cheatoskar/TM-Vibrant-Shaders/releases/latest) and extract the whole zip.
2. Close TrackMania and run **`TM-Vibrant-Shaders-Setup.exe`**.
3. Choose where to install:

   | Button | When to pick it |
   |---|---|
   | **Install for the TrackMania ModLoader** | You use the [ModLoader (TMLoader)](https://tomashu.dev/software/tmloader/). Afterwards tick *TM Vibrant Shaders* in your ModLoader profile. |
   | **Install into TrackMania Nations Forever** / **United Forever** | No ModLoader. The setup lists every TrackMania it finds, each with its own button, and puts a `d3d9.dll` next to `TmForever.exe`. |
   | **Install into another game folder** | Your game isn't in the list. Pick the folder that contains `TmForever.exe`. |

4. Start the game. Press **`F8`** for the menu.

Run the setup again to update or uninstall; your settings stay. Windows SmartScreen may warn because the setup isn't code-signed: *More info → Run anyway*, or install by hand. Manual install, updating and the recommended game settings: **[docs/install.md](docs/install.md)**. Deutsche Anleitung: **[docs/INSTALL_DE.md](docs/INSTALL_DE.md)**.

---

## Features

| | |
|---|---|
| **Light and shadow** | Ambient occlusion. Sun shadows, plus long-range shadows from a height map of the track that builds up while you drive, so a low sun casts long shadows from objects off screen too. Light shafts, volumetric light with shadows in the haze, bounce light (one bounce of coloured light), neon borders that light up the road around them. |
| **Weather** | Rain as particles that streak with the camera, splashes, ripples, streams down walls, drops on the lens. Wet roads and puddles with reflections. Thunderstorms with lightning bolts and flashes. Spray behind the car. |
| **Snow** | Falling snow up to a blizzard, flakes on the lens, snow on the ground in piles and drifts or as a closed blanket, sparkling in the sun. Your car keeps its paint and throws up powder snow. A wind sound for the storm. |
| **Sound** | Rain and thunder from real recordings, wind in a snow storm: a rain loop that follows the rain amount and ten different thunders, from close cracks to far rumbles. Each strike sounds a little different and comes from the side of the flash. |
| **Water** | Pools, rivers and the TMUF seas get waves, refraction and reflections. The mod takes the water height from the game, so nothing else turns into water. |
| **Sky** | Volumetric clouds. Clear sky, starry night with shooting stars, aurora, a black hole that bends light and lights the scene, and the Ring World: Saturn, Jupiter, an ice giant or an exotic planet, seen from far away, next to the rings or right above them. Night skies work on day maps. |
| **Camera** | HDR bloom, auto exposure, filmic tone curve. FXAA, temporal anti-aliasing, AMD CAS sharpening. Motion blur and depth of field that switch on by themselves in replays and the video export. |
| **Extras** | A neon light trail behind your car for the whole run. Track reflections up to a mirror finish. Grass detail and mowing stripes. |
| **Comfort** | `F8` menu with a *Simple* and an *Advanced* view, a search box and a reset arrow for every changed setting. Auto quality holds your frame rate. A live per-effect cost panel. A preset per map mood (day, sunset, night). Your own presets as shareable text files. Map authors can ship a look inside their map ([Author's Shader](#authors-shader)). |

![Original and Realistic](docs/images/compare-realistic.jpg)

![Original and Storm](docs/images/compare-rain.jpg)

| | |
|---|---|
| ![Sunset: original and Vibrant](docs/images/compare-sunset.jpg) | ![Day: original and Golden Hour](docs/images/compare-day.jpg) |
| ![Night: original and Neon](docs/images/compare-night.jpg) | ![On the rings of a gas giant, with a black hole in the sky](docs/images/ring-world.jpg) |
| ![Black hole](docs/images/black-hole.jpg) | ![Aurora](docs/images/aurora.jpg) |

---

## Presets

| Preset | Look |
|---|---|
| **Vibrant** | The default. Warm sun, rich colours, glowing borders, light shafts. |
| **Realistic** | TrackMania's own colours and sky. Only the light is new: shadows, ambient occlusion, bounce light, a little haze. |
| **Golden Hour** | Clear sky, low warm sun, long light shafts. |
| **Dreamy** | Soft pastel bloom, pink and teal. |
| **Neon** | Starry night, bright neon that lights up the whole track. |
| **Horizon** | A glowing black hole and a ringed planet over a dark stadium; the black hole lights the scene. |
| **Aurora** | Northern lights, green and teal. |
| **Rainy** | Overcast, wet track, puddles, rain. |
| **Storm** | Low clouds, pouring rain, deep puddles, lightning and thunder. |
| **Snowstorm** | A blizzard: driving snow, snow drifts on the ground, haze and the howl of the wind. Work in progress. |
| **Replay Cinema** | Film look with motion blur and depth of field, for replays and the video export. |
| **Competition** | Clarity first: AO and contact shadows, no haze or lens effects. |
| **Performance** | Vibrant with the expensive parts off. |

By default the mod picks **Vibrant** on day maps, **Golden Hour** on sunset maps and **Horizon** on night maps. Change it under *Which preset for which maps* in the menu. Every sky works with every preset. Change any value and the preset becomes *Custom*; save it under your own name in the *Advanced* view.

---

## Author's Shader

A map can bring its own look. When a map author stores a look in the map, everyone with the mod sees the map exactly that way. On the next map without one, your own look is back. Your `settings.ini` is never changed.

**As a player** there is nothing to do: the look loads with the map, takes priority over your preset per map mood, and the menu shows *Author's Shader* as the preset. Change any value and it becomes *Custom*; *Reset preset* brings the author's look back. To always keep your own look, untick *Load Author's Shaders* (*Advanced* view → *Author's Shader*).

**As a map author:**

1. Open your map in the map editor.
2. Build the look you want with the `F8` menu: start from a preset and change values.
3. In the *Advanced* view, open *Author's Shader* and press **Set as Author's Shader**.
4. Save the map. The look is now part of the map file and goes wherever the map goes.

The look is stored in the map's comments as a short code, `[TMVS:` followed by a few letters and digits per changed setting and `]`. Whatever else you wrote in the comments stays; pressing the button again replaces only the old code. *Copy code* puts the code on the clipboard, so you can also paste it into the comments yourself.

You can also write a readable line by hand into the comments, with the preset and the [setting keys](docs/settings.md#all-settings):

```
[TMVS] Preset=Snowstorm Snow=1.5 SnowCover=0.8 SkyMode=3
```

Preset names with spaces take an underscore (`Golden_Hour`). Machine settings (auto quality, target FPS, sound volume, the neon trail, *Only in replays and video export*, TAA jitter, the MSAA and depth options) are never taken from a map.

---

## Controls

| Key | Action |
|---|---|
| `F8` | Open or close the menu |
| `F7` | Shaders on or off, for a before/after look |
| `F12` | Save a screenshot and a frame capture (attach both to bug reports) |

![The F8 menu](docs/images/UI.png)

Settings, presets and the log live in `Documents\TrackMania\TMVS\`. Everything is saved automatically.

---

## Performance

- **Runs on:** any GPU with Shader Model 3.0, from Intel UHD 620 and GTX 750 upwards. Pick *Performance* and *Effect quality: Low* on weak GPUs.
- **Comfortable at 1080p:** GTX 1060, RX 580, Radeon 780M, Intel Arc 140V.
- **Everything on at 1440p:** RTX 3060, GTX 1080 Ti and faster.

**Auto quality** is on by default. It turns the effect quality down when your frame rate drops below the target and back up when there is room, without touching your settings. The *Performance* section in the menu shows what each effect costs on your GPU, live. Measured numbers and the most expensive settings: **[docs/performance.md](docs/performance.md)**.

---

## Compatibility

| | |
|---|---|
| TrackMania Nations Forever | Yes |
| TrackMania United Forever | Yes (the mod detects the game build and the map's environment) |
| TrackMania Nations ESWC, TrackMania 2 / 2020 | No |
| TrackMania ModLoader (TMLoader) | Yes, or without it as `d3d9.dll` |
| Other `d3d9.dll` mods (ReShade and similar) | Not as `d3d9.dll` in the same game folder. Together with the ModLoader install: untested |
| Windows | 10 and 11 |

## Known issues

- **Flickering:** some spots can flicker now and then. If you see it, press `F12` there and attach the screenshot and the capture to an [issue](https://github.com/cheatoskar/TM-Vibrant-Shaders/issues).
- **Snow is work in progress:** the snow on the ground doesn't follow the game's textures and surfaces well yet (it can lie across markings, kerbs and edges). The *Snowstorm* preset will be tuned further.
- **Water:** the horizontal stripes on pools are mostly gone in 1.3, but a few faint lines can still show in some views.

---

## Documentation

| Page | Contents |
|---|---|
| [Installation](docs/install.md) | Setup, manual install, ModLoader or game folder, updating, uninstalling, recommended game settings |
| [Settings](docs/settings.md) | Every setting in the menu, mood presets, your own presets |
| [Performance](docs/performance.md) | Hardware, measured costs, what to turn off for more FPS |
| [Troubleshooting](docs/troubleshooting.md) | No effects, no sound, low FPS, HUD problems, how to report a bug |
| [How it works](docs/how-it-works.md) | Hooks, depth buffer, the render passes, the two game builds |
| [Building](docs/building.md) | Build from source, the offline previewer, project layout |
| [Installation (Deutsch)](docs/INSTALL_DE.md) | Installation und Bedienung auf Deutsch |
| [Changelog](CHANGELOG.md) | What changed in each version |

---

## Development Note

This mod was developed with the help of [Claude Code](https://claude.com/claude-code). The ideas, presets, design and testing are my own, with AI assisting throughout the development process. If you find a bug, please report it in the [issues](https://github.com/cheatoskar/TM-Vibrant-Shaders/issues).

---

## Credits

- [Dear ImGui](https://github.com/ocornut/imgui) (MIT) for the menu, [minimp3](https://github.com/lieff/minimp3) (CC0) for the sound. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- Rain and thunder recordings from [freesound.org](https://freesound.org) (CC0) by Rubaoliva, loganzsound, seth-m, Fission9, TRP, Kinoton, Martineerok, kingsrow and bastipictures. See [sounds/CREDITS.md](sounds/CREDITS.md).
- Anti-aliasing and sharpening follow FXAA 3.11 (Timothy Lottes) and AMD FidelityFX CAS. The look is inspired by the Minecraft shader packs Sildur's Vibrant, BSL and IterationT; no code from them is used.
- TrackMania is a trademark of Ubisoft / Nadeo. This is a fan project and has nothing to do with them.

## License

© 2026 Oskar (cheatoskar). [MIT License](LICENSE). Third-party parts keep their own licenses ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
