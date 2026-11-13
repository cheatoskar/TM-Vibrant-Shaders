#pragma once
#include <cstdint>

// Which of the game's cameras is the main 3D camera: the one the effects run on, that sizes the
// pipeline's render targets and that the minimap, OverlapFix and the caster recording follow.
// Free of Direct3D so tools/maincameratest.cpp can run it on plain numbers.
namespace tmshaders {
namespace maincamera {

struct Target {
    unsigned width = 0, height = 0;           // the camera's render target
    unsigned depthWidth = 0, depthHeight = 0; // the depth buffer bound with it
    bool depthReadable = false;               // an INTZ depth (the screen's shadow or a game buffer made readable)
    bool depthIsScreens = false;              // the shadow of the automatic depth-stencil (the back buffer's)
};

// The rule since 1.0: a readable depth buffer of the target's own size, at least 320 wide.
inline bool sameSizeAsDepth(const Target& t) {
    return t.depthReadable && t.width == t.depthWidth && t.height == t.depthHeight && t.width >= 320;
}

// A camera into a texture smaller than the screen, with a depth buffer of its own: a view the game
// renders for a material, not the screen. TrackMania Sunrise draws such a view every frame (512x512
// with its own 512x512 depth buffer, which the readable game depth makes INTZ like any other
// buffer from 320x200 up): it was shaded as a second main camera, before the real one, and the
// pipeline made all its render targets again twice a frame (1.6.0-rc.1, 15 FPS at 1440p).
// referenceWidth/Height: the screen (the automatic depth-stencil), or the video export's frame
// while it runs; 0 = not known (no check, as before). The screen's own depth buffer never counts
// as such a view: a camera with it is the screen's (menus, replays, camera effects).
inline bool isTextureView(const Target& t, unsigned referenceWidth, unsigned referenceHeight) {
    if (t.depthIsScreens || !referenceWidth || !referenceHeight) return false;
    return static_cast<uint64_t>(t.width) * t.height < static_cast<uint64_t>(referenceWidth) * referenceHeight;
}

// The main camera's size over the last frames. A camera smaller than one shaded within the last
// kHoldFrames frames is not the main camera: the pipeline keeps the larger size instead of making
// its targets again for each (any view that slips past isTextureView). The larger one going away
// for longer (a menu, a smaller window after a device reset) hands the size over.
class SizeHistory {
public:
    static constexpr unsigned kHoldFrames = 8;

    // frame: the device's frame counter (one per Present). True = this camera may be the main one.
    bool accept(unsigned width, unsigned height, unsigned frame) {
        if (m_have && width == m_width && height == m_height) {
            m_frame = frame;
            return true;
        }
        const bool held = m_have && frame - m_frame <= kHoldFrames;
        if (held && static_cast<uint64_t>(width) * height <= static_cast<uint64_t>(m_width) * m_height) return false;
        m_width = width;
        m_height = height;
        m_frame = frame;
        m_have = true;
        return true;
    }

    // A device reset, the video export starting or ending: the next main camera sets the size.
    void reset() { m_have = false; }

    unsigned width() const { return m_have ? m_width : 0; }
    unsigned height() const { return m_have ? m_height : 0; }

private:
    unsigned m_width = 0, m_height = 0, m_frame = 0;
    bool m_have = false;
};

} // namespace maincamera
} // namespace tmshaders
