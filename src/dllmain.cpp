#include <windows.h>
#include "config.h"
#include "hook.h"
#include "tm_shaders_version.h"

namespace {

DWORD WINAPI boot(LPVOID) {
    tmshaders::Config::get().load();

    // Retry hook installation while game graphics device initializes
    for (int attempt = 1; attempt <= 20; attempt++) {
        if (tmshaders::hook::install()) {
            return 0;
        }
        Sleep(1200);
    }
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(module);
            WCHAR modPath[MAX_PATH];
            GetModuleFileNameW(module, modPath, MAX_PATH);
            tmshaders::Config::get().setModulePath(modPath);

            HANDLE thread = CreateThread(nullptr, 0, boot, nullptr, 0, nullptr);
            if (thread) CloseHandle(thread);
            break;
        }
        case DLL_PROCESS_DETACH:
            tmshaders::hook::remove();
            break;
        default:
            break;
    }
    return TRUE;
}
