#include "capture.h"
#include "log.h"
#include <cstdio>
#include <vector>

namespace tmshaders {
namespace capture {
namespace {

float halfToFloat(unsigned short h) {
    unsigned sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
    float value;
    if (exponent == 0) value = ldexpf(static_cast<float>(mantissa), -24);
    else if (exponent == 31) value = mantissa ? 0.0f : 65504.0f;
    else value = ldexpf(static_cast<float>(mantissa | 1024), static_cast<int>(exponent) - 25);
    return sign ? -value : value;
}

unsigned char toByte(float v) {
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<unsigned char>(v * 255.0f + 0.5f);
}

// Copies any render target (including multisampled) to a lockable system-memory surface.
IDirect3DSurface9* readback(IDirect3DDevice9* device, IDirect3DSurface9* surface, D3DSURFACE_DESC& desc) {
    surface->GetDesc(&desc);
    IDirect3DSurface9* source = surface;
    IDirect3DSurface9* resolved = nullptr;
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        if (FAILED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)))
            return nullptr;
        device->StretchRect(surface, nullptr, resolved, nullptr, D3DTEXF_NONE);
        source = resolved;
    }
    IDirect3DSurface9* system = nullptr;
    if (SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &system, nullptr))) {
        if (FAILED(device->GetRenderTargetData(source, system))) {
            system->Release();
            system = nullptr;
        }
    }
    if (resolved) resolved->Release();
    return system;
}

} // namespace

bool saveSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface, const std::wstring& path) {
    D3DSURFACE_DESC desc{};
    IDirect3DSurface9* system = readback(device, surface, desc);
    if (!system) {
        TMVS_LOG("capture: readback failed");
        return false;
    }
    D3DLOCKED_RECT locked{};
    if (FAILED(system->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        system->Release();
        return false;
    }

    const UINT w = desc.Width, h = desc.Height;
    const UINT rowBytes = (w * 3 + 3) & ~3u;
    std::vector<unsigned char> pixels(rowBytes * h);
    for (UINT y = 0; y < h; y++) {
        const BYTE* row = static_cast<const BYTE*>(locked.pBits) + static_cast<size_t>(locked.Pitch) * y;
        unsigned char* out = &pixels[static_cast<size_t>(rowBytes) * (h - 1 - y)];
        for (UINT x = 0; x < w; x++) {
            unsigned char r = 0, g = 0, b = 0;
            switch (desc.Format) {
                case D3DFMT_A8R8G8B8:
                case D3DFMT_X8R8G8B8:
                    b = row[x * 4 + 0]; g = row[x * 4 + 1]; r = row[x * 4 + 2];
                    break;
                case D3DFMT_A16B16G16R16F: {
                    const unsigned short* p = reinterpret_cast<const unsigned short*>(row) + x * 4;
                    r = toByte(halfToFloat(p[0])); g = toByte(halfToFloat(p[1])); b = toByte(halfToFloat(p[2]));
                    break;
                }
                case D3DFMT_A2R10G10B10: {
                    DWORD v = reinterpret_cast<const DWORD*>(row)[x];
                    r = static_cast<unsigned char>(((v >> 20) & 1023) >> 2);
                    g = static_cast<unsigned char>(((v >> 10) & 1023) >> 2);
                    b = static_cast<unsigned char>((v & 1023) >> 2);
                    break;
                }
                default:
                    break;
            }
            out[x * 3 + 0] = b; out[x * 3 + 1] = g; out[x * 3 + 2] = r;
        }
    }
    system->UnlockRect();
    system->Release();

    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(pixels.size());
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(w);
    ih.biHeight = static_cast<LONG>(h);
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biSizeImage = static_cast<DWORD>(pixels.size());
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(pixels.data(), 1, pixels.size(), f);
    fclose(f);
    return true;
}

bool saveFloatSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface, const std::wstring& path) {
    D3DSURFACE_DESC desc{};
    IDirect3DSurface9* system = readback(device, surface, desc);
    if (!system) return false;
    D3DLOCKED_RECT locked{};
    if (FAILED(system->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        system->Release();
        return false;
    }
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (f) {
        unsigned header[2] = {desc.Width, desc.Height};
        fwrite(header, sizeof(header), 1, f);
        std::vector<float> row(desc.Width);
        for (UINT y = 0; y < desc.Height; y++) {
            const BYTE* src = static_cast<const BYTE*>(locked.pBits) + static_cast<size_t>(locked.Pitch) * y;
            for (UINT x = 0; x < desc.Width; x++) {
                if (desc.Format == D3DFMT_R32F) row[x] = reinterpret_cast<const float*>(src)[x];
                else if (desc.Format == D3DFMT_R16F) row[x] = halfToFloat(reinterpret_cast<const unsigned short*>(src)[x]);
                else if (desc.Format == D3DFMT_A32B32G32R32F) row[x] = reinterpret_cast<const float*>(src)[x * 4];
                else row[x] = 0.0f;
            }
            fwrite(row.data(), sizeof(float), desc.Width, f);
        }
        fclose(f);
    }
    system->UnlockRect();
    system->Release();
    return f != nullptr;
}

} // namespace capture
} // namespace tmshaders
