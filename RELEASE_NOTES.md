## TM Vibrant Shaders 1.4.0

Real-time lighting, weather and skies for **TrackMania Nations & United Forever** and **TrackMania 2 (ManiaPlanet 4)**. This release replaces 1.4.0-preview.

---

### New: TrackMania 2 (ManiaPlanet 4) Support

**TM Vibrant Shaders now runs in ManiaPlanet 4: TrackMania² Stadium, Canyon, Valley and Lagoon**, natively on the game's Direct3D 11 renderer (tested in the game on Stadium). It provides the same mod experience with the same `F8` menu, presets and settings as in Nations and United Forever, built on what ManiaPlanet's engine knows: surface normals, material colours, sun shadow maps, motion vectors and HDR image.

![TrackMania 2 Stadium in photo mode's compare split: left the game, right the mod with the black hole sky](https://raw.githubusercontent.com/cheatoskar/TM-Vibrant-Shaders/{{TAG}}/docs/images/tm2-black-hole-split.jpg)

*TrackMania 2 Stadium, photo mode's compare split: left the game as it is, right with the mod (black hole sky).*

**What it does in TrackMania 2:**
- **Skies:** Clear sky, starry night, black hole, aurora, ring world, with the map darkened to a real night beneath them (*Real night*): the engine's sky light comes down to moonlight, lamps and neon stay lit.
- **Light:** Ambient occlusion and contact shadows, bounce light (GI) and neon light on the surroundings, from the engine's own normals. Lamps, light strips, screens and halos glow as the engine draws them (*Game light sources*).
- **Soft sun shadows** (on, 1°): sharp where a thing touches the ground, softer the farther they fall. Nothing in the game's memory changes; whether ManiaPlanet's anti-cheat minds the replaced shadow shader is not known (0 turns it off).
- **Atmosphere:** Haze and fog, light shafts, volumetric light through the engine's shadow maps, volumetric clouds, lens flare.
- **Weather:** Rain, snow, wet roads, puddles with reflections, lightning, drops on the lens, rain and howling storm sound.
- **Cinema:** Depth of field, motion blur and TAA with the engine's motion vectors; photo mode (`F6`) with the compare split (`Shift+F6`), also against a preset.
- **Neon trail** following the car's real position in the engine.
- **Author's Shader:** Looks stored in a map's comments are applied when the map loads; *Set as Author's Shader* works in the map editor.
- The interface stays sharp: the mod draws before the HUD and its tone map replaces the engine's.

**Install (TrackMania 2 / ManiaPlanet 4):**
1. Download **`TM-Vibrant-Shaders.zip`** (one zip for both games) and extract it.
2. Close ManiaPlanet and run **`TM-Vibrant-Shaders-Setup.exe`**.
3. Start ManiaPlanet **64-bit**. `F8` opens the menu, **`F5`** switches effects on/off (`F7` is ManiaPlanet's friends list).

---

### Highlights for TrackMania Nations Forever & United Forever

![The new F8 menu: every preset as a card with a picture](https://raw.githubusercontent.com/cheatoskar/TM-Vibrant-Shaders/{{TAG}}/docs/images/menu.jpg)

**New `F8` Menu**
- Every preset as an interactive card with a picture, brief description, and GPU load.
- Point at a preset for a live preview; click to apply; right-click to assign per map mood (Day, Sunset, Night).
- Dedicated pages for *Customize*, *Studio* (all settings), *Maps* (Author's Shader & mood defaults), and *Performance*.
- Save custom presets with live in-game thumbnails, or share a look as one line of text (*Copy share code* / *Paste share code*, works between both games).
- Hotkeys can be changed; *Reload shaders* moved to `Ctrl+F9`, so the mod works next to the 100% TMX + Bingo mod.

**Light from the game itself**
- *Game HDR* and *Game light sources* (on by default): lamps, floodlights, start lights and neon borders glow as the game draws them, nothing is guessed from bright pixels. Floodlit white stays even at night, grass and car paint no longer glow.
- **Long mesh shadows** from the game's own meshes, from the sun or the black hole; fences and grids cast their lattice.
- **Real night under night skies (on by default):** day and sunset maps under night skies are lit at midnight by the engine itself (moonlight and lamps, no baked sun); under the black hole the game's moon shines from it.

**Photo Mode (`F6`, automatic with free camera cam 7)**
- Full quality with extended draw distance: ambient occlusion, reflections, shadows, and bounce light reach 3x as far (long shadows cover 960m).
- **Compare split (`Shift+F6`):** the vanilla look or any preset on the other side of a movable line (`Ctrl+Shift+Wheel`), with both names in the photos.
- **Works even with shaders off (`F7`):** aperture / depth of field and CAS sharpening on the vanilla game look, with autofocus on the car.
- `F12` saves photos without HUD; `Shift+F12` saves high-resolution tiled photos.

**Performance and video**
- **Render scale with AMD FSR 1** (off by default): the game and the effects at a fraction of the screen's resolution, scaled up before the HUD; at 0.67 the effects cost about half.
- **MP4 video export:** the replay editor's *Shoot video* writes H.264 or HEVC with AAC sound, encoded by the graphics card, with the effects at full quality on the video's own clock.
- Auto quality measures every effect on your GPU and turns off just enough of them, never the long shadows, the light shafts or the neon light.

**Neon Trail & Audio**
- The neon trail tracks the car's true position directly from the engine: no flicker, no gaps, along banked roads and loops; earlier runs can stay, dimmer.
- Realistic storm audio: recorded winter storm wind for snowstorms.

**Known issues**
- Real night: the end-of-race replay still shows the map's own sunlight (the race and photo mode are at night).
- Night presets: some blue printed signs (the blue "d" of the Trackmania United banner) glow like neon strips; the game draws them with the same material as the strips.

**Install (TrackMania Nations Forever / United Forever):**
1. Download **`TM-Vibrant-Shaders.zip`** and extract it.
2. Run **`TM-Vibrant-Shaders-Setup.exe`** to install into TrackMania ModLoader or game folders.
3. In-game: `F8` opens menu, `F7` toggles shaders.

Full details in the [changelog](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/main/CHANGELOG.md).
