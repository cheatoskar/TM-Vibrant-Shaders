#include "mp4writer.h"
#include "log.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <strmif.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <vector>

#pragma comment(lib, "mfuuid.lib")

namespace tmshaders {
namespace mp4 {
namespace {

// Media Foundation, loaded on first use (no import: the mod must load on Windows N too).
struct Api {
    bool tried = false, ok = false;
    decltype(&MFStartup) startup = nullptr;
    decltype(&MFCreateMediaType) createMediaType = nullptr;
    decltype(&MFCreateSample) createSample = nullptr;
    decltype(&MFCreateMemoryBuffer) createMemoryBuffer = nullptr;
    decltype(&MFCreateAttributes) createAttributes = nullptr;
    decltype(&MFTEnumEx) enumEx = nullptr;
    decltype(&MFCreateSinkWriterFromURL) createSinkWriter = nullptr;
    decltype(&MFCreateSourceReaderFromURL) createSourceReader = nullptr;
    std::string error;
};
Api g_api;

template <typename T>
bool load(HMODULE module, const char* name, T& out) {
    out = reinterpret_cast<T>(GetProcAddress(module, name));
    return out != nullptr;
}

Api& api() {
    if (g_api.tried) return g_api;
    g_api.tried = true;
    HMODULE plat = LoadLibraryW(L"mfplat.dll");
    HMODULE rw = LoadLibraryW(L"mfreadwrite.dll");
    if (!plat || !rw) {
        g_api.error = "Media Foundation is not installed (Windows N: the Media Feature Pack)";
        return g_api;
    }
    if (!load(plat, "MFStartup", g_api.startup) || !load(plat, "MFCreateMediaType", g_api.createMediaType) ||
        !load(plat, "MFCreateSample", g_api.createSample) || !load(plat, "MFCreateMemoryBuffer", g_api.createMemoryBuffer) ||
        !load(plat, "MFCreateAttributes", g_api.createAttributes) || !load(plat, "MFTEnumEx", g_api.enumEx) ||
        !load(rw, "MFCreateSinkWriterFromURL", g_api.createSinkWriter) || !load(rw, "MFCreateSourceReaderFromURL", g_api.createSourceReader)) {
        g_api.error = "Media Foundation functions missing";
        return g_api;
    }
    const HRESULT hr = g_api.startup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        char text[96];
        snprintf(text, sizeof(text), "MFStartup failed (0x%08lx)", static_cast<unsigned long>(hr));
        g_api.error = text;
        return g_api;
    }
    g_api.ok = true;
    return g_api;
}

template <typename T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

std::string hex(HRESULT hr) {
    char text[32];
    snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(hr));
    return text;
}

std::string narrow(const wchar_t* text) {
    if (!text) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], n, nullptr, nullptr);
    return out;
}

// Frame rate as a fraction: whole rates exactly, others (29.97) to a thousandth.
void rational(double fps, UINT32& num, UINT32& den) {
    if (fabs(fps - std::round(fps)) < 1e-4) {
        num = static_cast<UINT32>(std::round(fps));
        den = 1;
    } else {
        num = static_cast<UINT32>(std::round(fps * 1000.0));
        den = 1000;
    }
}

// The video's colours: BT.709 with studio range (what players expect from an H.264 / HEVC MP4).
void setColour(IMFMediaType* type) {
    type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
}

