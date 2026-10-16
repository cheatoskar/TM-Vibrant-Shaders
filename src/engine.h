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

// Sub-pixel offset (NDC) added to every perspective projection the engine computes from now
// on, for TAA: each frame samples the pixels at a slightly different spot. 0, 0 = off.
void setProjectionJitter(float x, float y);

// Hi-res photos: every perspective projection zooms in on one tile of the view
// (NDC x' = scaleX x + offsetX, y likewise). 1, 1, 0, 0 = off.
void setProjectionTile(float scaleX, float scaleY, float offsetX, float offsetY);

// The camera (CHmsCamera) the plugin shades: zoom and tiles apply to its projection only.
void setMainCamera(const void* camera);

// Photo mode zoom: every perspective projection magnified about the centre (1 = off; only
// in, never out, since the engine culls with its own field of view).
void setProjectionZoom(float zoom);

// Hides the race / replay interface (HUD) while true: the photo mode. Through the game's own
// switch (CGamePlaygroundInterface), so menus and the 3D view are drawn as always.
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
// What the material being drawn is (from its shader file), Other between draws.
enum class Material : unsigned char { Other = 0, Light = 1, Car = 2, Glass = 3, Grass = 4, Water = 5 };
Material currentMaterial();
// The class and shader file of a material (currentShader; false: no name). For F12's
// material ids (capture_NNN.mat).
Material materialClass(const void* shader);
bool materialFile(const void* shader, wchar_t* out, size_t size);
// Development: log each material once with its file (tmvs.log "shader ...").
void shaderCensus(bool on);

// The free camera (cam 7 in replays, the editor's) is on. False without the hook.
bool freeCamActive();

// Counters of race (re)starts / map loads and of respawns (for the neon trail).
int raceResets();
int respawns();

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
// screen). False otherwise, or without the hooks (another game build).
bool carLocation(float position[3]);
// The comments of the map that is open now (race, replay or editor), read and written in the
// game's memory. Written comments are saved into the file when the author saves the map.
bool currentMapComments(std::string& comments);
bool setCurrentMapComments(const std::string& comments);

// Which of the cinematic hooks fired since the last call (bit 0 clip player, 1 clip viewer,
// 2 video export), for the log.
unsigned cinematicSources();

} // namespace engine
} // namespace tmshaders
