#pragma once
#include <cstdint>
#include <string>

// tmvs-core.dll's side of Horizon Host (TMVS 2.0 as Horizon modules, src/modules/modules.h). The four
// module plugins (src/modules/module.cpp) carry no code of their own: their HorizonPlugin_* exports
// call the core's TMVS_Core_* exports (src/modules/coreexports.cpp), which land here.
//
// The first attach claims TM Vibrant Shaders for this process (the same named mutex as the standalone
// DLL: only one copy runs, the other stays passive, each log says which) and boots the core in hosted
// mode. Each module then registers what is its own with its own plugin handle:
//   - device callbacks: every module registers them; the lowest attached module (the lowest enabled one
//     for pre_present) is the one whose calls the core takes, so each call is taken once;
//   - hotkeys in Horizon's table, with the keys from settings.ini / clips.ini as the defaults (the
//     1.5 keys carry over; rebinding is Horizon's from then on);
//   - panels: one per menu page of the module; its Settings tab is DrawMenu.
// The host's capture flag is set through the module that takes pre_present.
//
// The game's thread only.
struct HorizonHostApi;
struct HorizonEvent;

namespace tmshaders {
namespace hostbridge {

// TMVS_Core_Attach's answers (the module logs them; only kAttached runs anything).
enum Result : int {
    kAttached = 0,
    kAlready = 1,          // this module is attached already
    kOldHost = 2,          // no table, another ABI, or a host before ABI 1.2 (panels, host flags)
    kImGuiDiffers = 3,     // the host's Dear ImGui is another version than the core's
    kStandaloneActive = 4, // the standalone DLL (d3d9.dll or the ModLoader's) runs in this game
    kBadModule = 5,        // not a module id, or another core interface version
};

// The interface between module.cpp and coreexports.cpp: both are built from the same tree, a module
// refuses a core of another version (an old folder left in plugins\).
constexpr uint32_t kInterface = 1;

int attach(int module, const HorizonHostApi* host);
void detach(int module);
void setEnabled(int module, bool enabled);
// The module's Settings tab (HorizonPlugin_DrawMenu), drawn with the host's ImGui context current.
void drawMenu(int module);
// Tests (tools/moduletest.cpp): every file under root instead of Documents\TrackMania. Before the first
// attach only; false afterwards.
bool setDataRoot(const wchar_t* root);
// Tests: the core's state as "key=value;..." (hosted, owner, attached, enabled, shaders, capture, ...).
std::string query();

} // namespace hostbridge
} // namespace tmshaders
