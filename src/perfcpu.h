#pragma once
#include <cstdint>

// Where the game thread's time goes, frame by frame, for the perf line (perflog.cpp). The game
// runs everything on one thread (window messages, game logic, physics, rendering), so the time
// between two frames splits into phases told apart by the engine's frame calls:
//
//   ... RenderFrameEnd returns | logic | RenderFrameBegin | begin | rendering | RenderFrameEnd | end ...
//
// - logic: the game's own work between two frames (input, physics, game logic, menus).
// - begin: inside RenderFrameBegin. With a "& Half" Cpu/Gpu synchro the game waits there for the
//   GPU (CVisionViewportDx9::SyncGpuWaitQuery at the frame's start).
// - rendering: the cameras, the HUD; the mod's pipeline runs in here.
// - end: inside RenderFrameEnd: EndScene, Present (the mod's Present work, Horizon Host's) and,
//   with every other Cpu/Gpu synchro but "None", the wait until the GPU has finished the frame
//   (TMNF's default "Immediate" waits for the frame just sent).
//
// The mod's own time is measured in scopes around its callbacks (nested scopes count once) and
// booked to the phase it ran in; "blocked" is the part of it spent waiting for the GPU (a lock of a
// read-back). A cycle runs from one RenderFrameBegin to the next and counts when its Present was a
// shaded frame. Without the engine's frame calls (no engine hooks) a cycle runs from Present to
// Present and all of it is "logic".
//
// Times are QueryPerformanceCounter ticks; everything runs on the game's thread.
namespace tmshaders {
namespace perfcpu {

enum class Point { FrameBeginEnter, FrameBeginDone, FrameEndEnter, FrameEndDone };
enum class Phase { Logic = 0, Begin, Render, End, Count };

// Sums over the counted cycles, ms.
struct Totals {
    unsigned cycles = 0;
    double phase[static_cast<int>(Phase::Count)] = {}; // wall time of each phase
    double mod[static_cast<int>(Phase::Count)] = {};   // the mod's scopes in each phase
    double blocked = 0.0;                              // of the mod's time: waiting for the GPU
    double presentCall = 0.0;                          // the driver's Present (where it is seen)
    unsigned presentCalls = 0;                         // cycles that saw it
    bool marks = false;                                // a cycle had the engine's frame calls

    double modTotal() const;
    double total() const; // every phase
};

class Tracker {
public:
    // A segment longer than this (a loading screen, the game in the background) drops its cycle.
    static constexpr double kLongMs = 250.0;

    explicit Tracker(double msPerTick = 1.0) : m_msPerTick(msPerTick) {}
    void setMsPerTick(double msPerTick) { m_msPerTick = msPerTick; }

    void point(Point p, int64_t now);
    // The mod's scopes (nested: only the outermost counts).
    void modEnter(int64_t now);
    void modLeave(int64_t now);
    void blocked(double ms);
    void presentCall(double ms);
    // At Present (once a frame). `shaded`: the cycle it is in counts.
    void present(int64_t now, bool shaded);

    Phase phase() const { return m_phase; }
    const Totals& totals() const { return m_totals; }
    void resetTotals() { m_totals = Totals(); }

private:
    struct Cycle {
        double phase[static_cast<int>(Phase::Count)] = {};
        double mod[static_cast<int>(Phase::Count)] = {};
        double blocked = 0.0, presentCall = 0.0;
        bool presentCallSeen = false;
        bool presented = false, shaded = false, marks = false, broken = false;
    };
    void addPhase(Phase p, int64_t from, int64_t to);
    void close();
    double ms(int64_t ticks) const { return static_cast<double>(ticks) * m_msPerTick; }

    double m_msPerTick;
    Phase m_phase = Phase::Logic;
    int64_t m_phaseStart = 0;
    bool m_havePhase = false;  // m_phaseStart is set
    bool m_marks = false;      // the engine's frame calls were seen (then cycles close at FrameBeginEnter)
    int m_depth = 0;
    int64_t m_modStart = 0;
    Phase m_modPhase = Phase::Logic;
    int64_t m_lastPresent = 0;
    bool m_havePresent = false;
    Cycle m_cycle;
    Totals m_totals;
};

// The game thread's tracker (perflog drives it; set its clock with setMsPerTick).
Tracker& tracker();
// Waiting for the GPU inside the mod (a lock that wasn't ready), from code that has no perflog.
void blocked(double ms);
// QueryPerformanceCounter now, and its ms per tick.
int64_t now();
double msPerTick();

// RAII: time inside the mod's own code on the game thread.
struct ModScope {
    ModScope() { tracker().modEnter(now()); }
    ~ModScope() { tracker().modLeave(now()); }
    ModScope(const ModScope&) = delete;
    ModScope& operator=(const ModScope&) = delete;
};

// RAII: a wait for the GPU (counted as blocked; inside a ModScope it is part of the mod's time).
struct BlockedScope {
    int64_t start;
    BlockedScope() : start(now()) {}
    ~BlockedScope() { blocked(static_cast<double>(now() - start) * msPerTick()); }
    BlockedScope(const BlockedScope&) = delete;
    BlockedScope& operator=(const BlockedScope&) = delete;
};

} // namespace perfcpu
} // namespace tmshaders
