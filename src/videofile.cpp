#include "videofile.h"
#include "config.h"
#include "engine.h"
#include "log.h"
#include "photo.h"
#include "soundcapture.h"
#include "videoencoder.h"
#include <algorithm>
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
    std::unique_ptr<venc::Encoder> encoder;
    bool video = false;             // the encoder takes the frames
    bool refused = false;           // the game's AVI is written (VideoCodec 0, or the encoder failed)
    int width = 0, height = 0, bpp = 3; // the game's frames
    double fps = 0.0;
    OwnFormat own;                  // what the encoder takes (its size: the crop of the game's)
    bool ownFrames = false;         // VideoOwnFrames: the plugin's frames are wanted
    WAVEFORMATEX sound{};
    bool hasSound = false;
    std::vector<char> early;        // sound that came before the encoder (it opens with the video)
    // The frames that went in: the plugin's, the game's (no frame of the plugin yet), and the
    // plugin's that came between two of the game's writes (the last of them went in).
    int fromPlugin = 0, fromGame = 0;
    bool readbackSkipped = false;   // the game skipped reading this frame back (the mod's goes in)
    int staleFrames = 0;            // ... and the mod's frame failed after all: the last read image went in
};
std::vector<Export> g_exports;
FrameSource g_source;

Export* find(void* file) {
    for (Export& e : g_exports) {
        if (e.file == file) return &e;
    }
    return nullptr;
}

Export& findOrAdd(void* file) {
    if (Export* e = find(file)) return *e;
    g_exports.emplace_back();
    g_exports.back().file = file;
    return g_exports.back();
}

std::string narrow(const std::wstring& text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &out[0], n, nullptr, nullptr);
    return out;
}

