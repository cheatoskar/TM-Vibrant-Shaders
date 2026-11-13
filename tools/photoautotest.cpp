// Tests when photo mode comes on by itself with the free camera (src/photoauto.h): never on a server (the
// game's own switch to the free camera at the end of a round, while spectating, between two maps), yes in
// the replay editor, a solo race and a replay watched from the menu; F6 everywhere. Exit code 0 = all passed.
#include "photoauto.h"
#include <cstdio>

using namespace tmshaders::photo;

static int g_failed = 0;
#define CHECK(...) \
    do { \
        if (!(__VA_ARGS__)) { \
            printf("FAIL line %d: %s\n", __LINE__, #__VA_ARGS__); \
            g_failed++; \
        } \
    } while (0)

struct Clock {
    AutoGate gate;
    uint32_t t = 1000;
    // `ms` of frames (16 ms each) in one state; the answer of the last frame
    bool run(uint32_t ms, bool mapOpen, bool replayEditor, RaceKind race) {
        bool allowed = false;
        for (uint32_t done = 0; done < ms; done += 16, t += 16) allowed = gate.update(t, mapOpen, replayEditor, race);
        return allowed;
    }
};

int main() {
    // The report of 2026-11-13: a server, rounds; at the end of each round the game puts the player into the free
    // camera. The race is a network race all along: never allowed.
    {
        Clock c;
        CHECK(!c.run(3000, false, false, RaceKind::None)); // menu
        CHECK(!c.run(500, true, false, RaceKind::None));   // the server's map loads
        CHECK(!c.run(60000, true, false, RaceKind::Net));  // rounds, spectator, podium
        CHECK(c.gate.online());
        // the next map of the server: about a second without map and race (Horizon's log: "offline", "Main menu",
        // "online" again 0.1-1.3 s later), then a few seconds with the map but no race yet
        CHECK(!c.run(1300, false, false, RaceKind::None));
        CHECK(!c.run(3000, true, false, RaceKind::None)); // longer than kNoRaceMs: still online
        CHECK(c.gate.online());
        CHECK(!c.run(10000, true, false, RaceKind::Net));
        // the free camera at the end of the round: photo mode stays off, F6 still works
        CHECK(!wanted(-1, true, true, c.gate.update(c.t, true, false, RaceKind::Net)));
        CHECK(wanted(1, true, true, false));
        CHECK(!wanted(0, true, true, true));
        // an editor object in the app's editor slot on a server (Horizon Host reports "Editor" while online):
        // the network race still wins
        CHECK(!c.run(1000, true, true, RaceKind::Net));
    }
    // Leaving the server: the menu for a while, then the replay editor: allowed again at once.
    {
        Clock c;
        c.run(5000, true, false, RaceKind::Net);
        CHECK(!c.run(AutoGate::kMenuMs + 100, false, false, RaceKind::None));
        CHECK(!c.gate.online());
        CHECK(c.run(100, true, true, RaceKind::None));
    }
    // The replay editor right after a network race (no menu time): the editor alone says offline.
    {
        Clock c;
        c.run(5000, true, false, RaceKind::Net);
        CHECK(c.run(32, true, true, RaceKind::None));
        CHECK(!c.gate.online());
    }
    // A solo race (also its replay at the end) and the map editor's test drive: allowed.
    {
        Clock c;
        CHECK(c.run(100, true, false, RaceKind::Solo));
        CHECK(c.run(100, true, false, RaceKind::Other));
        CHECK(wanted(-1, true, true, true));
        CHECK(!wanted(-1, false, true, true)); // PhotoAuto off
        CHECK(!wanted(-1, true, false, true)); // no free camera
    }
    // A replay watched from the menu (a map, no race): allowed after kNoRaceMs, not before.
    {
        Clock c;
        c.run(2000, false, false, RaceKind::None);
        CHECK(!c.run(AutoGate::kNoRaceMs / 2, true, false, RaceKind::None));
        CHECK(c.run(AutoGate::kNoRaceMs, true, false, RaceKind::None));
    }
    // A race that can't be read (another build): treated as online; the replay editor still works.
    {
        Clock c;
        CHECK(!c.run(5000, true, false, RaceKind::Unknown));
        CHECK(!c.run(5000, true, false, RaceKind::None));
        CHECK(c.run(100, true, true, RaceKind::Unknown));
    }
    // No map: never.
    {
        Clock c;
        CHECK(!c.run(10000, false, false, RaceKind::None));
        CHECK(!c.run(100, false, true, RaceKind::Solo));
    }
    // The clock wraps (GetTickCount after 49.7 days).
    {
        Clock c;
        c.t = 0xffffff00u;
        CHECK(c.run(AutoGate::kNoRaceMs + 500, true, false, RaceKind::None));
    }
    printf(g_failed ? "photoautotest: %d failed\n" : "photoautotest: all passed\n", g_failed);
    return g_failed ? 1 : 0;
}
