#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The pictures the water effects (splash, wake, foam, clear water) draw with, in one small texture:
// r the droplet (a soft sprite with a highlight), g the foam (tiles), b and a the ripple's slope
// (the normal map's x and y, 0.5 = flat; tiles). Each comes from a file of the same name in the
// textures folder (TMVS/textures/water/water_droplet.png, water_foam.png, water_ripple.png; PNG,
// JPEG, TGA or BMP, any size) when it is there, else it is made here, so the built-in look needs
// no files at all. A texture pack drops its own into that folder.
namespace tmshaders {
namespace waterfx {

constexpr int kAtlasSize = 128;

// kAtlasSize^2 texels in A8R8G8B8 layout (a << 24 | r << 16 | g << 8 | b), rows top-down.
// `folder` empty: the built-in pictures only. `report` (if given) says where each came from.
std::vector<uint32_t> buildAtlas(const std::wstring& folder, std::string* report = nullptr);

} // namespace waterfx
} // namespace tmshaders
