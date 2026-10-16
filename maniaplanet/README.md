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
| Shift+F6 / Ctrl+Shift+wheel | Photo mode: compare split (the game's own image or a preset on one side) / move its line |

These are the defaults; the menu's *Settings* page (*Hotkeys*) rebinds them, and they are stored in the `[Keys]` section of settings.ini (see docs/settings.md).
| Middle click / Ctrl+middle click / Ctrl+wheel | Photo mode: focus here / no focus / aperture |

**Compare split with a preset:** menu, *Settings* page, *Photo mode* panel, *Other side*: the game's
image (as before) or any preset. With a preset, a second renderer (`hooks.cpp`, `g_rendererB`) runs
that preset's effects and HDR stage on the same engine inputs with its own history, and each look
draws only its side of the line (a scissor on the final passes). Photo mode only; it costs about
one more run of the effects while shown and is released when the split, photo mode or the preset
goes. Photos (F12) get both names at the line (`SplitPhotoLabels`). Offline:
`mp_preview <capture> out.bmp --hdr --compare Horizon [--split 0.4] [--swap] [--labels "Horizon|Vibrant"]`.

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
- **Light sources from the engine's draws** (`EngineLightDraws`, on by default since the owner's test: grass no longer glows, the road borders still do): the lamps, light
  strips, screens, halos and the car's lights glow as the engine draws them, nothing is guessed
  from colours (TMNF's EngineLights). The draws of self-illuminated materials (`TMapSelfIllum`,
  `TMapAdvert`, `TMapIllum`) and the additive ones (`Tech3_Block_TAdd`: halos) are drawn a second
  time into a float buffer of their own (a self-illuminated one twice: the second time with its
  light texture black, subtracted, so only its own light stays); later draws over them take their
  share away again. The debug view "Light sources" shows what counts as light.
- **Black hole moon** (`BlackHoleMoon`, 0 – 2, **off by default**; TMNF's `EngineMoon` *from the
  black hole*, done in the shaders): on night maps under a sky with the black hole, the game's moon
  (its blue glint on chrome, glass and the ring sculptures, and its light) is taken out of the
  picture and the black hole lights the scene instead, warm, as bright as the moon was (1) or
  brighter. The engine's lighting pass (`Block_DefReadP1_p`) lights with light x N.L x shadow
  for the diffuse and light x N.L x shadow x (N.H)^n x n x MSpecular.x² x SpecScale x the light's w
  for the glint, n = MSpecular.w x SpecMaxExp + 1, all x 2 x its cloud factor; the constants are
  in SceneP (read from its upload, as the camera), the material's specular in the G-buffer. So
  the moon's glint is rebuilt exactly (up to the cloud factor, taken halfway between its darkest
  and brightest), taken out, and drawn again from the black hole's direction with the same
  formula. Shadows: the engine's shadow map is the moon's; it counts as the black hole's as far
  as the two directions agree (Horizon: 18° apart, almost fully), else only the contact shadows
  toward the black hole. Nothing in the game's memory changes; EngineMoon doesn't matter on those
  maps while it is on. Debug view "The moon's glint" shows the rebuilt glint. Not on day or
  sunset maps, not under other skies.
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
- *Soft sun shadows* (on by default, 1°) draws the engine's sun shadow pass with the mod's own shader.
  Nothing in the game's memory changes, but whether the anti-cheat minds a replaced shader is not known.
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

## Soft sun shadows

Setting *Soft sun shadows* (Settings page, panel "ManiaPlanet"; `SoftShadows` in
`maniaplanet.ini`), **on by default (1°)**, 0 turns it off. Its value is the sun's size in the sky in degrees (the real
sun: 0.53). The shadows are then sharp where a thing touches the ground and get wider the farther
they fall from it, like real ones.

The engine's sun shadows (`Tech3/DeferredShadowPssm_p`, a full-screen pass into the light mask):
- Inputs: the linear depth (t0), two static shadow maps of the whole map (t1, t2, 4096², about 1 m
  per texel), the split atlas (t3, 6400 x 1280: five 1280² splits, the coarsest about 370 m wide,
  the finest about 11 m) and the face normal. Its constants (`ShaderP`, 11 + 5 x splits registers)
  hold the camera ray, the maps' matrices and each split's rectangle in the atlas.
