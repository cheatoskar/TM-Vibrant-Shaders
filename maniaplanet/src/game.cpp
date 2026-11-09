#include "game.h"
#include "log.h"
#include <windows.h>
#include <cstring>

namespace tmvs_mp {
namespace game {
namespace {

// CTrackMania (0x24001000) <- CGameManiaPlanet (0x03013000) <- CGameCtnApp (0x030D3000): its base
// constructor keeps `this` in a global (kAppGlobal, RVA).
constexpr uintptr_t kAppGlobal = 0x1C8B410;
constexpr uint32_t kAppIds[] = {0x24001000, 0x03013000, 0x030D3000};
// CGameCtnMediaTracker: the replay editor (also the replay viewer and Shoot video) in the app's
// Editor slot (the map editors are others there: maplook.cpp).
constexpr uint32_t kMediaTrackerId = 0x030B1000;
// CGamePlayground (0x0300D000)::ReplayRecord at 0x1E8 and ::PrevReplayRecord at 0x1F0
// (CGameCtnReplayRecord 0x03093000): the reflection table its static initializer builds (RVA
// 0xD7780). Only logged, to learn when the game sets them.
constexpr size_t kPlaygroundReplayRecord = 0x1E8, kPlaygroundPrevReplayRecord = 0x1F0;
constexpr uint32_t kReplayRecordId = 0x03093000;

uintptr_t g_base = 0;
uintptr_t g_textStart = 0, g_textEnd = 0;
bool g_checked = false, g_attached = false;
bool g_replayEditor = false;

void setUp() {
    if (g_checked) return;
    g_checked = true;
    HMODULE exe = GetModuleHandleW(nullptr);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(exe, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    if (!name || _wcsicmp(name + 1, L"ManiaPlanet.exe") != 0) return;
    g_base = reinterpret_cast<uintptr_t>(exe);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!memcmp(sec->Name, ".text", 6)) {
            g_textStart = g_base + sec->VirtualAddress;
            g_textEnd = g_textStart + sec->Misc.VirtualSize;
        }
    }
    g_attached = g_textEnd > g_textStart;
}

} // namespace

bool attached() {
    setUp();
    return g_attached;
}

uintptr_t base() {
    setUp();
    return g_base;
}

bool inCode(uintptr_t address, size_t bytes) {
    return attached() && address >= g_textStart && address + bytes <= g_textEnd;
}

bool codeIs(uintptr_t rva, const unsigned char* bytes, size_t count) {
    return inCode(g_base + rva, count) && !memcmp(reinterpret_cast<const void*>(g_base + rva), bytes, count);
}

uint32_t classOf(uintptr_t object) {
    if (!attached()) return 0;
    __try {
        if (!object) return 0;
        const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(object);
        if (vtable < g_base || vtable >= g_base + 0x4000000) return 0;
        const uintptr_t fn = reinterpret_cast<const uintptr_t*>(vtable)[3];
        if (fn < g_textStart || fn + 9 > g_textEnd) return 0;
        const auto* code = reinterpret_cast<const unsigned char*>(fn);
        if (code[0] != 0xC7 || code[1] != 0x02 || code[6] != 0x48 || code[7] != 0x8B || code[8] != 0xC2) return 0;
        uint32_t id;
        memcpy(&id, code + 2, 4);
        return id;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uintptr_t readPointer(uintptr_t at) {
    __try {
        return *reinterpret_cast<const uintptr_t*>(at);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool readBytes(uintptr_t at, void* out, size_t bytes) {
    __try {
        memcpy(out, reinterpret_cast<const void*>(at), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool writeBytes(uintptr_t at, const void* in, size_t bytes) {
    __try {
        memcpy(reinterpret_cast<void*>(at), in, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t app() {
    if (!attached()) return 0;
    const uintptr_t a = readPointer(g_base + kAppGlobal);
    const uint32_t id = classOf(a);
    for (uint32_t ok : kAppIds)
        if (id == ok) return a;
    return 0;
}

bool replayEditor() {
    return g_replayEditor;
}

void tick() {
    if (!attached()) return;
    const uintptr_t a = app();
    const uint32_t editor = a ? classOf(readPointer(a + kAppEditor)) : 0;
    const uintptr_t playground = a ? readPointer(a + kAppCurrentPlayground) : 0;
    const uint32_t playgroundClass = classOf(playground);
    g_replayEditor = editor == kMediaTrackerId;
    // The playground's replay records (only logged): set while a replay plays, or while one records?
    int records = 0;
    if (playgroundClass) {
        if (classOf(readPointer(playground + kPlaygroundReplayRecord)) == kReplayRecordId) records |= 1;
        if (classOf(readPointer(playground + kPlaygroundPrevReplayRecord)) == kReplayRecordId) records |= 2;
    }
    static uint32_t s_editor = ~0u, s_playground = ~0u;
    static int s_records = -1, s_lines = 0;
    if ((editor != s_editor || playgroundClass != s_playground || records != s_records) && s_lines < 60) {
        s_lines++;
        TMVS_LOG("game: editor class %08x%s, playground class %08x (replay record %s, previous %s): %s", editor,
                 g_replayEditor ? " (the replay editor)" : "", playgroundClass, (records & 1) ? "set" : "none",
                 (records & 2) ? "set" : "none", g_replayEditor ? "replay" : "driving or menus");
    }
    s_editor = editor;
    s_playground = playgroundClass;
    s_records = records;
}

} // namespace game
} // namespace tmvs_mp
