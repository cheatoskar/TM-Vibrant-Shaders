#include "autoquality11.h"
#include "effects.h"
#include "log.h"
#include <cstring>

namespace tmvs_mp {

namespace {
constexpr double kWindow = 2.0; // seconds of frames per decision
constexpr double kSettle = 3.0; // after a change, let the new frame rate show

// What auto quality can turn down, in order: the most milliseconds for the least look first,
// the glowing neon (the look itself) last. `share` = the part of the passes' time the knob saves.
struct Knob {
    const char* name;
    const char* passes[4];
    float share;
};
constexpr Knob kKnobTable[AutoQuality::kKnobs] = {
    {"GI", {"GI", "GITemporal"}, 1.0f},
    {"fewer samples", {"OcclusionShadow", "Clouds", "GI"}, 0.3f},
    {"reflections", {"Reflect"}, 1.0f},
    {"volumetric clouds", {"Clouds"}, 1.0f},
    {"TAA", {"TAA"}, 1.0f},
    {"lowest samples", {"OcclusionShadow", "Clouds", "GI"}, 0.3f},
    {"depth of field, motion blur", {"Focus", "DofBlur", "Cinematic"}, 1.0f},
    {"light shafts", {"RayMask", "RayBlur"}, 1.0f},
    {"rain and snow", {"RainDrop", "RainSplash", "SnowFlake"}, 0.5f},
    {"contact shadows", {"OcclusionShadow"}, 0.4f},
    {"neon light", {"SpillDown", "SpillBlur"}, 1.0f},
};

void applyKnob(int knob, tmshaders::Settings& s) {
    switch (knob) {
        case 0: s.globalIllumination = 0.0f; break;
        case 1: if (s.quality > 1) s.quality = 1; break;
        case 2: s.reflections = 0.0f; break;
        case 3: s.volumetricClouds = 0.0f; break;
        case 4: s.taa = false; break;
        case 5: s.quality = 0; break;
        case 6: s.depthOfField = 0.0f; s.motionBlur = 0.0f; break;
        case 7: s.godRays = 0.0f; break;
        case 8: s.rain = s.rain > 1.0f ? 1.0f : s.rain; s.snow = s.snow > 1.0f ? 1.0f : s.snow; break;
        case 9: s.shadowStrength = 0.0f; break;
        case 10: s.neonLight = 0.0f; break;
        default: break;
    }
}

bool matters(int knob, const tmshaders::Settings& s) {
    switch (knob) {
        case 0: return s.globalIllumination > 0.0f;
        case 1: return s.quality > 1;
        case 2: return s.reflections > 0.0f;
        case 3: return s.volumetricClouds > 0.0f;
        case 4: return s.taa;
        case 5: return s.quality > 0;
        case 6: return s.depthOfField > 0.0f || s.motionBlur > 0.0f;
        case 7: return s.godRays > 0.0f;
        case 8: return s.rain > 1.0f || s.snow > 1.0f;
        case 9: return s.shadowStrength > 0.0f;
        case 10: return s.neonLight > 0.0f;
        default: return false;
    }
}
} // namespace

void AutoQuality::measure(const Profiler& profiler) {
    if (!m_passesKnown) {
        for (int k = 0; k < kKnobs; k++) {
            for (int i = 0; i < 4; i++) {
                m_passes[k][i] = -1;
                if (!kKnobTable[k].passes[i]) continue;
                for (int p = 0; p < kPassCount; p++)
                    if (!strcmp(passName(p), kKnobTable[k].passes[i])) m_passes[k][i] = p;
            }
        }
        m_passesKnown = true;
    }
    for (int k = 0; k < kKnobs; k++) {
        if (m_reduced[k]) continue;
        float ms = 0.0f;
        for (int i = 0; i < 4; i++)
            if (m_passes[k][i] >= 0) ms += profiler.time(m_passes[k][i]);
        m_cost[k] = ms * kKnobTable[k].share;
    }
}

void AutoQuality::reduce(int knob, double now) {
    m_reduced[knob] = true;
    m_order[m_reducedCount++] = knob;
    if (now - m_restoredAt[knob] < kSettle + kWindow * 3.0) m_blockUntil[knob] = now + 90.0;
}

void AutoQuality::restore(int knob, double now) {
    m_reduced[knob] = false;
    int j = 0;
    for (int i = 0; i < m_reducedCount; i++)
        if (m_order[i] != knob) m_order[j++] = m_order[i];
    m_reducedCount = j;
    m_restoredAt[knob] = now;
}

void AutoQuality::onFrame(double now, bool gameplay, const Profiler& profiler, const tmshaders::Settings& s) {
    if (!s.autoQuality || !s.enabled) {
        for (bool& r : m_reduced) r = false;
        m_reducedCount = 0;
        m_lastCutCount = 0;
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
        m_effectsSum += profiler.total();
    }
    if (m_windowStart < 0.0) m_windowStart = now;
    if (now - m_windowStart < kWindow) return;

    const bool valid = m_frames > 10 && m_gameplayFrames > m_frames / 2;
    if (valid) {
        m_fps = static_cast<float>(m_frames / m_frameTime);
        m_effectsMs = static_cast<float>(m_effectsSum / m_gameplayFrames);
    }
    m_windowStart = now;
    m_frames = 0;
    m_frameTime = 0.0;
    m_gameplayFrames = 0;
    m_effectsSum = 0.0;
    if (!valid || now - m_lastChange < kSettle || now < m_quietUntil) return;
    measure(profiler);

    const float target = s.targetFps;
    const float frameMs = 1000.0f / m_fps;
    const float targetMs = 1000.0f / target;
    const bool timed = m_effectsMs > 0.0f;

    // The last cut won far less than its passes cost: the effects aren't what holds the frame
    // rate back (the game, the CPU, vsync). Undo it, stop cutting for a while.
    if (m_lastCutCount > 0) {
        const float gained = m_frameMsBeforeCut - frameMs;
        const int count = m_lastCutCount;
        m_lastCutCount = 0;
        if (timed && m_expectedGain > 0.6f && gained < m_expectedGain * 0.25f) {
            for (int i = 0; i < count; i++) restore(m_lastCut[i], now);
            m_noCutUntil = now + 90.0;
            m_lastChange = now;
            TMVS_LOG("auto quality: turning effects off won %.1f of %.1f ms - the game or the CPU limits the frame rate, effects back on",
                     gained, m_expectedGain);
            return;
        }
    }

    if (m_fps < target * 0.95f && now > m_noCutUntil) {
        const float need = frameMs - targetMs * 0.97f;
        if (timed && need > m_effectsMs * 0.8f && m_effectsMs < frameMs * 0.3f) {
            m_noCutUntil = now + 30.0;
            for (int k = 0; k < kKnobs; k++)
                if (m_reduced[k]) restore(k, now);
            m_lastChange = now;
            TMVS_LOG("auto quality: %.0f FPS (target %.0f), %.1f ms missing but the effects take %.1f ms: the game limits the frame rate, nothing turned off",
                     m_fps, target, need, m_effectsMs);
            return;
        }
        float expected = 0.0f;
        m_lastCutCount = 0;
        for (int k = 0; k < kKnobs; k++) {
            if (m_reduced[k] || !matters(k, s) || (timed && m_cost[k] < 0.05f)) continue;
            reduce(k, now);
            m_lastCut[m_lastCutCount++] = k;
            expected += m_cost[k];
            if (!timed || expected >= need) break;
        }
        if (m_lastCutCount > 0) {
            m_expectedGain = expected;
            m_frameMsBeforeCut = frameMs;
            m_lastChange = now;
            TMVS_LOG("auto quality: %.0f FPS (target %.0f, effects %.1f ms, %.1f ms missing) -> off: %s", m_fps, target, m_effectsMs,
                     frameMs - targetMs, summary());
        }
    } else if (m_reducedCount > 0) {
        const float spare = targetMs - frameMs;
        for (int k = kKnobs - 1; k >= 0; k--) {
            if (!m_reduced[k] || now < m_blockUntil[k] || spare < m_cost[k] * 1.2f + targetMs * 0.03f) continue;
            restore(k, now);
            m_lastChange = now;
            TMVS_LOG("auto quality: %.0f FPS (target %.0f): %s back on (%.1f ms)", m_fps, target, kKnobTable[k].name, m_cost[k]);
            break;
        }
    }
}

void AutoQuality::apply(tmshaders::Settings& s) const {
    if (!s.autoQuality) return;
    for (int k = 0; k < kKnobs; k++)
        if (m_reduced[k]) applyKnob(k, s);
    if (m_reduced[6] && s.bokehSize > 8.0f) s.bokehSize = 8.0f;
}

const char* AutoQuality::summary() const {
    if (m_reducedCount == 0) return "your settings";
    m_summary[0] = 0;
    for (int k = 0; k < kKnobs; k++) {
        if (!m_reduced[k]) continue;
        if (m_summary[0]) strncat(m_summary, ", ", sizeof(m_summary) - strlen(m_summary) - 1);
        strncat(m_summary, kKnobTable[k].name, sizeof(m_summary) - strlen(m_summary) - 1);
    }
    return m_summary;
}

} // namespace tmvs_mp
