// STUB: delete this file (and its line in CMakeLists.txt) when the real src/blockorder.cpp from
// branch zfight-re is merged. It only lets the render side (OverlapFix in plugin.cpp) build and
// be tested before the engine side exists: no block has a rank, so nothing is biased.
// For testing the bias path without the engine side see FakeBlockRank in dev.ini (plugin.cpp).
#include "blockorder.h"

namespace blockorder {
int currentDrawRank() { return 0; }
void invalidate() {}
}