// BGR(A) rows -> NV12 (BT.709, studio range). Chroma from the average of each 2 x 2 block.
void toNV12(const uint8_t* pixels, int stride, int bpp, bool bottomUp, int w, int h, uint8_t* out) {
    uint8_t* yPlane = out;
    uint8_t* uv = out + static_cast<size_t>(w) * h;
    for (int y = 0; y < h; y += 2) {
        const uint8_t* r0 = pixels + static_cast<ptrdiff_t>(bottomUp ? h - 1 - y : y) * stride;
        const uint8_t* r1 = pixels + static_cast<ptrdiff_t>(bottomUp ? h - 2 - y : y + 1) * stride;
        uint8_t* y0 = yPlane + static_cast<size_t>(y) * w;
        uint8_t* y1 = y0 + w;
        uint8_t* c = uv + static_cast<size_t>(y / 2) * w;
        for (int x = 0; x < w; x += 2) {
            const uint8_t* p[4] = {r0 + x * bpp, r0 + (x + 1) * bpp, r1 + x * bpp, r1 + (x + 1) * bpp};
            int sb = 0, sg = 0, sr = 0;
            uint8_t* dst[4] = {y0 + x, y0 + x + 1, y1 + x, y1 + x + 1};
            for (int k = 0; k < 4; k++) {
                const int b = p[k][0], g = p[k][1], r = p[k][2];
                *dst[k] = static_cast<uint8_t>((11966 * r + 40254 * g + 4064 * b + 32768) / 65536 + 16);
                sb += b;
                sg += g;
                sr += r;
            }
            // Sums of four: the coefficients / 4 (>> 18 instead of >> 16).
            const int u = (-6597 * sr - 22187 * sg + 28784 * sb + (1 << 17)) >> 18;
            const int v = (28784 * sr - 26148 * sg - 2637 * sb + (1 << 17)) >> 18;
            c[x] = static_cast<uint8_t>(std::clamp(u + 128, 0, 255));
            c[x + 1] = static_cast<uint8_t>(std::clamp(v + 128, 0, 255));
        }
    }
}

// The name of the encoder the sink writer put in front of stream `stream`, and whether it is
// the graphics card's.
std::string encoderOf(IMFSinkWriter* writer, DWORD stream) {
    IMFSinkWriterEx* ex = nullptr;
    if (FAILED(writer->QueryInterface(IID_PPV_ARGS(&ex)))) return "unknown encoder";
    std::string name = "unknown encoder";
    for (DWORD i = 0;; i++) {
        GUID category{};
        IMFTransform* transform = nullptr;
        if (FAILED(ex->GetTransformForStream(stream, i, &category, &transform))) break;
        if (category == MFT_CATEGORY_VIDEO_ENCODER || category == MFT_CATEGORY_AUDIO_ENCODER) {
            IMFAttributes* attributes = nullptr;
            if (SUCCEEDED(transform->GetAttributes(&attributes)) && attributes) {
                wchar_t* friendly = nullptr;
                UINT32 length = 0;
                if (SUCCEEDED(attributes->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &length))) {
                    name = narrow(friendly);
                    CoTaskMemFree(friendly);
                } else {
                    name = "an encoder without a name";
                }
                UINT32 size = 0;
                if (SUCCEEDED(attributes->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &size))) name += " (hardware)";
                attributes->Release();
            }
        }
        transform->Release();
    }
    ex->Release();
    return name;
}

IMFMediaType* videoType(const GUID& subtype, int w, int h, UINT32 num, UINT32 den) {
    IMFMediaType* type = nullptr;
    if (FAILED(api().createMediaType(&type))) return nullptr;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(type, MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(type, MF_MT_FRAME_RATE, num, den);
    MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    setColour(type);
    return type;
}

bool aacRate(DWORD rate) {
    return rate == 44100 || rate == 48000;
}

IMFMediaType* pcmType(const WAVEFORMATEX& f) {
    IMFMediaType* type = nullptr;
    if (FAILED(api().createMediaType(&type))) return nullptr;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, f.nSamplesPerSec);
    type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, f.nChannels);
    type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, f.nBlockAlign);
    type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, f.nSamplesPerSec * f.nBlockAlign);
    return type;
}

IMFMediaType* aacType(const WAVEFORMATEX& f) {
    IMFMediaType* type = nullptr;
    if (FAILED(api().createMediaType(&type))) return nullptr;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, f.nSamplesPerSec);
    type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, f.nChannels);
    type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000); // 192 kbit/s, the encoder's highest
    type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    return type;
}

bool writeBuffer(IMFSinkWriter* writer, DWORD stream, const void* data, DWORD bytes, LONGLONG time, LONGLONG duration) {
    IMFMediaBuffer* buffer = nullptr;
    IMFSample* sample = nullptr;
    BYTE* p = nullptr;
    bool ok = SUCCEEDED(api().createMemoryBuffer(bytes, &buffer)) && SUCCEEDED(buffer->Lock(&p, nullptr, nullptr));
    if (ok) {
        memcpy(p, data, bytes);
        buffer->Unlock();
        buffer->SetCurrentLength(bytes);
        ok = SUCCEEDED(api().createSample(&sample)) && SUCCEEDED(sample->AddBuffer(buffer)) && SUCCEEDED(sample->SetSampleTime(time)) &&
             SUCCEEDED(sample->SetSampleDuration(duration)) && SUCCEEDED(writer->WriteSample(stream, sample));
    }
    release(sample);
    release(buffer);
    return ok;
}

} // namespace

