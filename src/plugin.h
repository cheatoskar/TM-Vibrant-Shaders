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
// The copy that holds the instance in this process ("standalone (d3d9.dll)", "standalone (ModLoader)",
// "Horizon modules (tmvs-core.dll)"), for the log of a copy that stays passive.
std::string activeOwner();
// TMVS 2.0 as Horizon modules: the clips start with the Capture module (the standalone boots them itself).
void startClips();

void boot();         // background thread started from DllMain
void ensureBooted(); // synchronous, from the d3d9.dll exports (re-entrant, waits for a running boot)
bool booted();       // ensureBooted() finished
void shutdown();

} // namespace plugin
} // namespace tmshaders
