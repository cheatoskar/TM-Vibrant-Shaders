#pragma once
#include <d3d9.h>

namespace tmshaders {
namespace overlay {

void init(IDirect3DDevice9* device);
void preReset();
void postReset();
void draw(IDirect3DDevice9* device);
void toggle();
bool isVisible();
LRESULT handleWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

} // namespace overlay
} // namespace tmshaders
