#include "modules/modules.h"

namespace tmshaders {
namespace modules {
namespace {

bool g_hosted = false;
bool g_attached[kCount] = {};
bool g_enabled[kCount] = {};
uint32_t g_changes = 0;

bool valid(Id id) {
    return id >= 0 && id < kCount;
}

} // namespace

const char* pluginId(Id id) {
    static const char* const kIds[kCount] = {"tmvs", "tmvs-capture", "tmvs-upscaling", "tmvs-packs"};
    return valid(id) ? kIds[id] : "";
}

const char* displayName(Id id) {
    static const char* const kNames[kCount] = {"TM Vibrant Shaders", "TMVS Capture", "TMVS Upscaling", "TMVS Packs"};
    return valid(id) ? kNames[id] : "";
}

void setHosted() {
    g_hosted = true;
}

bool hosted() {
    return g_hosted;
}

void setAttached(Id id, bool attached) {
    if (!valid(id) || g_attached[id] == attached) return;
    g_attached[id] = attached;
    g_changes |= 1u << id;
}

void setEnabled(Id id, bool enabled) {
    if (!valid(id) || g_enabled[id] == enabled) return;
    g_enabled[id] = enabled;
    g_changes |= 1u << id;
}

bool attached(Id id) {
    return valid(id) && g_attached[id];
}

bool enabled(Id id) {
    return valid(id) && g_enabled[id];
}

bool on(Id id) {
    if (!g_hosted) return true;
    return valid(id) && g_attached[id] && g_enabled[id];
}

bool upscaleOnly(float renderScale, bool effectsOn) {
    return g_hosted && (!on(kShaders) || !effectsOn) && on(kUpscaling) && renderScale < 0.99f;
}

uint32_t takeChanges() {
    const uint32_t c = g_changes;
    g_changes = 0;
    return c;
}

} // namespace modules
} // namespace tmshaders
