#pragma once
#include <cstdint>
#include <vector>

namespace tmshaders {
namespace noise {

// Tiling 3D noise for the volumetric clouds, size^3 texels in A8R8G8B8 layout:
// r = Perlin-Worley (cloud shapes), g/b/a = Worley fBm at 4, 8 and 16 cells (erosion).
// Computed once and cached; takes a moment for size 64.
const std::vector<uint32_t>& cloudVolume(int size);

} // namespace noise
} // namespace tmshaders
