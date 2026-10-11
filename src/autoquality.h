#pragma once
#include "settings.h"

namespace tmshaders {

class Pipeline;

// Adapts the effects to the GPU so the target FPS holds. The pipeline measures every pass
// on the GPU; when the frame is too slow, auto quality works out how many milliseconds are
// missing and turns off just enough effects - the ones that cost the most for the least
// look first - to win them back, all in one step. With time to spare it brings them back,
// one at a time, when the measured cost fits. Your settings and presets stay as they are:
// the reductions only go into the copy that is rendered.
class AutoQuality {
public:
    // Once per presented frame. gameplay = a 3D scene was shaded this frame.
    void onFrame(double now, bool gameplay, const Pipeline& pipeline, const Settings& settings);
    // Writes the current reductions into a copy of the settings.
    void apply(Settings& s) const;

    float fps() const { return m_fps; }
    float pipelineMs() const { return m_pipelineMs; }
    int reducedCount() const { return m_reducedCount; }
    // "your settings", or the effects that are off right now ("volumetric light, GI").
    const char* summary() const;

    static constexpr int kKnobs = 13;

private:
    bool isReduced(int knob) const { return m_reduced[knob]; }
    void measure(const Pipeline& pipeline);
    void reduce(int knob, double now);
    void restore(int knob, double now);

    bool m_reduced[kKnobs] = {};
    int m_order[kKnobs] = {};      // knobs in the order they were turned off (a stack)
    int m_reducedCount = 0;
    float m_cost[kKnobs] = {};     // measured ms while the knob was on
    double m_blockUntil[kKnobs] = {}; // brought back and too slow again: leave it off a while
    double m_restoredAt[kKnobs] = {};
    int m_passes[kKnobs][4] = {};  // pass indices per knob (-1 = none), looked up by name
    bool m_passesKnown = false;

    double m_windowStart = -1.0;
    double m_lastFrame = -1.0;
    double m_lastChange = -100.0;
    int m_frames = 0;
    double m_frameTime = 0.0;
    int m_gameplayFrames = 0;
    double m_pipelineSum = 0.0;
    float m_fps = 0.0f;
    float m_pipelineMs = 0.0f;
    // The last cut: what it should have won. Winning far less means the GPU effects aren't
    // the limit (the game or the CPU, vsync): undo it and stop cutting for a while.
    int m_lastCut[kKnobs] = {};
    int m_lastCutCount = 0;
    float m_expectedGain = 0.0f;
    float m_frameMsBeforeCut = 0.0f;
    double m_noCutUntil = -1.0;
    // Loading a map, the first frames and stalls (alt-tab) have low frame rates that say
    // nothing about the GPU: no decisions until this time.
    double m_quietUntil = -1.0;
    bool m_wasGameplay = false;
    mutable char m_summary[320] = {};
};

} // namespace tmshaders
