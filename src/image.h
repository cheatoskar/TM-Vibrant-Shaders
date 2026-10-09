#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tmshaders {
namespace image {

// Preset pictures in the menu: 16:9, small enough to embed a dozen in the DLL.
constexpr int kThumbWidth = 384;
constexpr int kThumbHeight = 216;

// Saves the middle 16:9 of an image (0xAARRGGBB, rows top-down) as a kThumbWidth x
// kThumbHeight JPEG, scaled down by area averaging (no shimmering edges).
bool writeThumbnail(const std::wstring& path, const uint32_t* pixels, int width, int height, int pitchPixels);

// JPEG -> 0xFFRRGGBB pixels, rows top-down.
bool decodeJpeg(const void* data, size_t size, std::vector<uint32_t>& pixels, int& width, int& height);
bool readJpeg(const std::wstring& path, std::vector<uint32_t>& pixels, int& width, int& height);

} // namespace image
} // namespace tmshaders
