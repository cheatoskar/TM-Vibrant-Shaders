## TM Vibrant Shaders 1.4.2

A release candidate for **TrackMania Nations & United Forever**: a new video export, resource packs on every map, a Horror look with car headlights, clear water and splashes, a fix for flickering overlapped blocks, photo mode in the MediaTracker, and a Misc page with split times (and a minimap in the replay editor and exported videos). TrackMania 2 gets car spray from the real tyres and menu pages in their own windows.

**This is a pre-release.** It is what I test with right now; things may still change before 1.4.2. Tell me what breaks (with `Documents\TrackMania\TMVS\tmvs.log`).

---

### The video export

**The mod renders each frame itself.** With *Shoot video*'s *High quality* the game draws several samples and motion blur sub-frames per video frame. The mod now lights and sums all of them in float, then runs bloom, exposure and the colour grade once on the average. Bright lights streak like on a real camera instead of a grey smear, edges are clean, and the frame goes to the encoder in 10 bits or with a fine dither, so the sky has no bands.

**Better quality per megabyte.** One slider (*Video quality*, 80) that I calibrated with VMAF on a grainy 1440p60 replay: 80 is hard to tell from the source, 92 is for editing. On NVIDIA cards (driver R570 or newer) the mod drives NVENC directly instead of going through Windows. At the same 75 Mbit/s it scored 98.0 where x264 medium scored 97.1, Windows' NVENC path needed 82 Mbit/s for that, and it encodes faster than the export renders. AMD and Intel cards still use Windows' encoders, now at a higher bitrate (I could not measure them).

**HDR10.** With HEVC in 10 bits and the *HDR10* preset, lamps, the sun and glows stay brighter than white on an HDR screen (up to 1000 nits, white at 203). Up to white it is the same picture as the SDR video.

**The game's own sound.** The export's sound pass used to record Windows' default recording device: a microphone gave the room, no recording device gave silence. The mod now hands the game its own output (Windows 10 2004 or newer). Rain is in it also with the game in the background, thunder comes after the flashes you see in the video, and a clip that doesn't start at 0 gets the right sound. The sound pass still needs the sound button in the *Shoot video* dialog.

**A Video page in the `F8` menu** with presets for what the file is for (*YouTube*, *Best*, *Editing*, *Small*, *Discord*, *HDR10*) and its look (*Replay*, or *Cinema* with a film shutter, lens depth of field and 2.39:1). Also new: 10-bit HEVC and AV1, ffmpeg as encoder (x264, x265, SVT-AV1 through your own `ffmpeg.exe`, the better choice with AMD or Intel), film grain and a crop for the video.

**Faster.** The encoder runs on its own thread and the colour conversion on the graphics card, and with the mod's frames the game no longer reads each frame back to the CPU.

---

### Overlapping blocks no longer flicker

On TMUnlimiter maps two blocks placed into each other used to flicker in stripes. Now the block placed last is shown (*Overlapping blocks: last placed on top*, on). Action blocks win over plain ones in this order: engine off, boosters, start/finish/checkpoint, everything else, so a booster inside a start block stays visible. It also works with the effects off.

### Resource packs on every map

A new **Resource packs** page: your environment mods from `Skins\<Environment>\Mod` (DarkBlue, NEONglowSTICKS, ...) on every map you load, with a card and picture per pack. *Map's own*, *None* or a pack, optionally one per map mood (day, sunset, night). *Apply* switches the loaded map at once, without reloading. It uses the game's own mod mechanism, so the map file, replays and the server never get your pack.

### Horror look and car headlights

The **Horror** preset is a black night with every lamp off, thick fog, and your car's headlights as the only light: they light the road and walls ahead and show as beams in the fog. *Car headlights* works with any preset, with *Headlight range* (how far they reach) and *Headlight beam* (how strongly the cone shows in the air).

### Water

**Clear water** (*Water clarity*): the murky grey pools turn clear, sandy in the shallows and blue-green deep, with caustics and foam at the edges. **Water splash** (*Water splash*): the car throws up a fountain, rings and foam when it enters water, a bow wave while it drives through, and drips when it comes out. Also in exported videos.

### Photo mode in the replay editor

- Photo mode works in the MediaTracker: the focus click lands where you click in the small 3D view, and F12 captures only the view.
- **Hide editor**: the editor's panels disappear (the game's own `Tab` switch) and the view fills the screen.
- **Hide car**: the car and its shadow are left out of the picture.
- A compact photo panel (`Ctrl+F6`): exposure, aperture, zoom, vignette, grain, focus.

### Misc page: split times, session, minimap for replays

All off until you switch them on. The panels sit at the top right and scale with the game window.
- **Minimap** in the replay editor and in exported videos, never while you drive: the track from above with the car. *Real look* draws the blocks as the game shows them, in their own colours. *Auto zoom* follows the car and zooms with its speed.
- **Split times** at each checkpoint, time and speed, against the map's record from TMX (fetched by itself), the ghost you race against, your best run or a replay file.
- **Session**: attempts, best, median and last time, respawns, and your theoretical best from all your runs and replays of the map.
- **Finish effects**: confetti, fireworks for a new best run.
- No ideal line, no checkpoint highlights, no minimap in the race, nothing that changes the race.

### Menu

Every menu page can be popped out into a window of its own (move, resize, minimize, keep it on screen over the game). Shorter, plainer texts throughout. After the game starts a badge at the top shows the mod's version and which keys open the menu and switch the effects.

---

### Install (Nations & United Forever)

**With the ModLoader, updates itself (the beta address gets pre-releases like this one):** close the ModLoader, open `%LOCALAPPDATA%\TMLoader\config.yaml` with Notepad and add this line under `servers:`:

```yaml
  - 'https://cheatoskar.github.io/TM-Vibrant-Shaders/modloader/beta/'
```

Then tick *TM Vibrant Shaders* in your profile. New versions arrive on their own from then on.

**With the setup:** download `TM-Vibrant-Shaders.zip`, extract it and run `TM-Vibrant-Shaders-Setup.exe`.

In the game `F8` opens the menu and `F7` turns the effects on and off. Step by step: [installation guide](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/main/docs/install.md) ([Deutsch](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/main/docs/INSTALL_DE.md)). TrackMania 2 players: the same zip, see 1.4.1's notes.

All changes: [changelog](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/{{TAG}}/CHANGELOG.md).
