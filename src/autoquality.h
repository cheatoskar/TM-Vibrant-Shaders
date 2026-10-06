#pragma once
#include "settings.h"

namespace tmshaders {

// Adapts the effect quality to the GPU: watches the frame rate and the pipeline's own GPU
// time, and steps down (or back up) through a few levels until the target FPS holds.
// The levels only change what is rendered; your settings and presets stay as they are.
class AutoQuality {
public:
    static constexpr int kLevels = 4; // 3 = your settings ... 0 = cheapest

    // Once per presented frame. gameplay = a 3D scene was shaded this frame.
    void onFrame(double now, bool gameplay, float pipelineMs, const Settings& settings);
    // Writes the current level's reductions into a copy of the settings.
    void apply(Settings& s) const;

    int level() const { return m_level; }
    float fps() const { return m_fps; }
    float pipelineMs() const { return m_pipelineMs; }
    static const char* levelName(int level);

private:
    int m_level = kLevels - 1;
    double m_windowStart = -1.0;
    double m_lastFrame = -1.0;
    double m_lastChange = -100.0;
    double m_blockUpUntil = -1.0;
    int m_frames = 0;
    double m_frameTime = 0.0;
    int m_gameplayFrames = 0;
    double m_pipelineSum = 0.0;
    float m_fps = 0.0f;
    float m_pipelineMs = 0.0f;
    bool m_lastStepUp = false;
    // A step down that gains nothing means the GPU isn't the limit (vsync, CPU, the game
    // itself): undo it and stop trying for a while.
    bool m_checkGain = false;
    float m_fpsBeforeDown = 0.0f;
    double m_blockDownUntil = -1.0;
};

} // namespace tmshaders
