#include "image.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

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

bool writePng(const std::wstring& path, const unsigned char* rgb, int width, int height) {
    if (!rgb || width <= 0 || height <= 0) return false;
    std::vector<unsigned char> png;
    if (!stbi_write_png_to_func(appendBytes, &png, width, height, 3, rgb, width * 3)) return false;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
    fclose(f);
    if (!ok) _wremove(path.c_str()); // no half-written photo
    return ok;
}

namespace {

std::wstring wideName(const std::string& text) {
    // Preset names are UTF-8 (file names); anything else as the system's code page.
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int n = MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) {
        codePage = CP_ACP;
        flags = 0;
        n = MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
    }
    std::wstring out(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), &out[0], n);
    return out;
}

// A line of text as coverage (0..255, rows top-down), drawn by GDI with grey anti-aliasing.
bool textMask(const std::wstring& text, int pixelHeight, std::vector<unsigned char>& cover, int& w, int& h) {
    if (text.empty()) return false;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    HFONT font = CreateFontW(-pixelHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    SIZE size{};
    bool ok = font && GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size) && size.cx > 0 && size.cy > 0;
    void* bits = nullptr;
    HBITMAP bitmap = nullptr;
    if (ok) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = size.cx;
        info.bmiHeader.biHeight = -size.cy; // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        ok = bitmap && bits;
    }
    if (ok) {
        HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        memset(bits, 0, static_cast<size_t>(size.cx) * size.cy * 4);
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkMode(dc, TRANSPARENT);
        ok = TextOutW(dc, 0, 0, text.c_str(), static_cast<int>(text.size())) != FALSE;
        GdiFlush();
        if (ok) {
            w = size.cx;
            h = size.cy;
            cover.resize(static_cast<size_t>(w) * h);
            const unsigned char* src = static_cast<const unsigned char*>(bits);
            for (size_t i = 0; i < cover.size(); i++) cover[i] = src[i * 4 + 1]; // white text: any channel is its coverage
        }
        SelectObject(dc, oldBitmap);
    }
    if (bitmap) DeleteObject(bitmap);
    if (oldFont) SelectObject(dc, oldFont);
    if (font) DeleteObject(font);
    DeleteDC(dc);
    return ok;
}

} // namespace

void drawSplitLabels(unsigned char* pixels, int width, int height, int bytesPerPixel, size_t pitch, float at, const std::string& left,
                     const std::string& right) {
    if (!pixels || width <= 0 || height <= 0 || bytesPerPixel < 3) return;
    const int textHeight = height * 22 / 1000 > 12 ? height * 22 / 1000 : 12; // 24 px at 1080p, 32 at 1440p
    const int padX = textHeight / 2, padY = textHeight / 6;
    const int margin = textHeight * 4 / 5;
    // Clear of the split's line (photo::applySplit: 2 px light and 2 px dark at 1080p each side).
    const int line = height >= 1080 ? height / 540 : 1;
    const int gap = 2 * line + textHeight / 2;
    const float a = at < 0.0f ? 0.0f : (at > 1.0f ? 1.0f : at);
    const int x = static_cast<int>(a * width + 0.5f);
    for (int side = 0; side < 2; side++) {
        std::vector<unsigned char> cover;
        int tw = 0, th = 0;
        if (!textMask(wideName(side ? right : left), textHeight, cover, tw, th)) continue;
        const int boxW = tw + 2 * padX, boxH = th + 2 * padY;
        // At the line, as the bar on the screen; a side too narrow for its name gets none (a name
        // over the line would label the wrong side).
        const int boxX = side ? x + gap : x - gap - boxW;
        const int boxY = height - margin - boxH;
        if (boxX < margin / 2 || boxX + boxW > width - margin / 2 || boxY < 0) continue;
        for (int y = 0; y < boxH; y++) {
            unsigned char* row = pixels + static_cast<size_t>(boxY + y) * pitch + static_cast<size_t>(boxX) * bytesPerPixel;
            const int ty = y - padY;
            for (int i = 0; i < boxW; i++) {
                unsigned char* p = row + static_cast<size_t>(i) * bytesPerPixel;
                const int tx = i - padX;
                const int c = tx >= 0 && tx < tw && ty >= 0 && ty < th ? cover[static_cast<size_t>(ty) * tw + tx] : 0;
                for (int k = 0; k < 3; k++) {
                    const int dark = p[k] * 2 / 5; // the box: 60 % black, the scene shows through
                    p[k] = static_cast<unsigned char>(dark + (235 - dark) * c / 255);
                }
            }
        }
    }
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
