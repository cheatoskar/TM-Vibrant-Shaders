#pragma once
#include <cstdint>

// When photo mode may come on by itself with the free camera (PhotoAuto): only offline. On a server the
// game itself switches to the free camera (spectator, the end of a round), and photo mode came on every
// time. No Windows, no game here, so tools/photoautotest.cpp runs it on made-up sequences.
//
// What the engine tells (engine::raceKind, every frame): the race the game runs now. A network race (LAN or
// internet, also while spectating and on the podium) is online; the app keeps it through a whole map on a
// server. Between two maps of a server there is no race for about a second (Horizon's log: "offline", "Main
// menu", then "online" again 0.1-1.3 s later), so "online" holds until something says offline for sure: a solo
// race, any other race that is not a network race, the replay editor, or no map at all for kMenuMs (the menu).
// Auto photo mode then needs a positive offline sign: the replay editor, a race that is not a network race, or
// a map open without any race for kNoRaceMs (a replay watched from the menu). A race that can't be read
// (another build) counts as online: then only the replay editor brings photo mode on by itself.
namespace tmshaders {
namespace photo {

enum class RaceKind {
    None,    // no race (menu, a map loading, a replay watched, the map editor)
    Solo,    // a solo race (CTrackManiaRace1P and its subclasses)
    Net,     // a network race: LAN or internet (class name with "Net")
    Other,   // another race (the map editor's test drive, a class not known): offline
    Unknown, // not readable (another build): treated as online, so nothing comes on by itself
};

class AutoGate {
public:
    static constexpr uint32_t kMenuMs = 5000;   // no map this long: the menu, offline
    static constexpr uint32_t kNoRaceMs = 2000; // a map without a race this long: a replay or an editor

    // Once a frame. now: a millisecond clock (GetTickCount). Returns whether auto photo mode may follow the
    // free camera now.
    bool update(uint32_t now, bool mapOpen, bool replayEditor, RaceKind race) {
        if (!mapOpen) {
            if (!noMap_) {
                noMap_ = true;
                noMapSince_ = now;
            }
            if (now - noMapSince_ >= kMenuMs) online_ = false;
        } else {
            noMap_ = false;
        }
        if (race != RaceKind::None || !mapOpen) noRace_ = false;
        else if (!noRace_) {
            noRace_ = true;
            noRaceSince_ = now;
        }
        // A network race wins over everything (Horizon Host sees an editor object in the app's editor slot all
        // through an online session); the replay editor wins over a race that can't be read.
        if (race == RaceKind::Net) online_ = true;
        else if (replayEditor || race == RaceKind::Solo || race == RaceKind::Other) online_ = false;
        else if (race == RaceKind::Unknown) online_ = true;
        if (online_ || !mapOpen) return false;
        if (replayEditor || race == RaceKind::Solo || race == RaceKind::Other) return true;
        return noRace_ && now - noRaceSince_ >= kNoRaceMs;
    }
    bool online() const { return online_; }

private:
    bool online_ = false;
    bool noMap_ = false, noRace_ = false;
    uint32_t noMapSince_ = 0, noRaceSince_ = 0;
};

// The wish of the frame: F6 (manual: -1 follow the free camera, 0 off, 1 on) wins everywhere; else on with the
// free camera when PhotoAuto is on and the gate allows it.
inline bool wanted(int manual, bool photoAuto, bool freeCam, bool autoAllowed) {
    if (manual >= 0) return manual == 1;
    return photoAuto && freeCam && autoAllowed;
}

} // namespace photo
} // namespace tmshaders
