#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// The video export's encoder: frames in, an MP4 (or whatever the path's extension says for the
// ffmpeg backend) out, with the sound muxed in at the end.
//
// Two backends:
//  - Media Foundation, which every Windows since 7 brings (Windows N without its Media Feature
//    Pack: open() fails and says so). On an NVIDIA card NVENC is driven directly (src/nvenc.cpp,
//    the driver's nvEncodeAPI.dll; Media Foundation only writes the MP4). Otherwise, or when
//    NVENC refuses, the encoder is the one Windows picks for the format: the graphics card's
//    (NVIDIA, AMD, Intel) when its driver registers one, else Microsoft's software encoder.
//    The DLLs are loaded on first use, so the mod still loads without them.
//  - ffmpeg, only when the user installed it (the mod is MIT and bundles no GPL code): frames go
//    to ffmpeg.exe through a pipe, encoded by libx264, libx265 or libsvtav1.
//
// Threading: open(), acquire()/submit()/pushBgr() and close() come from one thread (the game's).
// Everything heavy, the colour conversions and every encoder call, runs on a worker thread
// behind a small queue of pooled frame buffers, so the game only waits when the encoder is
// slower than the game for several frames in a row (Stats::waitMs says how long).
//
// Video and audio may start in either order and the audio may stop early: the sound goes into a
// temporary file as it comes, and close() muxes it into the final file (the video is copied,
// not encoded again). If that fails the video file stays and the sound is kept as a .wav beside
// it.
namespace tmshaders {
namespace venc {

enum class Codec { H264 = 1, HEVC = 2, AV1 = 3 };
enum class Backend { MediaFoundation = 0, Ffmpeg = 1 };

// NV12 / P010: the Y plane (width x height) then the interleaved UV plane (width x height / 2),
// tightly packed, top row first. P010 is 16-bit little endian with the value in the top 10 bits.
// BGR24 / BGR32: bottom-up rows (a Windows bitmap), stride = width * bytes per pixel.
// All of them BT.709 studio range, or with Options::hdr10 PQ BT.2020 studio range (YUV only).
enum class Pixels { NV12, P010, BGR24, BGR32 };

// Who decides how many bits go where (Options::rateControl). Auto, bitrate 0: Media Foundation
// tries a constant QP from `quality`, then the encoder's own quality scale (CODECAPI
// AVEncCommonQuality), then VBR at a bitrate from `quality`; ffmpeg uses CRF. Auto, bitrate > 0:
// VBR. The others force one mode for tests and experts; Media Foundation still falls back to
// VBR when the encoder refuses the forced mode.
enum class RateControl { Auto, Quality, ConstantQP, VBR };

struct Options {
    Backend backend = Backend::MediaFoundation;
    Codec codec = Codec::H264;
    bool tenBit = false;        // HEVC Main10 / AV1 10-bit (H.264 stays 8-bit in Media Foundation; with ffmpeg libx264 High10 is allowed)
    int width = 0, height = 0;  // even
    double fps = 0.0;
    int quality = 80;           // 0..100, higher = better (constant quality or QP; without a quality mode a bitrate from it)
    double bitrate = 0.0;       // > 0: bits a second on average instead of the quality mode
    bool hdr10 = false;         // PQ + BT.2020 tags and metadata (10-bit HEVC / AV1); the frames are PQ BT.2020 YUV already
    int hdrPeakNits = 1000;     // HDR10 metadata: mastering display peak and MaxCLL
    int hdrAverageNits = 400;   // HDR10 metadata: MaxFALL
    std::wstring ffmpegPath;    // Backend::Ffmpeg: ffmpeg.exe (empty: <data dir>\ffmpeg.exe, then PATH)
    int ffmpegPreset = 1;       // 0 fast, 1 balanced, 2 best (x264/x265 presets, SVT-AV1 preset numbers)
    RateControl rateControl = RateControl::Auto;
    bool hardware = true;       // Media Foundation: false = Windows' software encoders only (for a driver whose encoder misbehaves)