Writer::~Writer() {
    std::string report;
    if (m_writer || m_pcm != INVALID_HANDLE_VALUE) close(report);
}

bool Writer::openVideo(int width, int height, double fps, Codec codec, double bitrate, std::string& error) {
    if (m_writer) return true;
    Api& mf = api();
    if (!mf.ok) {
        error = mf.error;
        return false;
    }
    if (width < 16 || height < 16 || (width & 1) || (height & 1) || fps <= 0.0) {
        error = "unsupported size or frame rate";
        return false;
    }
    if (!m_comInit) {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        m_comInit = hr == S_OK || hr == S_FALSE; // RPC_E_CHANGED_MODE: the thread has COM already
    }
    m_width = width;
    m_height = height;
    m_fps = fps;
    UINT32 num = 0, den = 0;
    rational(fps, num, den);
    for (int attempt = 0; attempt < 2; attempt++) {
        const Codec c = attempt == 0 ? codec : Codec::H264;
        if (attempt == 1 && codec == Codec::H264) break;
        const bool hevc = c == Codec::HEVC;
        // Bits per pixel and frame: high enough that the game's sharp edges and the effects'
        // fine grain survive (4K at 30 fps: 50 Mbit/s H.264, 30 Mbit/s HEVC).
        double rate = bitrate > 0.0 ? bitrate : static_cast<double>(width) * height * fps * (hevc ? 0.12 : 0.20);
        rate = std::clamp(rate, 2.0e6, hevc ? 200.0e6 : 240.0e6);
        IMFAttributes* attributes = nullptr;
        IMFSinkWriter* writer = nullptr;
        IMFMediaType* out = nullptr;
        IMFMediaType* in = nullptr;
        DWORD stream = 0;
        HRESULT hr = mf.createAttributes(&attributes, 2);
        if (SUCCEEDED(hr)) {
            attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            hr = mf.createSinkWriter(m_path.c_str(), nullptr, attributes, &writer);
        }
        if (SUCCEEDED(hr)) {
            out = videoType(hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264, width, height, num, den);
            in = videoType(MFVideoFormat_NV12, width, height, num, den);
            hr = out && in ? S_OK : E_OUTOFMEMORY;
        }
        if (SUCCEEDED(hr)) {
            out->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(rate));
            out->SetUINT32(MF_MT_MPEG2_PROFILE, hevc ? 1u /* Main */ : static_cast<UINT32>(eAVEncH264VProfile_High));
            in->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(width));
            hr = writer->AddStream(out, &stream);
        }
        if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(stream, in, nullptr);
        if (SUCCEEDED(hr)) {
            // Variable bitrate at the chosen average, and the encoder's best quality (encoders
            // that don't know these keep their defaults).
            ICodecAPI* codecApi = nullptr;
            if (SUCCEEDED(writer->GetServiceForStream(stream, GUID_NULL, IID_PPV_ARGS(&codecApi))) && codecApi) {
                VARIANT v;
                VariantInit(&v);
                v.vt = VT_UI4;
                v.ulVal = eAVEncCommonRateControlMode_UnconstrainedVBR;
                codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
                v.ulVal = static_cast<ULONG>(rate);
                codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
                v.ulVal = 100;
                codecApi->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &v);
                codecApi->Release();
            }
            hr = writer->BeginWriting();
        }
        release(in);
        release(out);
        release(attributes);
        if (SUCCEEDED(hr)) {
            m_writer = writer;
            m_stream = stream;
            m_codec = c;
            m_encoder = encoderOf(writer, stream);
            char text[160];
            snprintf(text, sizeof(text), "%s %dx%d at %.4g fps, %.1f Mbit/s, %s", hevc ? "HEVC" : "H.264", width, height, fps, rate / 1e6,
                     m_encoder.c_str());
            m_encoder = text;
            return true;
        }
        release(writer);
        DeleteFileW(m_path.c_str());
        error += std::string(error.empty() ? "" : "; ") + (hevc ? "HEVC" : "H.264") + " encoder: " + hex(hr) +
                 (hr == MF_E_TOPO_CODEC_NOT_FOUND || hr == MF_E_INVALIDMEDIATYPE ? " (none for this size or format)" : "");
    }
    return false;
}

