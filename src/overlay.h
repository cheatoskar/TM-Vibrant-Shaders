#pragma once
#include <d3d9.h>

namespace tmshaders {
class Pipeline;
class AutoQuality;

namespace overlay {

struct Status {
    bool depthAvailable = false;
    bool engineHooks = false;
    bool sunKnown = false;
    float sunDirection[3] = {};
    const char* shaderError = nullptr;
    const Pipeline* pipeline = nullptr;       // GPU time per pass (measured while the menu is open)
    const AutoQuality* autoQuality = nullptr;
};

void preReset();
void postReset();
void setStatus(const Status& status);
// Draws the menu (F8) after the game's HUD. Must be called from Present.
void draw(IDirect3DDevice9* device);
// Set when the user asks for a shader reload from the menu; cleared by the caller.
bool consumeReloadRequest();

} // namespace overlay
} // namespace tmshaders
