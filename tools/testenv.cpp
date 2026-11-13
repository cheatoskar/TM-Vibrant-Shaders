// Linked into the test tools (CMakeLists: TMVS_SANDBOXED_TESTS): their log and anything else under
// log::dataDir() goes to <exe folder>\testdata, never into Documents\TrackMania\TMVS. A test run must not
// move the game's tmvs.log to tmvs.prev.log (log.cpp does that at the first line) or touch settings.ini.
// A test that sets its own folder (log::setDataDir in main) still does.
#include "log.h"
#include <windows.h>
#include <string>

namespace {

struct TestEnv {
    TestEnv() {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring dir = path;
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) dir.resize(slash);
        tmshaders::log::setDataDir(dir + L"\\testdata");
    }
} g_env;

} // namespace
