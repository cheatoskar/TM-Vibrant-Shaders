#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ".tmcap" frame capture: everything the post pipeline consumes for one frame, so the
// exact same shaders can be run offline (tools/preview) on real game frames.
namespace tmshaders {

struct FrameCapture {
    uint32_t width = 0;
    uint32_t height = 0;
    float view[16] = {};        // world -> view (D3D row-vector convention)
    float projection[16] = {};  // view -> clip
    float sunDirection[4] = {}; // world space, towards the sun (w = 1 when known)
    float sunColor[4] = {};     // game light colour (w = 1 when known)
    float time = 0.0f;
    std::vector<uint32_t> color; // BGRA8 rows, top-down
    std::vector<float> depth;    // raw hardware depth [0,1]

    bool save(const std::wstring& path) const;
    bool load(const std::wstring& path);
};

} // namespace tmshaders
