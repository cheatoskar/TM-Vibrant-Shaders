# TM Vibrant Shaders

Direct3D 9 post-processing shader runtime and preset suite for TrackMania Nations Forever and TrackMania United Forever.

Packaged as a native plugin for the [TrackMania ModLoader](https://tomashu.dev/software/tmloader/) with zero external dependencies.

---

## Features

- **Linear-Space Rendering Pipeline:** Processes backbuffer frames in linear color space to eliminate highlight clipping and color distortion.
- **FidelityFX Contrast-Adaptive Sharpening (CAS):** 5-tap adaptive edge enhancement to eliminate 2008-era texture blur and restore fine track detail.
- **Contact Shading & Micro-AO:** Local crevice darkening that grounds stadium blocks, barriers, and vehicle wheels.
- **Tarmac Specular Sheen:** Fresnel-weighted road surface reflectance for a modern track aesthetic.
- **Calibrated Emissive Bloom:** High-threshold bloom strictly targeting active lights (boost pads, taillights, sun) without washing out the road surface.
- **Anamorphic Lens Flare Streaks:** Horizontal flare scattering for cinematic broadcast and night-race aesthetics.
- **Bounded Sky Rays:** Crepuscular light scattering restricted to the upper hemisphere to protect driving visibility.
- **In-Game ImGui Overlay:** Interactive parameter configuration and instant preset switching via hotkey.

---

## Presets

| Preset | Target Aesthetic | Characteristics |
|---|---|---|
| **Stadium 2020** | Modern Trackmania (TM2020) | Neutral daylight, FidelityFX CAS sharpness, deep contact shadows, tarmac sheen, clean road. |
| **Golden Hour** | Warm afternoon sunlight | Rich grass and sky saturation, subtle warm white balance, soft sky-bounded light shafts. |
| **Clear Daylight** | High-contrast competition | Cool neutral color balance, maximized edge definition, zero bloom glare for pure visibility. |
| **Grand Prix Cinematic** | Broadcast & night racing | Film contrast curve, horizontal cyan anamorphic flare streaks on taillights and floodlights. |
| **Custom** | Manual parameter tuning | Full real-time control over all shader parameters via in-game sliders. |

---

## Requirements

- TrackMania Nations Forever (TMNF) or TrackMania United Forever (TMUF)
- [TrackMania ModLoader](https://tomashu.dev/software/tmloader/) with `CoreMod` installed
- Windows 10 or Windows 11 (32-bit execution environment)

---

## Installation

### TrackMania ModLoader (Recommended)

1. Open PowerShell in the project directory and execute:
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\install-modloader.ps1
   ```
2. The script deploys the plugin and presets to:
   ```
   %LOCALAPPDATA%\TMLoader\database\TmForever\products\TM Vibrant Shaders\1.0.0\
   ```
3. Open the **TrackMania ModLoader** application.
4. Check **TM Vibrant Shaders** in your active profile mod list.
5. Launch the game.

### Standalone ReShade Usage

The repository also includes standalone ReShade FX files in `Shaders/` and configuration presets in `Presets/` for standard ReShade installations.

---

## Keybindings

- **`F8`**: Toggle in-game configuration menu.
- **`F7`**: Quick toggle post-processing pipeline on / off (A/B comparison).

---

## Technical Notes

- **Linear vs. Gamma Space:** Direct3D 9 presents frames in non-linear sRGB gamma space. Applying color multipliers directly causes severe highlight bleaching. This runtime converts color data to linear space (`x^2.2`) prior to filtering, then maps back through a filmic tone curve to ensure highlights roll off naturally.
- **Antialiasing Setting:** In `TmForeverLauncher.exe` (*Configure* -> *Advanced*), set hardware MSAA to *None* or *Low* if using depth-based effects.
- **Runtime Dependencies:** The binary (`TMVibrantShaders.dll`) is statically compiled (`/MT`) against the Visual C++ runtime.

---

## Building from Source

### Prerequisites

- Visual Studio 2022 with C++ Build Tools
- Windows 10/11 SDK (includes `d3dcompiler.lib`)
- CMake 3.21 or newer

### Build Commands

```powershell
cmake -B build -A Win32
cmake --build build --config Release
powershell -ExecutionPolicy Bypass -File .\install-modloader.ps1
```

---

## Project Structure

```
TrackMania-Vibrant-Shaders/
├── CMakeLists.txt              # CMake configuration for 32-bit D3D9 target
├── description.yaml            # Product metadata for TrackMania ModLoader
├── install-modloader.ps1       # Automated ModLoader deployment script
├── LICENSE                     # MIT License
├── README.md                   # Technical documentation
├── src/
│   ├── config.h / .cpp         # Preset and parameter definitions
│   ├── dllmain.cpp             # Entry point and initialization thread
│   ├── hook.h / .cpp           # Direct3D 9 VTable and IAT detours
│   ├── overlay.h / .cpp        # Dear ImGui interface implementation
│   ├── renderer.h / .cpp       # Linear-space Direct3D 9 post-processing pipeline
│   └── version.h.in            # Version string configuration template
├── Shaders/                    # Standalone ReShade FX shader sources
│   ├── ReShade.fxh
│   ├── TM_AtmosphericFog.fx
│   ├── TM_CinematicBloom.fx
│   ├── TM_DepthShading.fx
│   ├── TM_SunRays.fx
│   └── TM_VibrantColor.fx
├── Presets/                    # Predefined configurations
│   ├── Clear_Daylight.ini
│   ├── Golden_Hour.ini
│   ├── Grand_Prix_Cinematic.ini
│   └── Stadium_2020.ini
└── Docs/
    ├── INSTALL_DE.md
    └── INSTALL_EN.md
```

---

## License

This project is licensed under the [MIT License](LICENSE).
