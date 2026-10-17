#include "autoquality.h"
#include "log.h"
#include "pipeline.h"
#include <cstring>

namespace tmshaders {

namespace {
constexpr double kWindow = 2.0;   // seconds of frames per decision
constexpr double kSettle = 3.0;   // after a change, let the new frame rate show
// Against cutting and restoring in a loop (the owner, target 61: at 57 FPS six effects off,
// one back every 4 - 6 s, all off again 20 s later, the long shadows popping in front of the
// car): a cut stays at least kHold, a knob comes back only after kCalmWindows windows in a row
// with room for it (the tightest of them counts), and one cut again within kBounce of coming
// back stays off for kBlocked.
constexpr double kHold = 20.0;
constexpr int kCalmWindows = 3;
constexpr double kBounce = 60.0;
constexpr double kBlocked = 300.0;

// What auto quality can turn down, in order: the most milliseconds for the least look
// first. The light shafts and the glowing neon (the look itself) keep their places but are
// never turned off (keepsLook). `share` = the part of the passes' time
// the knob saves (fewer samples make the same passes cheaper, long shadows are a part of
// the AO/shadow pass); `whole` = passes the knob saves entirely. Exact pass names: the
// lighting and spill passes have engine variants (SpillDownEngine), each listed.
struct Knob {
    const char* name;
    const char* passes[4];
    float share;
    const char* whole[4];
};
constexpr Knob kKnobTable[AutoQuality::kKnobs] = {
    {"volumetric light", {"Volumetric", "ShadowHeight"}, 1.0f},
    {"GI", {"GI", "GITemporal"}, 1.0f},
    {"fewer samples", {"OcclusionShadow", "Clouds", "Volumetric", "GI"}, 0.3f},
    {"long shadows", {"OcclusionShadow", "HeightSplat", "HeightMerge"}, 0.4f, {"LightGridInit", "LightGridSweep", "Casters"}},
    {"reflections, grass detail", {"Reflect"}, 1.0f},
    {"volumetric clouds", {"Clouds"}, 1.0f},
    {"TAA", {"TAA"}, 1.0f},
    {"FXAA", {"FXAA"}, 1.0f},
    {"lowest samples", {"OcclusionShadow", "Clouds", "Volumetric", "GI"}, 0.3f},
    {"depth of field, motion blur", {"Focus", "DofBlur", "Cinematic"}, 1.0f},
    {"light shafts", {"RayMask", "RayBlur"}, 1.0f},
    {"lens flare, sharpening", {"Sharpen"}, 1.0f},
    {"neon light", {"SpillDown", "SpillDownEngine", "SpillBlur"}, 1.0f},
};

void applyKnob(int knob, Settings& s) {
    switch (knob) {
        case 0: s.volumetricLight = 0.0f; break;
        case 1: s.globalIllumination = 0.0f; break;
        case 2: if (s.quality > 1) s.quality = 1; break;
        case 3: s.longShadows = 0.0f; break;
        case 4: s.reflections = 0.0f; s.grassDetail = 0.0f; break;
        case 5: s.volumetricClouds = 0.0f; break;
        case 6: s.taa = false; break;
        case 7: s.fxaa = false; break;
        case 8: s.quality = 0; break;
        case 9: s.depthOfField = 0.0f; s.motionBlur = 0.0f; break;
        case 10: s.godRays = 0.0f; break;
        case 11: s.lensFlare = 0.0f; s.sharpen = 0.0f; break;
        case 12: s.neonLight = 0.0f; break;
        default: break;
    }
}
// Whether turning the knob down changes anything with these settings (an effect that is
// already off saves nothing).
bool matters(int knob, const Settings& s) {
    switch (knob) {
        case 0: return s.volumetricLight > 0.0f;
        case 1: return s.globalIllumination > 0.0f;
        case 2: return s.quality > 1;
        case 3: return s.longShadows > 0.0f;
        case 4: return s.reflections > 0.0f || s.grassDetail > 0.0f;
        case 5: return s.volumetricClouds > 0.0f;
        case 6: return s.taa;
        case 7: return s.fxaa;
        case 8: return s.quality > 0;
        case 9: return s.depthOfField > 0.0f || s.motionBlur > 0.0f;
        case 10: return s.godRays > 0.0f;
        case 11: return s.lensFlare > 0.0f || s.sharpen > 0.0f;
        case 12: return s.neonLight > 0.0f;
        default: return false;
    }
}
// The light shafts and the neon light are the look itself: auto quality never turns them off.
// They cost little (0.2 - 0.4 ms), and cut for a fraction of a millisecond (or for a target no
// cut could reach: 240 FPS on a 144 Hz game) the presets lost what they are picked for.
bool keepsLook(int knob) {
    return knob == 10 || knob == 12;
}
// The order cuts go in (restores in reverse): the table's, but the long shadows late, after
// TAA, FXAA and the lowest samples. Turned off and on they pop: a stand's shadow on the road
// goes bright, and dark again as the car drives up to it. The look knobs aren't in it.
constexpr int kCutOrder[] = {0, 1, 2, 4, 5, 6, 7, 8, 9, 3, 11};
constexpr int kCutKnobs = static_cast<int>(sizeof(kCutOrder) / sizeof(kCutOrder[0]));
} // namespace

void AutoQuality::measure(const Pipeline& pipeline) {
    if (!m_passesKnown) {
        for (int k = 0; k < kKnobs; k++) {
            for (int i = 0; i < 8; i++) {
                m_passes[k][i] = -1;
                const char* name = i < 4 ? kKnobTable[k].passes[i] : kKnobTable[k].whole[i - 4];
                if (!name) continue;
                for (int p = 0; p < pipeline.passCount(); p++) {
                    if (!strcmp(Pipeline::passName(p), name)) m_passes[k][i] = p;
                }
            }
        }
        m_passesKnown = true;
    }
    // A knob's cost is known while it is on; once off, its last measurement stands.
    for (int k = 0; k < kKnobs; k++) {
        if (m_reduced[k]) continue;
        float ms = 0.0f, whole = 0.0f;
        for (int i = 0; i < 8; i++) {
            if (m_passes[k][i] >= 0) (i < 4 ? ms : whole) += pipeline.passTime(m_passes[k][i]);
        }
        m_cost[k] = ms * kKnobTable[k].share + whole;
    }
}

void AutoQuality::reduce(int knob, double now) {
    m_reduced[knob] = true;
    m_order[m_reducedCount++] = knob;
    m_cutAt[knob] = now;
    // Brought back not long ago and too slow again: leave it off for a while.
    if (m_restoredAt[knob] > 0.0 && now - m_restoredAt[knob] < kBounce) {
        m_blockUntil[knob] = now + kBlocked;
        TMVS_LOG("auto quality: %s came back %.0f s ago and the frame rate dropped again: it stays off for %.0f s", kKnobTable[knob].name,
                 now - m_restoredAt[knob], kBlocked);
    }
}

void AutoQuality::restore(int knob, double now) {
    m_reduced[knob] = false;
    int j = 0;
    for (int i = 0; i < m_reducedCount; i++) {
        if (m_order[i] != knob) m_order[j++] = m_order[i];
    }
    m_reducedCount = j;
    m_restoredAt[knob] = now;
}

void AutoQuality::onFrame(double now, bool gameplay, const Pipeline& pipeline, const Settings& s) {
    if (!s.autoQuality || !s.enabled) {
        for (bool& r : m_reduced) r = false;
        m_reducedCount = 0;
        m_lastCutCount = 0;
        m_calm = 0;
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
        m_pipelineSum += pipeline.totalTime();
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
    measure(pipeline);

    const float target = s.targetFps;
    const float frameMs = 1000.0f / m_fps;
    const float targetMs = 1000.0f / target;
    const bool timed = m_pipelineMs > 0.0f; // GPU timestamps work on this driver

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
        m_calm = 0;
        // Turn off just enough (by the measured cost) to win back the missing time, with a
        // little margin, in one step.
        const float need = frameMs - targetMs * 0.97f;
        // More missing than all the effects cost together, and the effects a small part of
        // the frame: the game or the CPU holds the frame rate (TMUF Island: 40 FPS, effects
        // 4.6 of 25 ms, 8.2 ms missing). Cutting would turn everything down to the fewest
        // samples (striped, grainy shadows and reflections) and still not reach the target.
        // (A weak GPU, where the effects are a large part, still gets its cuts.)
        if (timed && need > m_pipelineMs * 0.8f && m_pipelineMs < frameMs * 0.3f) {
            m_noCutUntil = now + 30.0;
            // What an earlier cut turned off comes back: it didn't help either.
            for (int k = 0; k < kKnobs; k++) {
                if (m_reduced[k]) restore(k, now);
            }
            m_lastChange = now;
            TMVS_LOG("auto quality: %.0f FPS (target %.0f), %.1f ms missing but the effects take %.1f ms: the game limits the frame rate, nothing turned off",
                     m_fps, target, need, m_pipelineMs);
            return;
        }
        // Everything it may still turn off wouldn't win the missing time either (a target the
        // GPU can't reach with these effects at all: 83 FPS, target 240, 7.8 ms missing, the
        // effects 5.6 ms): cutting all of it would leave the plainest look and still miss.
        if (timed) {
            float available = 0.0f;
            for (int k = 0; k < kKnobs; k++) {
                if (!m_reduced[k] && !keepsLook(k) && matters(k, s) && m_cost[k] >= 0.05f) available += m_cost[k];
            }
            if (need > available) {
                m_noCutUntil = now + 30.0;
                m_lastChange = now;
                TMVS_LOG("auto quality: %.0f FPS (target %.0f), %.1f ms missing but turning effects off could win only %.1f ms: target out of reach, nothing (more) turned off",
                         m_fps, target, need, available);
                return;
            }
        }
        auto candidate = [&](int k) { return !m_reduced[k] && matters(k, s) && !(timed && m_cost[k] < 0.05f); };
        int picks[kCutKnobs] = {};
        int count = 0;
        float expected = 0.0f;
        // One knob that wins it all alone, the first in the order (the least look): 1.3 ms
        // missing turned off five effects in a row when the reflections alone were 1.5 ms.
        for (int i = 0; i < kCutKnobs && timed; i++) {
            const int k = kCutOrder[i];
            if (!candidate(k) || m_cost[k] < need) continue;
            picks[count++] = k;
            expected = m_cost[k];
            break;
        }
        // Else in order until the sum wins it (without timings: one at a time), then the ones
        // it can do without go back out (from the back: the earlier ones cost the least look).
        if (count == 0) {
            for (int i = 0; i < kCutKnobs && (count == 0 || (timed && expected < need)); i++) {
                if (!candidate(kCutOrder[i])) continue;
                picks[count++] = kCutOrder[i];
                expected += m_cost[kCutOrder[i]];
            }
            for (int j = count - 2; j >= 0 && timed; j--) {
                if (expected - m_cost[picks[j]] < need) continue;
                expected -= m_cost[picks[j]];
                for (int m = j; m < count - 1; m++) picks[m] = picks[m + 1];
                count--;
            }
        }
        m_lastCutCount = 0;
        for (int i = 0; i < count; i++) {
            reduce(picks[i], now);
            m_lastCut[m_lastCutCount++] = picks[i];
        }
        if (m_lastCutCount > 0) {
            m_expectedGain = expected;
            m_frameMsBeforeCut = frameMs;
            m_lastChange = now;
            TMVS_LOG("auto quality: %.0f FPS (target %.0f, effects %.1f ms, %.1f ms missing) -> off: %s", m_fps, target, m_pipelineMs,
                     frameMs - targetMs, summary());
        }
    } else if (m_reducedCount > 0) {
        // Time to spare, several windows in a row: the effect that matters most for the look
        // (the last in the order) comes back, if its measured cost fits the tightest of them.
        const float spare = targetMs - frameMs;
        if (spare <= 0.0f) {
            m_calm = 0;
            return;
        }
        m_worstSpare = m_calm == 0 ? spare : (spare < m_worstSpare ? spare : m_worstSpare);
        if (++m_calm < kCalmWindows) return;
        m_calm = 0; // fits or not, the next one needs a new run of windows
        for (int i = kCutKnobs - 1; i >= 0; i--) {
            const int k = kCutOrder[i];
            if (!m_reduced[k] || now < m_blockUntil[k] || now - m_cutAt[k] < kHold || m_worstSpare < m_cost[k] * 1.2f + targetMs * 0.03f) continue;
            restore(k, now);
            m_lastChange = now;
            TMVS_LOG("auto quality: %.0f FPS (target %.0f): %s back on (%.1f ms)", m_fps, target, kKnobTable[k].name, m_cost[k]);
            break;
        }
    } else {
        m_calm = 0;
    }
}

void AutoQuality::apply(Settings& s) const {
    if (!s.autoQuality) return;
    for (int k = 0; k < kKnobs; k++) {
        if (m_reduced[k]) applyKnob(k, s);
    }
    if (m_reduced[9] && s.bokehSize > 8.0f) s.bokehSize = 8.0f;
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

} // namespace tmshaders
