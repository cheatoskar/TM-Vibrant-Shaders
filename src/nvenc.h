#pragma once
#include "videoencoder.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// NVIDIA's hardware encoder driven directly through the driver's nvEncodeAPI.dll, for the video
// export (src/videoencoder.cpp uses it before Media Foundation's NVENC MFT). The MFT takes no
// B-frames, no adaptive quantisation and no lookahead, and its constant QP spends without
// measure; here all of that is set, with a constant quality and a cap on the bitrate.
//
// Nothing is linked: the DLL is loaded on first use (a 32-bit process gets the one in
// SysWOW64), the API version is checked against the driver's, and the encoder gets a D3D11
// device of its own on the NVIDIA adapter (nothing to do with the game's D3D9 device). Frames
// come from the CPU (NV12 / P010) into input buffers of the encoder's own.
//
// One thread at a time per Session (the encoder's worker).
namespace tmshaders {
namespace venc {
namespace nvenc {

// What the driver offers (looked up once, cached; any thread).
struct Support {
    bool ok = false;           // DLL, API version, an NVIDIA adapter and an encode session
    std::string why;           // !ok: why not
    unsigned driverApi = 0;    // the driver's newest API, major << 4 | minor
    unsigned builtApi = 0;     // the API the mod is built against, the same way
    std::string adapter;       // the card's name
    bool h264 = false, hevc = false, av1 = false;
    std::string caps;          // per codec, for the log
};
const Support& support();

// One encoded picture, in decode order. Valid during the callback only.
struct Packet {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
    int64_t pts = 0;           // the frame's number
    bool key = false;          // IDR
};
using PacketSink = std::function<bool(const Packet&)>;

class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Codec, depth and size as `o` says (no fallback here: the caller walks its chain). The
    // rate control: Options::bitrate > 0 VBR at that average; RateControl::ConstantQP a QP of
    // `qp`; else constant quality `cq` with a cap of `maxRate` bits a second (0: none).
    bool open(const Options& o, double cq, int qp, double maxRate, std::string& error);
    // One frame, tightly packed NV12 (8-bit) or P010 (10-bit) as opened. Packets that became
    // ready go to `sink` (with B-frames and lookahead they come a few frames later).
    bool encode(const uint8_t* frame, const PacketSink& sink, std::string& error);
    // The end: every packet still in the encoder.
    bool flush(const PacketSink& sink, std::string& error);

    // SPS / PPS (HEVC: VPS too), Annex B, for the container.
    const std::vector<uint8_t>& sequenceHeader() const;
    // How many frames the decode order may run ahead of the display order (the B-frames).
    int reorderDelay() const;
    // e.g. "CQ 23, cap 90 Mbit/s, P7 high quality, 2 B-frames, lookahead 24, spatial + temporal AQ, two passes (quarter)"
    const std::string& summary() const;

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

} // namespace nvenc
} // namespace venc
} // namespace tmshaders
