# TM Vibrant Shaders

Direct3D 9 post-processing shader runtime and preset suite for TrackMania Nations Forever and TrackMania United Forever, inspired by Minecraft shaderpacks (Sildur's Vibrant, BSL, and IterationT).

Packaged as a native plugin for the [TrackMania ModLoader](https://tomashu.dev/software/tmloader/) and compatible with ReShade.

---

## Features

- **Direct3D 9 Pipeline Integration:** Intercepts backbuffer rendering directly in `EndScene` with zero external dependencies.
- **Tone Mapping & Color Grading:** ACES Filmic tone mapping, color temperature shifts, and selective hue saturation targeting sky and foliage.
- **Volumetric Sun Rays:** Real-time screen-space crepuscular light shafts with decay falloff and adjustable density.
- **Bloom & Anamorphic Flares:** Multi-tap bloom combined with horizontal anamorphic flare streaks inspired by IterationT.
- **In-Game Overlay:** Integrated Dear ImGui overlay for live parameter tuning and real-time preset switching.
- **ReShade FX Suite:** Standalone `.fx` shaders and `.ini` presets included for use with standard ReShade installations.

---

## Presets

| Preset | Description | Key Characteristics |
|---|---|---|
| **Sildur's Vibrant** | High-energy, warm presentation | Warm sunlight tint, saturated foliage/sky, high god-ray intensity. |
| **BSL Clean** | Balanced, neutral grading | ACES filmic rolloff, subdued haze, controlled highlight bloom. |
| **IterationT Cinematic** | High dynamic range aesthetic | Wide horizontal anamorphic flares, elevated bloom, filmic contrast. |
| **Custom** | Manual parameter control | User-configurable values for all pipeline stages. |

---

## Requirements

- TrackMania Nations Forever (TMNF) or TrackMania United Forever (TMUF)
- [TrackMania ModLoader](https://tomashu.dev/software/tmloader/) with `CoreMod` installed
- Windows 10 or Windows 11 (32-bit execution environment)

---

## Installation

### Method 1: TrackMania ModLoader (Recommended)

1. Open PowerShell in the project root directory and execute:
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\install-modloader.ps1
   ```
2. The script populates the product database at:
   ```
   %LOCALAPPDATA%\TMLoader\database\TmForever\products\TM Vibrant Shaders\1.0.0\
   ```
3. Open the **TrackMania ModLoader** application.
4. Check **TM Vibrant Shaders** in your active profile mod list.
5. Launch the game.

### Method 2: Manual Installation

1. Copy the `TM Vibrant Shaders` folder into:
   ```
   %LOCALAPPDATA%\TMLoader\database\TmForever\products\
   ```
2. Ensure `TMVibrantShaders.dll`, `description.yaml`, and the `Shaders/` and `Presets/` directories are present inside version `1.0.0/`.
3. Select the mod in the ModLoader interface.

### Method 3: Standalone ReShade

If running without the ModLoader:
1. Install [ReShade](https://reshade.me/) targeting `TmForever.exe` (Direct3D 9).
2. Copy the files in `Shaders/` to `reshade-shaders/Shaders/`.
3. Copy the `.ini` files in `Presets/` to the game root directory.
4. Select the desired preset through the ReShade interface (`Home` key by default).

---

## Keybindings

- **`F8`**: Toggle in-game configuration menu.
- **`F7`**: Quick toggle shader pipeline on / off (A/B comparison).

---

## Technical Notes

- **Antialiasing Configuration:** In the TrackMania launcher (`TmForeverLauncher.exe`), under *Configure* -> *Advanced*, set hardware Multisampling Antialiasing (MSAA) to *None*. Certain Direct3D 9 graphics drivers lock the depth stencil buffer when hardware MSAA is active, which impedes depth-assisted post-processing effects.
- **Runtime Dependencies:** The plugin binary (`TMVibrantShaders.dll`) is compiled with the static Microsoft Visual C++ runtime library (`/MT`), requiring no additional redistributable packages on the target system.

---

## Building from Source

### Prerequisites

- Visual Studio 2022 with C++ Build Tools
- Windows 10/11 SDK (includes `d3dcompiler.lib`)
- CMake 3.21 or newer

### Build Instructions

```powershell
# Generate 32-bit build files
cmake -B build -A Win32

# Compile Release binary
cmake --build build --config Release

# Deploy to local ModLoader installation
powershell -ExecutionPolicy Bypass -File .\install-modloader.ps1
```

The output DLL and ASI binaries are written to `build/Release/`.

---

## Project Structure

```
TrackMania-Vibrant-Shaders/
├── CMakeLists.txt              # CMake configuration for 32-bit D3D9 target
├── description.yaml            # Product metadata for TrackMania ModLoader
├── install-modloader.ps1       # Automated ModLoader installation script
├── LICENSE                     # MIT License
├── README.md                   # Technical documentation
├── src/
│   ├── config.h / .cpp         # Preset and parameter definitions
│   ├── dllmain.cpp             # Entry point and initialization thread
│   ├── hook.h / .cpp           # Direct3D 9 VTable and IAT detours
│   ├── overlay.h / .cpp        # Dear ImGui interface implementation
│   ├── renderer.h / .cpp       # Native Direct3D 9 post-processing pipeline
│   └── version.h.in            # Version string configuration template
├── Shaders/                    # ReShade FX shader sources
│   ├── ReShade.fxh
│   ├── TM_AtmosphericFog.fx
│   ├── TM_CinematicBloom.fx
│   ├── TM_DepthShading.fx
│   ├── TM_SunRays.fx
│   └── TM_VibrantColor.fx
├── Presets/                    # Predefined shader configurations
│   ├── TM_BSL_Clean.ini
│   ├── TM_IterationT_Cinematic.ini
│   └── TM_Sildurs_Vibrant.ini
└── Docs/
    ├── INSTALL_DE.md
    └── INSTALL_EN.md
```

---

## License

This project is licensed under the [MIT License](LICENSE).
