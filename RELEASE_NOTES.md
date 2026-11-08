## TM Vibrant Shaders 1.4.3 (pre-release)

Fixes for **TrackMania Nations & United Forever**, and the mod now works next to the VR mod [TrackMania Forever OpenXR](https://github.com/jiink/TrackManiaForeverOpenXR). TrackMania 2 is unchanged.

Tell me what breaks (with `Documents\TrackMania\TMVS\tmvs.log`).

---

### Fixed

- **Dirt no longer mirrors walls** after you were in the replay editor (also online): the mod kept the old materials' kinds for new ones the game put at the same place in memory.
- **Your car stays smooth with TAA**, driving and standing still. It used to look grainy while driving, more so with *Sharpening*.
- **No foam on the water above a sunken car.**
- **The effects run on maps where `F7` changed nothing** (a TMUnlimiter map of mine set no camera view of its own). On such maps the image also shook; it doesn't any more.
- When the effects are on but nothing is shaded for 2 seconds, `tmvs.log` now says why.

### VR (experimental)

With the VR mod [TrackMania Forever OpenXR](https://github.com/jiink/TrackManiaForeverOpenXR) both mods now run together (both from the ModLoader). The mod's menu (`F8`) shows in the headset. *Effects in VR* (page *Misc & VR*, off; restart the game after switching it) shades both eyes, with *Comfort*, *Quality in VR*, *TAA in VR* and *Rain on the windshield*.

This needs a VR mod build that hands its device to this mod. That change is a [pull request to the VR mod](https://github.com/jiink/TrackManiaForeverOpenXR/pull/27); until it is merged, use the build from [my fork](https://github.com/cheatoskar/TrackManiaForeverOpenXR-TMVS). The hand-over is by @wial663, who also tested it in the headset. Thanks!

---

### Install (Nations & United Forever)

**With the ModLoader, updates itself:** close the ModLoader, open `%LOCALAPPDATA%\TMLoader\config.yaml` with Notepad and add this line under `servers:`:

```yaml
  - 'https://cheatoskar.github.io/TM-Vibrant-Shaders/modloader/'
```

Then tick *TM Vibrant Shaders* in your profile. New versions arrive on their own from then on. This is a pre-release: that server keeps 1.4.2. To get the pre-releases through the ModLoader, use `https://cheatoskar.github.io/TM-Vibrant-Shaders/modloader/beta/` instead.

**With the setup:** download `TM-Vibrant-Shaders.zip`, extract it and run `TM-Vibrant-Shaders-Setup.exe`.

In the game `F8` opens the menu and `F7` turns the effects on and off. Step by step: [installation guide](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/main/docs/install.md) ([Deutsch](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/main/docs/INSTALL_DE.md)). TrackMania 2 players: the same zip, see 1.4.1's notes.

All changes: [changelog](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/{{TAG}}/CHANGELOG.md).
