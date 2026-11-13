## TM Vibrant Shaders 1.6.1 and Horizon modules 2.0.1

A fix release for 1.6.0 and the Horizon modules 2.0.0.

Tell me what breaks (with `Documents\TrackMania\TMVS\tmvs.log`; ESWC `tmvs-eswc.log`, Sunrise `tmvs-sunrise.log`, Original `tmvs-original.log`; TrackMania 2 `Documents\ManiaPlanet\TMVS\tmvs-mp.log`).

---

### Which file

| You play | Download |
|---|---|
| Nations/United Forever, Nations ESWC, Sunrise, Original, TrackMania 2, without Horizon Client | `TM-Vibrant-Shaders.zip` (setup), or `TMVibrantShaders.dll` alone |
| Nations/United Forever, Nations ESWC, Sunrise, Original, with Horizon Client | `TMVS-2.0.1-horizon.zip` (four modules for Horizon's plugins folder) |

Not both in the same game: with the standalone mod and the modules installed, only one runs.

### Fixed

- **Photo mode no longer switches itself on online.** On a server the game puts you into the free camera at the end of every round and while spectating, and *Auto photo mode* turned photo mode on each time. It now follows the free camera only offline (replay editor, solo race, map editor test drive, a replay from the menu). `F6` works everywhere as before.
- **Fewer short stutters.** The sea level measurement on the water, the minimap's picture copy and the HDR numbers in the log read the GPU's picture back at once, which made the CPU wait for the whole frame. They are now read a frame or two later, without waiting.

### Changed

- **The game's Cpu/Gpu synchro is in the log, with advice.** All five Direct3D 9 games default to *Immediate*: the CPU waits for the GPU after every frame, so the effects' GPU time adds to the frame time in full. *1 Frame* in the game's launcher (Advanced) lets the two overlap; Horizon Client 0.3.2 can switch it in the game. Details: [docs/performance.md](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/{{TAG}}/docs/performance.md#cpugpu-synchro-the-game-waits-for-the-gpu).
- **The perf line in `tmvs.log` shows where the CPU's time goes**: the game's logic, its rendering, the mod's own time, Present and the wait for the GPU.

Both fixes are covered by tests without the game; in the game they have only been tried in parts so far.

---

### Install (Nations & United Forever)

**With the ModLoader, updates itself:** close the ModLoader, open `%LOCALAPPDATA%\TMLoader\config.yaml` with Notepad and add this line under `servers:`:

```yaml
  - 'https://cheatoskar.github.io/TM-Vibrant-Shaders/modloader/'
```

Then tick *TM Vibrant Shaders* in your profile. New versions arrive on their own from then on.

**With the setup:** download `TM-Vibrant-Shaders.zip`, extract it and run `TM-Vibrant-Shaders-Setup.exe`. It also installs into TrackMania 2, Nations ESWC, Sunrise and Original (*Install into another game folder*).

In the game `F8` opens the menu and `F7` turns the effects on and off. Step by step: [installation guide](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/{{TAG}}/docs/install.md) ([Deutsch](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/{{TAG}}/docs/INSTALL_DE.md)).

All changes: [changelog](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/{{TAG}}/CHANGELOG.md).
