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
- `GameHdr` (off by default, diagnostic): at the main camera's begin a float target
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
