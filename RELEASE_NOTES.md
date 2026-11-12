## TM Vibrant Shaders 1.6.0 and Horizon modules 2.0.0

New: **TrackMania Sunrise** and **TrackMania Original** (experimental), **clips and the MP4 video export** on the three older games, and the whole mod as four **Horizon Client modules** (2.0.0).

Tell me what breaks (with `Documents\TrackMania\TMVS\tmvs.log`; ESWC `tmvs-eswc.log`, Sunrise `tmvs-sunrise.log`, Original `tmvs-original.log`; TrackMania 2 `Documents\ManiaPlanet\TMVS\tmvs-mp.log`).

---

### Which file

| You play | Download |
|---|---|
| Nations/United Forever, Nations ESWC, Sunrise, Original, TrackMania 2, without Horizon Client | `TM-Vibrant-Shaders.zip` (setup), or `TMVibrantShaders.dll` alone |
| Nations/United Forever, Nations ESWC, Sunrise, Original, with Horizon Client | `TMVS-2.0.0-horizon.zip` (four modules for Horizon's plugins folder) |

Not both in the same game: with the standalone mod and the modules installed, only one runs.

### TrackMania Sunrise and TrackMania Original (experimental)

Only the Windows 10 repack of Sunrise (`TmSunrise.exe`) and the Online Patch v2 of Original (`TMORIGINAL.EXE`); other builds are refused cleanly. The game's own `d3d9.dll` (the repack's proxy, dgVoodoo) stays in use as `d3d9.chain.dll`; the setup does that for you. The look, photo mode, the car and the race (neon trail, headlights, spray, long mesh shadows, Hide car, split times), clips and the MP4 export. Not yet: water, the game's materials and light, resource packs, the minimap.

### Nations ESWC, Sunrise, Original: clips and MP4 export

- **Clips**: the *Clips* page, `F3`, `Ctrl+F3`, `Ctrl+Shift+F3` and *Save on finish*, as on Forever.
- **Shoot video** writes an MP4 with the mod's encoder (H.264, HEVC or AV1) and the game's own sound (no more microphone in the video). *Game's AVI* on the *Video* page gives the game's file as before.

### Horizon modules 2.0.0

*TM Vibrant Shaders*, *TMVS Capture*, *TMVS Upscaling* and *TMVS Packs*: the same features as the standalone 1.6.0, each with its own page, switch and keys in Horizon's menu. For Horizon Client 0.3.1 (the pages follow Horizon's menu size and content scale); they also load in older Horizon versions. TrackMania 2 has no Horizon and keeps the standalone mod. [docs/horizon-modules.md](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/{{TAG}}/docs/horizon-modules.md)

### Also

- **Instant replay is off for new players**: switch it on at the top of the *Clips* page for the background recording (`F3`, saved best runs). If you used clips in 1.5, nothing changes for you.
- **Checkpoint clips** (*Clips* page, off by default): a clip of each checkpoint's sector.

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
