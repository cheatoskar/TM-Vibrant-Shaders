# Clips

[Back to the README](../README.md)

Clips save your runs as MP4 videos, without rendering anything again. The game is recorded into memory all the time, and when a run is finished (or when you press a key) the part you want is written to a file. I use it to keep every personal best without thinking about it.

For TrackMania Nations Forever, United Forever and TrackMania Nations ESWC (1.80 patch). It comes in two forms that share the code and the settings file:

- **In TM Vibrant Shaders**: the *Clips* page in the `F8` menu.
- **As TM Clips**: a small mod of its own for the ModLoader (`TMClips.dll`), for people who don't want the shaders. Its settings are in a small window on `Shift+F3`.

Not in TrackMania 2 yet. In TrackMania Nations ESWC only in TM Vibrant Shaders (its `d3d9.dll`; there is no ModLoader for it): the same page, keys and files, the race read from the game's own objects (see [ESWC](#trackmania-nations-eswc)).

## What it does

- Records the game's picture and sound into a buffer in memory (60 s by default, or longer on long maps, see [Adaptive buffer](#adaptive-buffer)). Nothing is written to disk until you save.
- After a finish it saves the run from 3 s before the start to 3 s after the finish, within about a second. By default only after a personal best. A restart starts a new run, a respawn at a checkpoint doesn't.
- On a key it saves the last 30 seconds, the last finished run, or the run you are driving now.
- No replay is saved, nothing is rendered again, and the game isn't changed. I read the race (start, finish, race time, map) from the game's memory and write nothing, so it is fine online.

## Install

**TM Vibrant Shaders:** nothing extra. The clips are built in, see the [installation guide](install.md) (ModLoader or `d3d9.dll` in the game folder). Open the menu with `F8` and go to *Clips*.

**TM Clips on its own (ModLoader only):**