bool Writer::writeFrame(const uint8_t* pixels, int stride, int bytesPerPixel, bool bottomUp) {
    if (!m_writer || !pixels) return false;
    const DWORD bytes = static_cast<DWORD>(m_width) * m_height * 3 / 2;
    IMFMediaBuffer* buffer = nullptr;
    IMFSample* sample = nullptr;
    BYTE* p = nullptr;
    bool ok = SUCCEEDED(api().createMemoryBuffer(bytes, &buffer)) && SUCCEEDED(buffer->Lock(&p, nullptr, nullptr));
    if (ok) {
        toNV12(pixels, stride, bytesPerPixel, bottomUp, m_width, m_height, p);
        buffer->Unlock();
        buffer->SetCurrentLength(bytes);
        // Times from the frame number (100 ns units): no drift at 29.97 or 59.94.
        const LONGLONG t0 = static_cast<LONGLONG>(std::llround(m_frames * 1e7 / m_fps));
        const LONGLONG t1 = static_cast<LONGLONG>(std::llround((m_frames + 1) * 1e7 / m_fps));
        ok = SUCCEEDED(api().createSample(&sample)) && SUCCEEDED(sample->AddBuffer(buffer)) && SUCCEEDED(sample->SetSampleTime(t0)) &&
             SUCCEEDED(sample->SetSampleDuration(t1 - t0)) && SUCCEEDED(m_writer->WriteSample(m_stream, sample));
    }
    release(sample);
    release(buffer);
    if (ok) m_frames++;
    else if (m_failedWrites++ < 5) TMVS_LOG("mp4: frame %d could not be written", m_frames);
    return ok;
}

