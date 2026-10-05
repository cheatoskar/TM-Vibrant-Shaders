#include <windows.h>
#include "plugin.h"

namespace {

DWORD WINAPI boot(LPVOID) {
    tmshaders::plugin::boot();
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(module);
            tmshaders::plugin::setModule(module);
            if (tmshaders::plugin::claimInstance()) {
                HANDLE thread = CreateThread(nullptr, 0, boot, nullptr, 0, nullptr);
                if (thread) CloseHandle(thread);
            }
            break;
        }
        case DLL_PROCESS_DETACH:
            if (tmshaders::plugin::active()) tmshaders::plugin::shutdown();
            break;
        default:
            break;
    }
    return TRUE;
}
