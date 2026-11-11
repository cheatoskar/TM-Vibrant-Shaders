## TM Vibrant Shaders 1.5.0 (pre-release)

New: **instant replay clips** for TrackMania Nations & United Forever, the car and the race in **TrackMania Nations ESWC**, and photo zoom, hi-res photos, Hide car and the Misc page in **TrackMania 2**.

Tell me what breaks (with `Documents\TrackMania\TMVS\tmvs.log`, in TrackMania 2 `tmvs-mp.log`, in ESWC `tmvs-eswc.log`).

---

### Clips (Nations & United Forever, new)

The game is recorded into memory all the time, and after a finish the run is saved as an MP4 with the game's sound: 3 s before the start to 3 s after the finish, within about a second, without a replay and without rendering anything. By default only after a personal best.

- `F3` saves the last 30 s (or 60, 90). `Ctrl+F3` saves the last finished run, also after a restart. `Ctrl+Shift+F3` saves the run you drive now, from its start.
- The buffer follows the map (author time x 1.5 + 10 s, at least 60 s), within the memory limit of the 32-bit game.
- Sound: the game's own, the game and your microphone, everything the PC plays, or none.
- Clips go to `Documents\TrackMania\Clips`. Settings are on the new *Clips* page of the menu.
- NVIDIA's encoder (NVENC) driven directly, else the graphics card's H.264 encoder through Windows (AMD, Intel).
- It reads the race from the game's memory and writes nothing, so it is fine online.
- **TM Clips** is the same as a mod of its own for the ModLoader (`TM-Clips.zip`), with a small window on `Shift+F3`. With both installed, TM Vibrant Shaders records and TM Clips stays idle.

All of it in [docs/clips.md](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/{{TAG}}/docs/clips.md).

### TrackMania Nations ESWC (experimental)

The car and the race: *Neon trail*, *Car headlights* (also *Horror*), *Spray*, *Long mesh shadows*, *Hide car*, motion blur and *Cinematic in replays only*, and on the *Misc* page the split times, the Session panel and the finish effects. Still not there: the video export, water, the game's materials and light, the minimap. Only the unofficial 1.80 patch (`TmNationsESWC.exe` 0.1.7.9).

### TrackMania 2

- **Game camera** (*Settings*, off by default): photo zoom (`Shift`+wheel), hi-res photos (`Shift+F12`) and *TAA jitter*. This writes into the game's memory and is not tested against ManiaPlanet's anti-cheat, so it stays off unless you switch it on.
- **Only in replays** knows replays: depth of field and motion blur run in the replay editor and in photo mode, not while driving.
- **Hide car** in photo mode.
- **Misc page**: split times against your best run, the Session panel and finish effects. Read from the game only, so fine online.

### Fixed

- **With CenterSpeed loaded the menu and the effects never started** ("no game device 30 s after the start" in `tmvs.log`). The mod now puts its hook in front of the game's device again if something overwrote it, and takes the device from the game's viewport if it still went past.

---

### Install (Nations & United Forever)

**With the ModLoader, updates itself:** close the ModLoader, open `%LOCALAPPDATA%\TMLoader\config.yaml` with Notepad and add this line under `servers:`:

```yaml
  - 'https://cheatoskar.github.io/TM-Vibrant-Shaders/modloader/'
```

Then tick *TM Vibrant Shaders* in your profile. New versions arrive on their own from then on. This is a pre-release: that server keeps the last stable version. To get the pre-releases through the ModLoader, use `https://cheatoskar.github.io/TM-Vibrant-Shaders/modloader/beta/` instead.

**With the setup:** download `TM-Vibrant-Shaders.zip`, extract it and run `TM-Vibrant-Shaders-Setup.exe`.

In the game `F8` opens the menu and `F7` turns the effects on and off. Step by step: [installation guide](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/main/docs/install.md) ([Deutsch](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/main/docs/INSTALL_DE.md)). TrackMania 2 players: the same zip, see 1.4.1's notes.

All changes: [changelog](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/{{TAG}}/CHANGELOG.md).
