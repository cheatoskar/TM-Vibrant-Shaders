# TM Vibrant Shaders for ManiaPlanet (TrackMania 2)

**Status: first version (0.1.0), tested offline only.** It grades ManiaPlanet's finished image:
colour, contrast, local contrast, sharpening, vignette and LUT looks. The interface (HUD, menus,
chat) is left alone.

ManiaPlanet 4 is a 64-bit Direct3D 11 game with a deferred HDR renderer. Real-time shadows,
reflections, bloom and tone mapping are already the engine's own. So this build doesn't re-do
lighting. It starts where the engine's image is finished. Later steps (HDR before the engine's tone
map, depth-based effects) build on the captures this version can take.

## Install

1. Close ManiaPlanet.
2. Run `install.ps1` from this folder:
   `powershell -ExecutionPolicy Bypass -File install.ps1`
   It copies `d3d11.dll` next to `ManiaPlanet.exe`, which needs administrator rights under
   `C:\Program Files (x86)`. If `d3d11.dll` already exists and is not this mod (ReShade, another
   mod), the script stops.
3. Start ManiaPlanet **64-bit**. A message at the top says the mod is running.

Uninstall: `install.ps1 -Uninstall`, or delete `d3d11.dll` from the game folder.

**If the game doesn't start:** delete `d3d11.dll` from the game folder. The mod is 64-bit only. If
the launcher starts `ManiaPlanet32.exe`, switch the launcher to 64-bit.

## Controls

| Key | |
|---|---|
| F8 | Menu |
| F7 | Effects off/on (compare with the game's own image) |
| F12 | Capture one frame (for bug reports and development) |

The same keys as the TMNF mod.

In the menu:
- **Presets:** Vibrant (default), Natural, Punchy, Golden Hour, Cold Night, Clean.
- **Sliders:** for every value.
- **Compare (split):** shows the game's image left and the mod's right.
- **Depth view:** shows whether the scene depth was found.
- **Status lines:** say where the effects were applied.

Settings, log (`tmvs-mp.log`), LUTs (`luts\*.cube`) and captures are in
`Documents\ManiaPlanet\TMVS`.

## Online, Openplanet

- The mod only changes the picture. It reads nothing from the game's logic and changes nothing in it.
- Whether servers or competitions allow a `d3d11.dll` mod is not confirmed. Check the rules of where you play.
- Openplanet can run at the same time (it loads as its own DLL). If something breaks with
  Openplanet, test once without it and report it with a capture.

## How it finds the right place in the frame

ManiaPlanet ships its compiled shaders in `Packs\GpuCache_D3D11_SM*.zip`.

1. `tools/gen_shaderdb.py` turns them into a table: the DXBC checksum of each of the 6419 pixel
   shaders, with its name and role (scene, tone map, post effect, 2D).
2. When the game creates a pixel shader, the mod looks it up. So it knows which draw is the tone
   map (`ToneMapGlobalFilmCurve`, ...), which are post effects (FXAA, bloom, colour grading), and
   which is the first interface draw.
3. The mod's pass runs right before that first interface draw.
4. If the interface start can't be found in a frame, the pass runs at Present instead (the
   interface is graded too, and the menu says so).

The scene depth is the last screen-sized depth buffer a scene shader used. It is used for the
depth view and the captures.

## Captures (F12)

Captures go to `Documents\ManiaPlanet\TMVS\captures\capture_NNN\`:

| File | Content |
|---|---|
| `trace.txt` | Every draw of the frame: shader name, role, render targets, depth, inputs |
| `hdr_t*.dds` | What the engine's tone map reads (its HDR image) |
| `input.dds/.bmp` | The image before the mod |
| `output.dds/.bmp` | The finished frame |
| `depth.dds` | The scene depth |

## Build and test

```powershell
cmake -S maniaplanet -B build-mp -A x64
cmake --build build-mp --config Release      # build-mp/Release/d3d11.dll, mp_harness.exe
```

`mp_harness.exe` tests the proxy without the game. It draws like the engine, using the engine's
real tone-map, interface and scene pixel shaders taken from the GpuCache:

```powershell
python -I maniaplanet/tools/gpucache.py --dump "<ManiaPlanet>/Packs/GpuCache_D3D11_SM5.zip" ToneMapGlobalFilmCurve tonemap.dxbc
python -I maniaplanet/tools/gpucache.py --dump "<...>.zip" PHLSL/RasterBitmap.PHlsl ui.dxbc
python -I maniaplanet/tools/gpucache.py --dump "<...>.zip" Tech3/Block_TDSN_DefWrite_p scene.dxbc
$env:TMVS_MP_DATA = "$env:TEMP\mp-test"
build-mp/Release/mp_harness.exe build-mp/Release/d3d11.dll tonemap.dxbc ui.dxbc scene.dxbc [--srgb]
build-mp/Release/mp_harness.exe ... --grade in.bmp out.bmp "Punchy"   # a picture through a preset
```

It checks that:
- the effects land between the tone map and the interface;
- the interface stays untouched;
- the depth buffer is found;
- the Present fallback works;
- a capture writes its files;
- ResizeBuffers still works.

## Files

| Path | What |
|---|---|
| `src/proxy.cpp`, `src/forward.asm`, `src/d3d11.def` | d3d11.dll proxy: device creation is hooked, everything else jumps to the system DLL |
| `src/hooks.cpp` | Device, context and swap chain vtable hooks; the frame logic (scene, tone map, post, interface) |
| `src/shaderdb.cpp`, `src/shaderdb_table.inc` | Engine shader checksums (generated by `tools/gen_shaderdb.py`) |
| `src/renderer.cpp`, `shaders/mp.hlsl` | The passes (luma blur for clarity, grade), state save/restore |
| `src/overlay.cpp` | F8 menu (ImGui, DX11) |
| `src/capture.cpp` | Frame capture (trace, DDS/BMP) |
| `src/settings.cpp` | Presets and `settings.ini` |
| `../src/lut.cpp` | `.cube` LUT reader, shared with the TMNF mod |
