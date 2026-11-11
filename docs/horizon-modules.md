# TM Vibrant Shaders as Horizon modules (2.0)

From 2.0 on TM Vibrant Shaders comes in two forms, built from the same code:

- **Standalone**, as before: `TMVibrantShaders.dll` as a ModLoader product, or `d3d9.dll` in the game folder. Its own menu (`F8`), its own keys. Nothing changes here compared with 1.5.
- **Horizon modules**: four plugins for Horizon Host (TM Horizon's in-game menu, 0.2.0 or newer, plugin ABI 1.2). Each module has its own page in the Horizon menu, its own switch and its keys in Horizon's *Hotkeys* list. `TMVS-<version>-horizon.zip` holds them.

Games: TrackMania Nations Forever, TrackMania United Forever and TrackMania Nations ESWC (the 32-bit Direct3D 9 games). TrackMania 2 (ManiaPlanet) stays standalone only; Horizon Host doesn't run there.

## The four modules

| Module | Folder / plugin id | What it does | Horizon page |
|---|---|---|---|
| **TM Vibrant Shaders** | `tmvs` | The shaders: presets, sky, light, weather, colour, the photo look. Holds `tmvs-core.dll`, which all four use. | *Settings* (live preview, update check) and the tabs Shaders, Customize, Studio, Maps, Performance, Misc, About |
| **TMVS Capture** | `tmvs-capture` | Photos (`F12`, hi-res `Shift+F12`, photo mode `F6`), clips of your runs and, new, of each checkpoint's sector, and the video export. Works without the shaders. | *Settings* (photo mode, files) and the tabs Clips, Video |
| **TMVS Upscaling** | `tmvs-upscaling` | Render scale with AMD FSR 1: the game draws at a lower resolution and FSR brings it back up. Works without the shaders. | *Settings* (render scale, sharpness) |
| **TMVS Packs** | `tmvs-packs` | Resource packs (texture mods from `Skins\<Environment>\Mod`) on every map, chosen per environment, loaded live. | *Settings* (the packs page) |

What each one does without the others:

- **Shaders off, Capture on**: photos, clips and the video export show the game's own picture.
- **Shaders off, Upscaling on**: the game is drawn smaller and FSR scales it up, nothing else is changed (no colour, no TAA).
- **Shaders on, Upscaling off**: render scale stays at 1.0 even if `settings.ini` says otherwise; the setting is kept for later.
- **Capture off**: no photo keys, no clips (the recorder stops and frees its memory), no MP4 video export (the game's own export still works).
- **Packs off**: maps load with their own mod or none, as without TMVS.

Switching a module off in Horizon's list takes effect at once (live toggle); nothing needs a restart.

## Install (Horizon)

Copy the four folders from the zip into Horizon's plugins folder:

- TrackMania Nations/United Forever (TMLoader): `%LOCALAPPDATA%\TMLoader\database\TmForever\products\Horizon Host\<version>\plugins`
- TrackMania Nations ESWC (Horizon's own loader): `<game folder>\horizon\plugins`

Keep `tmvs\tmvs-core.dll`, even if you switch the TM Vibrant Shaders module itself off: the other three need it.

## Standalone and Horizon together: only one runs

Both forms are the same mod. When both are installed, the one the game loads first runs and the other stays passive (usually the standalone, which loads with the game's Direct3D). The passive one says so:

- `tmvs.log`: `exclusive: this copy runs (standalone (d3d9.dll)); another copy of TM Vibrant Shaders in this game ... stays passive`, or on the modules' side `TM Vibrant Shaders already runs in this game as standalone (d3d9.dll): it stays in charge, the modules stay off`.
- Horizon's log and each module's page show the same sentence; the modules register no keys and draw nothing then.

Remove one of the two. Nothing is drawn twice and no key fires twice in the meantime.

## Settings

Both forms use the same files, so switching keeps your look, your keys and your clips settings:

- `Documents\TrackMania\TMVS\settings.ini` and the presets next to it (the log of TrackMania Nations ESWC is `tmvs-eswc.log`, as before)
- `Documents\TrackMania\TMClips\clips.ini` (clips, shared with the TM Clips mod as in 1.5)

2.0 writes `SettingsVersion=2` into `[General]`. A 1.5 file (no such key) is read as it is; nothing in it is renamed. 1.5 ignores the new key, so going back to 1.5 works.

New setting: **Checkpoint clips** (`CheckpointClips` in `clips.ini`, off by default, *Clips* page): saves a clip of each checkpoint's sector, from the previous checkpoint (or the run's start) to one second after this one, named `<map>_<date>_CP<n>_<time>.mp4`. It reads the race from the game like the finish clips and writes nothing.

## Keys

- **Standalone**: as in 1.5 (`F8` menu, `F5` effects, `F12` photo, `F3` clips and so on), from `settings.ini` and `clips.ini`.
- **Horizon**: Horizon owns the keyboard. The modules register their keys in Horizon's *Hotkeys* list with the keys from your `settings.ini`/`clips.ini` as defaults; change them there. The menu key is Horizon's (`Insert`). With Horizon Host ABI 1.3 (`open_page`) your old menu key (`F8`) also opens the TMVS page in Horizon's menu; an older Horizon has no such key.

## Online and TMX rules

Nothing changes against 1.5: no input is sent to the game, the race is only read (also for checkpoint clips), no checkpoint or trigger display, no ideal or brake line, no camera or MediaTracker change. All four modules are marked online-safe for Horizon. Photos, clips and the video export never show Horizon's interface (the modules set Horizon's capture flag while they take a picture).

## How it is built

One code base, one `cmake --build build --config Release` builds both:

- `tmvs_runtime` (all of TMVS) is linked into `TMVibrantShaders.dll` (standalone, d3d9 exports) and into `tmvs-core.dll` (exports `TMVS_Core_*` for the modules).
- Four small plugin DLLs (`tmvs.dll`, `tmvs-capture.dll`, `tmvs-upscaling.dll`, `tmvs-packs.dll`, `src/modules/module.cpp`) find `tmvs-core.dll` (their own folder, then `..\tmvs\`) and attach to it. Their interface: attach/detach/enable, draw the page, query state (`src/modules/hostbridge.cpp`).
- In Horizon, TMVS doesn't hook `Present` and `Reset` itself: Horizon's device callbacks drive it (`hook::installHosted`). The other device hooks (draws, render targets, the depth texture) stay TMVS's own, as Horizon offers no callbacks for them.
- `release_zip` makes `TM-Vibrant-Shaders.zip`, `TMVibrantShaders.dll` and `TMVS-<version>-horizon.zip`; `horizon_zip` makes the Horizon zip alone.

Tests without the game: `tmvs_moduletest` (fake Horizon host: loading, unloading, exclusivity in both orders, settings of a 1.5 file, Capture without the shaders writes an MP4), `tmvs_modulelogictest` (module states, settings version, checkpoint tracking). `tmvs_hzpreview <folder>` draws Horizon's real menu with the four modules into PNGs (needs a horizon-host checkout next to this repo).

## My test plan (2.0 against 1.5)

Each point twice: once standalone (`TMVibrantShaders.dll` of 2.0 in the ModLoader, Horizon modules not installed), once with the Horizon modules (standalone removed). Compare with 1.5 on the same map (A01 by day, one night map).

1. **Start**: `tmvs.log` says `starting (standalone ...)` or `starting (Horizon modules ...)` and `exclusive: this copy runs`. Horizon: four modules *On* in the menu, keys in *Hotkeys*.
2. **Look**: my preset loads as in 1.5 (same `settings.ini`), Shaders tab changes it live, effects key on/off.
3. **Photos**: `F12`, `Shift+F12` (hi-res), photo mode `F6` with Hide HUD/Hide car. Horizon: no Horizon bar or menu in the photo.
4. **Clips**: finish with a PB saves a clip; `F3`, `Ctrl+F3`, `Ctrl+Shift+F3`. Horizon: keys from the Hotkeys list, the clip has no Horizon interface.
5. **Checkpoint clips**: switch on (Clips page), drive a run with checkpoints: one `..._CP<n>_...` clip per checkpoint, each from the previous checkpoint.
6. **Video export**: a replay, *Shoot video* with the mod's MP4 export, sound and length as in 1.5.
7. **FSR**: render scale 0.67, with and without the shaders (Horizon: TMVS module off, Upscaling on). Sharp picture, no shader look when TMVS is off, photos at full size.
8. **Packs**: a pack for Stadium, load two maps; switch Packs off in Horizon: the next map has no pack.
9. **Both installed**: standalone and modules together: only one runs, the log line says which; no double keys, no double drawing.
10. **Back to 1.5**: start 1.5 with the 2.0 settings files: look and clips settings as before.
