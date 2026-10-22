#pragma once

// The video export's file: with VideoCodec 1-3 the game's AVI writer is bypassed (no AVI video
// stream, no codec dialog) and an MP4 is written next to the game's .avi by the mod's encoder
// (src/videoencoder.cpp: the graphics card's through Media Foundation, or the player's ffmpeg),
// with its sound as AAC. If the encoder can't start the game's AVI is written as before.
//
// The frames: with VideoOwnFrames the plugin renders each video frame itself (every sample summed
// in float, graded once, converted to the encoder's planes on the GPU) and hands it over each
// time the game writes a frame, so the video keeps exactly the game's frame count and its sound
// stays in sync. Until the plugin has a frame (the effects off, the first frames) the game's own
// image goes in.
namespace tmshaders {
namespace venc {
class Encoder;
}
namespace videofile {

void install(); // after engine::install

// The format of the frames the open export takes from the plugin.
struct OwnFormat {
    int width = 0, height = 0; // the encoder's size (multiples of 8; smaller than the game's with VideoAspect)
    bool tenBit = false;       // P010 wanted (else NV12)
    float crop[4] = {0.0f, 0.0f, 1.0f, 1.0f}; // the part of the game's frame the video shows (uv: x, y, w, h)
    float hdrWhite = 0.0f;     // HDR10 (P010 in PQ, BT.2020): the frame's 1.0 in nits; 0 = SDR
    float hdrPeak = 0.0f;      // HDR10: the brightest light in nits
};
// An export is running whose encoder takes the plugin's frames (VideoOwnFrames, MP4): its format.
bool ownFormat(OwnFormat& out);

// The plugin's side. deliver: put the frame finished last into `encoder` (acquire, fill, submit);
// false = none (the game's image goes in instead). written: the game wrote a video frame (after
// deliver), the plugin's count of the sub-frames of the next one starts over.
struct FrameSource {
    bool (*deliver)(venc::Encoder& encoder, const OwnFormat& format) = nullptr;
    void (*written)() = nullptr;
    // Asked at the end of the frame the game is about to write: will deliver succeed? true lets
    // the game skip reading its own image back (engine::VideoFileHooks::ownFrameReady).
    bool (*ready)(const OwnFormat& format) = nullptr;
};
void setFrameSource(const FrameSource& source);

} // namespace videofile
} // namespace tmshaders
