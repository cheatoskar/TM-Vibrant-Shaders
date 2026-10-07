## TM Vibrant Shaders 1.3

Real-time lighting, weather and skies for TrackMania Nations Forever and United Forever.

This release brings **snow**, the **Author's Shader** (a look that travels with a map), a black hole that lights the space skies, a searchable menu and fixes for water and long shadows.

![Horizon: the black hole lights the stadium through the haze](https://raw.githubusercontent.com/cheatoskar/TM-Vibrant-Shaders/v1.3.0/docs/images/black-hole-loop.jpg)

### Download and install

**`TM-Vibrant-Shaders.zip`** is all you need. Unzip it and run **`TM-Vibrant-Shaders-Setup.exe`**. You can install:
- **for the [TrackMania ModLoader](https://tomashu.dev/software/tmloader/)**, then tick the mod in the ModLoader, or
- **into TrackMania Nations Forever or United Forever without the ModLoader**: the setup lists every TrackMania it finds, each with its own button, and adds a `d3d9.dll` next to `TmForever.exe`.

Updating: run the new setup, it replaces the old version. Your settings and presets in `Documents\TrackMania\TMVS` stay.

In the game: `F8` opens the menu, `F7` switches the shaders on and off. For the best result set *Antialiasing: off* and *Anisotropic filtering: 16x* in TrackMania's options.

### What's new

**Snow**
- Falling snow up to a blizzard: flakes that streak past the camera and drift with the wind, flakes on the lens, a white veil in the distance.
- Snow on the ground: piles and drifts with bare patches between them, or a closed blanket at full cover. It sparkles in the sun and gathers on walls that face the wind.
- Your car keeps its paint, powder snow flies up behind it, and the storm has its own wind sound.
- New preset **Snowstorm**.

**Author's Shader**
- Map authors can store their look in the map: build the look, open the map in the editor, press *Set as Author's Shader* (Advanced menu) and save the map. Everyone with the mod sees the map that way; on the next map their own look is back. How it works: [README](https://github.com/cheatoskar/TM-Vibrant-Shaders#authors-shader).

**Sky**
- The black hole lights the space skies: light shafts, shadows, flare and glow come from it.
- Move the black hole and the planet anywhere in the sky; size 0 turns either off.
- Smoother Ring World planets and sharper rings, clouds that move with the wind, adjustable aurora speed.

**Menu and presets**
- Search box for every setting, a reset arrow next to each changed value, coloured sections.
- Switching presets glides over a second instead of jumping.
- Presets tuned. *Rainy Day* is now **Rainy**, **Horizon** uses the black hole sky.

**Fixes**
- Far fewer horizontal stripes on pools and the sea seen from mid distance.
- No more black rectangles in the snow around the car.
- Long-range shadows no longer leave blotches that slide with the camera.
- `F12` captures on maps with water are complete again.

### Known issues

- Some spots can flicker now and then. If you see it, press `F12` there and attach the screenshot and the capture to an [issue](https://github.com/cheatoskar/TM-Vibrant-Shaders/issues).
- Snow is work in progress: the snow on the ground doesn't follow the game's textures and surfaces well yet.
- Water: the stripes on pools are mostly gone, but a few faint lines can still show in some views.

Full details in the [changelog](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/main/CHANGELOG.md).
