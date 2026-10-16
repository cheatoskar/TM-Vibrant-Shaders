# TM Vibrant Shaders for ManiaPlanet (TrackMania 2)

**Status: preview (1.4.0-preview), tested in the game on Stadium.** The interface (HUD, menus,
chat) is left alone. The TMNF mod's effects, on what ManiaPlanet's deferred HDR renderer
knows (its normals, material colours, sun shadow maps, motion vectors and HDR image):

- **Light:** the engine's light is taken apart (sun, ambient, and the rest: light maps, lamps,
  neon, reflections), so the effects work on each part. With every effect at zero the parts add up
  to the game's image exactly. Ambient occlusion, contact shadows, bounce light (GI), neon light
  on the surroundings, sun warmth and sky tint, haze, volumetric light through the engine's own
  shadow maps.
- **Sky:** the custom skies (clear, starry night, black hole, aurora, ring world), the game's sky
  enhanced, volumetric clouds, light shafts, lens flare. Under a night sky over a day map the
  engine's sun goes and "real night" brings its sky light down to moonlight; lamps and neon stay.
- **Weather:** wet roads, puddles, screen-space reflections, snow cover, rain, snow, splashes,
  lightning, drops on the lens, the weather sound.
- **Cinema:** depth of field, motion blur (the engine's motion vectors), TAA, photo mode with the
  compare split.
- **Neon trail** behind the car (the car's position from the engine's own constants).
- **Look:** the HDR stage in place of the engine's tone map (curves, bloom), grading, LUT looks.
- **Auto quality** (the TMNF mod's, on the effects' GPU times) and the Performance page.

- **Author's Shaders:** a look stored in the map's comments is applied when the map loads; in the
  editor *Set as Author's Shader* writes it into the map (save the map to keep it).

What ManiaPlanet can't do (yet): the spray behind the car, long-range shadows (the engine has its own shadow maps), TAA jitter, hi-res photos and zoom (they
need the engine's projection), "only in replays" (replays aren't told from driving yet: with it on,
depth of field and motion blur run in photo mode only), water surfaces, grass detail.

## Install

1. Close ManiaPlanet.
2. Run **`TM-Vibrant-Shaders-Setup.exe`** (or double-click `Install.cmd`).
   It finds ManiaPlanet under Program Files or in Steam libraries and copies `d3d11.dll` next to `ManiaPlanet.exe`; under Program Files Windows asks for
       administrator rights. If `d3d11.dll` already exists and is not this mod (ReShade, another mod),
       the setup will offer to backup and restore it.
3. Start ManiaPlanet **64-bit**. A message at the top says the mod is running.

Uninstall: `Uninstall.cmd` (`install.ps1 -Uninstall`), or delete `d3d11.dll` from the game folder.

**If the game doesn't start:** delete `d3d11.dll` from the game folder. The mod is 64-bit only. If
the launcher starts `ManiaPlanet32.exe`, switch the launcher to 64-bit.

## Controls

