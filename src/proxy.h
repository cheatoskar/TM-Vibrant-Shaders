#pragma once
#include <windows.h>

struct IDirect3D9;

namespace tmshaders {
namespace proxy {

// The system's d3d9.dll, loaded by full path (in standalone mode this DLL is "d3d9.dll" itself).
HMODULE systemD3D9();
IDirect3D9* createDirect3D9(UINT sdkVersion);

} // namespace proxy
} // namespace tmshaders
