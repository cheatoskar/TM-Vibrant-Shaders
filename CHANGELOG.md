# Changelog

## Unreleased (1.0.0-beta.2)

- Long-range shadows from a height map of the track that builds up while you drive: long evening shadows, shadows from off-screen objects
- Neon light: coloured borders and signs light up the surfaces around them
- Wet roads, puddles with ripples, rain, screen-space reflections, water surfaces (TMUF, experimental)
- Grass detail: patches, mowing stripes, blades near the camera
- Volumetric clouds
- Ring World sky: Saturn, Jupiter, ice giant or exotic gas giant, distant or next to the rings, with a far black hole
- Temporal anti-aliasing, motion blur and depth of field (cinematic)
- Replay camera blends: every camera of a frame is shaded, temporal effects pause during cuts and blends
- New presets: Rainy Day, Ring World, Replay Cinema
- Vibrant: more sun, stronger shadows and light shafts, neon glows in daylight too
- Fixed: the sun's glare on the road counted as a light source (blotchy glow, washed-out road)
- Previewer: `--batch` for many variants per run, `--move` to test motion blur

## 1.0.0-beta.1

First public beta. This is a complete rewrite of the earlier colour-filter prototype as a real lighting pipeline.

- Engine-level injection before the HUD, readable scene depth (INTZ), camera and sun taken from the game
- Ambient occlusion, screen-space sun shadows, sun/sky relighting, light shafts, height fog
- HDR reconstruction of light sources, bloom, auto exposure, filmic tone mapping, FXAA, CAS sharpening
- Custom skies: clear sky with clouds, starry night, ray-traced black hole with ringed planet, aurora
- 10 presets, plus your own named presets
- Automatic preset per map mood (day / sunset / night)
- Every setting is saved automatically
- Effects also apply in replays and video export
- Setup.exe: install for the TrackMania ModLoader or straight into the game folder (d3d9.dll, no ModLoader needed), update, uninstall
