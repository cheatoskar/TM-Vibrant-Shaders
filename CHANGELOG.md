# Changelog

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
