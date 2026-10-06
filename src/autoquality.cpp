#include "autoquality.h"
#include "log.h"

namespace tmshaders {

namespace {
constexpr double kWindow = 2.0;   // seconds of frames per decision
constexpr double kSettle = 4.0;   // after a change, let the new level show its frame rate
}

const char* AutoQuality::levelName(int level) {
    switch (level) {
        case 3: return "your settings";
        case 2: return "medium samples";
        case 1: return "lighter effects";
        default: return "lowest";
    }
}

void AutoQuality::onFrame(double now, bool gameplay, float pipelineMs, const Settings& s) {
    if (!s.autoQuality || !s.enabled) {
        m_level = kLevels - 1;
        m_windowStart = -1.0;
        m_lastFrame = -1.0;
        return;
    }
    if (m_lastFrame >= 0.0) {
        const double dt = now - m_lastFrame;
        // Loading screens and alt-tab stalls say nothing about the GPU.
        if (dt > 0.0 && dt < 0.25) {
            m_frames++;
            m_frameTime += dt;
        } else if (dt >= 0.25) {
            m_quietUntil = now + 4.0;
        }
    }
    m_lastFrame = now;
    if (gameplay && !m_wasGameplay) m_quietUntil = now + 8.0; // a map just started
    m_wasGameplay = gameplay;
    if (gameplay) {
        m_gameplayFrames++;
        m_pipelineSum += pipelineMs;
    }
    if (m_windowStart < 0.0) m_windowStart = now;
    if (now - m_windowStart < kWindow) return;

    const bool valid = m_frames > 10 && m_gameplayFrames > m_frames / 2;
    if (valid) {
        m_fps = static_cast<float>(m_frames / m_frameTime);
        m_pipelineMs = static_cast<float>(m_pipelineSum / m_gameplayFrames);
    }
    m_windowStart = now;
    m_frames = 0;
    m_frameTime = 0.0;
    m_gameplayFrames = 0;
    m_pipelineSum = 0.0;
    if (!valid || now - m_lastChange < kSettle || now < m_quietUntil) return;

    const float target = s.targetFps;
    const float frameMs = 1000.0f / m_fps;
    if (m_checkGain) {
        m_checkGain = false;
        if (m_fps < m_fpsBeforeDown * 1.04f) {
            m_level++;
            m_lastChange = now;
            m_blockDownUntil = now + 120.0;
            TMVS_LOG("auto quality: lower level gained nothing (%.0f -> %.0f FPS), back to level %d", m_fpsBeforeDown, m_fps, m_level);
            return;
        }
    }
    // Too slow, and the effects are a real part of the frame (if the game itself is the
    // bottleneck, turning effects down would cost looks and gain nothing).
    if (m_fps < target * 0.93f && m_level > 0 && now > m_blockDownUntil && (m_pipelineMs <= 0.0f || m_pipelineMs > frameMs * 0.12f)) {
        // Stepping up didn't hold: stay below for a while instead of flip-flopping.
        if (m_lastStepUp && now - m_lastChange < kSettle + kWindow * 2.0) m_blockUpUntil = now + 120.0;
        m_level--;
        m_lastChange = now;
        m_lastStepUp = false;
        m_checkGain = true;
        m_fpsBeforeDown = m_fps;
        TMVS_LOG("auto quality: %.0f FPS (target %.0f, effects %.1f ms) -> level %d", m_fps, target, m_pipelineMs, m_level);
    } else if (m_level < kLevels - 1 && now > m_blockUpUntil) {
        // Room to spare: the next level costs roughly a third more effect time.
        const float predictedMs = frameMs + m_pipelineMs * 0.35f;
        if (1000.0f / predictedMs > target * 1.05f) {
            m_level++;
            m_lastChange = now;
            m_lastStepUp = true;
            TMVS_LOG("auto quality: %.0f FPS (target %.0f, effects %.1f ms) -> level %d", m_fps, target, m_pipelineMs, m_level);
        }
    }
}

void AutoQuality::apply(Settings& s) const {
    if (m_level >= 3) return;
    // Level 2: fewer samples (AO, shadows, clouds, long shadows).
    if (s.quality > 1) s.quality = 1;
    if (m_level >= 2) return;
    // Level 1: drop the extras that cost the most for the least.
    s.quality = 0;
    s.longShadows = 0.0f;
    s.grassDetail = 0.0f;
    s.reflections = 0.0f;
    s.volumetricLight = 0.0f;
    if (s.bokehSize > 8.0f) s.bokehSize = 8.0f;
    if (m_level >= 1) return;
    // Level 0: the look stays, the expensive passes go.
    s.taa = false;
    s.neonLight = 0.0f; // the glowing borders are the look: they go last
    s.godRays = 0.0f;
    s.volumetricClouds = 0.0f;
    s.depthOfField = 0.0f;
    s.motionBlur = 0.0f;
    s.lensFlare = 0.0f;
    s.sharpen = 0.0f;
}

} // namespace tmshaders
