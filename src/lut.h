#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tmshaders {

// Colour look-up tables: 3D ".cube" files (Resolve, Lightroom, Photoshop, OBS ...) from
// <Documents>\TrackMania\TMVS\luts, applied after the mod's own grading.
namespace lut {

struct Table {
    int size = 0;                 // N (texels per side), 0 = none
    std::vector<uint32_t> texels; // A8R8G8B8, x = red, y = green, z = blue (N*N*N)
};

// Reads a .cube file (LUT_3D_SIZE 2..64, DOMAIN_MIN/MAX honoured). False with a reason.
bool loadCube(const std::wstring& path, Table& out, std::string& error);

// The folder and the .cube files in it (names only, sorted).
std::wstring folder();
std::vector<std::string> list();
// The full path of a file in the folder (name in UTF-8, as list() gives it).
std::wstring path(const std::string& name);

} // namespace lut
} // namespace tmshaders
