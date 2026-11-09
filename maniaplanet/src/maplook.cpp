#include "maplook.h"
#include "config.h"
#include "game.h"
#include "log.h"
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace tmvs_mp {
namespace maplook {
namespace {

// ManiaPlanet.exe (x64, the last build: 31,314,016 bytes), relative to its base. Found with the
// engine's member reflection tables (name, class id, offset) and its class registrations:
//   CTrackMania (0x24001000) <- CGameManiaPlanet <- CGameCtnApp (0x030D3000): its base
//     constructor keeps `this` in a global (kAppGlobal).
//   CGameCtnApp::RootMap (0x030D3005) at 0x378, ::Editor (0x030D300A) at 0x7C8.
//   The editor (CGameCtnEditorCommon 0x0310E000)::Challenge at 0x410. ::Editor (declared as a
//     CGameCtnEditor) also holds the other editors (MediaTracker 0x030B1000, of the same size,
//     items, meshes, action maker ...): only the map editor's classes count. The engine makes
//     CGameCtnEditorSimple, ::Puzzle or ::Free there (registrations 0x1400ED7BF/85F/80F,
//     Simple <- Puzzle <- Free <- Common; no other class derives from Common), and its own code
//     that sets ::Editor asks IsKindOf(CGameCtnEditorCommon) the same way.
//   CGameCtnChallenge (0x03043000)::Comments (0x03043003) at 0xE0, a wide string {wchar_t*, u32
//     length}, set through the engine's own assign (kWStringAssign: copies into its own buffer).
// Every object's class is checked through its vtable's GetMwClassId (slot 3:
// `mov dword [rdx], id; mov rax, rdx; ret`) before anything is read.
// The app global and the class checks: game.cpp.
constexpr uintptr_t kWStringAssign = 0x142E50;
constexpr size_t kRootMap = game::kAppRootMap, kEditor = game::kAppEditor, kEditorMap = 0x410, kComments = 0xE0;
constexpr uint32_t kChallengeId = 0x03043000;
// The map editor: CGameCtnEditorFree, ::Puzzle, ::Simple and their base CGameCtnEditorCommon.
constexpr uint32_t kMapEditorIds[] = {0x0310F000, 0x03110000, 0x03115000, 0x0310E000};
// The assign's first bytes (mov [rsp+10h], rbx; mov [rsp+18h], rbp; push rdi; sub rsp, 30h;
// movaps xmm0, [rdx]): another build has something else there and nothing is written.
constexpr unsigned char kAssignStart[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18,
                                          0x57, 0x48, 0x83, 0xEC, 0x30, 0x0F, 0x28, 0x02};

bool g_ready = false;
bool g_checked = false;

using game::app;
using game::classOf;
using game::readPointer;

bool setUp() {
    if (g_checked) return g_ready;
    g_checked = true;
    if (!game::attached()) return false;
    g_ready = game::codeIs(kWStringAssign, kAssignStart, sizeof(kAssignStart));
    TMVS_LOG("maplook: %s", g_ready ? "ManiaPlanet's map comments found (Author's Shaders)"
                                    : "not the ManiaPlanet build this was made for: no Author's Shaders");
    return g_ready;
}

// The map in the editor, else the one being played (0: none).
uintptr_t currentMap(bool* inEditor) {
    const uintptr_t a = app();
    if (inEditor) *inEditor = false;
    if (!a) return 0;
    const uintptr_t editor = readPointer(a + kEditor);
    bool mapEditor = false;
    if (editor) {
        const uint32_t id = classOf(editor);
        for (uint32_t ok : kMapEditorIds) mapEditor = mapEditor || id == ok;
    }
    // Another editor there (the MediaTracker, items ...): its 0x410 is something else; the map
    // being played, if any, is the one shown.
    if (mapEditor) {
        const uintptr_t map = readPointer(editor + kEditorMap);
        if (classOf(map) == kChallengeId) {
            if (inEditor) *inEditor = true;
            return map;
        }
    }
    const uintptr_t map = readPointer(a + kRootMap);
    return classOf(map) == kChallengeId ? map : 0;
}

// Copies the comments (at most `capacity - 1` characters); false when they can't be read.
bool readComments(uintptr_t map, wchar_t* out, uint32_t capacity, uint32_t* length) {
    __try {
        const wchar_t* text = *reinterpret_cast<const wchar_t* const*>(map + kComments);
        uint32_t n = *reinterpret_cast<const uint32_t*>(map + kComments + 8);
        if (n > 0x100000) return false;
        if (!text) n = 0;
        if (n >= capacity) n = capacity - 1;
        if (n) memcpy(out, text, n * sizeof(wchar_t));
        out[n] = 0;
        *length = n;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The engine's wide string as it hands one over: pointer, length, 0 (16 bytes, aligned: the
// assign loads it with movaps).
struct alignas(16) WideRef {
    const wchar_t* text;
    uint32_t length;
    uint32_t zero;
};
using AssignFn = void*(__fastcall*)(void* target, const WideRef* source);

bool writeComments(uintptr_t map, const wchar_t* text, uint32_t length) {
    __try {
        WideRef source{text, length, 0};
        reinterpret_cast<AssignFn>(game::base() + kWStringAssign)(reinterpret_cast<void*>(map + kComments), &source);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string toUtf8(const wchar_t* text, uint32_t length) {
    if (!length) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), &out[0], n, nullptr, nullptr);
    return out;
}

wchar_t g_text[16384];

bool commentsOf(uintptr_t map, std::string& comments) {
    uint32_t length = 0;
    if (!readComments(map, g_text, 16384, &length)) return false;
    comments = toUtf8(g_text, length);
    return true;
}

uintptr_t g_lastMap = 0;
std::string g_lastComments;
bool g_told = false;
int g_frame = 0;
int g_serial = 0;

} // namespace

void tick() {
    if (++g_frame % 15) return;
    if (!setUp()) return;
    bool inEditor = false;
    const uintptr_t map = currentMap(&inEditor);
    static bool appLogged = false;
    if (!map && !appLogged && g_frame > 15 * 120) {
        appLogged = true;
        TMVS_LOG("maplook: the game's app object %s", app() ? "found" : "not found (Author's Shaders can't be read)");
    }
    std::string comments;
    if (map && !commentsOf(map, comments)) return;
    if (g_told && map == g_lastMap && comments == g_lastComments) return;
    if (map != g_lastMap) g_serial++;
    if (map != g_lastMap || !g_told)
        TMVS_LOG("maplook: %s (%u characters of comments)", map ? (inEditor ? "map in the editor" : "map loaded") : "no map",
                 static_cast<unsigned>(comments.size()));
    g_lastMap = map;
    g_lastComments = comments;
    g_told = true;
    tmshaders::Config::get().onMapComments(comments);
}

int mapSerial() {
    return g_serial;
}

bool editorComments(std::string& comments) {
    if (!setUp()) return false;
    bool inEditor = false;
    const uintptr_t map = currentMap(&inEditor);
    return map && inEditor && commentsOf(map, comments);
}

bool setEditorComments(const std::string& comments) {
    if (!setUp()) return false;
    bool inEditor = false;
    const uintptr_t map = currentMap(&inEditor);
    if (!map || !inEditor) return false;
    const int n = MultiByteToWideChar(CP_UTF8, 0, comments.c_str(), static_cast<int>(comments.size()), nullptr, 0);
    std::wstring wide(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, comments.c_str(), static_cast<int>(comments.size()), &wide[0], n);
    if (!writeComments(map, wide.c_str(), static_cast<uint32_t>(wide.size()))) return false;
    TMVS_LOG("maplook: comments of the map in the editor set (%u characters)", static_cast<unsigned>(wide.size()));
    return true;
}

} // namespace maplook
} // namespace tmvs_mp
