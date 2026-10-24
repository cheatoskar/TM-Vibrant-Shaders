#pragma once
#include <d3d9.h>
#include <string>

// Hooks into the TrackMania Forever "Vision" engine (CVisionViewportDx9).
//
// Addresses come from TmForever.map (build 2011-01-28, identical code in the
// TMLoader 2.12.0 executable). Every slot is verified against the expected
// function address before patching, so an unknown executable simply leaves
// the engine untouched and the plugin falls back to Present-time rendering.
namespace tmshaders {
namespace engine {

struct Matrix4 {
    float m[4][4];
};

// Camera data captured for the camera currently being rendered.
struct CameraInfo {
    void* camera = nullptr;
    Matrix4 view{};        // world -> view, row-vector convention (D3D)
    Matrix4 projection{};  // view -> clip, row-vector convention (D3D)
    float location[12]{};  // raw GmIso4 the engine passed (3x3 rotation + translation)
    bool hasView = false;
    bool hasProjection = false;
};

struct Callbacks {
    void (*frameBegin)() = nullptr;
    void (*cameraBegin)(void* camera) = nullptr;
    void (*cameraEnd)(void* camera, const CameraInfo& info) = nullptr;
    void (*overlayBegin)() = nullptr;
    void (*overlayEnd)() = nullptr;
    void (*frameEnd)() = nullptr;
};

bool install(const Callbacks& callbacks);
// Overrides the game's shadow quality (-1 = keep the game's setting).
void forceShadows(int mode);
bool active();

// Camera info of the camera currently between RenderCameraBegin/End (or the last one).
const CameraInfo& currentCamera();

// True while a MediaTracker clip plays (replays, intros, the replay editor) or the video
// export runs: the moments for motion blur and depth of field. False while driving.
bool cinematicActive();
// True while only the clip player runs (no clip viewer's cameras, no video export) and a race
// checks its player every frame: a clip of the map (its in-game triggers: a camera in a loop,
// texts at the start) plays while the player drives. cinematicActive() is true then too.
bool clipInRace();

// Sub-pixel offset (NDC) added to every perspective projection the engine computes from now
// on, for TAA: each frame samples the pixels at a slightly different spot. 0, 0 = off.
void setProjectionJitter(float x, float y);

// Hi-res photos: every perspective projection zooms in on one tile of the view
// (NDC x' = scaleX x + offsetX, y likewise). 1, 1, 0, 0 = off.
void setProjectionTile(float scaleX, float scaleY, float offsetX, float offsetY);

// The camera (CHmsCamera) the plugin shades: zoom and tiles apply to its projection only.
void setMainCamera(const void* camera);

// Accumulation depth of field: moves the main camera's eye by (x, y) metres in view space and shears
// the projection so the plane at focusDistance metres (view-space z, the depth the shaders use)
// stays put. Applied to every main camera projection until cleared (0, 0), after zoom, tiles and
// jitter. The engine computes a camera's projection BEFORE its RenderCameraBegin (both for the
// screen and for the export's samples rendered into textures), so a change made in the
// cameraBegin callback only reaches the next render: set it in frameBegin, or use the table below.
// cameraMatrices() and the SetTransform the plugin sees carry the offset matrix.
void setLensOffset(float x, float y, float focusDistance);
// The same with a table of offsets (count pairs x, y; at most 128): while the game supersamples
// the main camera (the video export's "high quality", sub-frames of its motion blur included)
// the render of its sample k (0 .. n n - 1 within one video frame, the game's own order) gets entry
// k % count; other main camera renders take the entries in turn from the call on. count 0 = off.
// One call before the export's first stepped frame is enough; calling it again restarts the turn.
void setLensSamples(const float* xy, int count, float focusDistance);
// The main camera's last projection (computed just before its RenderCameraBegin, so in the
// cameraBegin callback: the render that begins): the game's supersampling sample it was for
// (index 0 .. count - 1 within its video frame; false and -1 when the game didn't supersample it)
// and the lens table entry it got (-1 none).
bool mainCameraSample(int& index, int& count, int& lensEntry);

// The view and projection (D3D layout, as SetTransform gets them) the engine computed for the
// camera that began last, before its RenderCameraBegin. False when it computed none.
bool cameraMatrices(float view[16], float projection[16]);

// Photo mode zoom: every perspective projection magnified about the centre (1 = off; only
// in, never out, since the engine culls with its own field of view).
void setProjectionZoom(float zoom);

// Hides the race / replay interface (HUD) while true: the photo mode. Through the game's own
// switch (CGamePlaygroundInterface), so menus and the 3D view are drawn as always.
// Photo mode's Hide car: the game's draws of the cars (not their smoke or tyre marks) are skipped.
void hideCars(bool hide);
void hideOverlay(bool hide);

// Skips the game's own FX post-processing (bloom and colour filter, the launcher's "FX"
// option) while true: it worked on the same image as the mod (glow twice, smeared).
// The game's setting itself is not touched. The engine asks only when it starts its
// effects (map load), so a change shows from the next map on.
void skipGameFx(bool skip);

// No adaptive view distance (the launcher's "ZClip" cuts the far blocks when the frame rate
// drops) while true: the photo mode wants the whole map.
void fullViewDistance(bool full);

// The game's material (CPlugShader) of the draw call being made, null between draws and
// without the hooks. A first step towards per-material masks (lamps, glass, water).
const void* currentShader();
// The menu or photo mode used this Esc press: the game doesn't see it (it would pause) until
// the key is released.
void swallowEscape();
bool escapeSwallowed();

// A night sky replaces the sun: the game's halo around its sun (a lens flare drawn over the
// geometry in front of the sun) fades out.
void hideSunFlare(bool hide);
// A night sky replaces the game's: the environment's painted horizon (Island's sunset ring) isn't drawn.
void hideGameSky(bool hide);
// Real night under night skies: at each map load `decide` gets the map's time of day (0..1,
// night outside (0.25, 0.75]) and answers whether the game lights the map at midnight
// instead (its own moon, fog and sky; no baked sun). Visual only: the map stays as it is.
void setNightAtLoad(bool (*decide)(float dayTime));
// The game's moon on night maps under the black hole (EngineMoon). At each map load, before the
// scene is made, `decide` gets the map's time of day and whether it is lit at midnight, and
// answers what the game's moon does there: mode 1 shines from `toward` (world direction towards
// the light: the lightmap baked at load, the glints, the car), 2 off (ambient and lamps stay).
// Fixed for the map: its lightmap is baked once.
struct MoonLight {
    int mode = 0;
    float toward[3] = {0.0f, 1.0f, 0.0f};
};
void setMoonAtLoad(bool (*decide)(float dayTime, bool litAtNight, MoonLight& moon));
// The moon of this map is changed and the game's weather has used it.
bool moonReplaced();
// The loaded map's own time of day (-1 unknown) and whether it is lit at midnight instead.
float mapDayTime();
bool mapLitAtNight();
// Real night switched on or off while a map runs: the weather jumps to midnight or back to the
// map's time (only call it in gameplay: the weather of a map that was left may be gone).
bool relightNight(bool night);
// Real night kept: when the weather's clock left the forced midnight (an end-of-race replay may
// set it itself, the sun came back), it is logged and, with `fix`, jumped back. Once a frame in
// gameplay, before the scene is drawn. True when it jumped.
bool keepNight(bool fix);
// The game's lens flares since the last call (for the log): other lights, the sun, the sun's hidden.
void flareCounts(unsigned& other, unsigned& sun, unsigned& hidden);
// What the material being drawn is (from its shader file; Neon: from its textures, the neon
// strips of Stadium's road borders and curbs; Dirt: the terrain's Soil shaders), Other between
// draws. Seven at most: the class takes the stencil's low three bits.
enum class Material : unsigned char { Other = 0, Light = 1, Car = 2, Glass = 3, Grass = 4, Water = 5, Neon = 6, Dirt = 7 };
Material currentMaterial();
// The class and shader file of a material (currentShader; false: no name). For F12's
// material ids (capture_NNN.mat).
Material materialClass(const void* shader);
bool materialFile(const void* shader, wchar_t* out, size_t size);
// Its textures' files, "; " between them (false: none read).
bool materialTextures(const void* shader, wchar_t* out, size_t size);
// The draws' materials are known and their textures can be read: the neon strips are told by
// their material (Material::Neon), not by their colour. False on an unknown build, without the
// draw hooks, or while no texture had a file name yet.
bool neonMaterialsKnown();
// Development: log each material once with its file (tmvs.log "shader ...").
void shaderCensus(bool on);

// The free camera (cam 7 in replays, the editor's) is on. False without the hook.
bool freeCamActive();

// Photo mode's freeze: the game's own pause of a race (what its in-game menu does), TMNF / TMUF.
// freezeAllowed: why the race may be frozen now ("run over", "editor test"),
// null when it must not: online (any doubt counts as online), a live run, replays, no race.
// freezeRace pauses it (true when frozen, also when it was already); unfreezeRace resumes it.
// Nothing here writes a field of the game: only its own Pause / Resume / Start- and StopSimulation.
const char* freezeAllowed();
bool freezeRace();
void unfreezeRace();
bool raceFrozen();

// Counters of race (re)starts / map loads and of respawns (for the neon trail).
int raceResets();
int respawns();
// Counts the blocks the editor took out of the map (CasterShadows forgets the meshes it saw).
int blocksRemoved();

// The map is a Stadium map (always in Nations Forever; United Forever has six more
// environments).
bool stadium();

// The two water heights of this map: the water blocks' (always the same) and the sea level
// (from the map load or the plane the game renders the water reflection for; = blockY without
// a sea). False until a map load was seen (hooks unavailable): the shaders then guess by colour.
bool waterHeights(float& blockY, float& seaY);
// Counts map loads; mapComments() are the comments of the last loaded map (the map's author
// writes them in the editor), "" when it has none or its file can't be read (campaign maps
// sit in the game's packs).
int mapLoads();
std::string mapComments();
// Counts the times the game let go of its map (back to the menu, or before the next map), and
// whether a map is open now (race, replay or editor).
int mapCloses();
bool mapOpen();

// Where the player's car is drawn now (world space), while a race runs (also with the car off
// screen). False otherwise, or without the hooks (another game build). up (if given): the car's
// up axis in the world (unit length), its roof's way: on a banked curve, a wall or in a loop
// the road's up under the car.
bool carLocation(float position[3], float up[3] = nullptr);
// The car's whole pose, for the spray from its tyres: while a race runs the player's car (as
// carLocation), in the replay editor, its video export and replays the car the clip's cameras
// show. False otherwise.
struct CarPose {
    float position[3] = {};               // as carLocation's (about the height of the wheel centres)
    float forward[3] = {0.0f, 0.0f, 1.0f}; // the car's axes in the world, unit length
    float right[3] = {1.0f, 0.0f, 0.0f};  // (the side: its sign is the model's, the tyres are symmetric)
    float up[3] = {0.0f, 1.0f, 0.0f};
    // Where its tyres touch the road, in its axes from `position` (m): the front and the rear
    // axle along forward, half the track, the height along up (negative). Measured on the
    // environment's car standing at a start (wheelsMeasured), else the Stadium car's estimates.
    float frontAxle = 0.0f, rearAxle = 0.0f, halfTrack = 0.0f, contactHeight = 0.0f;
    bool wheelsMeasured = false;
    int source = 0; // 1 the race's player, 2 a clip's car
};
bool carPose(CarPose& pose);
// The comments of the map that is open now (race, replay or editor), read and written in the
// game's memory. Written comments are saved into the file when the author saves the map.
bool currentMapComments(std::string& comments);
bool setCurrentMapComments(const std::string& comments);

// Which of the cinematic hooks fired since the last call (bit 0 clip player, 1 clip viewer,
// 2 video export), for the log.
unsigned cinematicSources();

// The game's video export (replay editor, "Shoot video"), as its dialog set it up. The game
// renders each frame at the export's size into its own target, then copies it into the AVI
// at the end of the frame (CVisionViewportDx9::RenderFrameEnd): the mod's image is what it
// writes. Its clock runs at a fixed step instead of the real time.
struct VideoExport {
    int width = 0, height = 0; // the video's size
    float fps = 0.0f;
    bool hq = false;           // "high quality": supersampling, hqSamples x hqSamples samples
    int hqSamples = 1;         // per side (1 = none, the game takes at most 11)
    bool hqMotionBlur = false; // the dialog's motion blur
    bool subFrames = false;    // each rendered frame is one sample at its own moment, the game
                               // averages samplesPerVideoFrame of them (real motion blur)
    int samplesPerFrame = 1;   // main camera renders per rendered frame (sub-pixel samples, all
                               // into the same target; averaged by the game)
    int samplesPerVideoFrame = 1;
    bool writesVideo = true;   // false: a run that writes no video (DoShoot skipped the output)
    int stereo = 0;            // the dialog's 3D stereo mode (0 off)
    bool sound = false;        // the dialog's sound button (IsAudioStream, off by default): without it
                               // DoShoot skips its sound pass, the file gets no sound stream at all
    double step = 0.0;         // seconds the game's clock advances per rendered frame, 0 = real time
    double gameClock = 0.0;    // the game's clock (s), for the log
    double clipStart = 0.0, clipStop = 0.0; // the clip (s): the frames cover start .. stop, the sound
                               // pass 0 .. stop (it sets the clip's time from 0)
};
// The video export's file (the game's AVI writer, CVisionVideoAvi): the mod can write the
// frames and the sound into a file of its own. `file` identifies one export.
struct VideoFileHooks {
    void (*opened)(void* file, const wchar_t* path, bool ok) = nullptr; // the game made its .avi
    // The video starts (size already rounded up to 8, bottom-up BGR frames of bytesPerPixel):
    // true = the mod takes the frames; the game then opens no video stream (no codec dialog).
    bool (*videoOpen)(void* file, int width, int height, float fps, int bytesPerPixel) = nullptr;
    bool (*videoFrame)(void* file, const void* pixels) = nullptr; // true = taken
    void (*audioOpen)(void* file, const void* waveFormat) = nullptr; // WAVEFORMATEX without cbSize (16 bytes)
    void (*audioData)(void* file, const void* data, unsigned long blocks) = nullptr;
    void (*closed)(void* file) = nullptr; // the game is done with it (its .avi closed)
    // Asked at the end of a frame the game is about to write, after the frameEnd callback: true =
    // videoFrame will take the mod's own frame, so the game skips reading its image back
    // (GetRenderTargetData and the conversion); videoFrame then gets the last image it did read.
    bool (*ownFrameReady)(void* file) = nullptr;
};
void setVideoFileHooks(const VideoFileHooks& hooks);
bool videoFileHooked(); // false: another build, the game's AVI as always

// True while the export runs (its fiber ran this frame or the one before), with its settings.
// False without the hook or when the game's memory doesn't look as expected.
bool videoExport(VideoExport& out);

// Environment mods ("resource packs", Skins\<Environment>\Mod\*.zip): which one a map loads
// with. The game asks CGameCtnChallenge::GetModPackDesc at every map load (the load screen,
// then the decoration and its textures); the mod answers only there, never writing the map
// (its saved mod stays: the map's Chunk writes challenge +0x13c, which is never touched) nor the
// game's forced mods (CGameCtnApp +0x310, which a hosting game sends to its clients).
struct ModPackAsk {
    const char* environment = ""; // "Stadium", "Speed", ...
    const char* decoration = "";  // the map's decoration ("Day", "Sunset", "Night", ...; "" unknown)
    const wchar_t* mapMod = nullptr; // the map's own mod (the game's name for it), null: none
    bool loadScreen = false;      // the load screen's question (the decoration's comes right after)
};
struct ModPackAnswer {
    enum Kind { MapOwn, None, Pack } kind = MapOwn;
    std::wstring file;            // Pack: its file name in Skins\<environment>\Mod
    std::string note;             // for the log line ("" none)
};
void setModPackChooser(ModPackAnswer (*choose)(const ModPackAsk& ask));
bool modPacksHooked(); // false: another build, or the game's code not where expected
// What the last map loaded with, as its log line said ("" before the first map).
std::string modPackStatus();
// Whether the game has Skins\<environment>\Mod\<file> among its packs (it finds them at start:
// one added while it runs can't be used before the next start).
bool modPackKnown(const char* environment, const std::wstring& file);
// The loaded map's pack, switched without loading the map again: every texture of the
// environment gets the chosen pack's file (or the game's own) through the engine's path for a
// mod file that arrives while a map runs (CPlugBitmap::OnPackAvailable).
struct ModPackLive {
    bool possible = false;   // a map is loaded (not loading) and this build can
    bool differs = false;    // the choice now is not what its textures show
    std::string environment; // "Stadium"
    std::string shown;       // what its textures show ("DarkBlue.zip", "none")
    std::string wanted;      // what the choice gives for this map
};
ModPackLive modPackLive();
// Switches now (the game's thread, between frames). False with the reason in `status`; true with
// what was swapped ("Stadium: DarkBlue.zip -> NEONglowSTICKS.zip, 37 textures").
bool applyModPackNow(std::string& status);

} // namespace engine
} // namespace tmshaders
