#include "image.h"
#include <cstdio>

// stb (public domain / MIT): only the JPEG reader and writer.
#pragma warning(push, 0)
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#pragma warning(pop)

namespace tmshaders {
namespace image {

namespace {
void appendBytes(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<unsigned char>*>(context);
    out->insert(out->end(), static_cast<unsigned char*>(data), static_cast<unsigned char*>(data) + size);
}
} // namespace

bool writeThumbnail(const std::wstring& path, const uint32_t* pixels, int width, int height, int pitchPixels) {
    if (!pixels || width <= 0 || height <= 0) return false;
    // The middle 16:9 of the frame (a 4:3 or ultrawide screen loses its sides or top).
    double cropW = width, cropH = height;
    if (cropW * 9.0 > cropH * 16.0) {
        cropW = cropH * 16.0 / 9.0;
    } else {
        cropH = cropW * 9.0 / 16.0;
    }
    const double x0 = (width - cropW) * 0.5, y0 = (height - cropH) * 0.5;
    const double sx = cropW / kThumbWidth, sy = cropH / kThumbHeight;
    std::vector<unsigned char> rgb(static_cast<size_t>(kThumbWidth) * kThumbHeight * 3);
    for (int y = 0; y < kThumbHeight; y++) {
        const int ya = static_cast<int>(y0 + y * sy), yb = static_cast<int>(y0 + (y + 1) * sy);
        for (int x = 0; x < kThumbWidth; x++) {
            const int xa = static_cast<int>(x0 + x * sx), xb = static_cast<int>(x0 + (x + 1) * sx);
            unsigned sum[3] = {}, count = 0;
            for (int py = ya; py < (yb > ya ? yb : ya + 1) && py < height; py++) {
                for (int px = xa; px < (xb > xa ? xb : xa + 1) && px < width; px++) {
                    const uint32_t p = pixels[static_cast<size_t>(py) * pitchPixels + px];
                    sum[0] += (p >> 16) & 0xFF;
                    sum[1] += (p >> 8) & 0xFF;
                    sum[2] += p & 0xFF;
                    count++;
                }
            }
            unsigned char* out = &rgb[(static_cast<size_t>(y) * kThumbWidth + x) * 3];
            for (int c = 0; c < 3; c++) out[c] = static_cast<unsigned char>(count ? sum[c] / count : 0);
        }
    }
    std::vector<unsigned char> jpeg;
    if (!stbi_write_jpg_to_func(appendBytes, &jpeg, kThumbWidth, kThumbHeight, 3, rgb.data(), 88)) return false;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool ok = fwrite(jpeg.data(), 1, jpeg.size(), f) == jpeg.size();
    fclose(f);
    return ok;
}

bool decodeJpeg(const void* data, size_t size, std::vector<uint32_t>& pixels, int& width, int& height) {
    int channels = 0;
    unsigned char* rgb = stbi_load_from_memory(static_cast<const unsigned char*>(data), static_cast<int>(size), &width, &height, &channels, 3);
    if (!rgb) return false;
    pixels.resize(static_cast<size_t>(width) * height);
    for (size_t i = 0; i < pixels.size(); i++) {
        pixels[i] = 0xFF000000u | (static_cast<uint32_t>(rgb[i * 3]) << 16) | (static_cast<uint32_t>(rgb[i * 3 + 1]) << 8) | rgb[i * 3 + 2];
    }
    stbi_image_free(rgb);
    return true;
}

bool readJpeg(const std::wstring& path, std::vector<uint32_t>& pixels, int& width, int& height) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::vector<unsigned char> data;
    unsigned char buffer[16384];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) data.insert(data.end(), buffer, buffer + n);
    fclose(f);
    return !data.empty() && decodeJpeg(data.data(), data.size(), pixels, width, height);
}

} // namespace image
} // namespace tmshaders
