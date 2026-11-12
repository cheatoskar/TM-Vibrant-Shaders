#pragma once
#include <cstdint>

// TM Vibrant Shaders 2.0: one code base, two products (docs/horizon-modules.md).
//
//  - Standalone: TMVibrantShaders.dll (ModLoader) or d3d9.dll in the game folder, with everything in it
//    and its own F8 menu, exactly as 1.5. Nothing in this file changes anything there: hosted() is
//    false and every module counts as on.
//  - Hosted: Horizon Host loads four small plugins (src/modules/module.cpp: "tmvs", "tmvs-capture",
//    "tmvs-upscaling", "tmvs-packs"). Each loads the one shared core, tmvs-core.dll (the same code as the
//    standalone DLL, src/modules/hostbridge.cpp is its side of Horizon), and attaches to it. The host
//    owns Direct3DCreate9, CreateDevice, Present and Reset and hands them to the core; the core hooks
//    only what the host does not (depth, render targets, transforms, draws), as the standalone does.
//    Pages are Horizon panels, hotkeys are in Horizon's table, the F8 menu is never drawn.
//
// What each module switches (hosted only):
//  - Shaders: the look (every effect pass, presets, LUTs, the photo look). Off: the effects are off as
//    with F7, without writing that into settings.ini.
//  - Capture: clips (and checkpoint clips), the F12 frame capture and photos, the video export's own
//    encoder and frames. Off: none of them run; the game's own video export stays.
//  - Upscaling: RenderScale with FSR 1. Off: the full resolution. On without Shaders: the game's own
//    image, drawn smaller and brought up with FSR (Pipeline's upscale-only path).
//  - Packs: resource packs chosen per environment at map load. Off: the maps keep their own mods.
//
// The game's thread only, except the plain reads (on(), hosted()), which are single bytes.
namespace tmshaders {
namespace modules {

enum Id : int { kShaders = 0, kCapture = 1, kUpscaling = 2, kPacks = 3, kCount = 4 };

// The Horizon plugin id and the name shown for each module.
const char* pluginId(Id id);
const char* displayName(Id id);

// Set once, before boot, by tmvs-core.dll's first attach. Never true in the standalone DLL.
void setHosted();
bool hosted();

// Hosted: the module's plugin is loaded (Init .. Shutdown) and switched on in Horizon's menu.
void setAttached(Id id, bool attached);
void setEnabled(Id id, bool enabled);
bool attached(Id id);
bool enabled(Id id);

// What the core's features ask. Standalone: always true. Hosted: attached and enabled.
bool on(Id id);

// Hosted only: Upscaling runs while the shaders don't (the Shaders module off, or its effects switched
// off with the effects key: effectsOn false): the game's own image, scaled with FSR.
bool upscaleOnly(float renderScale, bool effectsOn = true);

// Changes of the module states since the last call (bit per Id), for the core's Present to react on
// (the look's block, clips' recording). Clears them.
uint32_t takeChanges();

} // namespace modules
} // namespace tmshaders