void Writer::openAudio(const WAVEFORMATEX& format) {
    if (m_audio) return;
    m_audioFormat = format;
    m_audio = true;
    m_pcmPath = m_path + L".sound.tmp";
    m_pcm = CreateFileW(m_pcmPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (m_pcm == INVALID_HANDLE_VALUE) TMVS_LOG("mp4: cannot keep the sound (%lu), the video stays silent", GetLastError());
}

void Writer::writeAudio(const void* data, size_t bytes) {
    if (m_pcm == INVALID_HANDLE_VALUE || !data || !bytes) return;
    DWORD written = 0;
    if (WriteFile(m_pcm, data, static_cast<DWORD>(bytes), &written, nullptr)) m_pcmBytes += written;
}

void Writer::saveWav(const std::wstring& path) {
    HANDLE wav = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (wav == INVALID_HANDLE_VALUE) return;
    const DWORD data = static_cast<DWORD>(std::min<uint64_t>(m_pcmBytes, 0xFFFFFF00u));
    DWORD written = 0;
    const DWORD fmtSize = 16;
    const DWORD riff = 4 + 8 + fmtSize + 8 + data;
    WriteFile(wav, "RIFF", 4, &written, nullptr);
    WriteFile(wav, &riff, 4, &written, nullptr);
    WriteFile(wav, "WAVEfmt ", 8, &written, nullptr);
    WriteFile(wav, &fmtSize, 4, &written, nullptr);
    WriteFile(wav, &m_audioFormat, fmtSize, &written, nullptr);
    WriteFile(wav, "data", 4, &written, nullptr);
    WriteFile(wav, &data, 4, &written, nullptr);
    SetFilePointer(m_pcm, 0, nullptr, FILE_BEGIN);
    std::vector<char> chunk(1 << 20);
    for (uint64_t left = data; left > 0;) {
        DWORD got = 0;
        if (!ReadFile(m_pcm, chunk.data(), static_cast<DWORD>(std::min<uint64_t>(left, chunk.size())), &got, nullptr) || got == 0) break;
        WriteFile(wav, chunk.data(), got, &written, nullptr);
        left -= got;
    }
    CloseHandle(wav);
}

// The final MP4: the video copied from `video` (no new encode) and the sound encoded to AAC,
// interleaved by time.
bool Writer::remux(const std::wstring& video, std::string& error) {
    Api& mf = api();
    IMFSourceReader* reader = nullptr;
    IMFSinkWriter* writer = nullptr;
    IMFMediaType* native = nullptr;
    IMFMediaType* aac = nullptr;
    IMFMediaType* pcm = nullptr;
    DWORD videoStream = 0, audioStream = 0;
    HRESULT hr = mf.createSourceReader(video.c_str(), nullptr, &reader);
    if (SUCCEEDED(hr)) hr = reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native);
    if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, native);
    if (SUCCEEDED(hr)) hr = mf.createSinkWriter(m_path.c_str(), nullptr, nullptr, &writer);
    if (SUCCEEDED(hr)) hr = writer->AddStream(native, &videoStream);
    if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(videoStream, native, nullptr);
    if (SUCCEEDED(hr)) {
        aac = aacType(m_audioFormat);
        pcm = pcmType(m_audioFormat);
        hr = aac && pcm ? writer->AddStream(aac, &audioStream) : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(audioStream, pcm, nullptr);
    if (SUCCEEDED(hr)) hr = writer->BeginWriting();
    // Sound in pieces of 1/10 s, always a little ahead of the video.
    const DWORD block = m_audioFormat.nBlockAlign;
    const DWORD rate = m_audioFormat.nSamplesPerSec;
    const DWORD piece = std::max<DWORD>(block, (rate / 10) * block);
    std::vector<char> chunk(piece);
    uint64_t audioDone = 0; // bytes
    SetFilePointer(m_pcm, 0, nullptr, FILE_BEGIN);
    const auto audioTime = [&](uint64_t bytes) { return static_cast<LONGLONG>(bytes / block * 10000000ull / rate); };
    const auto writeAudioUntil = [&](LONGLONG until) {
        while (SUCCEEDED(hr) && audioDone < m_pcmBytes && audioTime(audioDone) <= until) {
            DWORD got = 0;
            const DWORD want = static_cast<DWORD>(std::min<uint64_t>(piece, m_pcmBytes - audioDone));
            if (!ReadFile(m_pcm, chunk.data(), want, &got, nullptr) || got < block) {
                audioDone = m_pcmBytes;
                break;
            }
            got -= got % block;
            const LONGLONG t0 = audioTime(audioDone);
            audioDone += got;
            if (!writeBuffer(writer, audioStream, chunk.data(), got, t0, audioTime(audioDone) - t0)) hr = E_FAIL;
        }
    };
    int copied = 0;
    while (SUCCEEDED(hr)) {
        DWORD flags = 0;
        LONGLONG time = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &time, &sample);
        if (FAILED(hr)) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            release(sample);
            break;
        }
        if (!sample) continue;
        writeAudioUntil(time + 5000000);
        if (SUCCEEDED(hr)) hr = writer->WriteSample(videoStream, sample);
        release(sample);
        copied++;
    }
    writeAudioUntil(LLONG_MAX);
    if (SUCCEEDED(hr)) hr = writer->Finalize();
    release(pcm);
    release(aac);
    release(native);
    release(writer);
    release(reader);
    if (FAILED(hr)) {
        error = "muxing the sound in: " + hex(hr);
        return false;
    }
    if (copied < m_frames) {
        error = "muxing the sound in: only " + std::to_string(copied) + " of " + std::to_string(m_frames) + " frames copied";
        return false;
    }
    return true;
}

void Writer::discard() {
    release(m_writer);
    DeleteFileW(m_path.c_str());
    if (m_pcm != INVALID_HANDLE_VALUE) {
        CloseHandle(m_pcm);
        m_pcm = INVALID_HANDLE_VALUE;
        DeleteFileW(m_pcmPath.c_str());
    }
    m_audio = false;
    m_pcmBytes = 0;
    if (m_comInit) {
        CoUninitialize();
        m_comInit = false;
    }
}

