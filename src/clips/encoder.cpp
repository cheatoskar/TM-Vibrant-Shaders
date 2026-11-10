#include "clips/encoder.h"
#include "nvenc.h"
#include <cstdio>

namespace tmshaders {
namespace clips {
namespace {

class NvencEncoder : public PictureEncoder {
public:
    bool open(const EncoderSetup& s, std::string& error) {
        venc::Options o;
        o.codec = venc::Codec::H264;
        o.width = s.width;
        o.height = s.height;
        o.fps = s.fps;
        o.bitrate = s.bitrate;
        o.gopSeconds = s.gopSeconds;
        // Real time next to the game: P4 (twice P7's speed in my measurement, nvenc.cpp), no
        // B-frames (each picture is its frame), no lookahead.
        o.nvencPreset = 4;
        o.nvencBFrames = 0;
        o.nvencLookahead = 0;
        o.nvencAQ = 0;
        o.nvencMultipass = 0;
        if (!m_session.open(o, 0.0, 0, 0.0, error)) return false;
        char text[160];
        snprintf(text, sizeof(text), "NVENC H.264 %dx%d %.0f fps, ", s.width, s.height, s.fps);
        m_description = text + m_session.summary() + ", " + venc::nvenc::support().adapter;
        return true;
    }

    bool encode(const uint8_t* nv12, const PictureSink& sink, std::string& error) override {
        return m_session.encode(nv12, wrap(sink), error);
    }
    bool flush(const PictureSink& sink, std::string& error) override { return m_session.flush(wrap(sink), error); }
    std::string description() const override { return m_description; }
    std::vector<uint8_t> header() const override { return m_session.sequenceHeader(); }

private:
    static venc::nvenc::PacketSink wrap(const PictureSink& sink) {
        return [&sink](const venc::nvenc::Packet& p) {
            EncodedPicture e;
            e.data = p.data;
            e.size = p.size;
            e.frame = p.pts;
            e.key = p.key;
            sink(e);
            return true;
        };
    }
    venc::nvenc::Session m_session;
    std::string m_description;
};

} // namespace

std::unique_ptr<PictureEncoder> openNvenc(const EncoderSetup& setup, std::string& error) {
    auto e = std::make_unique<NvencEncoder>();
    if (!e->open(setup, error)) return nullptr;
    return e;
}

std::unique_ptr<PictureEncoder> openEncoder(const EncoderSetup& setup, std::string& log) {
    std::string why;
    if (setup.choice != 2) {
        if (auto e = openNvenc(setup, why)) return e;
        log += "NVENC: " + why;
        if (setup.choice == 1) return nullptr;
    }
    why.clear();
    if (auto e = openMft(setup, why)) return e;
    log += std::string(log.empty() ? "" : "; ") + "Media Foundation: " + why;
    return nullptr;
}

std::vector<uint8_t> parameterSets(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    size_t i = 0;
    auto startCode = [&](size_t at, size_t& len) {
        if (at + 3 <= size && data[at] == 0 && data[at + 1] == 0 && data[at + 2] == 1) {
            len = 3;
            return true;
        }
        if (at + 4 <= size && data[at] == 0 && data[at + 1] == 0 && data[at + 2] == 0 && data[at + 3] == 1) {
            len = 4;
            return true;
        }
        return false;
    };
    while (i < size) {
        size_t len = 0;
        if (!startCode(i, len)) {
            i++;
            continue;
        }
        const size_t nal = i + len;
        size_t end = nal;
        size_t next = 0;
        while (end < size && !startCode(end, next)) end++;
        if (nal < size) {
            const int type = data[nal] & 0x1f;
            if (type == 7 || type == 8) {
                out.insert(out.end(), {0, 0, 0, 1});
                out.insert(out.end(), data + nal, data + end);
            }
        }
        i = end;
    }
    return out;
}

} // namespace clips
} // namespace tmshaders
