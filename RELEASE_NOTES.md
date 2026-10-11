## TM Vibrant Shaders 1.4.0-preview

Real-time lighting, weather and skies for TrackMania Nations Forever and United Forever.

**A preview of 1.4.0.** It brings a new menu, a photo mode, real nights under night skies and a switch for the map's MediaTracker clips. The work on the game's own rendering (materials, lights) follows in 1.4.0. Please report what looks wrong (`F12` there, then an [issue](https://github.com/cheatoskar/TM-Vibrant-Shaders/issues) with the screenshot and the capture).

![Horizon: the black hole lights the stadium through the haze](https://raw.githubusercontent.com/cheatoskar/TM-Vibrant-Shaders/v1.4.0-preview/docs/images/black-hole-loop.jpg)

### Download and install

**`TM-Vibrant-Shaders.zip`** is all you need. Unzip it and run **`TM-Vibrant-Shaders-Setup.exe`**. You can install:
- **for the [TrackMania ModLoader](https://tomashu.dev/software/tmloader/)**, then tick the mod in the ModLoader, or
- **into TrackMania Nations Forever or United Forever without the ModLoader**: the setup lists every TrackMania it finds, each with its own button, and adds a `d3d9.dll` next to `TmForever.exe`.

Updating: run the new setup, it replaces the old version. Your settings and presets in `Documents\TrackMania\TMVS` stay.

In the game: `F8` opens the menu, `F7` switches the shaders on and off. For the best result set *Antialiasing: off* and *Anisotropic filtering: 16x* in TrackMania's options.

### What's new

**New menu**
- Every preset as a card with a picture, a line about it and its GPU load. Point at one and the game shows that look; click to use it, right click to make it the preset for day, sunset or night maps.
- *Customize* for the main settings, *Studio* for every setting, *Maps* for the preset per map type and the Author's Shader, *Performance* for what each effect costs.
- Your own presets get a picture of the game when you save them; share the .ini and .jpg.
- The menu tells you when a new version is out (can be switched off).

**Photo mode** (`F6`, on by itself with the free camera)
- No HUD, full quality, shadows and reflections reach three times as far.
- `F12` saves a photo without the HUD, `Shift+F12` a larger one from tiles. Middle click sets the focus, `Ctrl`+wheel the depth of field, `Shift`+wheel zooms.
- **Compare split** (`Shift+F6`): one side of a line shows the game without the mod, the other with it, also in photos. `Ctrl`+`Shift`+wheel moves the line.

**Night**
- **Real night under night skies** (experimental, off by default, on the *Settings* page): with a night sky or the black hole over a day or sunset map, the game lights the map at midnight itself. No more baked sunlight under the stars. Switching it in the menu relights the map at once. The map, its replays and validation stay as they are.
- Under a night sky the game's sunset clouds, the sunset ring around TMUF Island and the sunset in the sea are gone (*Keep the game's clouds under night skies* brings them back for photos).
- *Floodlit surfaces at night*: floodlit stands and lit roads no longer burn out to flat white; lamps and neon stay bright.
- The sun's halo and lens flare fade out under night skies.

**MediaTracker**
- **`Shift+F7`** switches the map's MediaTracker clips while driving on or off (forced cameras, black screens, texts). The map itself is not changed.
- In the MediaTracker editor only the 3D view is shaded, not the panels around it.

**Light, shadow and water**
- Long shadows without bright boxes around the car, without saw teeth and without grain.
- Reflections while driving as clean as in photo mode.
- TMUF Island: the sea is water now, with waves and clean reflections. Rally roads no longer turn into a mirror.
- Water mirrors the sky you picked (planets, the black hole) where nothing else is reflected. Stadium dirt at water height no longer turns into water.
- Colour looks from `.cube` LUT files (*Customize* > *Colour look*).
- *Highlight roll-off* sets where the tone curve starts to bend.

**Weather**
- Rain and snow stay outside: nothing falls under roofs and bridges or behind walls, and the lens dries under a wide roof.
- Snowstorm: thicker haze, no snow on the ground by default.

**Other**
- Auto quality measures every effect on your GPU and turns off just enough of them in one step. It no longer turns everything down when the game itself limits the frame rate.
- The game's own bloom and colour filter are turned off while the mod shades (they glowed over the mod's image a second time).
- *Picked preset applies to all maps of this type* is off by default.

### Known issues

- United Forever Island at night: headlights on very bright blocks burn out to white (the game draws them that way).
- Floodlit stadiums at night keep part of the game's burned-out white (*Floodlit surfaces at night* sets how much).
- Once, on Stadium A01, the game played a loud noise when the map loaded (with *Real night under night skies* on; it didn't come back in later tests, the cause is unknown). The setting stays off by default. If it happens to you, please send `tmvs.log` and `tmvs.prev.log` from `Documents\TrackMania\TMVS`.
- If something flickers or looks wrong, press `F12` there and attach the screenshot and the capture to an [issue](https://github.com/cheatoskar/TM-Vibrant-Shaders/issues).

Full details in the [changelog](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/main/CHANGELOG.md).