1. Download `TM-Clips.zip` from the [release](https://github.com/cheatoskar/TM-Vibrant-Shaders/releases) and extract it.
2. Copy the folder `TM Clips` into `%LOCALAPPDATA%\TMLoader\database\TmForever\products` (paste that into the Explorer address bar).
3. Tick *TM Clips* in your ModLoader profile and start the game through the ModLoader.

TM Clips has no `d3d9.dll` form. It hooks the game's `Present` and `Reset` and nothing else.

**Both installed:** TM Vibrant Shaders records and TM Clips stays idle, in either load order. They use the same settings file, so a choice made in one is in the other. If the clips in TM Vibrant Shaders don't run, TM Clips takes over after a few seconds. Each log says who records.

## Keys

The defaults. All of them can be changed on the *Clips* page (*Change*, then press the new key; `Esc` cancels). They are not under *Settings → Hotkeys*. The keys only react while the game window is in front.

| Key | What it saves |
|---|---|
| `F3` | The last 30 s (30, 60 or 90 s, *Last seconds*) |
| `Ctrl+F3` | The last finished run (3 s before its start to 3 s after its finish), also after a restart, a respawn or when you left the finish screen, as long as the buffer still holds it |
| `Ctrl+Shift+F3` | The run you are driving now, from its start to this moment |
| `Shift+F3` | TM Clips' own window (only the standalone) |

If you press a key and nothing is saved, a short message in the game says why ("Not recording", "No finished run yet", "No run now", "Run no longer in the buffer"). The same messages are in the log.

The *Clips* page also lists the last five finished runs (time, PB, how long ago) with a *Save* button each. A run the buffer no longer holds is greyed out. Below that are the last clips with their size and *Open folder*.

## Adaptive buffer

On by default. The buffer's length follows the map: author time x 1.5 + 10 s, at least 60 s (and at least what `F3` saves), within the memory limit. The page shows the length for the loaded map and what it costs in memory. A new map with another author time starts recording again with its own length; the runs of the previous map are gone from the buffer then.

Switch it off to set a fixed length (10 s up to what fits).

## Sound

*Sound* on the *Clips* page:

| Mode | Records |
|---|---|
| Off | No sound |
| Game | The game's own sound (default) |
| Game + mic | The game's sound and your default microphone, mixed into one track |
| All sound | Everything the PC plays (Discord, music, ...) |

The game's own sound without the rest needs Windows 10 version 2004 or newer. On older Windows the mod records everything the PC plays.

## Where files go

| What | Where |
|---|---|
| Clips | `Documents\TrackMania\Clips`, as `<map>_<date>_<race time>.mp4` (a run) or `<map>_<date>.mp4` (the last seconds, run so far). A name taken gets a number. |
| Settings | `Documents\TrackMania\TMClips\clips.ini` (shared by both) |
| Personal bests for *Save on finish* | `Documents\TrackMania\TMClips\bests.ini` |
| Log, in TM Vibrant Shaders | `Documents\TrackMania\TMVS\tmvs.log`, lines starting with `clips:` |
| Log, TM Clips alone | `Documents\TrackMania\TMClips\tmclips.log` |

A clip being written is called `*.part.mp4` until it is done. If the game ends meanwhile, the unfinished one is deleted at the next start.

## Settings

All on the *Clips* page of the menu (panels *Saving* and *Recording*), or in the TM Clips window.

| Setting | Default | What it does |
|---|---|---|
| Record clips | on | Records into the buffer. Off: nothing is recorded and no key saves |
| Save on finish | Personal best | Off, Personal best or Every finish |
| Before start / After finish | 3 s / 3 s | How much a run's clip takes before the start line and after the finish (0 to 10 s) |
| Last seconds | 30 s | What the save key takes: 30, 60 or 90 s |
| Save last run / Save run so far / Save last seconds | `Ctrl+F3` / `Ctrl+Shift+F3` / `F3` | The keys |
| Adaptive buffer | on | Buffer from the map's author time |
| Buffer | 60 s | Fixed length without the adaptive buffer; the page shows the memory it takes |
| Size | 1080p | Game size, 720p, 1080p or 1440p (at most this many rows; a smaller game window is not scaled up) |
| Frame rate | 60 fps | 30 or 60 fps |
| Bitrate | 12 Mbit/s | 4 to 50 Mbit/s; a higher one shortens the longest possible buffer |
| Encoder | Auto | Auto: NVIDIA's NVENC, else the graphics card's H.264 encoder through Windows (Media Foundation: AMD, Intel). Or one of them only |
| Sound | Game | See [Sound](#sound) |
| Without HUD | off | TM Vibrant Shaders only: the picture before the game draws its HUD. The menu and the mod's windows are never in a clip, either way |
| Saved message | on | A short "Clip saved 0:42.37" in the game (never in the clip) |

In `clips.ini`, section `[Clips]`, the same values are keys such as `SaveOnFinish`, `BufferSeconds`, `Bitrate`, `SoundSource`, `SaveKey`. The file is read again while the game runs when it changes.

## Limits

- **Memory.** The game is a 32-bit program with 2 GB of address space, most of it the game's. Picture and sound of the buffer together may take at most 350 MB. So the longest buffer shrinks with the bitrate: at 12 Mbit/s with sound it is about 2.5 minutes, at 50 Mbit/s under a minute. The page says "At most N s at X Mbit/s" and shortens the buffer and the save key's seconds by itself.
- **A run longer than the buffer** is saved from the oldest second the buffer holds. The log says so.
- **Cost.** The picture is converted on the GPU and read back one frame late; the log prints every minute what a frame cost on the game thread and in the encoder (`clips: last 60 s: ...`). On a slow GPU use 30 fps or 720p.
- **Video export.** While the replay editor's video export runs, nothing is recorded.
- **Hardware.** An NVIDIA card (NVENC), or an AMD or Intel card whose driver offers a hardware H.264 encoder to Windows.
- **Games.** Nations and United Forever, TrackMania Nations ESWC (1.80 patch, TM Vibrant Shaders only). Not TrackMania 2.
- **Rules.** The mod only records; it adds no hook to the game's code for this and writes nothing into its memory.

## TrackMania Nations ESWC

Only the unofficial 1.80 patch (`TmNationsESWC.exe` 0.1.7.9), with TM Vibrant Shaders as `d3d9.dll` in the game folder ([install](install.md#trackmania-nations-eswc)). Everything above works the same way, with these differences:

- **The race** is read from the game's own objects (the current race, your player's info: its state, start time, final time; the map's name and author time), nothing is hooked or written for it, so it is fine online. Solo and network races count (the game's first local player).
- **Personal best** is compared with your best clip time on that map in `bests.ini`. ESWC maps have no UID, so the map is told by its name (`eswc-` and a hash of it): two maps of the same name share one best.
- **Lap races** take the lap's time, as the game's own HUD and the split times do.
- **No video export pause**: the game's own *Shoot video* isn't hooked, so the clips keep recording while it runs (its frames come slowly then, the buffer holds what was on screen).
- **Files**: the same folders as in Nations Forever (`Documents\TrackMania\Clips`, `Documents\TrackMania\TMClips\clips.ini`); the log is `tmvs-eswc.log` with its `clips:` lines. Its first `clips:` line says `race read from TrackMania Nations ESWC`.

## Troubleshooting


Look in the log first (`clips:` lines in `tmvs.log`, or `tmclips.log`). The first lines say what records and with what (`clips: recording 1920x1080 ... NVENC`), each finish, and each saved clip.

- **No clip after a finish.** *Save on finish* is *Personal best* by default: a slower run isn't saved. Set it to *Every finish*, or press `Ctrl+F3` afterwards. If the message says "Not recording", the status line at the top of the *Recording* panel says why.
- **"Can't record" in the log.** The encoder didn't start. Try *Encoder* set to the other one, a lower size or bitrate, and update the graphics driver.
- **Nothing happens with both mods installed.** One of them records, the other is idle on purpose. The idle one says "stays idle" in its log. The settings are in the one that records.
- **A key does nothing.** Another mod or program may use the same key (the log warns when it is also a menu key of this mod). Change it on the page. The game window has to be in front.
- **No game sound, or other sounds in the clip.** The game's own sound needs Windows 10 2004 or newer; older Windows records everything. Switch *Sound* to *All sound* to test whether the problem is the per-game capture. The log has `sound capture:` lines.
- **The game stutters.** Lower *Frame rate* to 30 fps or *Size* to 720p; check `clips: last 60 s` in the log for the game thread's ms per frame.
- **The buffer is shorter than I set.** The memory limit; lower the bitrate or switch the sound off.
- **Clips are missing the start of a long run.** The buffer is shorter than the run; use the adaptive buffer or a longer fixed one.
- **TM Clips doesn't appear in the ModLoader.** The folder `TM Clips` must be directly in `products` (not a folder inside a folder), and it has to contain `description.yaml` and a version folder with `TMClips.dll`.
