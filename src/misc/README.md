# Misc page (src/misc)

TrackMania Nations/United Forever only (the TrackMania 2 build has none of it). Three small tools on
the menu's *Misc* page, each off until the player switches it on:

| Tool | What it shows | Where its data comes from |
|---|---|---|
| Minimap (`mapview.cpp`) | The track's shape seen from above and the car the camera follows | The map's block list in the game's memory (read only), `engine::carPose` |
| Split times (`splits.cpp`) | At each checkpoint, in the moment the game registers it: the time and the car's speed against a reference (the map's record, the ghost you race against, your best run, a replay file); a sector clock; after the finish each sector; the Session panel (attempts, best / median / last, theoretical best, spread, respawns, sector speeds) | The race's own checkpoint times (`engine::raceTimes`), its opponent ghosts' checkpoint times (`engine::raceGhostTimes`), the car's speed (`engine::carSpeed`), all read only; your best run in `Documents\TrackMania\TMVS\splits`; a replay's checkpoint times and speed at them (`replay.cpp`) from a local file or TMX (`tmx.cpp`) |
| Finish effects (`confetti.cpp`) | Confetti at the finish, fireworks for a new best run | The finish the split times see |

`misc.cpp` holds the settings (`[Misc]` in `settings.ini`), the page and the calls from the rest of the
mod; `misc.h` is the only header the rest includes.

## What it guarantees

Checked against the code in this folder and `engine.cpp`'s readers; the TMX moderators asked for each
point.

1. **Nothing shows where a checkpoint or trigger is.** The minimap draws every block by its name's
   kind only (road, circuit, platform and the like as track; grass, dirt, water as ground; the rest
   dimmed). A checkpoint, start, finish, booster or engine-off block is drawn exactly like any road or
   platform: `mapview.cpp` never reads a block info's waypoint field (+0x11c) and has no colour or
   marker for any of them (`kindOf`). Nothing is drawn in the 3D world at all (no beams, rays or
   markers): every tool is a 2D ImGui window over the finished image.