| Key | |
|---|---|
| F8 | Menu |
| F5 | Effects off/on (compare with the game's own image; TMNF: F7, which is ManiaPlanet's friends list) |
| F12 | Capture one frame (for bug reports and development); in photo mode: a photo (PNG) |
| F6 | Photo mode: no interface, full quality, depth of field on a clicked point |
| Shift+F6 / Ctrl+Shift+wheel | Photo mode: compare split (the game's own image on one side) / move its line |
| Middle click / Ctrl+middle click / Ctrl+wheel | Photo mode: focus here / no focus / aperture |

Otherwise the same keys as the TMNF mod. The night skies need *Real night under night skies*
(Settings page) for their dark nights: ManiaPlanet's time of day can't be changed, so the mod
brings the engine's sky light down to moonlight itself.

The F8 menu is the TMNF mod's own (src/menu.cpp, shared). It has the same presets, preset gallery,
own presets with pictures, presets per map type (day, sunset and night, from the engine's light
colour), the Studio and the settings. It shows only what ManiaPlanet can do (effects.cpp and
the stages' `supports()`). The Studio's debug views show what the effects see: depth, normals, AO,
shadows, the engine's light taken apart, light sources, light shafts, clouds, sky, focus, motion.

Settings and your presets are in `Documents\ManiaPlanet\TMVS` (`settings.ini`, `presets\`), in
the same format as in TMNF. ManiaPlanet's own options are in `maniaplanet.ini`, on the Settings
page, panel "ManiaPlanet":
- **HDR stage:** on = the look on the engine's HDR image, off = on its finished image.
- **Placement**, and the **compare and check views**.
- **Status lines.**
- **Compare (split)** shows the game's image left and the mod's right.
- **Checks** show what the mod reads from the engine: depth, normals, motion, albedo, and
  a world grid (lines every 8 m, bright every 32 m) drawn from depth and the engine's camera. The
  grid must stay on the ground while the camera moves. Magenta means not found.
- The **status lines** say where the effects were applied, whether the camera was found, and which
  engine textures were found.

The log (`tmvs-mp.log`), LUTs (`luts\*.cube`) and captures are in `Documents\ManiaPlanet\TMVS` too.

## Online, Openplanet

- The mod changes the picture. From the game it reads the camera, the sun and the car's position
  (from the constants the engine draws with) and the map's comments (for Author's Shaders). It
  writes only the comments of the map open in the editor, when you click *Set as Author's Shader*.
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

What else the mod reads from the engine:
- **Scene depth:** the last screen-sized depth buffer a scene shader used.
- **HDR image and exposure:** the tone map's two inputs.
- **G-buffer by binding name:** the table also stores, per shader, the slot it reads
  `TMapDeferredPixelNormalInC`, `TMapDeferredMDiffuse`/`MSpecular` (material colour and specular; the captures show `MDiffuse` is the albedo),
  `TMapDeferredLightMask` and `TMapMotion` at. The first lighting-read draw of a frame hands them
  over. Its second render target (R16G16 float) holds the motion vectors.
- **Camera:** the `SceneV` constant buffer (vertex slot 13, 704 bytes; layout from
  `tools/rdef.py`), copied while the engine uploads it (Map/Unmap, UpdateSubresource). It holds
  the world-to-clip matrix at 560, world-to-camera at 512, the camera projection at 432, the eye at
  624 and the sun colour at 224.

The D3D11 runtime rewrites a context's vtable when the game turns on multithread protection, and
writes the draw slots back on Present. The hooks are therefore checked and patched again around
every Present and before the game sets shaders or render targets.

## HDR stage (tone mapping and bloom)

The engine's tone map (`ToneMapGlobalFilmCurve`, disassembled with `fxc /dumpbin`):

```
x = min(hdr, 50000) * exposure            exposure = t1.z at TcAutoScale (2x2 texture, auto exposure)
f = (x(Ax + BC) + DE) / (x(Ax + B) + DF) - E/F      Hable's filmic curve, per channel
out = f * OutScale + (f.r + f.g + f.b) * Gray
```

Its input already holds the engine's bloom (`FxBloom_Final`). Per channel, Hable bleaches bright
colours toward white.

At that draw the mod runs its own passes on the same inputs, and the engine's draw is skipped:
- It reads the engine's constants directly: its cbuffer 0 is bound as `b2`, so there is no read-back.
- **Curves:**
  - *Engine:* rebuilds the engine's exactly. On a race capture it is 1.4/255 off the game's image,
    with constants fitted from that capture.
  - *TMVS:* the TMNF mod's curve, linear up to a knee, then a long shoulder, half on the brightest
    channel so lit colours keep their hue.
  - *ACES.*
- TMVS and ACES are scaled so mid grey (0.18) comes out as with the engine's curve: the game's
  overall brightness stays.
- **Bloom:** the TMNF mod's pyramid (Karis average, soft threshold, soft limit so the sun doesn't
  whiten the sky), added to the HDR image.
- With the engine's curve and nothing added, the engine's own draw runs.

## Captures (F12)

Captures go to `Documents\ManiaPlanet\TMVS\captures\capture_NNN\`:

| File | Content |
|---|---|
| `trace.txt` | Every draw of the frame: shader name, role, render targets, depth, blending, inputs, viewport, scissor, depth-stencil state and stencil ref, rasterizer |
| `hdr_t*.dds` | What the engine's tone map reads |
| `input.dds/.bmp`, `view.txt` | The image before the mod, and the format it was drawn with |
| `output.dds/.bmp` | The finished frame |
| `engine_*.dds` | The engine's textures: hdr, exposure, depth, normal, albedo (diffuse), specular, light mask, motion |
| `camera.txt`, `scenev.bin` | The engine camera, decoded and raw |
| `tonemap_cb.bin` | The engine's tone-map constants (for `mp_preview --hdr`) |
| `car.txt` | The car per frame over the last 240 frames: time, frame time, found, position, step (is the neon trail's source even?) |

`mp_preview` renders a capture through the same renderer as the game, without the game:

```powershell
build-mp/Release/mp_preview.exe <capture folder> out.bmp [--preset Punchy] [--view 6] [--set Exposure=0.5]
build-mp/Release/mp_preview.exe <capture folder> out.bmp --hdr [--set ToneCurve=1] [--set Bloom=0.3] [--tonecb file]
build-mp/Release/mp_preview.exe <capture folder> out.bmp --hdr --frames 40 --car x,y,z [--carfrom x,y,z]   # the neon trail of a car driving there
build-mp/Release/mp_preview.exe --trailtest   # the trail's recording at 144 fps: how evenly its end follows the car
```

## Build and test

```powershell
cmake -S maniaplanet -B build-mp -A x64
cmake --build build-mp --config Release      # d3d11.dll, mp_harness.exe, mp_preview.exe
```

`mp_harness.exe` tests the proxy without the game. It draws like the engine, using the engine's
real tone-map, interface and scene pixel shaders taken from the GpuCache:

```powershell
python -I maniaplanet/tools/gpucache.py --dump "<ManiaPlanet>/Packs/GpuCache_D3D11_SM5.zip" ToneMapGlobalFilmCurve tonemap.dxbc
python -I maniaplanet/tools/gpucache.py --dump "<...>.zip" PHLSL/RasterBitmap.PHlsl ui.dxbc
python -I maniaplanet/tools/gpucache.py --dump "<...>.zip" Tech3/Block_TDSN_DefWrite_p scene.dxbc
python -I maniaplanet/tools/gpucache.py --dump "<...>.zip" Tech3/Block_DefReadP1_p defread.dxbc
$env:TMVS_MP_DATA = "$env:TEMP\mp-test"
build-mp/Release/mp_harness.exe build-mp/Release/d3d11.dll tonemap.dxbc ui.dxbc scene.dxbc defread.dxbc [--srgb]
build-mp/Release/mp_harness.exe ... --grade in.bmp out.bmp "Punchy"   # a picture through a preset
python -I maniaplanet/tools/gpucache.py --dump "<...>.zip" Tech3/DeferredDeCompFaceNormal_p decomp.dxbc
build-mp/Release/mp_harness.exe ... defread.dxbc --decomp decomp.dxbc   # also: the light mask after a map change
```

It checks that:
- the effects land between the tone map and the interface;
- the interface stays untouched;
- the depth buffer is found;
- the camera is read from a SceneV upload (with multithread protection on, like the game);
- the G-buffer and motion are found by name;
- with `--decomp`: after a map change (new engine targets, the face-normal pass first) the light mask is the new one;
- the TMVS curve replaces the engine's tone map and matches its mid grey (UNORM and sRGB);
- the Present fallback works;
- a capture writes its files;
- ResizeBuffers still works.

## Files

| Path | What |
|---|---|
| `src/proxy.cpp`, `src/forward.asm`, `src/d3d11.def` | d3d11.dll proxy: device creation is hooked, everything else jumps to the system DLL |
| `src/hooks.cpp` | Device, context and swap chain vtable hooks; the frame logic (scene, tone map, post, interface); camera and engine textures |
| `src/engine_frame.cpp` | SceneV layout, camera decoding, readable formats |
| `tools/rdef.py` | Constant buffers and texture bindings of the engine's shaders |
| `tools/preview.cpp` | Offline previewer for captures |
| `src/shaderdb.cpp`, `src/shaderdb_table.inc` | Engine shader checksums (generated by `tools/gen_shaderdb.py`) |
| `src/renderer.cpp`, `shaders/mp.hlsl` | The passes (luma blur for clarity, grade), state save/restore |
| `src/overlay.cpp` | F8 menu (ImGui, DX11) |
| `src/capture.cpp` | Frame capture (trace, DDS/BMP) |
| `src/settings.cpp` | Presets and `settings.ini` |
| `../src/lut.cpp` | `.cube` LUT reader, shared with the TMNF mod |
