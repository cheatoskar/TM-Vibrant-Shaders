#pragma once

// The video export's file: with VideoCodec 1 or 2 the frames the game hands its AVI writer go
// into an MP4 (src/videoencoder.cpp) next to the game's .avi, with its sound as
// AAC, and the game opens no AVI video stream (no codec dialog). If Media Foundation fails the
// game's AVI is written as before.
namespace tmshaders {
namespace videofile {

void install(); // after engine::install

} // namespace videofile
} // namespace tmshaders
