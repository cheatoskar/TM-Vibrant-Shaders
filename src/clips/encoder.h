#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The clips' video encoder: NV12 frames in, H.264 pictures out (no B-frames: decode order = the
// frames' order, so each picture belongs to the frame of the same number). Two kinds:
//  - NVIDIA's encoder driven directly (src/nvenc.cpp, the video export's): when the card has one.
//  - Media Foundation's H.264 encoder of the graphics card (AMD, Intel; NVIDIA's too), an MFT
//    driven here (synchronous or asynchronous), for the other cards. Windows' software encoder only
//    when asked (tests): it would cost the game too much.
// One thread at a time (the recorder's worker).
namespace tmshaders {
namespace clips {

struct EncodedPicture {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
    int64_t frame = 0; // the frame's number (0, 1, 2, ... in the order they went in)
    bool key = false;
};
using PictureSink = std::function<void(const EncodedPicture&)>;

class PictureEncoder {
public:
    virtual ~PictureEncoder() = default;
    // One tightly packed NV12 frame (width x height Y, then the UV rows).
    virtual bool encode(const uint8_t* nv12, const PictureSink& sink, std::string& error) = 0;
    virtual bool flush(const PictureSink& sink, std::string& error) = 0;
    // e.g. "NVENC H.264 1920x1080 60 fps, VBR 12.0 Mbit/s, P4"
    virtual std::string description() const = 0;
    // The parameter sets (Annex B) if the encoder tells them apart from its pictures; empty: take
    // them from a key frame (parameterSets).
    virtual std::vector<uint8_t> header() const = 0;
};

struct EncoderSetup {
    int width = 0, height = 0;
    double fps = 60.0;
    double bitrate = 12e6;   // bits a second, average
    double gopSeconds = 1.0; // a key frame this often
    int choice = 0;          // 0 auto, 1 NVIDIA only, 2 Media Foundation only
    bool softwareOnly = false;  // Media Foundation: Windows' software encoder only (tests: the synchronous path)
};

// The first that works, every refusal in `log` ("NVENC: ...; Media Foundation: ..."). Null: none.
std::unique_ptr<PictureEncoder> openEncoder(const EncoderSetup& setup, std::string& log);
std::unique_ptr<PictureEncoder> openNvenc(const EncoderSetup& setup, std::string& error);
std::unique_ptr<PictureEncoder> openMft(const EncoderSetup& setup, std::string& error);

// The parameter sets (SPS and PPS, with start codes) of an Annex B key frame, empty if none.
std::vector<uint8_t> parameterSets(const uint8_t* data, size_t size);

} // namespace clips
} // namespace tmshaders
