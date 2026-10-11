#include "thumbnails.h"
#include "config.h"
#include "image.h"
#include "log.h"
#include <map>
#include <vector>

namespace tmshaders {
namespace thumbnails {

namespace {
constexpr int kThumbnailBase = 200; // resource id of the first preset (thumbnails/thumbnails.rc)

struct Entry {
    IDirect3DTexture9* texture = nullptr;
    int version = -1;            // Config::pictureVersion() when it was loaded
    ULONGLONG written = 0;       // your picture's file time when it was loaded
    DWORD checked = 0;           // when the file time was last looked at
};

// The file's last write time (0 = no file).
ULONGLONG writeTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}
std::map<std::string, Entry> g_cache;
IDirect3DDevice9* g_device = nullptr;

IDirect3DTexture9* upload(IDirect3DDevice9* device, const std::vector<uint32_t>& pixels, int width, int height) {
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr))) return nullptr;
    D3DLOCKED_RECT locked{};
    if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) {
        texture->Release();
        return nullptr;
    }
    for (int y = 0; y < height; y++) {
        memcpy(static_cast<char*>(locked.pBits) + static_cast<size_t>(locked.Pitch) * y, &pixels[static_cast<size_t>(y) * width],
               static_cast<size_t>(width) * 4);
    }
    texture->UnlockRect(0);
    return texture;
}

bool loadEmbedded(int index, std::vector<uint32_t>& pixels, int& width, int& height) {
    static const int s_anchor = 0; // any address inside this module
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&s_anchor), &module);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(kThumbnailBase + index), MAKEINTRESOURCEW(10)); // RT_RCDATA
    HGLOBAL handle = resource ? LoadResource(module, resource) : nullptr;
    const void* data = handle ? LockResource(handle) : nullptr;
    return data && image::decodeJpeg(data, SizeofResource(module, resource), pixels, width, height);
}
} // namespace

IDirect3DTexture9* get(IDirect3DDevice9* device, const std::string& preset) {
    if (device != g_device) {
        // A new device (the game recreated it): the old textures belong to the old one.
        for (auto& entry : g_cache) {
            if (entry.second.texture) entry.second.texture->Release();
        }
        g_cache.clear();
        g_device = device;
    }
    Config& config = Config::get();
    Entry& entry = g_cache[preset];
    const bool own = config.isUserPreset(preset);
    const int version = own ? config.pictureVersion() : 0;
    bool changed = entry.version != version;
    // Your pictures can be replaced in the folder while the game runs: look once a second.
    ULONGLONG written = entry.written;
    if (own && (changed || GetTickCount() - entry.checked > 1000)) {
        entry.checked = GetTickCount();
        written = writeTime(config.presetPicture(preset));
        changed |= written != entry.written;
    }
    if (!changed) return entry.texture;
    if (entry.texture) entry.texture->Release();
    entry.texture = nullptr;
    entry.version = version;
    entry.written = written;

    std::vector<uint32_t> pixels;
    int width = 0, height = 0;
    bool loaded = false;
    if (config.isUserPreset(preset)) {
        loaded = image::readJpeg(config.presetPicture(preset), pixels, width, height);
    } else {
        for (int i = 0; i < static_cast<int>(Preset::Custom); i++) {
            if (preset == presetName(static_cast<Preset>(i))) loaded = loadEmbedded(i, pixels, width, height);
        }
    }
    if (loaded) entry.texture = upload(device, pixels, width, height);
    return entry.texture;
}

bool savePicture(IDirect3DDevice9* device, IDirect3DSurface9* frame, const std::wstring& path) {
    D3DSURFACE_DESC desc{};
    frame->GetDesc(&desc);
    // The middle 16:9, scaled to twice the picture by the GPU, the rest averaged on the CPU.
    RECT source{0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
    if (desc.Width * 9 > desc.Height * 16) {
        const LONG w = static_cast<LONG>(desc.Height * 16 / 9);
        source.left = (static_cast<LONG>(desc.Width) - w) / 2;
        source.right = source.left + w;
    } else {
        const LONG h = static_cast<LONG>(desc.Width * 9 / 16);
        source.top = (static_cast<LONG>(desc.Height) - h) / 2;
        source.bottom = source.top + h;
    }
    const UINT w = image::kThumbWidth * 2, h = image::kThumbHeight * 2;
    IDirect3DSurface9* scaled = nullptr;
    IDirect3DSurface9* readback = nullptr;
    bool ok = SUCCEEDED(device->CreateRenderTarget(w, h, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &scaled, nullptr)) &&
              SUCCEEDED(device->CreateOffscreenPlainSurface(w, h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr)) &&
              SUCCEEDED(device->StretchRect(frame, &source, scaled, nullptr, D3DTEXF_LINEAR)) &&
              SUCCEEDED(device->GetRenderTargetData(scaled, readback));
    if (ok) {
        D3DLOCKED_RECT locked{};
        ok = SUCCEEDED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY));
        if (ok) {
            ok = image::writeThumbnail(path, static_cast<const uint32_t*>(locked.pBits), static_cast<int>(w), static_cast<int>(h), locked.Pitch / 4);
            readback->UnlockRect();
        }
    }
    if (scaled) scaled->Release();
    if (readback) readback->Release();
    TMVS_LOG("menu: preset picture %s", ok ? "saved" : "failed");
    return ok;
}

} // namespace thumbnails
} // namespace tmshaders
