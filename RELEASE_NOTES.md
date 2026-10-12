## TM Vibrant Shaders 1.4.0-preview

Real-time lighting, weather and skies for **TrackMania Nations & United Forever** and **TrackMania 2 (ManiaPlanet 4)**.

---

### New: TrackMania 2 (ManiaPlanet 4) Support

**TM Vibrant Shaders now runs in ManiaPlanet 4: TrackMania² Stadium, Canyon, Valley and Lagoon**, natively on the game's Direct3D 11 renderer. It provides the same mod experience with the same `F8` menu, presets and settings as in Nations and United Forever, built on what ManiaPlanet's engine knows: surface normals, material colours, sun shadow maps, motion vectors and HDR image.

![TrackMania 2 Stadium in photo mode's compare split: left the game, right the mod with the black hole sky](https://raw.githubusercontent.com/cheatoskar/TM-Vibrant-Shaders/v1.4.0-preview/docs/images/tm2-black-hole-split.jpg)

*TrackMania 2 Stadium, photo mode's compare split: left the game as it is, right with the mod (black hole sky).*

**What it does in TrackMania 2:**
- **Skies:** Clear sky, starry night, black hole, aurora, ring world, with the map darkened to a real night beneath them (*Real night under night skies*): the engine's sky light comes down to moonlight, lamps and neon stay lit.
- **Light:** Ambient occlusion and contact shadows, bounce light (GI) and neon light on the surroundings, from the engine's own normals.
- **Atmosphere:** Haze and fog, light shafts, volumetric light through the engine's shadow maps, volumetric clouds, lens flare.
- **Weather:** Rain, snow, wet roads, puddles with reflections, lightning, drops on the lens, rain and howling storm sound.
- **Cinema:** Depth of field, motion blur and TAA with the engine's motion vectors; photo mode (`F6`) with the compare split (`Shift+F6`).
- **Neon trail** following the car's real position in the engine.
- **Author's Shader:** Looks stored in a map's comments are applied when the map loads; *Set as Author's Shader* works in the map editor.
- The interface stays sharp: the mod draws before the HUD and its tone map replaces the engine's.

**Install (TrackMania 2 / ManiaPlanet 4):**
1. Download **`TM-Vibrant-Shaders-ManiaPlanet.zip`** and extract it.
2. Close ManiaPlanet and double-click **`Install.cmd`** (puts `d3d11.dll` next to `ManiaPlanet.exe`).
3. Start ManiaPlanet **64-bit**. `F8` opens the menu, **`F5`** switches effects on/off (`F7` is ManiaPlanet's friends list).

---

### Highlights for TrackMania Nations Forever & United Forever

![The new F8 menu: every preset as a card with a picture](https://raw.githubusercontent.com/cheatoskar/TM-Vibrant-Shaders/v1.4.0-preview/docs/images/menu.jpg)

**New `F8` Menu**
- Every preset as an interactive card with a picture, brief description, and GPU load.
- Point at a preset for a live preview; click to apply; right-click to assign per map mood (Day, Sunset, Night).
- Dedicated pages for *Customize*, *Studio* (all settings), *Maps* (Author's Shader & mood defaults), and *Performance*.
- Save custom presets with live in-game thumbnails.

**Photo Mode (`F6`, automatic with free camera cam 7)**
- Full quality with extended draw distance: ambient occlusion, reflections, shadows, and bounce light reach 3x as far (long shadows cover 960m).
- **Compare split (`Shift+F6`):** splits the screen between vanilla and modded look, movable with `Ctrl+Shift+Wheel`.
- **Works even with shaders off (`F7` / `F5`):** aperture / depth of field and CAS sharpening can be used on the vanilla game look with immediate autofocus on the car.
- `F12` saves photos without HUD; `Shift+F12` saves high-resolution tiled photos.

**Night Lighting**
- **Real night under night skies (on by default):** day and sunset maps under night skies are lit at midnight by the engine itself (moonlight and lamps, no baked sun).
- Sunset clouds and orange horizon rings are hidden under night skies.
- *Floodlit surfaces at night* prevents floodlit stands from blowing out to flat white.

**Neon Trail & Audio**
- The neon trail tracks the car's true position directly from the engine: seamless ribbon with no gaps at joints or fans around a stopped car.
- Realistic storm audio: recorded winter storm wind for snowstorms.

**Install (TrackMania Nations Forever / United Forever):**
1. Download **`TM-Vibrant-Shaders.zip`** and extract it.
2. Run **`TM-Vibrant-Shaders-Setup.exe`** to install directly or into TrackMania ModLoader.
3. In-game: `F8` opens menu, `F7` toggles shaders.

Full details in the [changelog](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/main/CHANGELOG.md).

<details>
<summary>SHA-256 checksums</summary>

| File | SHA-256 |
|---|---|
| `TM-Vibrant-Shaders.zip` | `42306f3d6155c002332237265b579cfd72401c4a2246f4db1cd9a716adfaa41f` |
| `TMVibrantShaders.dll` | `4069c934afdafda76807698f4f0d4acd82e7af3bfa1fe0e9f8f99f5bea6f7083` |
| `TM-Vibrant-Shaders-ManiaPlanet.zip` | `e2bbe392a9dc56a2fe513e967441a905caac8e39d766b3ff52d72820aff13cb2` |
| `d3d11.dll` | `b417f377042105519147e3156de3f2a90504d6db7525314210b88ae28c615cfe` |

Check a download in PowerShell: `Get-FileHash .\TM-Vibrant-Shaders.zip -Algorithm SHA256`
</details>
