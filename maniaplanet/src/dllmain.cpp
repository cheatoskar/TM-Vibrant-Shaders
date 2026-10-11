#include <windows.h>
#include <cwchar>

namespace tmvs_mp {
namespace proxy {
bool init(bool active);
} // namespace proxy
} // namespace tmvs_mp

namespace {

// Only the game itself (and the offline test) gets hooks; anything else in the game folder that
// loads d3d11.dll, such as the launcher, gets the system DLL untouched.
bool isGame() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"ManiaPlanet.exe") == 0 || _wcsicmp(name, L"ManiaPlanet32.exe") == 0 ||
           _wcsicmp(name, L"mp_harness.exe") == 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(module);
    // Nothing else here: DllMain runs under the loader lock. Logging and settings start with the
    // first device (proxy.cpp).
    return tmvs_mp::proxy::init(isGame()) ? TRUE : FALSE;
}
