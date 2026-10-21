#include "videofile.h"
#include "config.h"
#include "engine.h"
#include "log.h"
#include "videoencoder.h"
#include <memory>
#include <string>
#include <vector>

namespace tmshaders {
namespace videofile {
namespace {

// One export (normally one at a time; the game's file object is the key).
struct Export {
    void* file = nullptr;
    std::wstring avi;               // the game's .avi
    std::unique_ptr<venc::Encoder> mp4;
    bool video = false;             // the MP4 takes the frames
    bool refused = false;           // the game's AVI is written (VideoCodec 0, or Media Foundation failed)
    int width = 0, height = 0, bpp = 3;
    WAVEFORMATEX sound{};
    bool hasSound = false;
};
std::vector<Export> g_exports;

Export* find(void* file) {
    for (Export& e : g_exports) {
        if (e.file == file) return &e;
    }
    return nullptr;
}

std::string narrow(const std::wstring& text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &out[0], n, nullptr, nullptr);
    return out;
}

// The MP4 beside the game's .avi (same name); without one, TMVS\videos. Never over an earlier
// one: the game picks its next free .avi name, and the empty .avi is deleted after each export,
// so the next export got the same name and replaced the last video (my test, 2026-10-16).
std::wstring mp4Path(const std::wstring& avi) {
    if (avi.size() > 4 && _wcsicmp(avi.c_str() + avi.size() - 4, L".avi") == 0) {
        const std::wstring stem = avi.substr(0, avi.size() - 4);
        std::wstring path = stem + L".mp4";
        for (int i = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && i < 1000; i++) {
            path = stem + L"_" + std::to_wstring(i) + L".mp4";
        }
        return path;
    }
    const std::wstring dir = log::dataDir() + L"\\videos";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf(name, 64, L"\\video_%04u%02u%02u_%02u%02u%02u.mp4", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return dir + name;
}

void opened(void* file, const wchar_t* path, bool ok) {
    Export* e = find(file);
    if (!e) {
        g_exports.emplace_back();
        e = &g_exports.back();
        e->file = file;
    }
    e->avi = path;
    TMVS_LOG("video file: the game opens %s%s", narrow(e->avi).c_str(), ok ? "" : " (failed)");
}

bool videoOpen(void* file, int width, int height, float fps, int bytesPerPixel) {
    Export* e = find(file);
    if (!e) {
        g_exports.emplace_back();
        e = &g_exports.back();
        e->file = file;
    }
    const Settings& s = Config::get().settings;
    if (s.videoCodec != 1 && s.videoCodec != 2) {
        TMVS_LOG("video file: the game's AVI (VideoCodec 0), %dx%d at %.3f fps", width, height, fps);
        e->refused = true;
        if (e->mp4) e->mp4->discard();
        e->mp4.reset();
        return false;
    }
    if (!e->mp4) e->mp4 = std::make_unique<venc::Encoder>();
    e->mp4->setPath(mp4Path(e->avi));
    if (e->hasSound) e->mp4->openAudio(e->sound);
    std::string error;
    TMVS_LOG("video file: encoders: %s", venc::describeEncoders().c_str());
    venc::Options options;
    options.codec = s.videoCodec == 2 ? venc::Codec::HEVC : venc::Codec::H264;
    options.width = width;
    options.height = height;
    options.fps = fps;
    options.bitrate = s.videoBitrate * 1e6; // 0: constant quality
    if (!e->mp4->open(e->mp4->path(), options, error)) {
        TMVS_LOG("video file: no MP4 (%s): the game's AVI instead, with its codec dialog", error.c_str());
        e->mp4->discard();
        e->mp4.reset();
        e->refused = true;
        return false;
    }
    e->video = true;
    e->width = width;
    e->height = height;
    e->bpp = bytesPerPixel;
    TMVS_LOG("video file: MP4 %s -> %s", e->mp4->description().c_str(), narrow(e->mp4->path()).c_str());
    return true;
}

bool videoFrame(void* file, const void* pixels) {
    Export* e = find(file);
    if (!e || !e->video || !e->mp4) return false;
    e->mp4->pushBgr(static_cast<const uint8_t*>(pixels), e->bpp);
    return true; // also when it failed: the game has no stream to write it to (the log says it)
}

void audioOpen(void* file, const void* format) {
    Export* e = find(file);
    if (!e) {
        g_exports.emplace_back();
        e = &g_exports.back();
        e->file = file;
    }
    memcpy(&e->sound, format, 16); // SWaveFormat: WAVEFORMATEX without cbSize
    e->sound.cbSize = 0;
    e->hasSound = true;
    TMVS_LOG("video file: sound %u Hz, %u bits, %u channels (format %u)", e->sound.nSamplesPerSec, e->sound.wBitsPerSample, e->sound.nChannels,
             e->sound.wFormatTag);
    if (e->mp4) e->mp4->openAudio(e->sound);
}

void audioData(void* file, const void* data, unsigned long blocks) {
    Export* e = find(file);
    if (!e || e->refused || !e->hasSound || !e->sound.nBlockAlign) return;
    // The sound may come before the video: keep it (the writer exists from the first piece).
    if (!e->mp4) {
        const Settings& s = Config::get().settings;
        if (s.videoCodec != 1 && s.videoCodec != 2) return;
        e->mp4 = std::make_unique<venc::Encoder>();
        e->mp4->setPath(mp4Path(e->avi));
        e->mp4->openAudio(e->sound);
    }
    e->mp4->writeAudio(data, static_cast<size_t>(blocks) * e->sound.nBlockAlign);
}

void closed(void* file) {
    for (size_t i = 0; i < g_exports.size(); i++) {
        Export& e = g_exports[i];
        if (e.file != file) continue;
        if (e.mp4 && e.video) {
            std::string report;
            const bool complete = e.mp4->close(report);
            TMVS_LOG("video file: MP4 written: %s, %s", narrow(e.mp4->path()).c_str(), report.c_str());
            // The game's .avi has no video (its stream was never opened): gone once the MP4
            // holds everything. Kept when the MP4 lacks the sound (the .avi has it too).
            if (complete && !e.avi.empty() && DeleteFileW(e.avi.c_str())) TMVS_LOG("video file: the game's .avi (no video in it) deleted");
        } else if (e.mp4) {
            e.mp4->discard();
        }
        g_exports.erase(g_exports.begin() + static_cast<ptrdiff_t>(i));
        return;
    }
}

} // namespace

void install() {
    if (!engine::videoFileHooked()) return;
    engine::VideoFileHooks hooks;
    hooks.opened = opened;
    hooks.videoOpen = videoOpen;
    hooks.videoFrame = videoFrame;
    hooks.audioOpen = audioOpen;
    hooks.audioData = audioData;
    hooks.closed = closed;
    engine::setVideoFileHooks(hooks);
}

} // namespace videofile
} // namespace tmshaders
