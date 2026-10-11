#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>

namespace tmvs_mp {
namespace hooks {

// Set while the mod itself draws: our own context calls go through the patched vtables too and
// must not be mistaken for the game's.
struct Inside {
    Inside();
    ~Inside();
    Inside(const Inside&) = delete;
    Inside& operator=(const Inside&) = delete;
};
bool inside();

// Called by the proxy for every device the game creates (D3D11CreateDevice*).
void onDevice(ID3D11Device* device);
void onSwapChain(IDXGISwapChain* swapChain);

enum class Placed : int { None = 0, BeforeHud = 1, EndOfFrame = 2, Fallback = 3 };

struct Stats {
    uint32_t shadersCreated = 0;
    uint32_t shadersKnown = 0;
    uint32_t deferredContexts = 0;
    uint32_t commandLists = 0;     // ExecuteCommandList calls last frame
    uint32_t drawsLastFrame = 0;
    bool toneMapSeen = false;      // last frame
    Placed placed = Placed::None;  // last frame
    const char* hudShader = nullptr; // the draw the effect went in front of (last frame)
    bool depthFound = false;
    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0;
};
const Stats& stats();

} // namespace hooks
} // namespace tmvs_mp
