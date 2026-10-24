#pragma once

namespace blockorder {
// 0 = not a block or no overlap (no bias); 1..7 = place among the blocks overlapping at its spot, later placed = higher.
int currentDrawRank();
void invalidate();
}
