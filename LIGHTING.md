# LIGHTING.md – lights, highlights and shader polish

What we learned about lamps, glow, reflections and "clean" shading, so the next attempt starts
from it. Read with [GOTCHA.md](GOTCHA.md). Last update: 2026-10-10 (after the light reset).

## The reference

The owner judges every change against **the game without the mod** ("ohne Shader perfekt").
The game's image is finished 8-bit LDR: lamps at 255, its own bloom (FX post-processing) on
top, no screen-space reflections on the track. Whatever the mod adds must not make that worse:
a lamp the game drew white must not end up grey, a wall the game drew white must not glow.

## Where light comes from in the pipeline (PS_Lighting -> PS_Final)

| Step | What it does | How it can go wrong |
|---|---|---|
| `emissiveAmount` | Guesses light sources in the LDR image (brighter than the 8 surrounding samples, or strongly coloured and bright = neon). Excludes sun glare (white and facing up, or in the sun's mirror direction). | A lamp panel lit evenly with bright surroundings isn't "brighter than around it" -> no glow. White print, floodlit walls, the edge of a headlight pool look like lights -> glow where nothing shines. |
| glow `c * HighlightBoost * emissive` | Expands guessed lights back to HDR. | Everything wrongly guessed is multiplied by 3-6. |
| `removeGameSun` | Night skies over day maps: takes the baked sun out (direct light, glare cap, "flatten" of what's still bright). | `flatten` compresses everything bright - also lamp light and headlight pools (grey). Under a black-hole sky it used the hole's direction instead of the game's sun (fixed in the reverted batch, see below). |
| Neon spill (`PS_SpillDown`) | Strongly coloured bright pixels light their surroundings (blurred light buffer). | Coloured print (the green START) spills as a blurred second copy of itself. |
| Reflections (`PS_Reflect` + composite) | Screen-space mirror of the scene; dry track reflections (`Reflections`, night presets 1.1 = polished mirror); the hit gets `1 + HighlightBoost * 0.5` for bright pixels so lamps stay lights in the mirror; Fresnel near 1 at grazing angles. | The game has no track reflections at all: every lamp gets a new mirror image, boosted ~2.75x, then bloom on top -> white, "overexposed" roads. 16 coarse steps + per-pixel noise without TAA -> dashed / grainy reflections. |
| Tone curve (`shoulderCurve`, knee 0.5) | Linear to 0.5, long shoulder above. | The game's white (1.0) ends at ~0.72 linear (~87 % sRGB): grey unless the glow pushed it up. This is why lamps only look right where the guess hit. |
| Night lamp floor (PS_Final, night maps) | Pixels the game drew near white keep at least the game's brightness. | By brightness alone it also caught the road in a lamp's light right at the threshold: white speckles ("frost"). |

## Why "with shader overexposed, without shader fine"

Three things add up, all mod-made:
1. **New reflections the game never had**: dry track reflections are on in most presets (night 1.1 = mirror). Every lamp now has a mirror image on the road.
2. **Boosted reflections**: the mirrored lamp is multiplied by `1 + HighlightBoost * 0.5` (~2.75 with 3.5) and bloom glows on it.
3. **Guessed lights**: whatever `emissiveAmount` mistakes for a light gets `HighlightBoost` (3-6x).

And the opposite effect, grey lamps: the tone curve pulls the game's white down to ~87 %; only boosted (guessed) lamps reach white.

## The engine knows the materials (Ghidra, 2026-10-10)

- Every 3D draw goes through `CDx9ShaderKeeper::UpdateAndRender` / `ShadeVisualForCurrentPass` with its `CPlugShader`; its file name tells the material type (census `LogShaders=1`):
  `TSelfI ...` self-illuminated (lamps, light panels, screens), `TAdd ...` additive glows (drawn over whole blocks), `Vehicles\...` cars / `...Glass`, `Grass ...`, `TDiff_Spec_Nrm ...` / `Tween ...` ordinary surfaces, `WheelMarks`, `ParticleSmoke`, `Sky`, `Clouds`.
- The plugin can tag each draw's class into the stencil (INTZ has 8 stencil bits) and the pipeline turns it into a mask (`buildMaterialMask`, debug view "Materials"). Currently **only for the debug view**.
- Learned when it was used for lighting (and reverted):
  - Additive draws (`TAdd`, headlight cones, decals; ZWRITE off) must not tag: they covered whole blocks and made them "lamps" (big white flashes). Only depth-writing draws tag.
  - Not every lamp-looking thing is `TSelfI`: with "glow only for `TSelfI`" lamps went grey and the **blue neon road borders stopped glowing**. Check in the debug view which materials the borders, the start arches and floodlight panels really are before relying on the mask.
  - Masks are in-game only: the previewer has no stencil, so every mask change is untested until the owner runs it.

## What was tried on 2026-10-10 and reverted (all in one batch - the mistake)

Material-only emissive, lamp floor on all maps, spill only from light materials, tone curve knee 0.8 ("the game's white stays white"), sun removal with the game's direction and full strength under black-hole skies, hiding the game's sun halo (`LensFlareAdd`). Result in game: lamps grey day and night, neon borders dark. Reset to commit 8e14c69 (owner: "Lampen sind behoben"), checked pixel-identical on four captures.
Most of these ideas are still right; they failed together and untested. Bring them back **one per test**.

## How to change lights from now on

1. **One change per owner test.** Lights, tone curve, reflections, spill: never two at once.
2. Prefer a **setting / A-B switch** (or the debug view) so the owner can compare in the same spot.
3. Before handing over: render the captures in `~/tools/tm-re/captures` (day A03/A01, night A13, TMUF Island/Rally) with the new build and with the confirmed shader (`tmvs_preview <cap> out.bmp --preset X --shaders <dir with the old tmvs.hlsl>`), and show the difference. Unchanged captures must stay pixel-identical.
4. Things that only exist in game (material masks, the game's halo, interface) need the debug view and a short test list.
5. The game's own image is the floor: no mod step may make a light darker or a non-light brighter than the game drew it.

## Findings 2026-10-11 (captures 011-020, photos 2-5)

- Island headlight pool: already near white in the game's image; `emissiveAmount` takes it for a light and the glow is added after the night darkening (`c *= lerp(1, .16, night)` comes first). The pool is an additive draw without depth writes: no class in the stencil (an "additive light" stencil bit would mark it).
- Reflections: half resolution, 16 quadratic steps to 120 m, static noise without TAA (dashes); every bright hit is boosted x2.75, not only lights (white dots).
- `removeGameSun` uses `u_SunWorld`, the black hole's direction under Horizon; the game's sun is in c43 but no shader reads it.
- Grey lamps in screen 20 came from the reverted batch: the current shader renders capture_020 with white lamps.

## Candidate next steps (by expected value, each its own test)

1. **Reflections calmer** (the "overexposed" complaint): reflected lamps not boosted (or only light materials), night presets' dry reflections below 1 (sheen, not mirror), more steps / less noise outside photo mode. Low risk, previewer-testable.
2. **Tone curve**: knee 0.8 on its own, compared on the captures; with the current glow it may make guessed lights too bright - test alone.
3. **Night skies on day maps the clean way** (owner's choice): built 2026-10-11 as `GameNight` (on by default since 2026-10-11): the decoration's time of day is set to midnight at map load (`LoadScene3d`), so the game lights the map with its moon and there is no baked sun to take out. Day lightmaps hold only masks, so this works without recomputing them; NightOnly lamps and baked lamp light stay off (that would need the night decoration, see GOTCHA.md "Time of day"). Owner test pending.
4. **Material mask for emissive**, after the debug view shows which materials the neon borders and floodlights are (add them to the light class), and keep the guess for anything unknown.
5. **Spill only from lights** (fixes the doubled START text) - once 4 works.
6. Sun halo off under night skies (`LensFlareAdd` hook is in place, switched off).

## Reverted 2026-10-11 (owner: "lamps not like before, rectangles around them")

- The "added light" stencil bit (draws with one-one blending, no depth writes) marks the whole
  quad of a glow sprite, not just its glowing part. Turning `emissive` off there and flooring the
  colour at 80 % of the game's (Island headlight pools) changed lamps in rectangles around them.
  Both are out again, as is "only lamp material is boosted in reflections" (lamps are rarely in
  the light class). The bit is still built and shown in the debug view.
- Needed before the bit can be used for light: tag only where the sprite adds something (alpha
  test or a colour threshold while the stencil is written), checked in the debug view on the
  start arch and a floodlight.
- "Only small light sources keep the game's white" (a ring of 8 samples in PS_Final) made the
  night lamps clearly dimmer for the owner: reverted. Floodlit stands stay burned out on
  Stadium night maps (the game's own image is 25 % clipped there).

## Findings 2026-10-12 (owner: "everything clips"; screens 004, 011, photo 3)

Measured in the previewer on captures 004 (A13, night), 011 and 041, preset Horizon.

- **The game's own image is already clipped where it looks overexposed.** Pixels >= 250 in
  the *game's* image: 7.2 % (004), 11.6 % (011): start ramp, banners, car body. No post
  step can bring back what the 8-bit target cut. Hence the float target (`GameHdr`, below).
- **The frost on floodlit white was the mod's.** The same game white (255) came out
  anywhere between 160 and 255 (game 250 -> 224 on average, game 255 -> 209: not even
  monotonic). Three per-pixel switches acted on the same clipped white:
  1. the sun-glare taming (`c *= lerp(1, 0.6, glare)` in PS_Lighting), active on night maps
     too, gated by `1 - emissive`;
  2. the lamp guess (`emissiveAmount`), which on night maps calls any white facing up a lamp;
  3. the night lamp floor's ring test (PS_Final): the border of a large floodlit surface has
     few bright samples around it too and keeps full white (a frayed white band).
  With all three off the ramp is even (255 -> 205..231), but lamps drop from 252 to 211.
- **Fixed now (`CleanHighlights`, on by default):** no sun-glare taming on night maps (no sun
  there). Ramp spread (std of game-255 pixels) 18.8 -> 15.1, lamps unchanged (252.5), day
  captures pixel-identical, night captures 002-009 change (floodlit concrete closer to the game).
- **Tried and dropped:** an "edge of a large surface" test (one side of an opposite pair
  bright, the other dark) in `emissiveAmount` (no gain) and in the floor's ring (frost 18.8 ->
  10.0, but the lamp clusters of the start arch are larger than the ring: lamps 252 -> 237,
  beige and blotchy). In 8 bit a lamp and a floodlit wall are both 255: any pixel test for it
  is a guess. The real fix is light above 1.0 from the game.
- **Black crush** (day map under a night sky, 041): black pixels 0.4 % -> 2.7 %, 1st percentile
  6 -> 0 (night darkening x0.16, contrast 1.18 in log space, vignette; the tone curve has no toe).
- **Grain on the car at night** is the static `ign()` noise of AO, shadows and the reflection
  blur without TAA (the owner has TAA off): noise measure 2.38, 1.85 with TAA, 2.30 without GI.
  GI is not the cause.

## The game's HDR (GameHdr, 2026-10-12)

- The main camera draws straight into the back buffer (trace: 590 draws, 52 fixed function,
  no StretchRect or target switch in between), and `TmForever.exe` compiles its shaders at run
  time (`d3dx9_30.dll`: `D3DXCompileShader`, `D3DXAssembleShader`; ps_2_0 / ps_3_0).
- `GameHdr` (on by default since 1.4.0, which EngineLights needs; first a diagnostic): at the main camera's begin a float target
  (A16B16G16R16F) stands in for the game's; at its end it is resolved into the game's target,
  cut to 0..1 (`PS_Resolve`), so the pipeline and the look are unchanged. The log says once
  per map how much of the image lies above 1.0; F12 writes it next to the capture as
  `capture_NNN.hdr16` ("TMH1", width, height, RGBA half floats, rows top-down).
- `gameshaders`: when the folder `TMVS\gameshaders` exists, every shader the game compiles is
  written there with its defines, includes and disassembly. Answers whether the game's output
  is cut to 1.0 (`saturate`, `_sat` on `oC0`) and how the lightmap is scaled.
- Next, depending on the result: light above 1.0 arrives -> tone curve and lamps tuned offline
  on the `.hdr16` captures, the 8-bit guesses (emissive, night floor) off for HDR input; the
  game's shaders cut it -> rewrite their source at compile time (drop the final saturate or
  scale the output); neither -> the clean 8-bit path stays and flat white is a hard limit.

### Owner test 1 of GameHdr (2026-10-12, A01 at midnight, capture 044)

- Float target works (blending supported, nothing broken). 1.1 % of the image above 1.0,
  brightest 3.0: car body (5 % of its pixels), neon borders, floodlights. The car's rear wing
  has a gradient in the float image where the 8-bit one is flat.
- The game's shaders (dumped): blocks are `Diffuse * (2 * Lightmap) * Clouds * 2 + specular`,
  no saturate on the output (`ps_2_a`, `Common.PHlsl.txt`); only `LightAD_Dir1` caps the
  directional light at 1. So what clips is the 8-bit target, not the shaders.
- Now the lighting pass takes the float image on s0 (the lamp guess and the glow clamp it to
  1), and the tone curve's shoulder is longer with HDR input (width 3.0 instead of 1.7):
  linear 1 / 2 / 3 -> sRGB about 211 / 235 / 245 instead of 220 / 242 / 252.
- The washed-out car at night was the lamp guess: its bright body passed for a lamp and
  glowed x HighlightBoost (car p95 210 vs. the game's 191; 135 without the boost). With
  `CleanHighlights` the car material (stencil) is no lamp. In game only (no stencil in the
  previewer). PS_Lighting is at 3897 of 3900 slots.

### Owner test 2 of GameHdr (2026-10-12): ramp fine, lamps darker

- Why the lamps got darker: in the game's float image a lamp is about as bright as floodlit
  concrete (floodlight heads 1.0..1.25 on A01, a floodlit surface up to ~1.5). The game never
  drew lamps brighter than what they light. Any one tone curve treats both alike: the longer
  shoulder that gave the ramp its texture back pulled every lamp the guess missed to grey (the
  night floor holds big lamp clusters only at `NightSurfaces`, ~204). No global curve can fix
  both; per pixel the image cannot tell them apart either. The engine can: it knows the material.
- Now with GameHdr: pixels of the light class (`TSelfI` materials, the cars' lights; stencil)
  glow in full from 0.4 to 0.8 of the game's white, whatever the guess says (c45.zw).
  Untested in game: it helps only the lamps that really are `TSelfI` (in the 2026-10-10 batch
  many were not).
- **F12 now records which material drew each pixel** (`capture_NNN.mat`, with the material
  masks on): the frame after F12 tags every depth-writing draw with its material's own id
  (1..255) instead of its class, the pipeline reads the ids back (one pass per stencil bit)
  and builds the class mask from a palette, so the frame looks as usual. The file lists each
  material's shader file, class, draws and whether it writes depth or adds light (glows have
  no pixels: they write no depth). `python tools/materials.py capture_NNN.mat [--map ids.png]`
  shows how much of the image each material covers and how bright the game drew it there
  (8-bit and float). The previewer reads the `.mat` next to a capture: material masks (car
  test, lamps, debug view 8) are testable offline from now on (`--nomat`: without).
- Next: an A13 capture with `.hdr16` and `.mat` tells which materials the arch lamps, the
  floodlights and the neon borders are. Those that aren't `TSelfI` go into the light class
  by name (`engine::classify`): lamps known, not guessed, and the guess can step back.
- The car with the car test (now visible offline on capture 044 with a synthetic `.mat`): no
  pink-white glow, texture on the body, but darker than the game (car p95 194 vs. 255).

## Normals from the game's shaders (GameGBuffer, 2026-10-16, off by default)

- **Why:** the pipeline's normals come from the depth buffer (PS_Prepare): stair steps on curved
  surfaces (the car, ramps), no bump detail, noise where decals z-fight. TMNF compiles its
  material pixel shaders from HLSL at run time (`gameshaders`), so their source can be made to
  write the normal they shade with.
- **The patch (`src/gbufpatch.cpp`)**, on the source handed to `D3DXCompileShader`: one more
  `PS_OUTPUT` field (`COLOR1`) and, before each `return Output;`, `float4(normalize(N), |E|)`:
  - N = what the shader's own editor debug view calls its world normal (`MDbg_NormalInW(...)`:
    `nNormal`, normal-mapped on blocks; `NormalInWorld` on water), else the interpolated input
    `NormalInW` (car, decorations). Shaders that show a constant there (`float3(0,1,0)`: the flat
    ground and road blends) or have neither are left alone.
  - E = the eye vector the vertex shader already passes (`EyeToPos`, `EyeToPosInW`, `PosToEyeInW`,
    `PosInW` = `Position - GbxEyeInVisual` in world orientation); only its length is used.
  - The game's compile runs first; the patched result replaces it only when it compiled, writes
    `oC1` and every constant/sampler of the game's table is in the same register. Otherwise the
    game gets its own bytes.
- **Dumped Stadium shaders (88 pixel shaders, `tmvs_gbuftest <gameshaders> --device`):** all
  recompile from the dumps, 86 of 86 with a hash to the game's exact bytecode. 33 patched (19
  `nNormal`, 13 `NormalInW`, 1 `NormalInWorld`), +2..7 instruction slots, none failed; 48 have no
  world normal (ground/road blends, shadows, depth, post effects), 5 no eye vector (the light
  accumulation shaders take an absolute position), 2 another structure. On the GTX 1080 Ti all
  33 draw COLOR0 bit-identical to the game's shader with random inputs and textures; COLOR1 is a
  unit normal and its w the eye distance. Both compiles cost 1.7 ms more per patched shader.
- **Binding (`plugin.cpp`):** in the main camera, with GameHdr (two targets need the same bit
  depth: A16B16G16R16F), a cleared target is bound as render target 1 only for draws with a
  patched shader, depth writes on and no blending (blending acts on every target), into the float
  scene; unbound for any other draw and before the game's Clear (it would wipe it) and target
  switches. Measured on the device: fog leaves target 1 alone (NVIDIA), an alpha-tested pixel
  writes no normal, `SetRenderTarget(1, ...)` keeps viewport and scissor, a shader writing COLOR1
  with nothing bound draws as before (so turning the setting off while playing is safe).
- **Which normal is used (PS_PrepareGBuffer):** a draw with a shader without normals can come
  later and cover a block (road, terrain, ps_1_1 materials). So the game's normal counts only where
  its distance matches the depth's (1 % + 3 cm) and it is a unit vector; elsewhere the depth's
  normal stays. Two-sided faces seen from behind are turned to the camera. Everything reading the
  normal buffer (AO, contact and long shadows, reflections, GI, relighting) takes it; PS_Lighting
  is untouched (3899 slots). 0.13 ms instead of 0.09 ms.
- **Offline proof:** off (and on without `.nrm16`) pixel-identical on captures 044/081/083/085;
  synthetic `.nrm16` from the depth: 5 % wrong distances all rejected (identical to off),
  flipped normals identical to facing ones, a ripple shows in reflections and light.
- **Only the game can tell:** whether the game compiles at every start (no shader cache) and
  creates its shaders from the blob it got (the log's draw count says it); how many of the main
  camera's opaque draws have a patched shader (the earlier trace: 466 opaque draws, all with a
  pixel shader; which of them are blocks and car); the look: bump detail in the sun's relighting
  and reflections, and in AO (normal-mapped normals tilt the AO hemisphere: AO may pick up the
  bump pattern - if it looks noisy, AO should keep the depth's normal); TMUF environments (other
  shaders: the log's patched/skipped counts per environment).
- **Next: the light apart from the albedo (COLOR2)**: built as `GameLight`, below.

### Owner test of GameGBuffer

1. Menu, *Game* page: *Normals from the game's shaders* on (or `GameGBuffer=1` in settings.ini,
   `[Settings]`). Restart the game (the shaders are patched as it compiles them at its start).
2. Log at the start: `gbuffer: on, the game's pixel shaders are patched as it compiles them` and
   `gameshaders: D3DXCompileShader hooked, D3DXAssembleShader left alone`.
3. A Stadium map (A01), drive 10 s. Log: `gbuffer: N pixel shaders patched, M left as they are
   (...), 0 kept after a failed patch` (Stadium: N about 20-35), `gbuffer: the patched shaders'
   normals go into a second target (2560x1440)`, after 8 s `gbuffer: X opaque draws in the main
   camera, W wrote normals, ...` (W should be a large part of X). Not expected: `kept after a
   failed patch` > 0, `the game draws into a second target itself`.
4. The game's image must look exactly as before with F7 (shaders off): the patched shaders draw
   the same colour. With the mod on: the car's AO and reflections without stair steps, bumps on the
   blocks in the sun. Switch the setting off and on in the menu (works without restart now) to
   compare; *Debug view* *Normals* shows it best (bump detail on the blocks, a smooth car).
5. F12 on a spot with blocks and the car (also once at night). Log: `gbuffer (capture): normals
   on X% of the image; ...`. The capture gets `capture_NNN.nrm16`; send it with the `.tmcap`.
   Offline: `python tools/gbuffer.py capture_NNN.tmcap --map gbuf.png` and the previewer with
   `--set GameGBuffer=1` against `=0`.
6. Watch for: surfaces black, flickering or with wrong colour (a broken patch: not expected),
   noisy AO on bumpy blocks, water reflections changed, frame time (overlay) more than ~0.3 ms up.
   Anything wrong: setting off, everything as before.

## The game's light apart from the paint (GameLight, G-buffer step 5, 2026-10-17, off by default)

- **Why:** every night decision the mod makes on the image confuses paint with light: a white wall
  in the moonlight and dark asphalt under a lamp come out equally bright. The game's shaders know
  the difference: they multiply the paint (`Diffuse`) with the light (`LDiffus`). Steps 1-4 (the
  normals) changed only 0.5 % of the pixels by more than 8 of 255 (wall and border edges).
- **The patch (`gbufpatch`, `Mode::Light`)**: the light is what each shader's own editor debug view
  shows as `MDbg_LightDiffuse(X)`: blocks `LDiffus = (Ambient + PreLightGen) * Occlusion + Lighting`,
  the ground `Light` (at night its baked `PreLightGen`: dome, moon with shadows, lamps), the car
  `LHdr` / `LAmbient + LDiffus`. COLOR2 = `float4(X, |eye vector|)`. Every shader patched in this
  mode writes COLOR1 too (0 instead of a normal on the ground), so both targets come from the same
  draws. The ground shaders have only `PosToEyeV` (object space; the ground isn't scaled) as eye
  vector: accepted here, and a wrong length only means the pipeline drops the pixel. A light that
  doesn't compile in the shader (the three ps_2_0 water shaders: a constant would move) gives the
  same patch with 0 for the light. The `Clouds * 2` factor and the ground's `Occlusion` stay outside
  X (they multiply the paint and the light alike).
- **Dumped Stadium shaders (`tmvs_gbuftest <gameshaders> --light --device`):** 45 of 97 patched (35
  with their light: 18 blocks, 10 mown grass, 7 car; 10 with 0: water, the self-illuminated material
  and others without that debug line; the soil shaders have no eye vector and stay as they are), 0
  failed; the normal patch is still byte-identical to the game's dumped bytecode (95 of 95). On the
  GTX 1080 Ti with random inputs: COLOR2 equals the shader's own light (a reference that swaps it
  into COLOR0) on all 35, COLOR2.a = COLOR1.a; COLOR0 identical except one block shader where 1 of
  4096 pixels differed by one float16 step (0.0005: the compiler ordered an instruction
  differently; far below one 8-bit step). Three targets bound: fog only on the game's.
- **Cost:** full-screen 2560x1440 layers, averaged over the 35 shaders: game 0.083 ms, + normal
  target 0.126 ms, + light target 0.183 ms (+0.057 ms per layer, bandwidth); the light target's
  clear 0.005 ms. The patched draws cover 60-75 % of the image (`.nrm16` of 089/093), about one to
  two layers with overdraw: ~0.06-0.12 ms in game, plus 29.5 MB at 1440p. Lighting pass 1.739 ->
  1.772 ms (`--bench`, 083). Without the normals on, GameLight binds their target too (+0.04-0.08 ms).
  Compiling: ~0.5 ms more per patched shader at the game's start.
- **The use (PS_LightingGameLight, its own entry point: PS_Lighting and PS_LightingEngine are
  byte-identical to before):** under a night sky (u_Sky.y > 0) and only where the game's light is the
  night's (`u_Light2.y` -1: night map or a map lit at midnight), `lamp = smoothstep(1.0, 1.6, max(L))`
  where the light belongs to the surface seen (distance within 1 % + 3 cm of the depth's): the
  day-for-night darkening and blue tint step back by `lamp`. 1.0 is "the paint as it is" (the day
  sun's full light on the ground, which the ground shader caps there); at night only stacked lamp
  light gets above it. Where lamp = 0 or the light is unknown: exactly the old formula.
- **Considered and left out:** tinting the light instead of the paint (keeps colours saturated at
  night: the image-wide desaturation is the night look's intent, not an error); the black hole's
  light by the paint (`unlit / L`) instead of by the lit colour: the right physics, but its
  strength needs the real light's level first (next step, one change per test).
- **Not verified offline:** the real light's levels at night (no capture has a `.lit16` yet; the
  synthetic one from `tools/gbuffer.py --synth-light`, grey light from the blurred float image, only
  tests the plumbing: it takes white paint for light). Thresholds 1.0..1.6 are the first guess; the
  first F12 with GameLight calibrates them (`tools/gbuffer.py --light` prints the percentiles and how
  much counts as lamp light).

### Owner test of GameLight

1. Menu, *Game* page: *Night on game light* on (`GameLight=1`), *Game HDR* and *Game light sources*
   on. Restart the game.
2. Log at the start: `gbuffer: on with the light (GameLight), ...`. After a map: `gbuffer: N pixel
   shaders patched (L with their light), ...` (Stadium: L about 10-30), `gbuffer: the patched
   shaders' light goes into a third target (2560x1440)`, after 8 s `gbuffer: X of those draws wrote
   their light (GameLight)`. Not expected: `kept after a failed patch` > 0, `two targets at a time`.
3. A night map (A13 or C12) with the night preset (Horizon): switch *Night on game light* off and on
   in the menu at one spot. Expected: lamp-lit asphalt, the floodlit stands and the area right under
   the masts brighter and warmer with it on; dark, moonlit parts unchanged. Too bright or a whole
   floodlit field lifted: say so (the threshold is a guess).
4. F12 at the same spot, once with it on: the capture gets `capture_NNN.lit16`; the log says
   `gbuffer (capture): light on X% of the image, brightest Y`. Send `.tmcap`, `.lit16`, `.hdr16`.
5. A day map: nothing may change (no night sky). Frame time: ~0.1-0.2 ms more at most.
