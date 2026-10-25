# Misc page (src/misc)

TrackMania Nations/United Forever only (the TrackMania 2 build has none of it). Three small tools on
the menu's *Misc* page, each off until the player switches it on:

| Tool | What it shows | Where its data comes from |
|---|---|---|
| Minimap, only in the replay editor and exported videos (`mapview.cpp`, `topview.cpp`) | The track's shape seen from above and the car the camera follows; with *Real look* the blocks' meshes from above with their textures; *Auto zoom* by the car's speed (from its motion between the video's frames) | The map's block list in the game's memory (read only), `engine::carPose`; for *Real look* every block's meshes read from the game's memory as soon as the map is open (`blockmesh.cpp`: the game's own buffers, material and place), and the game's own solid draws inside the map's grid while a replay plays or the export runs (`casters::drawWorld` gives their place), drawn again into a texture; the minimap drawn into the export's own frames (`misc::drawIntoVideo`, an ImGui context of its own at the video's size) |
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
   marker for any of them (`kindOf`). *Real look* (`topview.cpp`) takes every solid draw inside the
   map's grid alike, with the same shader for all: it never asks which block a draw belongs to, so a
   checkpoint gate is its plain geometry, as in the editor. Its block reading (`blockmesh.cpp`) walks
   every block the same way (its mobils, their mesh trees, each mesh with its own material's colour
   map); what it draws never depends on the block info, its name or its waypoint field (the first
   block's name is read once per map for a log line only).
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
6. **Online play:** split times and finish effects are allowed online. The minimap is not (TMX: no
   minimap in the race), so it shows only in the replay editor (`engine::replayEditorOpen`) and in the video export's frames
   (`misc::drawIntoVideo`, from `finishOwnFrame` in `plugin.cpp`), never while you drive. There is no offline lock and none is needed.
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

`topview.cpp` draws the map's blocks again from straight above, from the game's own meshes: no mesh is
read from the files. Two ways in, with the same placement key, so a mesh comes once whichever finds it
first: the blocks read at once (`blockmesh.cpp`, below) and the camera's draws. In `plugin.cpp`'s `onDrawIndexed` (the game's state bound, before its draw) every
indexed draw of the main camera goes to `misc::sceneDraw` (the mod's own draws never get there:
`hook::gameCall`). A draw counts when it comes from inside the game's shader call, so its material is
known (`engine::currentShader`: CDx9ShaderKeeper::UpdateAndRender or ShadeVisualForCurrentPass, the
hooks the hide-car path uses), is opaque (depth and colour written, no blending, no alpha test), has
at least one texture bound (a depth or shadow pass has none), is from the managed pool (the game's
static meshes), not a car's or glass material, and has a world matrix in the game's constants: its place is the mesh -> clip
constant (GbxVisualPrCamera) with the camera undone (`casters::drawWorld`, the long shadows' recovery).
Its vertices' box, turned into the world, must lie inside the map's grid (16 m of margin) and below the
highest track cell plus 40 m: the sky, the clouds and the stadium around the grid stay out. Blocks are
not asked for by identity: on A01 only about 2 % of the main path's draws and 12 % of the sorted
passes' mapped to a block through `blockorder`'s corpus -> item chain (my first test), so that path was
dropped. Each placement is taken once per map, keyed by its buffers, index range, turn (1/20) and
place in 4 m cells; a place near a cell's edge also finds the neighbouring cell's key on that side.
The same placement drawn in several passes keeps the best colour it got: the material's colour map
over another colour texture over a grey one over the material's colour; a later pass with something
better draws it again over the first. The textures bound for each look are kept (two per placement),
so a pass seen before isn't looked at again. (My retest: keyed by the place to 0.25 m, the key changed every frame: the place the constants give
moves by up to about a metre, the camera's jitter in the projection, and 2500 meshes a second came as
new forever.) Instances of a block share their buffers and differ by place. It is queued with
references to the game's vertex and index buffers, its declaration's POSITION and TEXCOORD0 and the
colour texture, and drawn at Present, before the overlay: at most 400 a frame, inside a saved and
restored device state (a D3D9 state block plus the render and depth targets), into an A8R8G8B8 target
of 1 pixel per metre with its own depth buffer (higher covers lower) and mipmaps. New meshes are
measured (their vertices read for the box) at most 48 a frame, about 1 ms: the rest come again in the
next frames. The build-up ends when nothing new came for 5 s, or after 20 s; then only one draw in 16 is
looked at per frame and at most 5 new meshes a second are taken (something that moves would otherwise
come as new forever; the log counts what the cap refused).

The blocks read at once (`blockmesh.cpp`): when the map's block list is read (and again when the editor
changes it, after a device reset or with Real look switched on), each block's meshes are found in the
game's memory the way its renderer draws them, read only inside SEH guards, and queued with
`topview::addBlockMesh`: up to 300 a frame and 1.5 ms of reading, the queue's 4096 at most (a full
queue stops the block, it comes again next frame). From the block: its CSceneMobil (+0x30), a clip's
mobils (+0x38) and their child mobils (the links at mobil +0x38, each object at +0x18: a gate's
checkpoint part); each mobil's CHmsItem (+0x28, pointing back at +0x40), its CPlugSolid (+0x14) and
tree (solid +0x64), placed by each of the item's corpora (+0x34, the place a GmIso4 at corpus +0x18).
The tree: children at +0x28 (CPlugTreeVisualMip: then its levels at +0xac, level 0 taken), shown with
bit 8 of +0x9c, its own GmIso4 at +0x5c with bit 4, the visual +0x90 and material +0x94. The visual's
CDx9VisualKeeper (+0x14) holds what the game made for it: the vertex buffer (+0x30 + 12 p: the
CDx9VertexBuffer, its first vertex and count; stride at +0x0, the managed IDirect3DVertexBuffer9 at
+0x14), the static index buffer (+0x78: count and buffer at +0x8 + 12 p and +0x10 + 12 p), the split
table (+0x88) for a tree's share, the vertex format (+0x14, its declaration from
CDx9VertexDeclaration::s_Declarations) and the primitive kind (bits 16..21 of +0xc, as
RenderVisualGeometry switches on it). Only the static geometry loaders are taken (Static,
StaticVisualSplits, StaticSubVisuals, told by the keeper's loader at +0x2c) with static indices;
lines, sprites, dynamic and skinned meshes, quads and visuals the game hasn't made buffers for are
skipped and counted. The draw is the one the game makes (base vertex, first vertex, vertex count,
first index, primitive count), its place the corpus' times the tree's places, so the key is the
camera's (`misc: minimap: the camera's draws found N of the M placements read from the blocks`, 30 s
after the reading, says whether they meet). The colour: the material's own colour map
(`engine::materialDiffuse`) when its texture is a plain texture of the device's (the class of a
managed texture of ours), else the material's colour; a camera draw with something better draws it
again as before. Each mesh's bounds are measured once per buffer and range (instances share them).
The addresses compared (tree vtables' child functions, the three loaders, s_Declarations) are per
build in `engine::meshReadAddresses` (TMNF from its symbols; TMUF ported with
`tools/port_address.py`, each function confirmed over 256 bytes, so the offsets are the same).
`tmvs.log`: `misc: minimap: first block read: ...` (a block's grid cell against its first mesh's
place) and `misc: minimap: blocks read: N blocks, M meshes queued (K unreadable) ...` with what was
skipped.

The colour comes from the material itself (`engine::materialDiffuse`). Each block material is a
CPlugShaderApply with its own bitmaps; the colour map is the one whose file ends in D
(StadiumRoadBorderD; N is the normal map, S the specular map, L the lightmap, in all my material
captures), for grass StadiumGrass1 or 2. Its Direct3D texture is what the game hands to SetTexture:
CPlugBitmap +0x14 its CDx9TextureKeeper, that +0xc the IDirect3DBaseTexture9, read in
CDx9StateBlock::Apply (TMNF 0x00980740, TMUF 0x00980440, the same offsets in both). Only when that
pointer is bound on a stage of the draw is it used (no call on a pointer the device doesn't hold).
The textures are bound before the draw: UpdateAndRender (TMNF 0x00971b10, TMUF 0x009719e0) applies a
pass's state block, then CDx9VisualKeeper::RenderVisualGeometry (TMNF 0x0097c660, TMUF 0x0097c350)
calls DrawIndexedPrimitive; the device hook runs between the two. Without the colour map bound: the
sampler the pixel shader's constant table names for the colour, if it has names (the first game test
showed none: the shaders seen there had no table, or were the mod's patched ones); else the largest
colour texture, DXT of 256 px or more first. A luminance texture (L8, A8L8) tints the material's colour;
none usable: the material's colour (grass green, dirt brown, water blue, the rest grey, from the
engine's material class). The log names, for the first 16 materials of a map, the material, its
colour map, the textures on stages 0 to 3 (format and size) and the choice. (My first colour test
picked the stage from the constant table alone: no names, so no texture was even looked at, and every
material logged was unnamed: the draws taken then were outside the game's shader calls.)

The cost goes into `tmvs.log`: CPU per frame of drawing the minimap (`mapview::draw`, the redraws
included) and of the checks in `onDrawIndexed`, GPU per frame of the redraws (D3D9 timestamp queries
around them; the minimap's own two textured quads in ImGui's drawing are not timed), the meshes and the
video memory; once per map after the build-up and every 60 s. `tmvs_topviewtest --bench` measures the
same on 3000 meshes of 512 triangles at 1024 and 4096 pixels.

What `tools/topviewtest.cpp` proves on a real D3D9 device: a draw made like the game's lands at its
place (row 0 at the area's z0, the edges on the right pixels), with its texture, the higher over the
lower, each draw once, a draw without textures left out, a placement first drawn with the lightmap
drawn again with its colour map, nothing elsewhere, and the device state comes back; a mesh handed in
as read from the blocks lands at its place with its colour map, is known when read again, and the
camera's draw of the same placement (moved by the jitter) isn't taken again. What only the game can
prove: that the block draws are found (the log line's mesh count), that the colour map is bound in
the draws taken (the log line's count "the material's own colour map" and the material lines) and that
TEXCOORD0 is its coordinate (a wrong one still gives the texture's average colour at this size), and
the cost (the same log line). The frame-capture previewer can't emulate it: a capture holds the
finished image and depth, not the draw calls.
