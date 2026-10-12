#pragma once
// The Author's Shader in ManiaPlanet: the comments of the map being played or edited, read from
// (and in the editor written to) the engine's own map object. The places come from the engine's
// member reflection (CGameCtnApp::RootMap / Editor, the editor's Challenge, CGameCtnChallenge's
// Comments) and are checked against the class ids before they are used.
#include <string>

namespace tmvs_mp {
namespace maplook {

// Once a few frames, on the render thread: a new map (or its comments changed) gives the config
// its comments (Config::onMapComments), no map gives it none.
void tick();

// The comments of the map open in the editor. False: no map in the editor (or ManiaPlanet's build
// isn't the one this was made for).
bool editorComments(std::string& comments);
bool setEditorComments(const std::string& comments);

} // namespace maplook
} // namespace tmvs_mp