bool exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// The MP4 beside the game's .avi; without one, TMVS\videos. Never over an earlier one: the game
// picks its next free .avi name, and the empty .avi is deleted after each export, so the game
// names every export Video02.avi again. Its number goes up past every .mp4 and .avi already
// there (Video02 -> Video03.mp4), as the game's own numbering would; a name without a number gets
// _2, _3. The game still says its .avi's name at the end, the screen says the MP4's.
std::wstring mp4Path(const std::wstring& avi) {
    if (avi.size() > 4 && _wcsicmp(avi.c_str() + avi.size() - 4, L".avi") == 0) {
        const std::wstring stem = avi.substr(0, avi.size() - 4);
        size_t digits = stem.size();
        while (digits > 0 && iswdigit(stem[digits - 1])) digits--;
        if (digits < stem.size() && stem.size() - digits <= 6) {
            const std::wstring prefix = stem.substr(0, digits);
            const int width = static_cast<int>(stem.size() - digits);
            for (int n = _wtoi(stem.c_str() + digits); n < 1000000; n++) {
                wchar_t number[16];
                swprintf(number, 16, L"%0*d", width, n);
                const std::wstring name = prefix + number;
                if (!exists(name + L".mp4") && (name == stem || !exists(name + L".avi"))) return name + L".mp4";
            }
        }
        std::wstring path = stem + L".mp4";
        for (int i = 2; exists(path) && i < 1000; i++) path = stem + L"_" + std::to_wstring(i) + L".mp4";
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

bool mp4Codec(const Settings& s) {
    return s.videoCodec >= 1 && s.videoCodec <= 3;
}

// VideoAspect: the picture's height for the game's width (multiples of 8, never taller than the
// game's frame), the rest cropped evenly off the top and bottom.
int croppedHeight(int width, int height, int aspect) {
    if (aspect <= 0) return height;
    const double ratio = aspect == 1 ? 2.0 : 2.39;
    const int h = static_cast<int>(width / ratio / 8.0 + 0.5) * 8;
    return h >= 16 && h < height ? h : height;
}

void opened(void* file, const wchar_t* path, bool ok) {
    Export& e = findOrAdd(file);
    e.avi = path;
    TMVS_LOG("video file: the game opens %s%s", narrow(e.avi).c_str(), ok ? "" : " (failed)");
    soundcapture::exportOpened();
}

// The encoder for this export, opened at the first of the video and the sound (the sound may
// come first: the encoder then takes it from the start).
bool startEncoder(Export& e) {
    if (e.encoder) return true;
    const Settings& s = Config::get().settings;
    e.encoder = std::make_unique<venc::Encoder>();
    venc::Options o;
    o.backend = s.videoEncoder == 1 ? venc::Backend::Ffmpeg : venc::Backend::MediaFoundation;
    o.codec = static_cast<venc::Codec>(std::clamp(s.videoCodec, 1, 3));
    o.tenBit = s.video10Bit && s.videoCodec >= 2;
    o.width = e.own.width;
    o.height = e.own.height;
    o.fps = e.fps;
    o.quality = std::clamp(s.videoQuality, 0, 100);
    o.bitrate = s.videoBitrate > 0.0f ? s.videoBitrate * 1e6 : 0.0;
    o.ffmpegPreset = std::clamp(s.videoFfmpegPreset, 0, 2);
    std::string error;
    TMVS_LOG("video file: encoders: %s", venc::describeEncoders().c_str());
    bool opened = e.encoder->open(mp4Path(e.avi), o, error);
    if (!opened && o.backend == venc::Backend::Ffmpeg) {
        // No ffmpeg.exe (or it failed to start): the graphics card's encoder after all.
        TMVS_LOG("video file: ffmpeg not usable (%s): the graphics card's encoder instead", error.c_str());
        e.encoder->discard();
        e.encoder = std::make_unique<venc::Encoder>();
        o.backend = venc::Backend::MediaFoundation;
        error.clear();
        opened = e.encoder->open(mp4Path(e.avi), o, error);
    }
    if (!opened) {
        TMVS_LOG("video file: no MP4 (%s): the game's AVI instead, with its codec dialog", error.c_str());
        e.encoder->discard();
        e.encoder.reset();
        return false;
    }
    if (e.hasSound) e.encoder->openAudio(e.sound);
    if (!e.early.empty()) {
        e.encoder->writeAudio(e.early.data(), e.early.size());
        e.early.clear();
        e.early.shrink_to_fit();
    }
    e.own.tenBit = e.encoder->wants() == venc::Pixels::P010;
    return true;
}

bool videoOpen(void* file, int width, int height, float fps, int bytesPerPixel) {
    Export& e = findOrAdd(file);
    const Settings& s = Config::get().settings;
    if (!mp4Codec(s)) {
        TMVS_LOG("video file: the game's AVI (VideoCodec 0), %dx%d at %.3f fps", width, height, fps);
        e.refused = true;
        if (e.encoder) e.encoder->discard();
        e.encoder.reset();
        return false;
    }
    e.width = width;
    e.height = height;
    e.bpp = bytesPerPixel;
    e.fps = fps;
    e.own = OwnFormat();
    e.own.width = width;
    e.own.height = croppedHeight(width, height, s.videoAspect);
    e.own.crop[1] = static_cast<float>(height - e.own.height) * 0.5f / height;
    e.own.crop[3] = static_cast<float>(e.own.height) / height;
    e.ownFrames = s.videoOwnFrames && g_source.deliver;
    if (!startEncoder(e)) {
        e.refused = true;
        return false;
    }
    e.video = true;
    TMVS_LOG("video file: MP4 %s -> %s; frames %s%s", e.encoder->description().c_str(), narrow(e.encoder->path()).c_str(),
             e.ownFrames ? "rendered by the mod (the game's until its first)" : "the game's",
             e.own.height != height ? (", cropped to " + std::to_string(e.own.width) + "x" + std::to_string(e.own.height)).c_str() : "");
    return true;
}

bool videoFrame(void* file, const void* pixels) {
    Export* e = find(file);
    if (!e || !e->video || !e->encoder) return false;
    bool done = false;
    if (e->ownFrames && g_source.deliver && g_source.deliver(*e->encoder, e->own)) {
        e->fromPlugin++;
        done = true;
    }
    if (!done && e->readbackSkipped && e->staleFrames++ == 0)
        TMVS_LOG("video file: the mod's frame failed after the game skipped its read-back: the last image the game read goes in");
    e->readbackSkipped = false;
    if (!done) {
        // The game's frame (bottom-up rows): the crop is the middle rows, which lie together.
        const size_t stride = static_cast<size_t>(e->width) * e->bpp;
        const int skip = (e->height - e->own.height) / 2;
        e->encoder->pushBgr(static_cast<const uint8_t*>(pixels) + stride * skip, e->bpp);
        e->fromGame++;
    }
    if (g_source.written) g_source.written();
    return true; // also when it failed: the game has no stream to write it to (the log says it)
}

// The game is about to write a frame: the mod's will go in, so its own read-back can be skipped.
bool ownFrameReady(void* file) {
    Export* e = find(file);
    if (!e || !e->video || !e->encoder || !e->ownFrames || !g_source.ready || !g_source.ready(e->own)) return false;
    e->readbackSkipped = true;
    return true;
}

void audioOpen(void* file, const void* format) {
    Export& e = findOrAdd(file);
    memcpy(&e.sound, format, 16); // SWaveFormat: WAVEFORMATEX without cbSize
    e.sound.cbSize = 0;
    e.hasSound = true;
    TMVS_LOG("video file: sound %u Hz, %u bits, %u channels (format %u)", e.sound.nSamplesPerSec, e.sound.wBitsPerSample, e.sound.nChannels,
             e.sound.wFormatTag);
    if (e.encoder) e.encoder->openAudio(e.sound);
}

void audioData(void* file, const void* data, unsigned long blocks) {
    Export* e = find(file);
    if (!e || e->refused || !e->hasSound || !e->sound.nBlockAlign) return;
    // The sound may come before the video: kept by an encoder that opens with the video (its
    // size is only known then), meanwhile in memory.
    if (!e->encoder) {
        const Settings& s = Config::get().settings;
        if (!mp4Codec(s)) return;
        e->early.insert(e->early.end(), static_cast<const char*>(data), static_cast<const char*>(data) + static_cast<size_t>(blocks) * e->sound.nBlockAlign);
        return;
    }
    if (!e->early.empty()) {
        e->encoder->writeAudio(e->early.data(), e->early.size());
        e->early.clear();
        e->early.shrink_to_fit();
    }
    e->encoder->writeAudio(data, static_cast<size_t>(blocks) * e->sound.nBlockAlign);
}

void closed(void* file) {
    soundcapture::exportClosed();
    for (size_t i = 0; i < g_exports.size(); i++) {
        Export& e = g_exports[i];
        if (e.file != file) continue;
        if (e.encoder && e.video) {
            if (!e.early.empty()) e.encoder->writeAudio(e.early.data(), e.early.size());
            std::string report;
            const bool complete = e.encoder->close(report);
            TMVS_LOG("video file: MP4 written: %s, %s; frames from the mod %d, from the game %d%s", narrow(e.encoder->path()).c_str(), report.c_str(),
                     e.fromPlugin, e.fromGame,
                     e.staleFrames ? (" (" + std::to_string(e.staleFrames) + " of them an old image: the mod's frame failed after the read-back was skipped)").c_str() : "");
            // The game's .avi has no video (its stream was never opened): gone once the MP4
            // holds everything. Kept when the MP4 lacks the sound (the .avi has it too).
            if (complete && !e.avi.empty() && DeleteFileW(e.avi.c_str())) TMVS_LOG("video file: the game's .avi (no video in it) deleted");
            // The game's own message names its .avi: the real file on screen, for a while.
            const std::wstring& path = e.encoder->path();
            const size_t slash = path.find_last_of(L"\\/");
            const std::string text = (complete ? "Video saved: " : "Video saved (see tmvs.log): ") +
                                     narrow(slash == std::wstring::npos ? path : path.substr(slash + 1)) + " in " +
                                     narrow(slash == std::wstring::npos ? std::wstring() : path.substr(0, slash));
            photo::notify(text.c_str(), 20);
        } else if (e.encoder) {
            e.encoder->discard();
        }
        g_exports.erase(g_exports.begin() + static_cast<ptrdiff_t>(i));
        return;
    }
}

} // namespace

bool ownFormat(OwnFormat& out) {
    for (const Export& e : g_exports) {
        if (e.video && e.encoder && e.ownFrames) {
            out = e.own;
            return true;
        }
    }
    return false;
}

void setFrameSource(const FrameSource& source) {
    g_source = source;
}

void install() {
    // The sound pass's capture: its own imports, also where the file hooks are missing.
    soundcapture::install();
    if (!engine::videoFileHooked()) return;
    engine::VideoFileHooks hooks;
    hooks.opened = opened;
    hooks.videoOpen = videoOpen;
    hooks.videoFrame = videoFrame;
    hooks.audioOpen = audioOpen;
    hooks.audioData = audioData;
    hooks.closed = closed;
    hooks.ownFrameReady = ownFrameReady;
    engine::setVideoFileHooks(hooks);
}

} // namespace videofile
} // namespace tmshaders
