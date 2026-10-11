# Installation

[Back to the README](../README.md) · [Deutsch](INSTALL_DE.md)

## What you need

- TrackMania Nations Forever or TrackMania United Forever, on Windows 10 or 11.
- A GPU with Shader Model 3.0 (practically every GPU since 2008).
- Optional: the [TrackMania ModLoader (TMLoader)](https://tomashu.dev/software/tmloader/).

## ModLoader or game folder?

There are two ways to load the mod. Use one of them, not both.

| | ModLoader | Game folder (`d3d9.dll`) |
|---|---|---|
| How it loads | The ModLoader injects the mod when it starts the game | Windows loads `d3d9.dll` from the game folder; the mod passes every Direct3D call on to the real one |
| Switch it off | Untick it in the ModLoader profile | `F7` in game, or uninstall |
| Other mods | Combines with other ModLoader mods | Can't share the folder with another `d3d9.dll` (ReShade and similar) |
| Start the game | Through the ModLoader | As usual, launcher or desktop shortcut |
| Good for | People who already use the ModLoader | Everyone else |

## With the setup

1. Download **`TM-Vibrant-Shaders.zip`** from the [latest release](https://github.com/cheatoskar/TM-Vibrant-Shaders/releases/latest).
2. Extract the whole zip (right-click, *Extract All*). The setup needs the `TM Vibrant Shaders` folder next to it.
3. Close TrackMania and run **`TM-Vibrant-Shaders-Setup.exe`**. It shows what is installed and offers:
   - **Install for the TrackMania ModLoader.** Copies the mod into `%LOCALAPPDATA%\TMLoader\database\TmForever\products\`. Then open the ModLoader, tick *TM Vibrant Shaders* in your profile and start the game. If the ModLoader isn't installed, the setup offers its download page.
   - **Install into TrackMania Nations Forever** or **Install into TrackMania United Forever.** One button per game the setup finds (it looks at the Windows uninstall entries, Steam and the usual folders). It copies the mod as `d3d9.dll` next to `TmForever.exe`. A `d3d9.dll` that was there before is kept as `d3d9.dll.tmvs-backup` and restored when you uninstall.
   - **Install into another game folder.** For a game the setup didn't find. Pick the folder that contains `TmForever.exe`.
   - **Uninstall.** Removes the mod from the ModLoader and from every game folder it finds.
4. Start the game and press `F8`.

If the game is in *Program Files*, Windows asks for admin rights to write the `d3d9.dll` there. The setup only asks for that one step.

### SmartScreen and checksums

The setup isn't code-signed, so Windows SmartScreen may show a warning. Click *More info*, then *Run anyway*. The setup contains no mod code of its own: it copies the files that are next to it in the zip. If you'd rather not run it, install by hand.

Every release lists SHA-256 checksums. To check a download in PowerShell:

```powershell
Get-FileHash .\TM-Vibrant-Shaders.zip -Algorithm SHA256
```

## By hand

**With the ModLoader**

1. Open `%LOCALAPPDATA%\TMLoader\database\TmForever\products\` (paste it into the Explorer address bar).
2. Copy the folder `TM Vibrant Shaders` from the zip into it. Delete an older `TM Vibrant Shaders` folder first.
3. Tick *TM Vibrant Shaders* in your ModLoader profile.

**Without the ModLoader**

1. Copy `TM Vibrant Shaders\<version>\TMVibrantShaders.dll` from the zip into your TrackMania folder, next to `TmForever.exe`.
2. Rename it to **`d3d9.dll`**.

## Updating

Run the setup of the new version and pick the same button as before. It replaces the old version. Your settings and presets in `Documents\TrackMania\TMVS\` stay.

## Uninstalling

Run the setup and choose *Uninstall*. Or by hand: delete the `TM Vibrant Shaders` folder from the ModLoader products, or `d3d9.dll` from the game folder.

Your settings stay in `Documents\TrackMania\TMVS\`. Delete that folder to remove them too.

## Recommended game settings

In TrackMania's launcher, *Advanced* options:

| Game setting | Recommended | Why |
|---|---|---|
| Antialiasing | **Off** | The mod switches it off anyway (the depth effects need a readable depth buffer) and brings its own FXAA and TAA. |
| Anisotropic filtering | **16x** | Costs almost nothing and keeps the road and the thin neon strips sharp in the distance, so they still glow far away. |
| Shader quality | PC3 High | The richest base image to light. Lower works, it just starts flatter. |
| Shadows | Complex | Fine. The mod adds its own shadows on top. |
| Textures | High | |
| FX post-processing | **Off** if anything looks wrong | The game's own glow and blur can interfere with the shaders (overexposed, smeared or doubled spots). Off also keeps it from adding to the mod's bloom. |
| Water geometry, stadium water | On | |

The shaders also work with everything on low. They only need the finished image and the depth buffer.

## Files

| Path | What |
|---|---|
| `Documents\TrackMania\TMVS\settings.ini` | Your settings (saved automatically) |
| `Documents\TrackMania\TMVS\presets\*.ini` | Your own presets, one file each |
| `Documents\TrackMania\TMVS\tmvs.log` | Log of the last game start |
| `Documents\TrackMania\TMVS\capture_*.tmcap`, `screen_*.bmp` | Frame captures and screenshots from `F12` |
