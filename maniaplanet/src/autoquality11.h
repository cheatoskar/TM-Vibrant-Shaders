#pragma once
// Auto quality for ManiaPlanet: the TMNF mod's (src/autoquality.cpp) on the effects' GPU times.
// A copy rather than shared code, so the TMNF build keeps its own exactly as it is; the knobs are
// ManiaPlanet's (no long shadows or FXAA here: the engine has its own). The same behaviour: it
// never turns off the light shafts or the neon light, nothing when even all it may cut couldn't
// reach the target, as few effects as win the missing time, each cut stays off a while and comes
// back only after a few calm windows (no loop of effects popping off and on).
#include "settings.h"

namespace tmvs_mp {

class Profiler;

class AutoQuality {
public:
    // Once per presented frame. gameplay = the scene effects ran this frame (a race, not a menu).
    void onFrame(double now, bool gameplay, const Profiler& profiler, const tmshaders::Settings& settings);
    // Writes the current reductions into a copy of the settings.
    void apply(tmshaders::Settings& s) const;

    float fps() const { return m_fps; }
    int reducedCount() const { return m_reducedCount; }
    // "your settings", or the effects that are off right now.
    const char* summary() const;

    static constexpr int kKnobs = 12;

private:
    void measure(const Profiler& profiler);
    void reduce(int knob, double now);
    void restore(int knob, double now);

    bool m_reduced[kKnobs] = {};
    int m_order[kKnobs] = {};
    int m_reducedCount = 0;
    float m_cost[kKnobs] = {};
    double m_blockUntil[kKnobs] = {};
    double m_restoredAt[kKnobs] = {};
    double m_cutAt[kKnobs] = {}; // when each knob was last turned off (it stays off a while)
    int m_passes[kKnobs][4] = {};
    bool m_passesKnown = false;

    double m_windowStart = -1.0;
    double m_lastFrame = -1.0;
    double m_lastChange = -100.0;
    int m_frames = 0;
    double m_frameTime = 0.0;
    int m_gameplayFrames = 0;
    double m_effectsSum = 0.0;
    float m_fps = 0.0f;
    float m_effectsMs = 0.0f;
    int m_lastCut[kKnobs] = {};
    int m_lastCutCount = 0;
    float m_expectedGain = 0.0f;
    float m_frameMsBeforeCut = 0.0f;
    double m_noCutUntil = -1.0;
    double m_quietUntil = -1.0;
    bool m_wasGameplay = false;
    // Decision windows in a row with time to spare, and the least spare among them: a knob comes
    // back only when it fits that.
    int m_calm = 0;
    float m_worstSpare = 0.0f;
    mutable char m_summary[320] = {};
};

} // namespace tmvs_mp