2. **Split times appear only when the game registers a checkpoint.** `splits::update` reacts to the
   race's count of checkpoints passed (`CTrackManiaPlayerInfo` +0x334) going up, and shows the time
   the game stored for it (the game shows that time itself). The sector clock between checkpoints is
   a time against a time (seconds since the last checkpoint against the reference's sector time), with
   no position in it. Nothing says where the next checkpoint is or how far it is.
3. **Nothing draws a ghost's path.** The replay reader (`replay.cpp`) reads a replay's map UID, race
   time and checkpoint times, and the ghost's speed: its samples' positions are decoded only to get the
   speed between neighbouring samples and dropped right away (`readSpeeds`); no position is kept or
   shown. The speed is shown only at a checkpoint the game registered, next to yours. The ghost you
   race against is read from the race's own ghosts for its checkpoint times only.
4. **Nothing disables the game's cameras or MediaTracker clips.** No code here touches them. The mod's
   hooks on the in-game trigger checks (`CGameRace::MediaClipCheckInGameTriggers`,
   `CGameCtnMediaClipViewer::ClipGroupCheckTriggers` in `engine.cpp`) only note which race and player
   ran and then call the game's own function unchanged.
5. **Nothing is written into the game's memory** by these tools: the block list, the race times and the
   car are read inside SEH guards; a value that doesn't look like the game's turns the tool off.
6. **Online play:** these tools are allowed online (minimap without highlights, split times, finish
   effects). There is no offline lock and none is needed.
7. **TMX:** with *Compare with: Record (TMX, auto)* the record is fetched by itself when a map is
   loaded, otherwise never. One request at a time, at least 20 s apart, once per map and session, with a
   User-Agent naming the mod; the replay is kept on disk (`tmx::cached`) and a map found not to be on TMX
   is noted (`<UID>.notontmx`, `tmx::knownMissing`): neither is ever asked for again. `tmvs.log` says
   each lookup.
8. **Never in photos, the video export, F12 captures or screenshots**: the windows are drawn at Present
   (photos and the export take the image before it), and `overlay::Status::hideTools` keeps them away
   while the export runs, in photo mode and for a capture or screenshot.

## How to remove it

One line, one release: configure with `-DTMVS_MISC_TOOLS=OFF` (or set the option's default to `OFF` in
`CMakeLists.txt`). Then nothing of this folder is compiled in: `misc.h` turns every call into an empty
inline function, the menu gets no *Misc* page (`menu::Host::miscPage` stays empty), no window is drawn,
no replay is read, and no request goes to TMX. `settings.ini` may keep its `[Misc]` section; nothing
reads it. The engine's readers (`engine::raceTimes`, `currentMapUid`, `idText`, `currentMap`) stay, unused.

## Engine data (Ghidra, TMNF names; TMUF has the same offsets)

- Blocks: `CGameCtnChallenge` +0x54 `CFastBuffer<CGameCtnBlock*>`; block +0x24 info (name: the CMwId at
  info +0x24), +0x48 coordinate, +0x28 units (each unit's offset at +0x28). Stadium cells 32 x 8 x 32 m
  (`tmvs.log` checks the car's cell at the start once per map).
- Map UID: the CMwId at challenge +0xdc (`CGameCtnChallenge::GenerateUid`).
- The race's opponent ghosts: `CTrackManiaRace` +0x520 `CFastBuffer<CGameCtnGhost*>`
  (`Ghosts_GetBestCurrentOpponent`, `Ghosts_Restart`); a ghost's race time +0xe8 (`SetRaceTime`, -1 none),
  its checkpoint times +0x94 {time, score} (`SetCheckpoint`). The car's speed: `CSceneVehicleCar` +0x4b4
  (the async world speed, checked by its vtable slot as `engine.cpp`'s wheel reading does). Ported to
  TMUF over 256 bytes, the same offsets.
- Race times: the race (`this` of `CGameRace::MediaClipCheckInGameTriggers`, a `CTrackManiaRace`):
  clock +0x274, lap mode +0xf4. Its player +0x1c: `CTrackManiaPlayerInfo` (back pointer +0x238), run
  start +0x2ac / lap start +0x2b8, finished +0x33c, final time +0x2b0 / +0x2bc, checkpoints passed
  +0x334, checkpoint times `CFastArray` +0x2f8 of {time, score}. From `GetCurrentStandardTime`,
  `OnCheckpoint`, `InternalOnCheckpoint` and the `CTrackManiaPlayerInfo` accessors; each ported to TMUF
  over 256 bytes with the field offsets unchanged.
- Replays: `tools/replaytest.cpp` reads every TrackMania Forever replay it finds (300 of mine, and a
  TMX download) and checks the header's UID, the times never falling, the last checkpoint being the
  race time, and the ghost's speeds (under 1000 km/h, covering the race; a respawn's jump is left out).

## The minimap as a picture from above (not built)

The idea: draw the map's meshes once from straight above into a texture, with the game's textures
or shaded by height, instead of the cells. `casters.cpp` keeps the meshes for the long shadows, but
only those the main camera has drawn and that stood still for half a second: at a map's start the
list is nearly empty and fills in as you drive, and it isn't recorded at all while CasterShadows is
off or no pass uses it. Its meshes are drawn with a depth-only shader; the game's materials (textures,
constants) are kept only for alpha-tested ones, so a textured picture would need the game's material
shaders with our own camera constants for every mesh. A height-shaded picture would be possible from
the far shadow map's way (light straight down: a height map of what was seen), at 16 MB per 2048²
target in a 32-bit process that already holds three 64 MB shadow maps. Not worth the risk now: the
picture would be incomplete until the whole track was seen, and would depend on another feature's
state. The cell view stays.