bool Writer::close(std::string& report) {
    bool ok = true;
    char text[256];
    if (m_writer) {
        const HRESULT hr = m_frames > 0 ? m_writer->Finalize() : E_FAIL;
        release(m_writer);
        if (FAILED(hr)) {
            ok = false;
            snprintf(text, sizeof(text), "no video written (%d frames, finalize %s)", m_frames, hex(hr).c_str());
            report = text;
            DeleteFileW(m_path.c_str());
        } else {
            snprintf(text, sizeof(text), "%d frames (%.2f s)", m_frames, m_frames / m_fps);
            report = text;
        }
    } else {
        ok = false;
        report = "no video";
    }
    if (m_pcm != INVALID_HANDLE_VALUE) {
        const double seconds = m_audioFormat.nAvgBytesPerSec ? static_cast<double>(m_pcmBytes) / m_audioFormat.nAvgBytesPerSec : 0.0;
        const bool aacFormat = m_audioFormat.wFormatTag == WAVE_FORMAT_PCM && m_audioFormat.wBitsPerSample == 16 && aacRate(m_audioFormat.nSamplesPerSec) &&
                               (m_audioFormat.nChannels == 1 || m_audioFormat.nChannels == 2 || m_audioFormat.nChannels == 6) &&
                               m_audioFormat.nBlockAlign == m_audioFormat.nChannels * 2;
        const std::wstring base = m_path.size() > 4 ? m_path.substr(0, m_path.size() - 4) : m_path;
        if (m_pcmBytes == 0) {
            report += ", no sound came";
        } else if (ok && aacFormat) {
            const std::wstring video = base + L".video.tmp.mp4";
            DeleteFileW(video.c_str());
            std::string error;
            if (MoveFileW(m_path.c_str(), video.c_str()) && remux(video, error)) {
                DeleteFileW(video.c_str());
                snprintf(text, sizeof(text), ", sound %.2f s (AAC %u Hz, %u channels)", seconds, m_audioFormat.nSamplesPerSec, m_audioFormat.nChannels);
                report += text;
            } else {
                ok = false;
                DeleteFileW(m_path.c_str());
                MoveFileW(video.c_str(), m_path.c_str());
                saveWav(base + L".wav");
                report += ", the sound stays beside it as .wav (" + (error.empty() ? std::string("cannot move the video") : error) + ")";
            }
        } else {
            saveWav(base + L".wav");
            snprintf(text, sizeof(text), ", sound %.2f s as .wav beside it (format %u, %u Hz, %u bits, %u channels)", seconds, m_audioFormat.wFormatTag,
                     m_audioFormat.nSamplesPerSec, m_audioFormat.wBitsPerSample, m_audioFormat.nChannels);
            report += text;
            ok = false;
        }
        CloseHandle(m_pcm);
        m_pcm = INVALID_HANDLE_VALUE;
        DeleteFileW(m_pcmPath.c_str());
    }
    if (m_comInit) {
        CoUninitialize();
        m_comInit = false;
    }
    return ok;
}

std::string describeEncoders() {
    Api& mf = api();
    if (!mf.ok) return mf.error;
    // RPC_E_CHANGED_MODE: COM is already there in another mode, fine for enumerating.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::string out;
    const GUID formats[2] = {MFVideoFormat_H264, MFVideoFormat_HEVC};
    const char* names[2] = {"H.264", "HEVC"};
    for (int f = 0; f < 2; f++) {
        MFT_REGISTER_TYPE_INFO info = {MFMediaType_Video, formats[f]};
        IMFActivate** list = nullptr;
        UINT32 count = 0;
        out += out.empty() ? "" : "; ";
        out += names[f];
        out += ":";
        if (FAILED(mf.enumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                             nullptr, &info, &list, &count)) ||
            count == 0) {
            out += " none";
            continue;
        }
        for (UINT32 i = 0; i < count; i++) {
            wchar_t* friendly = nullptr;
            UINT32 length = 0;
            UINT32 flags = 0;
            list[i]->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &flags);
            if (SUCCEEDED(list[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &length))) {
                out += (i ? ", " : " ") + narrow(friendly) + ((flags & MFT_ENUM_FLAG_HARDWARE) ? " (hardware)" : "");
                CoTaskMemFree(friendly);
            }
            list[i]->Release();
        }
        CoTaskMemFree(list);
    }
    if (com == S_OK || com == S_FALSE) CoUninitialize();
    return out;
}

} // namespace mp4
} // namespace tmshaders
