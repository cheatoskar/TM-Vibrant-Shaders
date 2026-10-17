#include "thumbnails11.h"
#include "config.h"
#include "hooks.h"
#include "image.h"
#include "log.h"
#include <windows.h>
#include <map>
#include <vector>

namespace tmvs_mp {
namespace thumbnails {
namespace {

using tmshaders::Config;

constexpr int kThumbnailBase = 200; // resource id of the first preset (thumbnails/thumbnails.rc)

struct Entry {
    ID3D11ShaderResourceView* view = nullptr;
    int version = -1;      // Config::pictureVersion() when it was loaded
    ULONGLONG written = 0; // your picture's file time when it was loaded
    ULONGLONG checked = 0; // when the file time was last looked at
};

std::map<std::string, Entry> g_cache;
ID3D11Device* g_device = nullptr;
ID3D11ShaderResourceView* g_icon = nullptr;
int g_iconSize = 0; // the size g_icon was made for (also when that failed: tried once per size)

ULONGLONG writeTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

// 0xAARRGGBB pixels are B, G, R, A in memory.
ID3D11ShaderResourceView* upload(ID3D11Device* device, const std::vector<uint32_t>& pixels, int width, int height) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = static_cast<UINT>(width);
    d.Height = static_cast<UINT>(height);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {pixels.data(), static_cast<UINT>(width) * 4, 0};
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    hooks::Inside inside;
    if (SUCCEEDED(device->CreateTexture2D(&d, &init, &tex))) {
        device->CreateShaderResourceView(tex, nullptr, &view);
        tex->Release();
    }
    return view;
}

bool loadEmbedded(int index, std::vector<uint32_t>& pixels, int& width, int& height) {
    static const int s_anchor = 0; // any address inside this module
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&s_anchor), &module);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(kThumbnailBase + index), MAKEINTRESOURCEW(10)); // RT_RCDATA
    HGLOBAL handle = resource ? LoadResource(module, resource) : nullptr;
    const void* data = handle ? LockResource(handle) : nullptr;
    return data && tmshaders::image::decodeJpeg(data, SizeofResource(module, resource), pixels, width, height);
}

} // namespace

void release() {
    for (auto& entry : g_cache) {
        if (entry.second.view) entry.second.view->Release();
    }
    g_cache.clear();
    if (g_icon) g_icon->Release();
    g_icon = nullptr;
    g_iconSize = 0;
    g_device = nullptr;
}

ID3D11ShaderResourceView* icon(ID3D11Device* device, int size) {
    if (device != g_device) {
        release();
        g_device = device;
    }
    if (size == g_iconSize) return g_icon;
    if (g_icon) g_icon->Release();
    g_icon = nullptr;
    g_iconSize = size;
    std::vector<uint32_t> pixels;
    if (tmshaders::image::menuIcon(size, pixels)) g_icon = upload(device, pixels, size, size);
    return g_icon;
}

ID3D11ShaderResourceView* get(ID3D11Device* device, const std::string& preset) {
    if (device != g_device) {
        release();
        g_device = device;
    }
    Config& config = Config::get();
    Entry& entry = g_cache[preset];
    const bool own = config.isUserPreset(preset);
    const int version = own ? config.pictureVersion() : 0;
    bool changed = entry.version != version;
    // Your pictures can be replaced in the folder while the game runs: look once a second.
    ULONGLONG written = entry.written;
    if (own && (changed || GetTickCount64() - entry.checked > 1000)) {
        entry.checked = GetTickCount64();
        written = writeTime(config.presetPicture(preset));
        changed |= written != entry.written;
    }
    if (!changed) return entry.view;
    if (entry.view) entry.view->Release();
    entry.view = nullptr;
    entry.version = version;
    entry.written = written;

    std::vector<uint32_t> pixels;
    int width = 0, height = 0;
    bool loaded = false;
    if (own) {
        loaded = tmshaders::image::readJpeg(config.presetPicture(preset), pixels, width, height);
    } else {
        for (int i = 0; i < static_cast<int>(tmshaders::Preset::Custom); i++) {
            if (preset == tmshaders::presetName(static_cast<tmshaders::Preset>(i))) loaded = loadEmbedded(i, pixels, width, height);
        }
    }
    if (loaded) entry.view = upload(device, pixels, width, height);
    return entry.view;
}

bool savePicture(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Resource* frame, const std::wstring& path) {
    hooks::Inside inside;
    ID3D11Texture2D* tex = nullptr;
    if (!frame || FAILED(frame->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) return false;
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                      d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
    const bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                      d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    bool ok = false;
    if ((bgra || rgba) && d.SampleDesc.Count == 1) {
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        ID3D11Texture2D* staging = nullptr;
        if (SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &staging))) {
            ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, nullptr);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
                // writeThumbnail wants 0xAARRGGBB (B, G, R, A in memory).
                std::vector<uint32_t> pixels(static_cast<size_t>(d.Width) * d.Height);
                for (UINT y = 0; y < d.Height; y++) {
                    const auto* src = reinterpret_cast<const uint32_t*>(static_cast<const char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch);
                    uint32_t* dst = &pixels[static_cast<size_t>(y) * d.Width];
                    for (UINT x = 0; x < d.Width; x++) {
                        const uint32_t p = src[x];
                        dst[x] = bgra ? p : ((p & 0xff00ff00u) | ((p & 0xffu) << 16) | ((p >> 16) & 0xffu));
                    }
                }
                ctx->Unmap(staging, 0);
                ok = tmshaders::image::writeThumbnail(path, pixels.data(), static_cast<int>(d.Width), static_cast<int>(d.Height),
                                                      static_cast<int>(d.Width));
            }
            staging->Release();
        }
    }
    tex->Release();
    TMVS_LOG("menu: preset picture %s", ok ? "saved" : "failed");
    return ok;
}

} // namespace thumbnails
} // namespace tmvs_mp
