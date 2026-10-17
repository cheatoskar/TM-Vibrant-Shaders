// Tests the hotkey table (src/keys.cpp): every key name parses back to its key, the ini round trip,
// conflicts swap, plain letters are refused. Exit code 0 = all passed.
#include "keys.h"
#include "log.h"
#include <cstdio>
#include <string>
#include <windows.h>

using namespace tmshaders::keys;

static int g_failed = 0;
#define CHECK(...) \
    do { \
        if (!(__VA_ARGS__)) { \
            printf("FAIL line %d: %s\n", __LINE__, #__VA_ARGS__); \
            g_failed++; \
        } \
    } while (0)

int main() {
    tmshaders::log::setFileName(L"tmvs_keystest.log"); // never the game's log
    // Every nameable key with every modifier combination: name -> parse gives the same binding.
    int count = 0;
    for (int i = 0; unsigned vk = nameableKey(i); i++) {
        for (unsigned mods = 0; mods < 8; mods++) {
            Binding b{vk, mods}, back;
            const std::string text = name(b);
            if (!parse(text.c_str(), back) || back != b) {
                printf("FAIL round trip %u/%u as \"%s\"\n", vk, mods, text.c_str());
                g_failed++;
            }
            count++;
        }
    }
    printf("%d key/modifier combinations round-tripped\n", count);
    CHECK(count > 8 * 100);

    Binding b;
    CHECK(parse("shift + f6", b) && b == Binding{VK_F6, kShift});
    CHECK(parse("CONTROL+Alt+k", b) && b == Binding{'K', kCtrl | kAlt});
    CHECK(parse("Del", b) && b == Binding{VK_DELETE, 0});
    CHECK(parse("None", b) && b.vk == 0);
    CHECK(parse("", b) && b.vk == 0);
    CHECK(!parse("F25", b));
    CHECK(!parse("Shift+", b));
    CHECK(!parse("Banana+F1", b));
    CHECK(!parse("F1+F2", b));
    CHECK(name(Binding{VK_F8, 0}) == "F8");
    CHECK(name(Binding{VK_F6, kShift}) == "Shift+F6");

    // Defaults (this build is TMNF unless TMVS_MANIAPLANET).
    resetDefaults();
    takeChanged();
    CHECK(get(Action::Menu) == Binding{VK_F8, 0});
    CHECK(get(Action::Photo) == Binding{VK_F6, 0});
    CHECK(get(Action::PhotoSplit) == Binding({VK_F6, kShift}));
    CHECK(get(Action::Capture) == Binding{VK_F12, 0});
#ifdef TMVS_MANIAPLANET
    CHECK(get(Action::Effects) == Binding{VK_F5, 0});
#else
    CHECK(get(Action::Effects) == Binding{VK_F7, 0});
    CHECK(get(Action::Reload) == Binding{VK_F9, kCtrl});
    CHECK(name(get(Action::Reload)) == "Ctrl+F9");
    CHECK(get(Action::Trace) == Binding{VK_F11, 0});
#endif

    // Plain letters would eat the game's keys.
    CHECK(!assign(Action::Menu, Binding{'W', 0}));
    CHECK(!note().empty());
    CHECK(get(Action::Menu) == Binding{VK_F8, 0});
    CHECK(assign(Action::Menu, Binding{'M', kCtrl}));
    // A key another action has is swapped.
    CHECK(assign(Action::Menu, Binding{VK_F12, 0}));
    CHECK(get(Action::Capture) == Binding{'M', kCtrl});
    CHECK(!note().empty());

    // ini round trip.
    wchar_t dir[MAX_PATH], ini[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    swprintf(ini, MAX_PATH, L"%skeystest.ini", dir);
    DeleteFileW(ini);
    save(ini);
    resetDefaults();
    CHECK(get(Action::Menu) == Binding{VK_F8, 0});
    load(ini);
    CHECK(get(Action::Menu) == Binding{VK_F12, 0});
    CHECK(get(Action::Capture) == Binding{'M', kCtrl});
    // Hand-edited: garbage falls back to the default, a doubled key goes back to its default.
    WritePrivateProfileStringW(L"Keys", L"Menu", L"Banana", ini);
    WritePrivateProfileStringW(L"Keys", L"Photo", L"F12", ini);
    WritePrivateProfileStringW(L"Keys", L"Capture", L"F12", ini);
    load(ini);
    CHECK(get(Action::Menu) == Binding{VK_F8, 0});
    CHECK(get(Action::Photo) == Binding{VK_F12, 0});
    CHECK(get(Action::Capture) == Binding{VK_F12, 0} || get(Action::Capture) == Binding{});
    CHECK(get(Action::Photo) != get(Action::Capture));
    DeleteFileW(ini);

#ifndef TMVS_MANIAPLANET
    // Migration: a [Keys] section from before (no KeysVersion) with Reload=F9 moves to Ctrl+F9, once.
    WritePrivateProfileStringW(L"Keys", L"Reload", L"F9", ini);
    WritePrivateProfileStringW(L"Keys", L"Menu", L"F8", ini);
    load(ini);
    CHECK(get(Action::Reload) == Binding{VK_F9, kCtrl});
    CHECK(takeChanged()); // so it gets written back
    save(ini);
    wchar_t text[32] = {};
    GetPrivateProfileStringW(L"Keys", L"Reload", L"", text, 32, ini);
    CHECK(wcscmp(text, L"Ctrl+F9") == 0);
    CHECK(GetPrivateProfileIntW(L"Keys", L"KeysVersion", 0, ini) == 2);
    // Written by this version: a deliberate bare F9 stays.
    WritePrivateProfileStringW(L"Keys", L"Reload", L"F9", ini);
    load(ini);
    CHECK(get(Action::Reload) == Binding{VK_F9, 0});
    // Old file with another Reload key: untouched. No [Keys] at all: the default.
    DeleteFileW(ini);
    WritePrivateProfileStringW(L"Keys", L"Reload", L"F10", ini);
    load(ini);
    CHECK(get(Action::Reload) == Binding{VK_F10, 0});
    DeleteFileW(ini);
    load(ini);
    CHECK(get(Action::Reload) == Binding{VK_F9, kCtrl});
    // CenterSpeed loaded: the default F8 menu moves to F5 once, stamped; an F8 chosen afterwards
    // stays; CenterSpeed gone: back to F8, stamp cleared. A key the player chose is never touched.
    DeleteFileW(ini);
    load(ini);
    takeChanged();
    centerSpeed(false);
    CHECK(get(Action::Menu) == Binding{VK_F8, 0} && !takeChanged());
    centerSpeed(true);
    CHECK(get(Action::Menu) == Binding{VK_F5, 0} && takeChanged());
    save(ini);
    CHECK(GetPrivateProfileIntW(L"Keys", L"MenuMovedForCenterSpeed", 0, ini) == 1);
    load(ini);
    CHECK(get(Action::Menu) == Binding{VK_F5, 0});
    CHECK(assign(Action::Menu, Binding{VK_F8, 0}));
    save(ini);
    load(ini);
    centerSpeed(true);
    CHECK(get(Action::Menu) == Binding{VK_F8, 0});
    assign(Action::Menu, Binding{VK_F5, 0});
    centerSpeed(false);
    CHECK(get(Action::Menu) == Binding{VK_F8, 0});
    save(ini);
    CHECK(GetPrivateProfileIntW(L"Keys", L"MenuMovedForCenterSpeed", 1, ini) == 0);
    DeleteFileW(ini);
    WritePrivateProfileStringW(L"Keys", L"Menu", L"Insert", ini);
    load(ini);
    centerSpeed(true);
    CHECK(get(Action::Menu) == Binding{VK_INSERT, 0});
    DeleteFileW(ini);
    load(ini);
    takeChanged();
    // Hotkeys only react while the game window is in front: with a window that isn't, nothing fires.
    setGameWindow(CreateWindowW(L"STATIC", L"x", 0, 0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr));
    CHECK(!gameHasFocus());
    CHECK(!edge(Action::Reload));
    setGameWindow(nullptr);
    CHECK(gameHasFocus());
#endif
    DeleteFileW(ini);

    printf(g_failed ? "%d FAILED\n" : "all passed\n", g_failed);
    return g_failed ? 1 : 0;
}
