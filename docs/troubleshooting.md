# Troubleshooting

[Back to the README](../README.md)

Most problems show up in the log: `Documents\TrackMania\TMVS\tmvs.log`. It is rewritten on every game start, so look at it right after the problem happened.

## Nothing changes in the game

- **Is it loaded?** Press `F8`. No menu means the mod isn't running.
  - ModLoader install: is *TM Vibrant Shaders* ticked in the profile you start?
  - Game folder install: is `d3d9.dll` next to `TmForever.exe` of the game you start? Steam and the standalone version are different folders.
- **Menu opens, but no effects:** check that *Enabled* is ticked (`F7` toggles it) and look at the menu for a red message.
  - *Depth buffer unavailable*: the log has a line starting with `depth:`. The usual cause is the game's antialiasing; the mod turns it off, but some drivers need a game restart for that.
  - `INTZ depth textures are not supported by this driver`: the GPU driver doesn't support readable depth buffers. Update the driver. Without it the lighting effects can't work.

## The effects cover the HUD

The menu shows *Unknown game build: effects also apply to the HUD*. The mod knows the TmForever.exe of TrackMania Nations Forever and United Forever (version 2.11.26). Any other executable, for example a patched or very old one, runs without the engine hooks: the effects then go over the whole picture, and replay detection, respawn detection and water heights don't work. The log says `engine: unknown TmForever.exe build`.

## No rain or thunder sound

- The sound only plays with rain or lightning on, while you are in a race (not in menus) and the game window is in front.
- Volume: *Rain & thunder sound* in the menu, default 0.45, up to 2. It is the mod's own output: the game's sound and music sliders don't affect it. The Windows volume mixer does (entry *TmForever*).
- The log shows where the sound goes: `audio: weather sound started (device "...", mixer volume ...%)`, and every 20 seconds the level that goes out (`audio: output level`). Every thunder is listed with `audio: thunder`.

## Low frame rate

- Keep **Auto quality** on and set the target FPS you want.
- Pick the **Performance** preset or set *Effect quality* to Low.
- **Advanced → Performance** shows what each effect costs on your GPU. Turn off the biggest ones. See [performance.md](performance.md).

## Edges look more jagged than before

The game's own antialiasing (MSAA) is switched off on purpose: the depth effects need a single-sample depth buffer. FXAA and TAA replace it. Set anisotropic filtering to 16x in the game options for sharp roads in the distance.

## Fine patterns shimmer

Grates and thin lines can shimmer with *TAA: sub-pixel jitter*. It is off by default; if you turned it on, turn it off again (Advanced → Image).

## Blotchy shadows

Long-range shadows build up a height map of the track while you drive. It starts over on every restart and respawn. If you still see odd shadow blotches somewhere, take an `F12` capture there and report it.

## Water in the wrong place

Water is found by the height the game gives. On a map that changes heights or uses water in unusual ways it can go wrong. Lower *Water* in the weather settings, or set it to 0, and report the map.

## SmartScreen or antivirus warning

The setup isn't code-signed, so SmartScreen warns about any new version. Click *More info → Run anyway*, or install by hand (see [install.md](install.md#by-hand)). Each release lists SHA-256 checksums of all files.

## Reporting a bug

Open an issue on [GitHub](https://github.com/cheatoskar/TM-Vibrant-Shaders/issues) with:

1. What you did and what you saw.
2. Game (Nations or United Forever), install type (ModLoader or game folder), GPU.
3. `Documents\TrackMania\TMVS\tmvs.log` from that session.
4. If it's visual: press `F12` at the moment it happens and attach the `screen_*.bmp` and `capture_*.tmcap` from the same folder. The capture holds the frame, the depth buffer and the camera, so the problem can be reproduced outside the game.
