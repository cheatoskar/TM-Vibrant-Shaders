# Installation Guide for TrackMania ModLoader & ReShade

This guide covers setting up **TM Vibrant Shaders** in TrackMania Nations Forever or United Forever.

## Method 1: Via TMModloader (Recommended)

This project follows the official TrackMania ModLoader packaging standard (identical to the *100% TMX + Bingo Plugin* and *Competition Patch*).

1. Open PowerShell in the project directory:
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\install-modloader.ps1
   ```
2. The script deploys the mod to:
   `%LOCALAPPDATA%\TMLoader\database\TmForever\products\TM Vibrant Shaders\1.0.0\`
3. Open **TrackMania ModLoader (TMLoader)**.
4. Check the box for **TM Vibrant Shaders** in your active profile list.
5. Click **Launch**.
6. In game: Press **`[F8]`** to access the shader menu, or **`[F7]`** to quickly toggle shaders on/off.

---

## Method 2: Standalone via ReShade

If you want to use the shaders via ReShade directly:

1. Download [ReShade](https://reshade.me/) and select `TmForever.exe` (DirectX 9).
2. Copy the files from `Shaders/` into your `reshade-shaders/Shaders/` directory.
3. Copy the `.ini` presets from `Presets/` into your game root directory.
4. In game, open the ReShade overlay (`Home` key) and select a preset:
   - `TM_Sildurs_Vibrant.ini`
   - `TM_BSL_Clean.ini`
   - `TM_IterationT_Cinematic.ini`

---

## Troubleshooting: Depth Buffer Access

If depth effects (fog or ray occlusion) do not display:
1. Open `TmForeverLauncher.exe`.
2. Navigate to **Configure** -> **Advanced**.
3. Set **Antialiasing (MSAA)** to *None* / *Off*.
4. DirectX 9 drivers may clear or lock the depth buffer when hardware MSAA is active.
