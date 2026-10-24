# Misc page (src/misc)

TrackMania Nations/United Forever only (the TrackMania 2 build has none of it). Three small tools on
the menu's *Misc* page, each off until the player switches it on:

| Tool | What it shows | Where its data comes from |
|---|---|---|
| Minimap (`mapview.cpp`, `topview.cpp`) | The track's shape seen from above and the car the camera follows; with *Real look* the blocks' meshes from above with their textures; *Auto zoom* by the car's speed | The map's block list in the game's memory (read only), `engine::carPose`, `engine::carSpeed`; for *Real look* the game's own draws of the map's blocks (`blockorder::currentDrawItem` tells them from the rest, `casters::drawWorld` gives their place), drawn again into a texture at Present |
| Split times (`splits.cpp`) | At each checkpoint, in the moment the game registers it: the time and the car's speed against a reference (the map's record, the ghost you race against, your best run, a replay file); a sector clock; a fixed height with five scrolling checkpoint rows; after the finish each sector against your best and the reference; the Session panel (attempts, best / median / last, respawns, spread, your theoretical best from all your runs and replays of the map, the best combined with the reference, the last run's sectors) | The race's own checkpoint times (`engine::raceTimes`), its opponent ghosts' checkpoint times (`engine::raceGhostTimes`), the car's speed (`engine::carSpeed`), all read only; your best run and best sectors in `Documents\TrackMania\TMVS\splits`; your own replays of the map (driver names from `Tracks\Replays\Autosaves`); a replay's checkpoint times and speed at them (`replay.cpp`) from a local file or TMX (`tmx.cpp`) |
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
   marker for any of them (`kindOf`). *Real look* (`topview.cpp`) takes every draw of a block of the
   map alike, with the same shader for all: it never asks which block a draw is, only that it is one
   (`blockorder::currentDrawItem`), so a checkpoint gate is its plain geometry, as in the editor.
   *Real look* off (`MinimapTextured=0`) leaves the squares. Nothing is drawn in the 3D world at all (no
   beams, rays or markers): every tool is a 2D ImGui window over the finished image.
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
reads it. The engine's readers (`engine::raceTimes`, `currentMapUid`, `idText`, `currentMap`),
`blockorder::currentDrawItem` and `casters::drawWorld` stay, unused.

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

## The minimap's picture from above (Real look)

`topview.cpp` draws the map's blocks again from straight above, from the game's own draws: no mesh is
read from the files. In `plugin.cpp`'s `onDrawIndexed` (the game's state bound, before its draw) every
indexed draw of the main camera goes to `misc::sceneDraw`. A draw counts when `blockorder` maps it to a
block of the open map (corpus -> item -> block; my log: about 60 % of the sorted passes' draws are
blocks') or when it is the grass material's (the ground), and when it is opaque (depth written, no
blending, no alpha test). Its place is the game's mesh -> clip constant (GbxVisualPrCamera) with the
camera undone (`casters::drawWorld`, the long shadows' recovery). It is queued with references to the
game's vertex and index buffers, its declaration's POSITION and TEXCOORD0 and the texture on stage 0,
once per block item and index range, and drawn at Present, before the overlay: at most 400 a frame,
inside a saved and restored device state (a D3D9 state block plus the render and depth targets), into
an A8R8G8B8 target of 1 pixel per metre with its own depth buffer (higher covers lower) and mipmaps.
A block whose draws brought nothing new for 30 frames is skipped with one lookup from then on.

What `tools/topviewtest.cpp` proves on a real D3D9 device: a draw made like the game's lands at its
place (row 0 at the area's z0, the edges on the right pixels), with its texture, the higher over the
lower, each draw once, nothing elsewhere, and the device state comes back. What only the game can
prove: that the block draws are found (the log line's mesh count), that stage 0 holds a block's colour
texture (my material captures list the colour texture first for roads, platforms and grass, in
the engine's order; that the card's stage 0 follows it is a guess) and that
TEXCOORD0 is its coordinate (a wrong one still gives the texture's average colour at this size), and
the cost (the same log line). The frame-capture previewer can't emulate it: a capture holds the
finished image and depth, not the draw calls.
