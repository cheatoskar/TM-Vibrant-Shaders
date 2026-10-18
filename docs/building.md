# Building from source

[Back to the README](../README.md)

## Requirements

- Visual Studio 2022 with the *Desktop development with C++* workload (or the Build Tools)
- Windows 10/11 SDK
- CMake 3.21 or newer
- Internet access on the first configure: CMake fetches Dear ImGui and minimp3

## Build

TrackMania Forever is 32-bit, so always configure for Win32:

```powershell
cmake -B build -A Win32
cmake --build build --config Release                        # plugin, setup, previewer
cmake --build build --config Release --target release_zip   # dist/: zip, DLL, SHA256SUMS.txt
```

Set a version with `-DTM_SHADERS_VERSION=1.4.1` when configuring. The CI does this from the tag.

To try a local build in the game: `install-modloader.ps1` copies it into the ModLoader, or copy `build\Release\TMVibrantShaders.dll` next to `TmForever.exe` as `d3d9.dll`. Close the game first.

## Output

| File | What |
|---|---|
| `build/Release/TMVibrantShaders.dll` | The mod |
| `build/Release/TM-Vibrant-Shaders-Setup.exe` | The setup (copies the files next to it, embeds no DLL) |
| `build/Release/tmvs_preview.exe` | Offline previewer |
| `dist/` | Release zip, DLL and checksums |

## The previewer

`tmvs_preview` runs the same pipeline outside the game on frames captured with `F12` (`Documents\TrackMania\TMVS\capture_*.tmcap`).

```powershell
tmvs_preview capture_000.tmcap out.bmp --preset Realistic
tmvs_preview capture_000.tmcap out.bmp --preset Storm --set Rain=2 --bench
tmvs_preview capture_000.tmcap out.bmp --shaders .\shaders     # compile tmvs.hlsl from disk
tmvs_preview capture_000.tmcap out.bmp --batch jobs.txt        # many variants, one shader compile
tmvs_preview --sound rain.wav 1.6 1.0                          # rain and thunder as a WAV
tmvs_preview --play 20 0.5                                     # play the weather sound live
```

| Option | |
|---|---|
| `--preset <name>` | Start from a preset |
| `--set Key=Value` | Override a setting (keys as in `settings.ini`), repeatable |
| `--lut <file.cube>`, `--lutmix <0..1>` | Colour look-up table and how much of it |
| `--debug <n>` | Debug view: 1 depth, 2 normals, 3 AO, 4 shadows, 5 shafts, 6 bloom, 7 long shadows (8 materials: in game only) |
| `--before <file.bmp>` | Also write the unprocessed frame |
| `--bench` | GPU time per pass |
| `--batch <jobs.txt>` | One job per line: `out.bmp [cap=file.tmcap] [preset=Golden_Hour] [Key=Value ...]` |
| `--move x,y,z` / `--drive x,y,z` | Fake camera motion (motion blur, neon trail) |
| `--time s`, `--water y`, `--sun x,y,z`, `--suncolor r,g,b` | Time, water height and sun for the capture |

GPU clocks vary between runs; compare benchmarks by their ratio to the `LinearDepth` pass, which does a fixed amount of work.

## Project layout

| Path | What |
|---|---|
| `src/plugin.cpp` | Start-up, scene capture (camera, sun), the call into the pipeline, hotkeys |
| `src/engine.cpp` | Hooks inside TmForever.exe, address tables for both game builds |
| `src/hook.cpp`, `src/depth.cpp`, `src/proxy.cpp` | Direct3D 9 device hooks, readable depth buffer, `d3d9.dll` forwarding |
| `src/pipeline.cpp` | Every pass, render target and frame constant; shared with the previewer |
| `shaders/tmvs.hlsl` | All pixel shaders |
| `src/config.cpp`, `src/settings.h` | Settings, field table (INI and menu), presets |
| `src/overlay.cpp` | The `F8` menu (Dear ImGui) |
| `src/audio.cpp`, `sounds/` | Weather sound and the embedded recordings |
| `src/autoquality.cpp` | Auto quality levels |
| `tools/preview.cpp`, `tools/installer.cpp` | Previewer and setup |
| `packaging/` | ModLoader description files, the zip's README |

## Conventions

- A new pixel shader entry point goes into the `Pass` enum and `kEntryPoints` in `pipeline.cpp` and into `TMVS_ENTRIES` in `CMakeLists.txt`, in the same order.
- Every visual feature is a setting with a field in `fields()`, and is off or cheap by default. Passes that contribute nothing are skipped, not passed through.
- New engine hooks need an address for both game builds in `kBuilds` (`src/engine.cpp`).
- Every push to `main` builds on GitHub Actions; a `v*` tag publishes a release (a tag with a suffix is a pre-release).
- After a tag's release the job `modloader-server` rebuilds the ModLoader update server on GitHub Pages (branch `gh-pages`): `modloader/index.json` lists every release that is not marked as a pre-release, `modloader/beta/index.json` all of them, each from 1.4.0-rc.2 on with its `TMVS-modloader.zip` (the DLL alone, made from the release's `TMVibrantShaders.dll` and served from Pages next to the index, not attached to the release), SHA-256 and changelog from `CHANGELOG.md`. One-time setup: *Settings → Pages → Deploy from a branch → `gh-pages`, `/ (root)`*. Check the format locally with `python tools/modloader_server.py selftest` (uses `build/Release/TMVibrantShaders.dll`; `--reference https://fullspeed.mooo.com/modloader/index.json` compares with a working server).
