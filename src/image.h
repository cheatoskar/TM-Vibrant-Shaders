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

// Saves RGB pixels (rows top-down, no padding) as a PNG (photo mode).
bool writePng(const std::wstring& path, const unsigned char* rgb, int width, int height);

// Photo mode's compare split: the two sides' names burnt into the bottom of a photo, each on
// its side of the line (at * width from the left), white on a dark box, the text about 2 % of
// the image's height (a hi-res photo gets them as large, relative to its size). A name is left
// out when its side is too narrow for it. Any channel order (only greys are drawn), 3 or 4
// bytes a pixel, rows top-down `pitch` bytes apart. Names in UTF-8.
void drawSplitLabels(unsigned char* pixels, int width, int height, int bytesPerPixel, size_t pitch, float at, const std::string& left,
                     const std::string& right);

// JPEG -> 0xFFRRGGBB pixels, rows top-down.
bool decodeJpeg(const void* data, size_t size, std::vector<uint32_t>& pixels, int& width, int& height);
bool readJpeg(const std::wstring& path, std::vector<uint32_t>& pixels, int& width, int& height);

// The mod's icon (packaging/icons/icon_a_64.png, embedded by thumbnails/thumbnails.rc) for the
// menu's header: size x size 0xAARRGGBB pixels, area-averaged from the embedded PNG, rows
// top-down. False when the DLL holds no icon (the offline tools).
constexpr int kIconResource = 199;
bool menuIcon(int size, std::vector<uint32_t>& pixels);

} // namespace image
} // namespace tmshaders
