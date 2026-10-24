#pragma once
#include <d3d9.h>

// The Misc page's tools (TrackMania Nations/United Forever): a minimap of the track's shape with
// the cars, split times against your best run and a replay, and finish effects. Removable as a
// whole: CMake option TMVS_MISC_TOOLS (see src/misc/README.md). Off, these calls are empty and the
// page doesn't exist, so the rest of the mod calls them without asking.
namespace tmshaders {
class IniFile;
namespace misc {

struct Frame {
    IDirect3DDevice9* device = nullptr;
    bool menuOpen = false; // the F8 menu: the windows can be moved and resized then
    bool hide = false;     // the video export, photo mode, a frame capture or screenshot
};

#if defined(TMVS_MISC_TOOLS)
// The settings from settings.ini ([Misc]), once at startup (before the first frame).
void boot();
// The Misc page in the F8 menu (between ImGui's NewFrame and Render).
void drawPage();
// Something to draw this frame, also with the menu closed (the overlay then starts an ImGui frame).
bool wantsOverlay(const Frame& frame);
// The minimap, the split times and the finish effects (ImGui windows), between NewFrame and Render.
void drawOverlay(const Frame& frame);
// Writes [Misc] into settings.ini with the rest (Config::save).
void save(IniFile& ini);
constexpr bool kAvailable = true;
#else
inline void boot() {}
inline void drawPage() {}
inline bool wantsOverlay(const Frame&) { return false; }
inline void drawOverlay(const Frame&) {}
inline void save(IniFile&) {}
constexpr bool kAvailable = false;
#endif

} // namespace misc
} // namespace tmshaders