    // Backend::MediaFoundation with `hardware`: NVIDIA's encoder is driven directly (src/nvenc.cpp:
    // constant quality with a cap, the slowest preset; B-frames, AQ and lookahead on request)
    // when the card and driver allow it; else, or with nvenc = false, the encoder Windows
    // offers (the MFT).
    bool nvenc = true;
    // Expert and test knobs (tools/encbench), -1 = my defaults from the measurements.
    int nvencPreset = -1;       // 1..7: P1 (fastest) .. P7 (best)
    int nvencBFrames = -1;      // B-frames between references; cut to what the card takes (Pascal: H.264 only)
    int nvencLookahead = -1;    // rate control lookahead in frames, 0 = off
    int nvencAQ = -1;           // 0 off, 1 spatial, 2 spatial + temporal, 3 temporal (where the card has it)
    int nvencAQStrength = -1;   // spatial AQ 1..15, 0 = the driver's choice
    int nvencMultipass = -1;    // 0 off, 1 two passes (the first at quarter resolution), 2 two full passes
    double nvencMaxRate = -1.0; // constant quality: the cap in Mbit/s, 0 = none
    double encoderQ = 0.0;      // > 0: the encoder's own number instead of `quality` (NVENC CQ, x264 / x265 / SVT-AV1 CRF)
    std::wstring ffmpegExtra;   // Backend::Ffmpeg: more encoder options after the mod's own (they win), e.g. L"-preset slow"
};

// Times in milliseconds, summed over all frames (divide by frames for the average).
struct Stats {
    int frames = 0;             // frames the encoder took
    double encodeMs = 0.0;      // in the encoder call (sink writer WriteSample / the write into ffmpeg's pipe), back-pressure included
    double waitMs = 0.0;        // the caller blocked in acquire() / pushBgr() because every buffer was queued
    double convertMs = 0.0;     // the worker's copy or colour conversion into the encoder's format
    int queuePeak = 0;          // most frames waiting at once
    int queueSlots = 0;         // pooled buffers
    int waits = 0;              // acquire() calls that blocked
    double encodeMaxMs = 0.0;   // the slowest single frame in the encoder
    double waitMaxMs = 0.0;     // the longest single wait of the caller
    double wallMs = 0.0;        // the first frame taken to the last one through the encoder (frames / wallMs = throughput)
    double openMs = 0.0;        // open(): finding the encoder (the caller waits for it)
    double finishMs = 0.0;      // close(): finalizing the video and muxing the sound
    int lostFrames = 0;         // submitted but not encoded (after a failure)
};

class Encoder {
public:
    Encoder();
    ~Encoder();                 // close() if still open
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    // The final file, for sound that comes before open() (open() sets it as well).
    void setPath(const std::wstring& path);
    const std::wstring& path() const;

    // Starts the worker and the encoder. AV1 and HEVC fall back down AV1 -> HEVC -> H.264 when
    // there is no encoder (10-bit is kept over the codec: AV1 10 -> HEVC 10 -> AV1 8 -> HEVC 8 ->
    // H.264), and Backend::Ffmpeg falls back to Media Foundation when ffmpeg is missing or has
    // no suitable encoder. Each refusal is logged. False: nothing written, `error` says why.
    bool open(const std::wstring& path, const Options& o, std::string& error);
    bool isOpen() const;
    // What the opened encoder takes (NV12 or P010): hand it exactly that and the worker only
    // copies. Anything else is converted on the worker.
    Pixels wants() const;
    // The options that took effect (codec and tenBit after the fallbacks, backend).
    const Options& options() const;

    static size_t frameBytes(Pixels format, int width, int height);
    size_t frameBytes(Pixels format) const;

    // A pooled buffer of frameBytes(format) for the next frame: fill it, then submit(). Blocks
    // while every buffer is queued. nullptr when not open or after a failure (failed()): the
    // frame is lost, submit() then does nothing.
    uint8_t* acquire(Pixels format);
    void submit();
    // Copies a game frame (bottom-up BGR, 3 or 4 bytes a pixel) into a buffer and queues it.
    bool pushBgr(const uint8_t* pixels, int bytesPerPixel);

    // Sound: PCM. Media Foundation encodes 16-bit 44.1 / 48 kHz with 1, 2 or 6 channels to AAC
    // (anything else stays a .wav beside the video); ffmpeg takes any PCM. Any time before close().
    void openAudio(const WAVEFORMATEX& format);
    void writeAudio(const void* data, size_t bytes);

    // Drains the queue, finalizes the video and muxes the sound. `report` says what was written
    // (or went wrong) and how fast. True: the file holds everything that came.
    bool close(std::string& report);
    // Gives up: nothing stays behind (no video, no .wav, no temporary file).
    void discard();

    // The encoder stopped taking frames (an encoder error, ffmpeg died); failure() says why.
    // The partial file is kept and close() reports it.
    bool failed() const;
    std::string failure() const;

    // e.g. "HEVC Main10 3840x2160 at 60 fps, quality 85 (constant quality, QP 20), NVIDIA HEVC Encoder MFT (hardware)"
    std::string description() const;
    Stats stats() const;
    int framesWritten() const;

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

// For the log: the H.264, HEVC and AV1 encoders Windows offers (hardware flagged), and ffmpeg
// if it is there with which of libx264 / libx265 / libsvtav1 it has.
std::string describeEncoders(const std::wstring& ffmpegPath = std::wstring());

// The ffmpeg.exe Options::ffmpegPath leads to (empty when none), and its encoders.
struct FfmpegInfo {
    std::wstring path;
    bool x264 = false, x265 = false, svtav1 = false;
    std::string version;
};
FfmpegInfo findFfmpeg(const std::wstring& hint = std::wstring());

} // namespace venc
} // namespace tmshaders
