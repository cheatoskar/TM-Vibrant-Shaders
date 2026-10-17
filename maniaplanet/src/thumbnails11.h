#pragma once
#include <d3d11.h>
#include <string>

namespace tmvs_mp {
namespace thumbnails {

// The picture of a preset for the menu (D3D11 counterpart of src/thumbnails.cpp): the built-in
// ones are embedded in the DLL (thumbnails/thumbnails.rc), yours are presets\<name>.jpg.
// nullptr = no picture.
ID3D11ShaderResourceView* get(ID3D11Device* device, const std::string& preset);

// The mod's icon for the menu's header, size x size pixels (drawn texel for pixel), kept until
// the size or the device changes (a window resize keeps it). nullptr = none.
ID3D11ShaderResourceView* icon(ID3D11Device* device, int size);

// Saves the middle of a finished frame (before the HUD) as a preset's picture.
bool savePicture(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Resource* frame, const std::wstring& path);

void release();

} // namespace thumbnails
} // namespace tmvs_mp
