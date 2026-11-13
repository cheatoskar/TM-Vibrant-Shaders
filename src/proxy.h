#pragma once
#include <windows.h>

struct IDirect3D9;

namespace tmshaders {
namespace proxy {

// The Direct3D 9 the mod forwards to and creates its factory with: `d3d9.chain.dll` next to this DLL
// when there is one (the game folder's own d3d9.dll the setup renamed: dgVoodoo in TM Original, the
// Sunrise repack's wrapper), else the system's d3d9.dll by full path (in standalone mode this DLL is
// "d3d9.dll" itself). Decided once, at the first call.
HMODULE systemD3D9();
IDirect3D9* createDirect3D9(UINT sdkVersion);
// What systemD3D9 loaded, for the log ("" before its first call): "system d3d9.dll (C:\...)" or
// "d3d9.chain.dll (dgVoodoo 2.8.2 - Direct3D9) in the game folder", with why a chain file was passed by.
const char* d3d9Source();
// A chain file was found and is what the mod forwards to.
bool chained();

} // namespace proxy
} // namespace tmshaders
