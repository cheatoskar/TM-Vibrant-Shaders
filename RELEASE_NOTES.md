## TM Vibrant Shaders 1.4.0

Lighting, weather and skies for **TrackMania Nations & United Forever**, and from this version on also for **TrackMania 2 (ManiaPlanet 4)**. This replaces 1.4.0-preview and the release candidates.

---

### TrackMania 2

The mod now runs in ManiaPlanet 4 (Stadium, Canyon, Valley, Lagoon) on its Direct3D 11 renderer, with the same `F8` menu, presets and settings. I tested it in the game on Stadium. It builds on what ManiaPlanet's engine already has: surface normals, material colours, its sun shadow maps, motion vectors and an HDR image.

![TrackMania 2 Stadium in photo mode's compare split: left the game, right the mod with the black hole sky](https://raw.githubusercontent.com/cheatoskar/TM-Vibrant-Shaders/{{TAG}}/docs/images/tm2-black-hole-split.jpg)

*TrackMania 2 Stadium in photo mode's compare split: the game on the left, the mod with the black hole sky on the right.*

- **Skies:** clear sky, starry night, black hole, aurora and ring world. Under a night sky the map gets a real night (*Real night*): the engine's sky light drops to moonlight, lamps and neon stay lit.
- **Light:** ambient occlusion, contact shadows, bounce light and neon light from the engine's normals. Lamps, light strips, screens and halos glow the way the engine draws them (*Game light sources*).
- **Soft sun shadows** (on, 1°): sharp where something touches the ground, softer the further they fall. Nothing in the game's memory is changed, but the mod does draw the shadow pass with its own shader, and I don't know whether ManiaPlanet's anti-cheat cares about that. Set it to 0 to turn it off.
- Haze, light shafts, volumetric light through the shadow maps, volumetric clouds and lens flare.
- Rain, snow, wet roads and puddles, lightning, drops on the lens, rain and storm sound.
- Depth of field, motion blur and TAA from the engine's motion vectors; photo mode (`F6`) with the compare split (`Shift+F6`).
- The neon trail follows the car's real position, and Author's Shaders load from the map's comments.

**Install:** download `TM-Vibrant-Shaders.zip` (one zip for both games), close ManiaPlanet and run `TM-Vibrant-Shaders-Setup.exe`. Start ManiaPlanet **64-bit**. `F8` opens the menu, **`F5`** turns the effects on and off (`F7` is the friends list there).

---

### Nations & United Forever

![The F8 menu: every preset as a card with a picture](https://raw.githubusercontent.com/cheatoskar/TM-Vibrant-Shaders/{{TAG}}/docs/images/menu.jpg)

**A new `F8` menu.** Every preset is a card with a picture and its GPU cost. Point at one and the game shows it, click to use it, right-click to use it only on day, sunset or night maps. *Customize* has the everyday sliders, *Studio* has all of them. Your own presets get a screenshot of the game when you save them, and a look can be shared as one line of text (*Copy share code*), which also works between the two games. Hotkeys can be changed.

**Light from the game itself.** The mod now gets the game's image before it is clipped to white (*Game HDR*) and takes lamps, floodlights, start lights and neon borders from what the game draws (*Game light sources*). No more glowing grass or car paint at night, and floodlit white stays even. Neon borders and dirt are recognised by the game's materials instead of their colour.

**Long shadows from the game's own meshes**, from the sun or the black hole; fences and grids cast their lattice. **Real night:** put a night sky over a day map and the game lights it at midnight itself.

**Photo mode** (`F6`, also turns on with the free camera): no HUD, full quality, and the effects reach three times as far. `Shift+F6` splits the screen between the plain game (or another preset) and your look. `F12` saves a photo, `Shift+F12` a high-res one.

**Image quality.** SMAA instead of FXAA, TAA with jitter on by default, sharpening at 0.8. *Render scale (FSR)* draws the game and the effects at a lower resolution and scales them up with AMD FSR 1 before the HUD; at 0.67 the effects cost about half.

**Video.** The replay editor's *Shoot video* writes MP4 (H.264 or HEVC, AAC sound) through the graphics card, with every effect at full quality.

**Auto quality** measures what each effect costs on your GPU and turns off only as many as it has to. The long shadows, the light shafts and the neon light always stay.

**Also new:** the neon trail follows the car's real position (no flicker or gaps, also through loops), rain and snow stay out from under roofs, the snowstorm has recorded wind, and colour looks can be loaded from `.cube` files.

**Works with CenterSpeed.** The speedometer mod and this one now run together. With both loaded, this mod's menu moves to `F5`, because CenterSpeed uses `F8`.

A lot got fixed on the way: the water's reflections and the water under TAA, replays lit like the race, dirt that mirrored or turned into water, the bars of the replay menu, hangs after alt-tab and stalls while saving the settings. Everything is in the changelog.

---

### Install (Nations & United Forever)

**With the ModLoader, updates itself:** close the ModLoader, open `%LOCALAPPDATA%\TMLoader\config.yaml` with Notepad and add this line under `servers:`:

```yaml
  - 'https://cheatoskar.github.io/TM-Vibrant-Shaders/modloader/'
```

Then tick *TM Vibrant Shaders* in your profile. New versions arrive on their own from then on.

**With the setup:** download `TM-Vibrant-Shaders.zip`, extract it and run `TM-Vibrant-Shaders-Setup.exe`.

In the game `F8` opens the menu and `F7` turns the effects on and off. Step by step: [installation guide](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/main/docs/install.md) ([Deutsch](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/main/docs/INSTALL_DE.md)).

All changes: [changelog](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/main/CHANGELOG.md).
