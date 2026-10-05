#pragma once
#include <windows.h>
#include <string>

namespace tmshaders {
namespace plugin {

void setModule(HMODULE module);
const std::wstring& moduleDir();

// One copy per process may run: the ModLoader copy and a d3d9.dll in the game folder could
// both be loaded. Returns false for the second one, which then only forwards d3d9 calls.
bool claimInstance();
bool active();

void boot();         // background thread started from DllMain
void ensureBooted(); // synchronous, from the d3d9.dll exports (re-entrant, waits for a running boot)
void shutdown();

} // namespace plugin
} // namespace tmshaders