- It takes the finest split that holds the pixel, with one bilinear comparison tap (a few more
  along the pixel's footprint at grazing views). No blend between splits, no bias in the shader
  (the maps are drawn with a depth bias). Outside the splits, the static maps.
- It writes lit² into the light mask. 48 variants in the GpuCache: 1 to 5 splits, with or without
  an opacity map and a sprite (cloud) map, SM5 and SM4.

The mod's shader (`shaders/softshadow.hlsli`, `src/softshadow.cpp`) draws that pass instead, with
the engine's own textures and constants, for that one draw: the engine's shader, `b5` and `s5`
are bound again right after it. Nothing in the game's memory changes.
- Outside the splits, and the opacity and sprite maps: the engine's code, unchanged.
- In the splits (PCSS): a blocker search around the pixel (8 gathers in the finest split that
  holds 40 m x the light's angle at 12 texels or less), against the receiver's plane from the face
  normal, not a fixed bias. From the blockers' mean height above it, the penumbra's radius (height
  x the light's angle). Then 12 gathered taps on a turned disc of that radius, in the finest split
  that holds it at 8 texels or less, stretched along the pixel's footprint. Deep in the shadow and
  in full light the filter is skipped.
- Only the 40 variants it knows (by DXBC checksum: the five-split one runs in races) and only if
  their constants and textures are bound as expected. Otherwise the engine's shader stays and the
  log says why, once. When it takes over, the log says
  `soft shadows: the mod's PCSS shader draws the engine's sun shadow pass (...)`. The menu's
  status line says which.
- Cost: about 1 ms at 2560 x 1440 on a GTX 1080 Ti (the engine's pass: 0.2 ms).
- Online: whether ManiaPlanet's anti-cheat minds a replaced shader is not known. That is why it is off by default.

`mp_shadowtest` checks it offline on the inputs of the pass (an F12 capture now saves the atlas,
`engine_shadowsplits.dds`, and the face normal, `engine_shadownormal.dds`). The engine's shader
comes from the GpuCache. The name has 48 variants; the last argument picks the five-split one by
its DXBC checksum (without it, `--dump` takes the first):

```powershell
python -I maniaplanet/tools/gpucache.py --dump "<...>/GpuCache_D3D11_SM5.zip" Tech3/DeferredShadowPssm_p pssm.dxbc 30a64492
build-mp/Release/mp_shadowtest.exe <capture folder> <out dir> pssm.dxbc --light 0.5,1,2 [--bench 50] [--variants <dir>]
```

It draws the pass with the engine's shader and with the mod's (as the hooks do) and checks: the
mod's port of the engine's shader gives the engine's light mask (`--variants`: every variant in a
folder), the engine's shader and slots come back after the draw, and an unknown variant, constants
too small, a missing atlas or light size 0 keep the engine's shader. It writes the results as
`.f32` (width, height, RGBA floats) and prints the GPU time of each.

## Captures (F12)

Captures go to `Documents\ManiaPlanet\TMVS\captures\capture_NNN\`:

| File | Content |
|---|---|
| `trace.txt` | Every draw of the frame: shader name, role, render targets, depth, blending, inputs, viewport, scissor, depth-stencil state and stencil ref, rasterizer |
| `hdr_t*.dds` | What the engine's tone map reads |
| `input.dds/.bmp`, `view.txt` | The image before the mod, and the format it was drawn with |
| `output.dds/.bmp` | The finished frame |
| `engine_*.dds` | The engine's textures: hdr, exposure, depth, normal, albedo (diffuse), specular, light mask, motion; the sun shadow pass's linear depth, static maps, split atlas and face normal; with EngineLightDraws `engine_lights.dds` (what its light draws added, before the exposure) |
| `shadow_cb.bin`, `scenep.bin` | The sun shadow pass's constants (for `mp_shadowtest`) |
| `camera.txt`, `scenev.bin` | The engine camera, decoded and raw |
| `tonemap_cb.bin` | The engine's tone-map constants (for `mp_preview --hdr`) |
| `car.txt` | The car per frame over the last 240 frames: time, frame time, found, position, step (is the neon trail's source even?), camera, which car was taken, and every car in the camera's view (ghosts) |

`mp_preview` renders a capture through the same renderer as the game, without the game:

```powershell
build-mp/Release/mp_preview.exe <capture folder> out.bmp [--preset Punchy] [--view 6] [--set Exposure=0.5]
build-mp/Release/mp_preview.exe <capture folder> out.bmp --hdr [--set ToneCurve=1] [--set Bloom=0.3] [--tonecb file]
build-mp/Release/mp_preview.exe <capture folder> out.bmp --hdr --frames 40 --car x,y,z [--carfrom x,y,z]   # the neon trail of a car driving there
build-mp/Release/mp_preview.exe --trailtest   # the trail's recording: how evenly its end and its start behind the car move
build-mp/Release/mp_preview.exe --cartest     # which car the trail follows with a ghost around (chase, cockpit, respawn)
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
python -I maniaplanet/tools/gpucache.py --dump "<...>.zip" "Tech3 Block DefReadP1 SelfIllum" selfillum.dxbc
build-mp/Release/mp_harness.exe ... defread.dxbc --selfillum selfillum.dxbc   # also: EngineLightDraws' light buffer
python -I maniaplanet/tools/gpucache.py --dump "<...>.zip" Tech3/DeferredShadowPssm_p pssm.dxbc 30a64492   # the five-split variant
build-mp/Release/mp_harness.exe ... [--decomp decomp.dxbc] --shadow pssm.dxbc   # also: soft shadows
```

It checks that:
- the effects land between the tone map and the interface;
- the interface stays untouched;
- the depth buffer is found;
- the camera is read from a SceneV upload (with multithread protection on, like the game);
- the G-buffer and motion are found by name;
- with `--decomp`: after a map change (new engine targets, the face-normal pass first) the light mask is the new one;
- with `--selfillum`: with EngineLightDraws the engine's image stays the same and the light buffer holds the
  material's own light, not its lit surface;
- with `--shadow`: soft shadows off, the engine's sun shadow pass draws; on, the mod's shader draws a
  penumbra at a shadow's edge and the same light and shadow elsewhere, says so in the log, and the
  engine's shader and slots are bound again after the draw; off again, the engine's light mask to the bit;
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
| `src/softshadow.cpp`, `shaders/softshadow*.hlsl*` | Soft sun shadows: the engine's shadow variants, the swap at its draw, the PCSS shader |
| `tools/shadowtest.cpp` | Offline test of the soft shadows against the engine's shadow shader |
| `src/overlay.cpp` | F8 menu (ImGui, DX11) |
| `src/capture.cpp` | Frame capture (trace, DDS/BMP) |
| `src/settings.cpp` | Presets and `settings.ini` |
| `../src/lut.cpp` | `.cube` LUT reader, shared with the TMNF mod |
