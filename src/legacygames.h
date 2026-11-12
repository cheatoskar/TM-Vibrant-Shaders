#pragma once
#include <windows.h>
#include <cwchar>

// The older TrackMania games on the 2006 engine that TM Vibrant Shaders supports as d3d9.dll: one
// community exe each (no StarForce, no ModLoader), told by its SHA-256. Header only: the mod
// (src/engine.cpp, the log's name) and the setup (tools/installer.cpp) share it.
//
//  - TrackMania Nations ESWC, the unofficial 1.80 patch (2021, reports 0.1.7.9).
//  - TrackMania Sunrise, the Windows 10 community repack (its own d3d9.dll proxy next to the exe).
//  - TrackMania Original, GreffMASTER's Online Patch v2 (2023; dgVoodoo 2.8.2 as d3d9.dll + DDraw.dll).
//
// Sunrise and Original need the game folder's own d3d9.dll: the setup renames it to d3d9.chain.dll
// and the mod forwards to that (src/proxy.cpp).
namespace tmshaders {
namespace legacy {

enum class Game { None = -1, Eswc = 0, Sunrise = 1, Original = 2 };

struct Exe {
    Game game;
    const wchar_t* file;          // the exe's name (compared without case)
    const char* name;             // for the log and the menu
    const wchar_t* wideName;      // for the setup's dialogs
    const char* sha256;           // the supported exe
    DWORD timestamp, imageSize;   // its link timestamp and size of image (a quick check before the hash)
    const wchar_t* logName;       // Documents\TrackMania\TMVS\<logName>
    const wchar_t* registryValue; // the setup's game folder
    bool needsChain;              // the game folder's own d3d9.dll stays as d3d9.chain.dll
    bool hashRequired;            // false: the timestamp and size are enough (ESWC, as 1.5 accepted it), the engine checks decide
};

constexpr Exe kExes[] = {
    {Game::Eswc, L"TmNationsESWC.exe", "TrackMania Nations ESWC", L"TrackMania Nations ESWC",
     "983bd12724739dcb0f876aeb3a4259154b068940f5167f57ede750ca452d8068", 0x5ce7272f, 0x008e8000, L"tmvs-eswc.log", L"GameFolder_ESWC", false, false},
    {Game::Sunrise, L"TmSunrise.exe", "TrackMania Sunrise", L"TrackMania Sunrise",
     "e59dfc7eaa5314e85b2b0df6f40ef7ba01a23bd5a9525f84407f5dff301df4fe", 0x5cc8f0cf, 0x007de000, L"tmvs-sunrise.log", L"GameFolder_Sunrise", true, true},
    {Game::Original, L"TMORIGINAL.EXE", "TrackMania Original", L"TrackMania Original",
     "75e129d769f4595ddde54a8ef484162915bc2bf998c1a2139cc963294fe8175e", 0x5d337477, 0x007d9000, L"tmvs-original.log", L"GameFolder_Original", true, true},
};

// The entry whose exe has this file name (any build of it), null: not one of them.
inline const Exe* byFileName(const wchar_t* path) {
    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* name = slash ? slash + 1 : path;
    for (const Exe& e : kExes) {
        if (_wcsicmp(name, e.file) == 0) return &e;
    }
    return nullptr;
}

inline const Exe* byGame(Game game) {
    for (const Exe& e : kExes) {
        if (e.game == game) return &e;
    }
    return nullptr;
}

} // namespace legacy
} // namespace tmshaders
