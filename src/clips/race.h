#pragma once
#include <functional>
#include <string>
#include <vector>

// The race as the clips see it: read from the game's memory, never written, no hook into the game's
// code (TrackMania Nations / United Forever, both TmForever.exe builds). The same reads in TM Vibrant
// Shaders and in the standalone TM Clips.
//
// The path (Ghidra and the exe's reflection tables, TMNF names; TMUF found by matching code, the
// same offsets): CGameApp::s_TheGame (TMNF 0x00d68c44, TMUF 0x00d6a2a4) is the app (CTrackMania);
// CTrackMania::GetPlayground returns app +0x454 (code checked in both builds), which during a race is
// the CTrackManiaRace (CGameRace derives from CGamePlayground). The race's playing player's info
// (CTrackManiaRace::GetPlayingPlayerInfo: race +0x330) points back at its player (+0x238), whose
// info is at +0x1c: both are checked. The rest as in engine.cpp (raceTimes): the race's clock +0x274,
// the info's run start +0x2ac, finished +0x33c, final time +0x2b0. The map: app +0x198, its name
// (ChallengeName) +0x108, its UID's CMwId +0xdc (read with CMwId::GetString, a read-only call).
namespace tmshaders {
namespace clips {

struct RaceSample {
    bool valid = false;       // a race with its player was read
    const void* race = nullptr;
    unsigned startClock = 0;  // the race clock at the run's start (0: not started); a new value = a new run
    bool started = false;
    bool finished = false;
    int time = 0;             // ms: the run's time now, the final time once finished
};

// Which build was found ("Nations Forever", "United Forever"), null: another game (TM Nations ESWC,
// an unknown exe): no race, only the save key works.
const char* raceBuild();
bool readRace(RaceSample& out);
// The open map's name without its colour codes ("" none) and UID ("" none).
std::string mapName();
std::string mapUid();
// The open map's author time (ms), -1 none or not known (CGameCtnChallenge +0xb4, its
// ChallengeParameters, +0x20 AuthorTime: the reflection tables of both builds).
int mapAuthorTime();

// Runs from samples (pure logic, tools/clipstest.cpp). Times in seconds of the same clock.
struct RunEvent {
    enum Kind { None, Start, Finish } kind = None;
    bool restart = false;     // Start: the run before was not finished (a restart or a respawn to the start)
    double runStart = 0.0;    // when the run's clock was at 0
    double finishAt = 0.0;    // Finish: when the finish was seen
    int raceTime = 0;         // Finish: the final time (ms)
};

class RunTracker {
public:
    RunEvent update(double now, const RaceSample& s);
    bool running() const { return m_running && !m_finished; }
    double runStart() const { return m_runStart; }

private:
    const void* m_race = nullptr;
    unsigned m_startClock = 0;
    bool m_running = false;
    bool m_finished = false;
    bool m_aborted = false;   // a run went back to its countdown unfinished
    int m_lastTime = 0;
    int m_invalid = 0;        // reads in a row without a race
    double m_runStart = 0.0;
};

// The last finished runs, newest last (pure logic, tools/clipstest.cpp): what "Save last run" saves
// after a restart, a respawn or a new run, and what the Clips page lists, as long as the buffer
// still holds them.
struct RunMarker {
    int id = 0;
    double runStart = 0.0;    // seconds, the clips' clock
    double finishAt = 0.0;
    int raceTime = 0;         // ms
    bool pb = false;          // a personal best when it finished
    std::string map;
    bool saved = false;       // a save of it was asked (on finish or by hand)
};

class RunLog {
public:
    static constexpr size_t kKept = 5;
    // Keeps the newest kKept; returns the run's id.
    int add(RunMarker run);
    const std::vector<RunMarker>& runs() const { return m_runs; }
    RunMarker* find(int id);
    RunMarker* last();        // the newest finished run, null none

private:
    std::vector<RunMarker> m_runs;
    int m_next = 1;
};

// How much of a span [from, to] a buffer that holds pictures from `oldest` on still has (seconds;
// oldest < 0: an empty buffer). Whole: all of it (a quarter second of slack, the cut starts at a key
// frame); Part: its end, not its start; Gone: nothing.
enum class InBuffer { Whole, Part, Gone };
InBuffer spanInBuffer(double from, double to, double oldest);

// "0:42.37", "1:02:03.45" (ms).
std::string formatTime(int ms);
// A map name for a file: colour codes gone, characters Windows refuses replaced, at most 60.
std::string fileSafe(const std::string& text);
// TrackMania's formatting codes ($o, $fff, $l[...]) taken out.
std::string stripCodes(const std::string& text);
// A clip's path: dir\name.mp4, else dir\name (2).mp4 and on, the first one `taken` says no to.
std::wstring uniqueClipPath(const std::wstring& dir, const std::wstring& name, const std::function<bool(const std::wstring&)>& taken);
// What uniqueClipPath and the MP4 writer add to a name at most (" (999)" and ".part.mp4").
size_t clipNameReserve();
// `name` cut to at most `most` characters (not inside a surrogate pair, no space or dot at the end).
std::wstring fitName(const std::wstring& name, size_t most);

} // namespace clips
} // namespace tmshaders
