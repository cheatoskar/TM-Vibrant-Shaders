#include "clips/race.h"
#include "clips/clips.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace tmshaders {
namespace clips {
namespace {

constexpr uintptr_t kPreferredBase = 0x00400000;

struct Build {
    const char* name;
    uintptr_t theGame;        // CGameApp::s_TheGame
    uintptr_t getPlayground;  // CTrackMania::GetPlayground: mov eax, [ecx + 0x454]; ret
    uintptr_t playingInfo;    // CTrackManiaRace::GetPlayingPlayerInfo: mov eax, [ecx + 0x330]
    uintptr_t idGetString;    // const char* CMwId::GetString() const (engine.cpp's idGetString)
};
constexpr Build kBuilds[] = {
    {"Nations Forever", 0x00d68c44, 0x00485520, 0x0047bb00, 0x00935400},
    {"United Forever", 0x00d6a2a4, 0x00485350, 0x0047b900, 0x00935290},
};
const unsigned char kPlaygroundCode[] = {0x8b, 0x81, 0x54, 0x04, 0x00, 0x00, 0xc3};
const unsigned char kPlayingInfoCode[] = {0x8b, 0x81, 0x30, 0x03, 0x00, 0x00};

constexpr size_t kAppPlayground = 0x454;
constexpr size_t kAppChallenge = 0x198;
constexpr size_t kRacePlayingInfo = 0x330;
constexpr size_t kRaceClock = 0x274;
constexpr size_t kInfoPlayer = 0x238;
constexpr size_t kPlayerInfo = 0x1c;
constexpr size_t kInfoStart = 0x2ac;
constexpr size_t kInfoFinalTime = 0x2b0;
constexpr size_t kInfoFinished = 0x33c;
constexpr size_t kChallengeName = 0x108;
constexpr size_t kChallengeUid = 0xdc;
constexpr size_t kChallengeParameters = 0xb4; // CGameCtnChallengeParameters*
constexpr size_t kParametersAuthorTime = 0x20;
constexpr size_t kParametersGoldTime = 0x1c;

// The 2006 engine's games (TrackMania Nations ESWC; TM Sunrise and TM Original are the same
// generation): no CGameCtn* classes, the app is a CTrackMania : CConstructionGame. One community
// exe each, told by its link timestamp and image size; every object is told by its class id before
// it is read (each class's vtable slot 2, GetMwClassId, is `mov eax, <id>; ret`). The offsets come
// from the classes' reflection tables and the code that uses them (engine.cpp has the derivation for
// ESWC: kEswcGamePointer and on). Nothing is called, nothing written.
struct LegacyBuild {
    const char* name;
    const char* uidPrefix;      // the map's identifier: prefix and a hash of its name (no UID there)
    DWORD timestamp, imageSize; // the exe's link timestamp and size of image
    uintptr_t game;             // the app's global (CConstructionGame's constructor stores it)
    size_t gameNetwork, gameChallenge, gameRace;
    size_t networkInfos;        // the network's player infos (a buffer's data; [0] the local player's)
    size_t infoPlayerType, infoPlayer, playerInfo;
    size_t raceGame, raceLapTimes, raceClock;
    size_t infoState, infoStart, infoFinalTime, infoLapStart, infoLapFinalTime;
    size_t challengeName, challengeParameters; // CFastStringInt (size, text); CConstructionChallengeParameters*
};
constexpr LegacyBuild kLegacyBuilds[] = {
    // TmNationsESWC.exe 0.1.7.9 (unofficial 1.80 patch): engine.cpp's ESWC reads, the same values;
    // ChallengeParameters +0xd4 from CConstructionChallenge's reflection table.
    {"Nations ESWC", "eswc", 0x5ce7272f, 0x008e8000, 0x00caee50, 0x100, 0x11c, 0x140, 0x1a4, 0xd8, 0x180, 0x34, 0x40, 0x194, 0x33c, 0x25c, 0x1f0,
     0x1f4, 0x1fc, 0x200, 0x124, 0xd4},
    // TmSunrise.exe (Windows 10 repack) and TMORIGINAL.EXE (Online Patch v2): engine.cpp's
    // kSunriseEngine and kOriginalEngine (the same layout, the race's clock at another place);
    // ChallengeParameters +0xe4 from their reflection tables.
    {"Sunrise", "sunrise", 0x5cc8f0cf, 0x007de000, 0x00bcce50, 0x100, 0x10c, 0x12c, 0x188, 0xa4, 0x14c, 0x34, 0x3c, 0x184, 0x2d0, 0x228, 0x1bc,
     0x1c0, 0x1c8, 0x1cc, 0x128, 0xe4},
    {"Original", "original", 0x5d337477, 0x007d9000, 0x00bc8084, 0x100, 0x10c, 0x12c, 0x188, 0xa4, 0x14c, 0x34, 0x3c, 0x184, 0x2b8, 0x228, 0x1bc,
     0x1c0, 0x1c8, 0x1cc, 0x128, 0xe4},
};
constexpr DWORD kLegacyGameId = 0x24001000;       // CTrackMania
constexpr DWORD kLegacyChallengeId = 0x24003000;  // CConstructionChallenge
constexpr DWORD kLegacyParametersId = 0x2400c000; // CConstructionChallengeParameters
constexpr DWORD kLegacyInfoId = 0x24036000;       // CTrackManiaPlayerInfo
constexpr DWORD kLegacyPlayerId = 0x24031000;     // CTrackManiaPlayer
// Every class deriving from CTrackManiaRace (solo, turn based and network).
constexpr DWORD kLegacyRaceIds[] = {0x24014000, 0x24018000, 0x24079000, 0x24078000, 0x24055000,
                                    0x24042000, 0x24044000, 0x24056000, 0x24037000, 0x24045000};
constexpr DWORD kLegacyStateRunning = 1, kLegacyStateFinished = 2; // the info's RaceState (0 before the start)

uintptr_t rebase(uintptr_t link) {
    return link - kPreferredBase + reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

const LegacyBuild* findLegacyBuild() {
    static const LegacyBuild* s_build = [] {
        const BYTE* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        for (const LegacyBuild& b : kLegacyBuilds) {
            if (nt->FileHeader.TimeDateStamp == b.timestamp && nt->OptionalHeader.SizeOfImage == b.imageSize) return &b;
        }
        return static_cast<const LegacyBuild*>(nullptr);
    }();
    return s_build;
}

// The class id of a 2006 engine object (its vtable's GetMwClassId: B8 <id> C3), 0 none. Inside __try.
DWORD legacyClassId(const void* nod) {
    if (!nod) return 0;
    const uintptr_t* vtable = *static_cast<const uintptr_t* const*>(nod);
    const BYTE* code = reinterpret_cast<const BYTE*>(vtable[2]);
    if (code[0] != 0xb8 || code[5] != 0xc3) return 0;
    return *reinterpret_cast<const DWORD*>(code + 1);
}

template <typename T> T at(const void* p, size_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const BYTE*>(p) + offset);
}

// The app, when it is one (inside __try).
const void* legacyGame(const LegacyBuild& b) {
    const void* game = *reinterpret_cast<const void* const*>(rebase(b.game));
    return legacyClassId(game) == kLegacyGameId ? game : nullptr;
}

const void* legacyChallenge(const LegacyBuild& b) {
    const void* game = legacyGame(b);
    const void* challenge = game ? at<const void*>(game, b.gameChallenge) : nullptr;
    return legacyClassId(challenge) == kLegacyChallengeId ? challenge : nullptr;
}

// The race the local player drives: the app's current race, the network's first player info (a
// local one), its player pointing back at it. The times as engine.cpp's eswcRaceTimes reads them.
bool readLegacyRace(const LegacyBuild& b, RaceSample& out) {
    __try {
        const void* game = legacyGame(b);
        if (!game) return false;
        const void* race = at<const void*>(game, b.gameRace);
        const DWORD raceId = legacyClassId(race);
        bool isRace = false;
        for (DWORD id : kLegacyRaceIds) isRace = isRace || raceId == id;
        if (!isRace || at<const void*>(race, b.raceGame) != game) return false;
        const void* network = at<const void*>(game, b.gameNetwork);
        if (!network) return false;
        const void* const* infos = at<const void* const*>(network, b.networkInfos);
        const void* info = infos ? infos[0] : nullptr;
        if (legacyClassId(info) != kLegacyInfoId || at<int>(info, b.infoPlayerType) != 0) return false;
        const void* player = at<const void*>(info, b.infoPlayer);
        if (legacyClassId(player) != kLegacyPlayerId || at<const void*>(player, b.playerInfo) != info) return false;
        const bool lap = at<DWORD>(race, b.raceLapTimes) != 0;
        const DWORD state = at<DWORD>(info, b.infoState);
        const DWORD start = at<DWORD>(info, lap ? b.infoLapStart : b.infoStart);
        out.race = race;
        out.finished = state == kLegacyStateFinished;
        out.started = state == kLegacyStateRunning || out.finished;
        out.startClock = out.started ? start : 0;
        if (out.finished) out.time = at<int>(info, lap ? b.infoLapFinalTime : b.infoFinalTime);
        else if (out.started) out.time = static_cast<int>(at<DWORD>(race, b.raceClock) - start);
        else out.time = 0;
        if (out.time < 0 || out.time > 24 * 3600 * 1000) return false;
        out.valid = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readLegacyMapName(const LegacyBuild& b, wchar_t* out, size_t size) {
    __try {
        const void* challenge = legacyChallenge(b);
        if (!challenge) return false;
        const uint32_t length = at<uint32_t>(challenge, b.challengeName);
        const wchar_t* text = at<const wchar_t*>(challenge, b.challengeName + 4);
        if (!text || length == 0 || length >= size) return false;
        memcpy(out, text, length * sizeof(wchar_t));
        out[length] = L'\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readLegacyAuthorTime(const LegacyBuild& b, int& out) {
    __try {
        const void* challenge = legacyChallenge(b);
        const void* parameters = challenge ? at<const void*>(challenge, b.challengeParameters) : nullptr;
        if (legacyClassId(parameters) != kLegacyParametersId) return false;
        const DWORD author = at<DWORD>(parameters, 0x20), gold = at<DWORD>(parameters, 0x1c); // AuthorTime, GoldTime
        if (author == 0 || author > 24u * 3600 * 1000 || (gold != 0xffffffffu && gold < author)) return false;
        out = static_cast<int>(author);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool codeMatches(uintptr_t link, const unsigned char* bytes, size_t size) {
    __try {
        return memcmp(reinterpret_cast<const void*>(rebase(link)), bytes, size) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const Build* findBuild() {
    static const Build* s_build = [] {
        for (const Build& b : kBuilds) {
            if (codeMatches(b.getPlayground, kPlaygroundCode, sizeof(kPlaygroundCode)) &&
                codeMatches(b.playingInfo, kPlayingInfoCode, sizeof(kPlayingInfoCode)))
                return &b;
        }
        return static_cast<const Build*>(nullptr);
    }();
    return s_build;
}

const BYTE* app() {
    const Build* b = findBuild();
    if (!b) return nullptr;
    return *reinterpret_cast<const BYTE* const*>(rebase(b->theGame));
}

bool readRaceRaw(RaceSample& out) {
    __try {
        const BYTE* a = app();
        if (!a) return false;
        const BYTE* race = *reinterpret_cast<const BYTE* const*>(a + kAppPlayground);
        if (!race) return false;
        const BYTE* info = *reinterpret_cast<const BYTE* const*>(race + kRacePlayingInfo);
        if (!info) return false;
        const BYTE* player = *reinterpret_cast<const BYTE* const*>(info + kInfoPlayer);
        // Not a race (the editor's playground, a replay): the back pointers don't hold.
        if (!player || *reinterpret_cast<const BYTE* const*>(player + kPlayerInfo) != info) return false;
        const DWORD start = *reinterpret_cast<const DWORD*>(info + kInfoStart);
        out.race = race;
        out.finished = *reinterpret_cast<const DWORD*>(info + kInfoFinished) != 0;
        out.started = start != 0 || out.finished;
        out.startClock = start;
        if (out.finished) out.time = *reinterpret_cast<const int*>(info + kInfoFinalTime);
        else if (start != 0) out.time = static_cast<int>(*reinterpret_cast<const DWORD*>(race + kRaceClock) - start);
        else out.time = 0;
        if (out.time < 0 || out.time > 24 * 3600 * 1000) return false;
        out.valid = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readMapName(wchar_t* out, size_t size) {
    __try {
        const BYTE* a = app();
        if (!a) return false;
        const BYTE* challenge = *reinterpret_cast<const BYTE* const*>(a + kAppChallenge);
        if (!challenge) return false;
        const uint32_t length = *reinterpret_cast<const uint32_t*>(challenge + kChallengeName);
        const wchar_t* text = *reinterpret_cast<const wchar_t* const*>(challenge + kChallengeName + 4);
        if (!text || length == 0 || length >= size) return false;
        memcpy(out, text, length * sizeof(wchar_t));
        out[length] = L'\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readAuthorTime(int& out) {
    __try {
        const BYTE* a = app();
        if (!a) return false;
        const BYTE* challenge = *reinterpret_cast<const BYTE* const*>(a + kAppChallenge);
        if (!challenge) return false;
        const BYTE* parameters = *reinterpret_cast<const BYTE* const*>(challenge + kChallengeParameters);
        if (!parameters) return false;
        const DWORD author = *reinterpret_cast<const DWORD*>(parameters + kParametersAuthorTime);
        const DWORD gold = *reinterpret_cast<const DWORD*>(parameters + kParametersGoldTime);
        // A time (not -1, at most 24 h), the gold medal's not faster.
        if (author == 0 || author > 24u * 3600 * 1000 || (gold != 0xffffffffu && gold < author)) return false;
        out = static_cast<int>(author);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using IdGetStringFn = const char*(__fastcall*)(const void*, void*);

bool readMapUid(char* out, size_t size) {
    __try {
        const Build* b = findBuild();
        const BYTE* a = app();
        if (!b || !a) return false;
        const BYTE* challenge = *reinterpret_cast<const BYTE* const*>(a + kAppChallenge);
        if (!challenge) return false;
        const char* text = reinterpret_cast<IdGetStringFn>(rebase(b->idGetString))(challenge + kChallengeUid, nullptr);
        if (!text) return false;
        strncpy_s(out, size, text, _TRUNCATE);
        return out[0] != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The 2006 engine's maps carry no UID the clips could use: the prefix and a hash of the name (as
// engine.cpp's eswcMapUid, the same identifier the split times keep).
bool readLegacyMapUid(const LegacyBuild& b, char* out, size_t size) {
    wchar_t name[257] = {};
    if (!readLegacyMapName(b, name, 257)) return false;
    uint32_t hash = 2166136261u;
    for (const wchar_t* c = name; *c; c++) hash = (hash ^ static_cast<uint16_t>(*c)) * 16777619u;
    snprintf(out, size, "%s-%08x", b.uidPrefix, hash);
    return true;
}

} // namespace

const char* raceBuild() {
    if (const Build* b = findBuild()) return b->name;
    if (const LegacyBuild* l = findLegacyBuild()) return l->name;
    return nullptr;
}

bool readRace(RaceSample& out) {
    out = RaceSample();
    const LegacyBuild* legacy = findBuild() ? nullptr : findLegacyBuild();
    if (legacy ? !readLegacyRace(*legacy, out) : !readRaceRaw(out)) {
        out = RaceSample();
        return false;
    }
    return true;
}

std::string mapName() {
    wchar_t text[256] = {};
    const LegacyBuild* legacy = findBuild() ? nullptr : findLegacyBuild();
    if (legacy ? !readLegacyMapName(*legacy, text, 256) : !readMapName(text, 256)) return std::string();
    char narrow[768] = {};
    WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow, sizeof(narrow), nullptr, nullptr);
    return stripCodes(narrow);
}

int mapAuthorTime() {
    int ms = -1;
    const LegacyBuild* legacy = findBuild() ? nullptr : findLegacyBuild();
    return (legacy ? readLegacyAuthorTime(*legacy, ms) : readAuthorTime(ms)) ? ms : -1;
}

int RunLog::add(RunMarker run) {
    run.id = m_next++;
    m_runs.push_back(run);
    if (m_runs.size() > kKept) m_runs.erase(m_runs.begin(), m_runs.begin() + static_cast<long>(m_runs.size() - kKept));
    return run.id;
}

RunMarker* RunLog::find(int id) {
    for (RunMarker& r : m_runs) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

RunMarker* RunLog::last() {
    return m_runs.empty() ? nullptr : &m_runs.back();
}

InBuffer spanInBuffer(double from, double to, double oldest) {
    if (oldest < 0.0 || to <= oldest) return InBuffer::Gone;
    return from >= oldest - 0.25 ? InBuffer::Whole : InBuffer::Part;
}

std::string mapUid() {
    char text[64] = {};
    const LegacyBuild* legacy = findBuild() ? nullptr : findLegacyBuild();
    if (legacy ? !readLegacyMapUid(*legacy, text, sizeof(text)) : !readMapUid(text, sizeof(text))) return std::string();
    for (const char* c = text; *c; c++) {
        if (!isalnum(static_cast<unsigned char>(*c)) && *c != '_' && *c != '-') return std::string(); // not a UID
    }
    return text;
}

RunEvent RunTracker::update(double now, const RaceSample& s) {
    RunEvent e;
    if (!s.valid) {
        // No race (the menu, the editor, a replay, another map loading): what ran is over. One read
        // alone doesn't say so (the race's time can read out of range for a frame around the
        // finish): two in a row do.
        if (++m_invalid < 2) return e;
        m_running = m_finished = m_aborted = false;
        m_race = nullptr;
        m_startClock = 0;
        return e;
    }
    m_invalid = 0;
    if (!s.started) {
        // The countdown (a new run comes): a run that didn't finish was restarted.
        if (m_running && !m_finished) m_aborted = true;
        m_running = false;
        m_startClock = 0;
        return e;
    }
    const bool newRun = !m_running || s.race != m_race || s.startClock != m_startClock || (!m_finished && !s.finished && s.time + 500 < m_lastTime);
    if (newRun) {
        const bool restart = (m_running && !m_finished && s.race == m_race) || m_aborted;
        m_race = s.race;
        m_startClock = s.startClock;
        m_running = true;
        m_finished = s.finished;
        m_aborted = false;
        m_lastTime = s.time;
        m_runStart = now - s.time / 1000.0;
        if (s.finished) return e; // first seen finished: its start was never seen
        e.kind = RunEvent::Start;
        e.restart = restart;
        e.runStart = m_runStart;
        return e;
    }
    m_lastTime = s.time;
    if (m_finished) return e;
    if (s.finished) {
        m_finished = true;
        e.kind = RunEvent::Finish;
        e.runStart = m_runStart;
        e.finishAt = now;
        e.raceTime = s.time;
        return e;
    }
    // The earliest estimate is the closest (each frame sees the clock up to a frame late); a pause
    // stops the clock, the estimate stays where it was.
    m_runStart = std::min(m_runStart, now - s.time / 1000.0);
    return e;
}

std::string formatTime(int ms) {
    if (ms < 0) ms = 0;
    const int cs = (ms / 10) % 100, s = (ms / 1000) % 60, m = (ms / 60000) % 60, h = ms / 3600000;
    char text[32];
    if (h > 0) snprintf(text, sizeof(text), "%d:%02d:%02d.%02d", h, m, s, cs);
    else snprintf(text, sizeof(text), "%d:%02d.%02d", m, s, cs);
    return text;
}

std::string stripCodes(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] != '$') {
            out += text[i];
            continue;
        }
        if (i + 1 >= text.size()) break;
        const char c = text[i + 1];
        if (c == '$') {
            out += '$';
            i++;
        } else if (isxdigit(static_cast<unsigned char>(c))) {
            size_t n = 0;
            while (n < 3 && i + 1 + n < text.size() && isxdigit(static_cast<unsigned char>(text[i + 1 + n]))) n++;
            i += n;
        } else if ((c == 'l' || c == 'L' || c == 'h' || c == 'H' || c == 'p' || c == 'P') && i + 2 < text.size() && text[i + 2] == '[') {
            const size_t close = text.find(']', i + 2);
            i = close == std::string::npos ? text.size() : close;
        } else {
            i++;
        }
    }
    return out;
}

std::string fileSafe(const std::string& text) {
    std::string out;
    for (char c : stripCodes(text)) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 32 || strchr("<>:\"/\\|?*", c)) out += '_';
        else out += c;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    if (out.size() > 60) {
        size_t cut = 60;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) cut--; // not inside a UTF-8 character
        out.resize(cut);
    }
    return out.empty() ? std::string("Clip") : out;
}

std::wstring uniqueClipPath(const std::wstring& dir, const std::wstring& name, const std::function<bool(const std::wstring&)>& taken) {
    std::wstring path = dir + L"\\" + name + L".mp4";
    for (int n = 2; taken(path); n++) {
        if (n > 999) {
            // Everything taken (it never is): the time in ms makes it new.
            path = dir + L"\\" + name + L" (" + std::to_wstring(GetTickCount64()) + L").mp4";
            break;
        }
        path = dir + L"\\" + name + L" (" + std::to_wstring(n) + L").mp4";
    }
    return path;
}

size_t clipNameReserve() {
    return 6 + 9; // " (999)" ".part.mp4"
}

std::wstring fitName(const std::wstring& name, size_t most) {
    std::wstring out = name;
    if (out.size() > most) {
        size_t cut = most;
        if (cut > 0 && out[cut - 1] >= 0xD800 && out[cut - 1] <= 0xDBFF) cut--; // not half a surrogate pair
        out.resize(cut);
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
    return out.empty() ? std::wstring(L"Clip") : out;
}

// Checkpoint clips (clips.h): pure logic, also for tools/modulelogictest.cpp.
bool CheckpointTracker::update(const void* race, int passed, bool finished, int lastTime, SectorClip& out) {
    // Another race (a new map or mode), a restart (fewer passed), or no run: start over.
    if (race != m_race || passed < m_passed) {
        m_race = race;
        m_passed = passed;
        m_lastTime = passed > 0 ? lastTime : -1;
        return false;
    }
    if (passed == m_passed) return false;
    const int previous = m_lastTime;
    m_passed = passed;
    m_lastTime = lastTime;
    // The finish's sector is in the run's clip (Save on finish); a time not read yet is skipped.
    if (finished || lastTime <= 0) return false;
    out.index = passed;
    out.fromMs = previous;
    out.toMs = lastTime;
    return true;
}

} // namespace clips
} // namespace tmshaders
