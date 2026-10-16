#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <cstdint>
#include <string>

// MP4 files (H.264 or HEVC video, AAC audio) through Windows Media Foundation, which every
// Windows since 7 brings (Windows N without its Media Feature Pack: open() fails and says so).
// The encoder is the one Windows picks for the format: the graphics card's (NVENC, AMD, Intel)
// when its driver registers one, else Microsoft's software encoder. Nothing is bundled; the
// Media Foundation DLLs are loaded on first use, so the mod still loads without them.
//
// Video and audio may start in either order and the audio may stop early: the video goes into
// the MP4 as it comes, the sound into a file next to it, and close() muxes the two into the
// final MP4 (the video is copied, not encoded again). If that fails the MP4 keeps its video and
// the sound stays as a .wav beside it.
struct IMFSinkWriter;

namespace tmshaders {
namespace mp4 {

enum class Codec { H264 = 1, HEVC = 2 };

class Writer {
public:
    Writer() = default;
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    // The file to write (its folder must exist). Nothing is created yet.
    void setPath(const std::wstring& path) { m_path = path; }
    const std::wstring& path() const { return m_path; }

    // Starts the video: 24-bit or 32-bit BGR frames of width x height (even), fps frames a
    // second, bitrate in bits a second (0 = chosen from the size and frame rate). HEVC falls
    // back to H.264 when no HEVC encoder is there. False: nothing written, `error` says why.
    bool openVideo(int width, int height, double fps, Codec codec, double bitrate, std::string& error);
    bool videoOpen() const { return m_writer != nullptr; }
    // One frame; rows `stride` bytes apart, bottom row first when bottomUp (Windows bitmaps).
    bool writeFrame(const uint8_t* pixels, int stride, int bytesPerPixel, bool bottomUp);

    // Sound: 16-bit PCM (1, 2 or 6 channels, 44.1 or 48 kHz becomes AAC; anything else stays a
    // .wav next to the video). Any time before close().
    void openAudio(const WAVEFORMATEX& format);
    void writeAudio(const void* data, size_t bytes);

    // Finishes the file. `report` says what was written (or went wrong). True: the MP4 holds
    // everything that came (the video and, if there was sound, the sound).
    bool close(std::string& report);
    // Gives up: nothing stays behind (no MP4, no .wav, no temporary file).
    void discard();

    const std::string& encoderName() const { return m_encoder; }
    int framesWritten() const { return m_frames; }

private:
    bool remux(const std::wstring& video, std::string& error);
    void saveWav(const std::wstring& path);

    std::wstring m_path;
    IMFSinkWriter* m_writer = nullptr;
    unsigned long m_stream = 0;
    int m_width = 0, m_height = 0;
    double m_fps = 0.0;
    Codec m_codec = Codec::H264;
    int m_frames = 0;
    int m_failedWrites = 0;
    std::string m_encoder;
    bool m_comInit = false;
    // Sound, kept in a file beside the video until close().
    WAVEFORMATEX m_audioFormat{};
    bool m_audio = false;
    HANDLE m_pcm = INVALID_HANDLE_VALUE;
    std::wstring m_pcmPath;
    uint64_t m_pcmBytes = 0;
};

// Which encoders Windows offers for H.264 and HEVC (for the log), "" when Media Foundation
// isn't there.
std::string describeEncoders();

} // namespace mp4
} // namespace tmshaders
