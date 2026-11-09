#pragma once
// The Misc page in TrackMania 2 (the TMNF mod's split times, Session panel and finish effects, made
// anew on what ManiaPlanet's memory holds: game.cpp readRace). Read only, so it is allowed online
// (TMX's rules allow split times in the race). Your best run per map is kept in
// Documents\ManiaPlanet\TMVS\splits.ini. No minimap: TMX allows it only in the replay editor and the
// video export, and TrackMania 2's would need the map's blocks read from the game.

namespace tmvs_mp {
namespace splits {

// Once a frame at Present (render thread): the local player's run.
void tick();
// Something to draw with the menu closed too (the split times, the Session panel, an effect).
bool wantsOverlay();
// The panels and the finish effect (ImGui, between NewFrame and Render). hide: photo mode, the
// video export (nothing of it there).
void drawOverlay(bool menuOpen, bool hide);
// The Misc page's content (the menu's Misc page).
void drawPage();

} // namespace splits
} // namespace tmvs_mp
