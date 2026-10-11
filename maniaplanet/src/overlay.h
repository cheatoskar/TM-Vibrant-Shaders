#pragma once
#include <d3d11.h>
#include <windows.h>

namespace tmvs_mp {
namespace overlay {

// F8 menu (ImGui). init() once the game's swap chain is known; frame() at every Present, after the
// mod's pass, onto the back buffer.
void init(HWND window, ID3D11Device* device, ID3D11DeviceContext* ctx);
void frame(ID3D11RenderTargetView* backBuffer);

} // namespace overlay
} // namespace tmvs_mp
