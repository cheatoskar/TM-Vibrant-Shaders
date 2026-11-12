// tmvs-core.dll only (never in the standalone TMVibrantShaders.dll): the functions the four module
// plugins call (src/modules/module.cpp), and DllMain. All __cdecl, plain C names.
#include "modules/hostbridge.h"
#include "plugin.h"
#include <windows.h>
#include <cstring>
#include <string>

extern "C" {

__declspec(dllexport) uint32_t __cdecl TMVS_Core_Interface(void) {
    return tmshaders::hostbridge::kInterface;
}

__declspec(dllexport) int __cdecl TMVS_Core_Attach(int module, const HorizonHostApi* host) {
    return tmshaders::hostbridge::attach(module, host);
}

__declspec(dllexport) void __cdecl TMVS_Core_Detach(int module) {
    tmshaders::hostbridge::detach(module);
}

__declspec(dllexport) void __cdecl TMVS_Core_SetEnabled(int module, int enabled) {
    tmshaders::hostbridge::setEnabled(module, enabled != 0);
}

__declspec(dllexport) void __cdecl TMVS_Core_DrawMenu(int module) {
    tmshaders::hostbridge::drawMenu(module);
}

// Tests (tools/moduletest.cpp, tools/hzpreview.cpp): every file under `root` instead of
// Documents\TrackMania. Only before the first attach; 0 afterwards.
__declspec(dllexport) int __cdecl TMVS_Core_SetDataRoot(const wchar_t* root) {
    return tmshaders::hostbridge::setDataRoot(root) ? 1 : 0;
}

// Tests: the core's state as "key=value;...". Returns the length (truncated to size - 1).
__declspec(dllexport) int __cdecl TMVS_Core_Query(char* out, int size) {
    const std::string text = tmshaders::hostbridge::query();
    if (out && size > 0) strncpy_s(out, static_cast<size_t>(size), text.c_str(), _TRUNCATE);
    return static_cast<int>(text.size());
}

} // extern "C"

// Nothing starts here: the core boots at the first module's attach (on the game's thread, from
// HorizonPlugin_Init). The process's end shuts it down as the standalone's DllMain does.
extern "C" BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        tmshaders::plugin::setModule(module);
        break;
    case DLL_PROCESS_DETACH:
        if (tmshaders::plugin::active() && tmshaders::plugin::booted()) tmshaders::plugin::shutdown();
        break;
    default:
        break;
    }
    return TRUE;
}
