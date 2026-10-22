## TM Vibrant Shaders 1.4.2

A new video export for **TrackMania Nations & United Forever**: *Shoot video* in the replay editor now renders its frames through the mod, encodes them on the graphics card's own encoder at a quality I measured, and records the game's real sound. TrackMania 2 is unchanged.

---

### The video export

**The mod renders each frame itself.** With *Shoot video*'s *High quality* the game draws several samples and motion blur sub-frames per video frame. The mod now lights and sums all of them in float, then runs bloom, exposure and the colour grade once on the average. Bright lights streak like on a real camera instead of a grey smear, edges are clean, and the frame goes to the encoder in 10 bits or with a fine dither, so the sky has no bands.

**Better quality per megabyte.** One slider (*Video quality*, 80) that I calibrated with VMAF on a grainy 1440p60 replay: 80 is hard to tell from the source, 92 is for editing. On NVIDIA cards (driver R570 or newer) the mod drives NVENC directly instead of going through Windows. At the same 75 Mbit/s it scored 98.0 where x264 medium scored 97.1, Windows' NVENC path needed 82 Mbit/s for that, and it encodes faster than the export renders. AMD and Intel cards still use Windows' encoders, now at a higher bitrate (I could not measure them).

**HDR10.** With HEVC in 10 bits and the *HDR10* preset, lamps, the sun and glows stay brighter than white on an HDR screen (up to 1000 nits, white at 203). Up to white it is the same picture as the SDR video.

**The game's own sound.** The export's sound pass used to record Windows' default recording device: a microphone gave the room, no recording device gave silence. The mod now hands the game its own output (Windows 10 2004 or newer). Rain is in it also with the game in the background, thunder comes after the flashes you see in the video, and a clip that doesn't start at 0 gets the right sound. The sound pass still needs the sound button in the *Shoot video* dialog.

**A Video page in the `F8` menu** with presets for what the file is for (*YouTube*, *Best*, *Editing*, *Small*, *Discord*, *HDR10*) and its look (*Replay*, or *Cinema* with a film shutter, lens depth of field and 2.39:1). Also new: 10-bit HEVC and AV1, ffmpeg as encoder (x264, x265, SVT-AV1 through your own `ffmpeg.exe`, the better choice with AMD or Intel), film grain and a crop for the video.

**Faster.** The encoder runs on its own thread and the colour conversion on the graphics card, and with the mod's frames the game no longer reads each frame back to the CPU.

---

### Install (Nations & United Forever)

**With the ModLoader, updates itself:** close the ModLoader, open `%LOCALAPPDATA%\TMLoader\config.yaml` with Notepad and add this line under `servers:`:

```yaml
  - 'https://cheatoskar.github.io/TM-Vibrant-Shaders/modloader/'
```

Then tick *TM Vibrant Shaders* in your profile. New versions arrive on their own from then on.

**With the setup:** download `TM-Vibrant-Shaders.zip`, extract it and run `TM-Vibrant-Shaders-Setup.exe`.

In the game `F8` opens the menu and `F7` turns the effects on and off. Step by step: [installation guide](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/main/docs/install.md) ([Deutsch](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/main/docs/INSTALL_DE.md)). TrackMania 2 players: the same zip, see 1.4.1's notes.

All changes: [changelog](https://github.com/cheatoskar/TM-Vibrant-Shaders/blob/main/CHANGELOG.md).
