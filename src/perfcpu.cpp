#include "perfcpu.h"
#include <windows.h>

namespace tmshaders {
namespace perfcpu {

double Totals::modTotal() const {
    double sum = 0.0;
    for (double m : mod) sum += m;
    return sum;
}

double Totals::total() const {
    double sum = 0.0;
    for (double p : phase) sum += p;
    return sum;
}

void Tracker::addPhase(Phase p, int64_t from, int64_t to) {
    const double d = ms(to - from);
    if (d < 0.0 || d > kLongMs) {
        m_cycle.broken = true;
        return;
    }
    m_cycle.phase[static_cast<int>(p)] += d;
}

void Tracker::close() {
    const Cycle& c = m_cycle;
    if (c.presented && c.shaded && !c.broken) {
        Totals& t = m_totals;
        t.cycles++;
        for (int i = 0; i < static_cast<int>(Phase::Count); i++) {
            t.phase[i] += c.phase[i];
            t.mod[i] += c.mod[i];
        }
        t.blocked += c.blocked;
        if (c.presentCallSeen) {
            t.presentCall += c.presentCall;
            t.presentCalls++;
        }
        t.marks = t.marks || c.marks;
    }
    m_cycle = Cycle();
}

void Tracker::point(Point p, int64_t now) {
    // The phase that ends here, and the one that starts.
    Phase next = Phase::Logic;
    switch (p) {
    case Point::FrameBeginEnter: next = Phase::Begin; break;
    case Point::FrameBeginDone: next = Phase::Render; break;
    case Point::FrameEndEnter: next = Phase::End; break;
    case Point::FrameEndDone: next = Phase::Logic; break;
    }
    if (m_havePhase) addPhase(m_phase, m_phaseStart, now);
    if (p == Point::FrameBeginEnter) {
        // A new frame: the cycle before it (its rendering, its end, the logic after it) is done.
        // The first frame call ends the Present-to-Present cycles of before.
        if (m_marks) {
            close();
        } else {
            m_cycle = Cycle();
            m_marks = true;
        }
        m_cycle.marks = true;
    }
    m_phase = next;
    m_phaseStart = now;
    m_havePhase = true;
}

void Tracker::modEnter(int64_t now) {
    if (m_depth++ == 0) {
        m_modStart = now;
        m_modPhase = m_phase;
    }
}

void Tracker::modLeave(int64_t now) {
    if (m_depth <= 0) return;
    if (--m_depth == 0) {
        const double d = ms(now - m_modStart);
        if (d >= 0.0 && d <= kLongMs) m_cycle.mod[static_cast<int>(m_modPhase)] += d;
        else m_cycle.broken = true;
    }
}

void Tracker::blocked(double ms) {
    if (ms > 0.0) m_cycle.blocked += ms;
}

void Tracker::presentCall(double ms) {
    m_cycle.presentCall += ms;
    m_cycle.presentCallSeen = true;
}

void Tracker::present(int64_t now, bool shaded) {
    if (m_marks) {
        m_cycle.presented = true;
        m_cycle.shaded = m_cycle.shaded || shaded;
        return;
    }
    // No frame calls: Present to Present, all of it "logic".
    if (m_havePresent) {
        m_cycle.presented = true;
        m_cycle.shaded = shaded;
        addPhase(Phase::Logic, m_lastPresent, now);
        close();
    } else {
        m_cycle = Cycle();
    }
    m_lastPresent = now;
    m_havePresent = true;
}

namespace {
Tracker g_tracker;
double g_msPerTick = 0.0;
} // namespace

double msPerTick() {
    if (g_msPerTick == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_msPerTick = 1000.0 / static_cast<double>(f.QuadPart);
        g_tracker.setMsPerTick(g_msPerTick);
    }
    return g_msPerTick;
}

Tracker& tracker() {
    msPerTick();
    return g_tracker;
}

void blocked(double ms) {
    tracker().blocked(ms);
}

int64_t now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

} // namespace perfcpu
} // namespace tmshaders
