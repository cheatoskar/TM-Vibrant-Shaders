#include "videoencoder.h"
#include "log.h"
#include "nvenc.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <strmif.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#pragma comment(lib, "mfuuid.lib")

namespace tmshaders {
namespace venc {
namespace {

// GUIDs newer than some SDKs (the CI's may be older than mine): defined here with their values.
const GUID kFormatAV1 = {0x31305641 /* 'AV01' */, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kMaxLuminance = {0x50253128, 0xc110, 0x4de4, {0x98, 0xae, 0x46, 0xa3, 0x24, 0xfa, 0xe6, 0xda}};        // MF_MT_MAX_LUMINANCE_LEVEL
const GUID kMaxFrameAverage = {0x58d4bf57, 0x6f52, 0x4733, {0xa1, 0x95, 0xa9, 0xe2, 0x9e, 0xcf, 0x9e, 0x27}};     // MF_MT_MAX_FRAME_AVERAGE_LUMINANCE_LEVEL
const GUID kMaxMastering = {0xd6c6b997, 0x272f, 0x4ca1, {0x8d, 0x00, 0x80, 0x42, 0x11, 0x1a, 0x0f, 0xf6}};        // MF_MT_MAX_MASTERING_LUMINANCE
const GUID kMinMastering = {0x839a4460, 0x4e7e, 0x4b4f, {0xae, 0x79, 0xcc, 0x08, 0x90, 0x5c, 0x7b, 0x27}};        // MF_MT_MIN_MASTERING_LUMINANCE
const GUID kSpatialAQ = {0x659cb943, 0x15ca, 0x448d, {0xb9, 0x9a, 0x87, 0x56, 0x19, 0xdb, 0x4d, 0xe4}};           // CODECAPI_AVEncVideoEnableSpatialAdaptiveQuantization

// ---------------------------------------------------------------------------------------------
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
std::mutex g_apiMutex; // the worker and describeEncoders() may come first at the same time

template <typename T>
bool load(HMODULE module, const char* name, T& out) {
    out = reinterpret_cast<T>(GetProcAddress(module, name));
    return out != nullptr;
}

Api& api() {
    std::lock_guard<std::mutex> lock(g_apiMutex);
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

std::wstring widen(const std::string& text) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &out[0], n);
    return out;
}

std::string strf(const char* fmt, ...) {
    char text[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    return text;
}

std::wstring wstrf(const wchar_t* fmt, ...) {
    wchar_t text[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(text, _countof(text), _TRUNCATE, fmt, args);
    va_end(args);
    return text;
}

double nowMs() {
    static const double scale = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1000.0 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * scale;
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

// The path without its extension (".mp4"), for the files beside it.
std::wstring stem(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    const size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return path;
    return path.substr(0, dot);
}

bool fileExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

uint64_t fileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

const char* codecName(Codec c) {
    return c == Codec::AV1 ? "AV1" : c == Codec::HEVC ? "HEVC" : "H.264";
}

// "HEVC Main10", "H.264 High", "AV1 10-bit": what goes into the description and the log.
std::string profileName(Codec c, bool tenBit) {
    if (c == Codec::H264) return tenBit ? "H.264 High10" : "H.264 High";
    if (c == Codec::HEVC) return tenBit ? "HEVC Main10" : "HEVC Main";
    return tenBit ? "AV1 10-bit" : "AV1 8-bit";
}

// ---------------------------------------------------------------------------------------------
// Colour conversions (on the worker). Everything BT.709 studio range, chroma sited between the
// four pixels it covers (the average of each 2 x 2 block).

// BGR(A) bottom-up rows -> NV12, or P010 with the extra precision the 2 x 2 average gives.
void bgrToYuv(const uint8_t* pixels, int bpp, int w, int h, uint8_t* out, bool p010) {
    const int stride = w * bpp;
    if (!p010) {
        uint8_t* yPlane = out;
        uint8_t* uv = out + static_cast<size_t>(w) * h;
        for (int y = 0; y < h; y += 2) {
            const uint8_t* r0 = pixels + static_cast<ptrdiff_t>(h - 1 - y) * stride;
            const uint8_t* r1 = pixels + static_cast<ptrdiff_t>(h - 2 - y) * stride;
            uint8_t* y0 = yPlane + static_cast<size_t>(y) * w;
            uint8_t* y1 = y0 + w;
            uint8_t* c = uv + static_cast<size_t>(y / 2) * w;
            for (int x = 0; x < w; x += 2) {
                const uint8_t* p[4] = {r0 + x * bpp, r0 + (x + 1) * bpp, r1 + x * bpp, r1 + (x + 1) * bpp};
                uint8_t* dst[4] = {y0 + x, y0 + x + 1, y1 + x, y1 + x + 1};
                int sb = 0, sg = 0, sr = 0;
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
        return;
    }
    uint16_t* yPlane = reinterpret_cast<uint16_t*>(out);
    uint16_t* uv = yPlane + static_cast<size_t>(w) * h;
    for (int y = 0; y < h; y += 2) {
        const uint8_t* r0 = pixels + static_cast<ptrdiff_t>(h - 1 - y) * stride;
        const uint8_t* r1 = pixels + static_cast<ptrdiff_t>(h - 2 - y) * stride;
        uint16_t* y0 = yPlane + static_cast<size_t>(y) * w;
        uint16_t* y1 = y0 + w;
        uint16_t* c = uv + static_cast<size_t>(y / 2) * w;
        for (int x = 0; x < w; x += 2) {
            const uint8_t* p[4] = {r0 + x * bpp, r0 + (x + 1) * bpp, r1 + x * bpp, r1 + (x + 1) * bpp};
            uint16_t* dst[4] = {y0 + x, y0 + x + 1, y1 + x, y1 + x + 1};
            int sb = 0, sg = 0, sr = 0;
            for (int k = 0; k < 4; k++) {
                const int b = p[k][0], g = p[k][1], r = p[k][2];
                // 10 bits: the 8-bit coefficients * 4 (>> 14 instead of >> 16), black at 64.
                *dst[k] = static_cast<uint16_t>((((11966 * r + 40254 * g + 4064 * b + 8192) >> 14) + 64) << 6);
                sb += b;
                sg += g;
                sr += r;
            }
            const int u = (-6597 * sr - 22187 * sg + 28784 * sb + (1 << 15)) >> 16;
            const int v = (28784 * sr - 26148 * sg - 2637 * sb + (1 << 15)) >> 16;
            c[x] = static_cast<uint16_t>(std::clamp(u + 512, 0, 1023) << 6);
            c[x + 1] = static_cast<uint16_t>(std::clamp(v + 512, 0, 1023) << 6);
        }
    }
}

// P010 -> NV12 for an encoder that refused 10 bits. A 2 x 2 ordered dither instead of plain
// rounding: the extra bits the renderer computed turn into fine grain rather than bands.
void p010ToNv12(const uint8_t* in, int w, int h, uint8_t* out) {
    static const int dither[2][2] = {{0, 2}, {3, 1}};
    const uint16_t* src = reinterpret_cast<const uint16_t*>(in);
    for (int y = 0; y < h; y++) {
        const uint16_t* s = src + static_cast<size_t>(y) * w;
        uint8_t* d = out + static_cast<size_t>(y) * w;
        const int* t = dither[y & 1];
        for (int x = 0; x < w; x++) d[x] = static_cast<uint8_t>(std::min(255, ((s[x] >> 6) + t[x & 1]) >> 2));
    }
    const uint16_t* sc = src + static_cast<size_t>(w) * h;
    uint8_t* dc = out + static_cast<size_t>(w) * h;
    for (int y = 0; y < h / 2; y++) {
        const uint16_t* s = sc + static_cast<size_t>(y) * w;
        uint8_t* d = dc + static_cast<size_t>(y) * w;
        const int* t = dither[y & 1];
        for (int x = 0; x < w; x++) d[x] = static_cast<uint8_t>(std::min(255, ((s[x] >> 6) + t[(x >> 1) & 1]) >> 2));
    }
}

// NV12 -> P010: the same values, two bits more (an 8-bit v is the 10-bit 4v).
void nv12ToP010(const uint8_t* in, int w, int h, uint8_t* out) {
    uint16_t* d = reinterpret_cast<uint16_t*>(out);
    const size_t n = static_cast<size_t>(w) * h * 3 / 2;
    for (size_t i = 0; i < n; i++) d[i] = static_cast<uint16_t>(in[i] << 8);
}

void convert(const uint8_t* src, Pixels from, uint8_t* dst, Pixels to, int w, int h) {
    if (from == Pixels::BGR24 || from == Pixels::BGR32) {
        bgrToYuv(src, from == Pixels::BGR32 ? 4 : 3, w, h, dst, to == Pixels::P010);
    } else if (from == Pixels::NV12 && to == Pixels::P010) {
        nv12ToP010(src, w, h, dst);
    } else if (from == Pixels::P010 && to == Pixels::NV12) {
        p010ToNv12(src, w, h, dst);
    } else {
        memcpy(dst, src, Encoder::frameBytes(to, w, h));
    }
}

// ---------------------------------------------------------------------------------------------
// The sound, kept in a temporary file until close().
struct Sound {
    WAVEFORMATEX format{};
    bool open = false;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring tempPath;
    uint64_t bytes = 0;

    double seconds() const { return format.nAvgBytesPerSec ? static_cast<double>(bytes) / format.nAvgBytesPerSec : 0.0; }

    // What Media Foundation's AAC encoder takes.
    bool aacReady() const {
        return format.wFormatTag == WAVE_FORMAT_PCM && format.wBitsPerSample == 16 && (format.nSamplesPerSec == 44100 || format.nSamplesPerSec == 48000) &&
               (format.nChannels == 1 || format.nChannels == 2 || format.nChannels == 6) && format.nBlockAlign == format.nChannels * 2;
    }

    std::string describe() const {
        return strf("%.2f s (format %u, %u Hz, %u bits, %u channels)", seconds(), format.wFormatTag, format.nSamplesPerSec, format.wBitsPerSample,
                      format.nChannels);
    }

    bool saveWav(const std::wstring& path) const {
        if (file == INVALID_HANDLE_VALUE) return false;
        HANDLE wav = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (wav == INVALID_HANDLE_VALUE) return false;
        const DWORD data = static_cast<DWORD>(std::min<uint64_t>(bytes, 0xFFFFFF00u));
        DWORD written = 0;
        const DWORD fmtSize = 16;
        const DWORD riff = 4 + 8 + fmtSize + 8 + data;
        bool ok = WriteFile(wav, "RIFF", 4, &written, nullptr) && WriteFile(wav, &riff, 4, &written, nullptr) &&
                  WriteFile(wav, "WAVEfmt ", 8, &written, nullptr) && WriteFile(wav, &fmtSize, 4, &written, nullptr) &&
                  WriteFile(wav, &format, fmtSize, &written, nullptr) && WriteFile(wav, "data", 4, &written, nullptr) &&
                  WriteFile(wav, &data, 4, &written, nullptr);
        SetFilePointer(file, 0, nullptr, FILE_BEGIN);
        std::vector<char> chunk(1 << 20);
        for (uint64_t left = data; ok && left > 0;) {
            DWORD got = 0;
            if (!ReadFile(file, chunk.data(), static_cast<DWORD>(std::min<uint64_t>(left, chunk.size())), &got, nullptr) || got == 0) break;
            ok = WriteFile(wav, chunk.data(), got, &written, nullptr) != FALSE;
            left -= got;
        }
        CloseHandle(wav);
        return ok;
    }

    void closeAndDelete() {
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
            DeleteFileW(tempPath.c_str());
        }
        open = false;
        bytes = 0;
    }
};

// ---------------------------------------------------------------------------------------------
// A backend: takes frames in the format it chose at open, finishes the file at the end. Only
// the worker thread calls it.
class Sink {
public:
    virtual ~Sink() = default;
    // A buffer for the next frame in the encoder's format, then commit() to encode it.
    virtual uint8_t* begin() = 0;
    virtual bool commit(double& encodeMs) = 0;
    // A frame that is in the encoder's format already.
    virtual bool writeDirect(const uint8_t* frame, size_t bytes, double& encodeMs, double& copyMs) {
        uint8_t* dst = begin();
        if (!dst) return false;
        const double t0 = nowMs();
        memcpy(dst, frame, bytes);
        copyMs = nowMs() - t0;
        return commit(encodeMs);
    }
    // Finalizes the video and muxes the sound in; `report` says what was written.
    virtual bool finish(Sound& sound, int frames, std::string& report) = 0;
    // Gives up: the partial files are deleted.
    virtual void abort() = 0;

    std::string lastError;     // why the last call failed
    std::string description; // codec, size, rate control, encoder
};

// ---------------------------------------------------------------------------------------------
// Media Foundation backend.

const GUID& subtypeOf(Codec c) {
    return c == Codec::AV1 ? kFormatAV1 : c == Codec::HEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264;
}

// The video's colours: BT.709 with studio range (what players expect from an MP4), or HDR10.
void setColour(IMFMediaType* type, const Options& o) {
    if (o.hdr10) {
        type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT2020_10);
        type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT2020);
        type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_2084);
        type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        // Mastering display and content light level: the MPEG-4 sink and some encoders put
        // them into the file; those that don't ignore them.
        type->SetUINT32(kMaxLuminance, static_cast<UINT32>(o.hdrPeakNits));
        type->SetUINT32(kMaxFrameAverage, static_cast<UINT32>(o.hdrAverageNits));
        type->SetUINT32(kMaxMastering, static_cast<UINT32>(o.hdrPeakNits));
        type->SetUINT32(kMinMastering, 50); // 0.005 nits, in 1/10000 nits
        return;
    }
    type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
}

IMFMediaType* videoType(const GUID& subtype, const Options& o, UINT32 num, UINT32 den) {
    IMFMediaType* type = nullptr;
    if (FAILED(api().createMediaType(&type))) return nullptr;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(type, MF_MT_FRAME_SIZE, o.width, o.height);
    MFSetAttributeRatio(type, MF_MT_FRAME_RATE, num, den);
    MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    setColour(type, o);
    return type;
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

// The kept sound into a sink writer's AAC stream, in pieces of 1/10 s, up to a time: called
// between the video samples so the file comes out interleaved.
class SoundFeed {
public:
    // `offset`: added to every time (100 ns units), for a video that starts late.
    SoundFeed(Sound& sound, IMFSinkWriter* writer, DWORD stream, LONGLONG offset = 0)
        : m_sound(sound), m_writer(writer), m_stream(stream), m_block(sound.format.nBlockAlign), m_rate(sound.format.nSamplesPerSec), m_offset(offset) {
        m_piece = std::max<DWORD>(m_block, (m_rate / 10) * m_block);
        m_chunk.resize(m_piece);
        SetFilePointer(sound.file, 0, nullptr, FILE_BEGIN);
    }

    // Every piece that starts at or before `until` (100 ns units). False: a write failed.
    bool until(LONGLONG until) {
        while (m_done < m_sound.bytes && time(m_done) <= until) {
            DWORD got = 0;
            const DWORD want = static_cast<DWORD>(std::min<uint64_t>(m_piece, m_sound.bytes - m_done));
            if (!ReadFile(m_sound.file, m_chunk.data(), want, &got, nullptr) || got < m_block) {
                m_done = m_sound.bytes;
                break;
            }
            got -= got % m_block;
            const LONGLONG t0 = time(m_done);
            m_done += got;
            if (!writeBuffer(m_writer, m_stream, m_chunk.data(), got, t0, time(m_done) - t0)) return false;
        }
        return true;
    }

private:
    LONGLONG time(uint64_t bytes) const { return m_offset + static_cast<LONGLONG>(bytes / m_block * 10000000ull / m_rate); }

    Sound& m_sound;
    IMFSinkWriter* m_writer;
    DWORD m_stream;
    DWORD m_block, m_rate, m_piece = 0;
    LONGLONG m_offset;
    std::vector<char> m_chunk;
    uint64_t m_done = 0; // bytes
};

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
                    // Microsoft's software encoders carry no name on the instance.
                    name = "a software encoder";
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

// How many encoders Windows registers for a format (hardware and software). 0: no point in
// building a sink writer for it.
UINT32 encoderCount(const GUID& subtype) {
    MFT_REGISTER_TYPE_INFO info = {MFMediaType_Video, subtype};
    IMFActivate** list = nullptr;
    UINT32 count = 0;
    if (FAILED(api().enumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                            nullptr, &info, &list, &count))) {
        return 0;
    }
    for (UINT32 i = 0; i < count; i++) list[i]->Release();
    CoTaskMemFree(list);
    return count;
}

// Quality 0..100 -> the rate control's numbers.
int qpFor(int quality, Codec c) {
    // H.264: q100 -> 14, q90 -> 19, q80 -> 24, q60 -> 34. HEVC two higher for about the same
    // look (its QP steps buy more per bit). AV1 MFTs take a q-index (0..255), about 4.2 per
    // HEVC QP step. A constant QP without B-frames and adaptive quantisation (what NVIDIA's MFT
    // takes) spends far more than x264 at the same number: QP 20 made 126 Mbit/s of a 1440p60
    // replay over Stadium's grid floor (my test, 2026-10-21), some 900 MB a minute.
    const double h264 = 14.0 + (100 - std::clamp(quality, 0, 100)) * 0.5;
    if (c == Codec::H264) return std::clamp(static_cast<int>(std::lround(h264)), 0, 51);
    if (c == Codec::HEVC) return std::clamp(static_cast<int>(std::lround(h264 + 2.0)), 0, 51);
    return std::clamp(static_cast<int>(std::lround((h264 + 2.0) * 4.2)), 0, 255);
}

// How the quality slider maps to each encoder's own scale. Calibrated by my measurement
// (tools/encbench, 2026-10-22): 8 s of a 1440p60 replay over Stadium's grid with film grain,
// scored with VMAF against a near-lossless 275 Mbit/s export of the same frames. Hard footage:
// the grain is noise every encoder must spend bits on. The targets: 80 (the default) VMAF 96 or
// more for as few bits as possible, 92 ("Editing") 98 or more, 62 ("Discord") small. Below 80
// the scales fall faster than above it.
double sliderStep(int quality, double above80, double below80) {
    const int q = std::clamp(quality, 0, 100);
    return q >= 80 ? (80 - q) * above80 : (80 - q) * below80;
}

// The graphics card's average bitrate through Media Foundation (the MFT, no B-frames, no AQ):
// bits per pixel and frame, 0.31 for H.264 at quality 80. My measurement of NVIDIA's MFT, peak
// VBR (Mbit/s -> VMAF mean / worst frame): 45.9 -> 91.37 / 80.6 (the old quality 80, 0.2 bit
// per pixel), 61.6 -> 95.15 / 85.7, 82.0 -> 97.76 / 90.5, 102.8 -> 99.09 / 94.3. VMAF 96 needs
// about 68 Mbit/s at 1440p60: quality 80 = 0.31, 92 = 0.39 (87 Mbit/s, about 98), 62 = 0.10
// (23 Mbit/s). AMD's and Intel's MFTs are not measured; they get the same rates. HEVC: 0.9 of
// it (NVIDIA's HEVC MFT: 45.6 -> 92.60, 60.7 -> 95.71, a little ahead of its H.264).
// Checked through the slider (H.264): 62 -> 23.7 Mbit/s, 79.50; 80 -> 70.5, 96.51 / 88.2;
// 92 -> 89.1, 98.28 / 92.0.
double bitrateFor(const Options& o, Codec c) {
    const double bpp = 0.31 * std::exp(-sliderStep(o.quality, 0.02, 0.06)) * (c == Codec::H264 ? 1.0 : 0.9);
    const double rate = bpp * o.width * o.height * o.fps;
    return std::clamp(rate, 2.0e6, c == Codec::H264 ? 240.0e6 : 200.0e6);
}

// NVENC driven directly (P7, no B-frames, no AQ; src/nvenc.cpp has why): quality -> its
// constant quality level (CQ, like a QP, fractions allowed). My measurement, H.264 (CQ ->
// Mbit/s, VMAF mean / worst frame): 21 -> 93.1, 99.16 / 95.5; 22 -> 84.6, 98.70 / 94.2;
// 23 -> 75.3, 97.95 / 92.6; 24 -> 66.5, 97.01 / 91.0; 26 -> 51.4, 94.28 / 88.5; 28 -> 39.8,
// 90.75 / 83.6; 30 -> 31.2, 86.92 / 78.7. For comparison x264 medium CRF 20 (through ffmpeg)
// gave 75.3 / 97.09 at 18 fps, NVENC CQ 23 the same rate at 97.95 and 117 fps. So quality 80 =
// CQ 24.5 (about 62 Mbit/s, VMAF 96.4), 92 = CQ 22.5 (about 80, 98.3), 62 = CQ 30.8 (about 28).
// HEVC (Pascal: no B-frames either) takes the same CQ: 24 -> 63.8, 96.97 / 91.8; 22 -> 81.8,
// 98.67 (and with lookahead 8: 25 -> 56.4, 95.68; 27 -> 44.9, 93.14), a little ahead of H.264
// at the same rate, at 80 fps instead of 120. Checked through the slider: quality 62 -> 27.4
// Mbit/s, VMAF 84.55; 70 -> 39.2, 90.57; 80 -> 61.3, 96.26 / 90.8; 86 -> 69.4, 97.39; 92 ->
// 78.6, 98.26 / 94.1; 100 -> 94.5, 99.15.
double cqFor(const Options& o, Codec c) {
    if (o.encoderQ > 0.0) return o.encoderQ;
    (void)c;
    return std::clamp(24.5 + sliderStep(o.quality, 1.0 / 6.0, 0.35), 1.0, 51.0);
}

// ... and the cap on its bitrate (bits a second), only against a burst of detail blowing up the
// file: twice what Media Foundation would average at that quality (quality 80, 1440p60: 137
// Mbit/s against the 62 the CQ spends on my test).
double capFor(const Options& o, Codec c) {
    if (o.nvencMaxRate >= 0.0) return o.nvencMaxRate * 1e6;
    return 2.0 * bitrateFor(o, c);
}

enum class Mode { Quality, QP, PeakVBR, VBR };

struct Plan {
    Mode mode = Mode::VBR;
    UINT32 quality = 0;
    UINT32 qp = 0;
    double mean = 0.0, peak = 0.0;
    const char* name() const {
        return mode == Mode::Quality ? "constant quality" : mode == Mode::QP ? "constant QP" : mode == Mode::PeakVBR ? "peak-constrained VBR" : "unconstrained VBR";
    }
    UINT32 rcMode() const {
        return mode == Mode::Quality || mode == Mode::QP ? eAVEncCommonRateControlMode_Quality
             : mode == Mode::PeakVBR ? eAVEncCommonRateControlMode_PeakConstrainedVBR
                                     : eAVEncCommonRateControlMode_UnconstrainedVBR;
    }
};

std::vector<Plan> plansFor(const Options& o, Codec c) {
    std::vector<Plan> plans;
    const bool vbr = o.bitrate > 0.0 || o.rateControl == RateControl::VBR;
    const double mean = o.bitrate > 0.0 ? o.bitrate : bitrateFor(o, c);
    Plan quality;
    quality.mode = Mode::Quality;
    quality.quality = static_cast<UINT32>(std::clamp(o.quality, 0, 100));
    quality.mean = mean;
    Plan qp;
    qp.mode = Mode::QP;
    qp.qp = static_cast<UINT32>(qpFor(o.quality, c));
    qp.quality = quality.quality; // for encoders that take only the quality value in this mode
    qp.mean = mean;
    Plan peak;
    peak.mode = Mode::PeakVBR;
    peak.mean = mean;
    peak.peak = mean * (vbr ? 1.5 : 2.0);
    Plan plain = peak;
    plain.mode = Mode::VBR;
    // Auto: an average with room for peaks (twice it). A constant QP is what NVIDIA's MFT offers
    // as its quality mode, without B-frames or adaptive quantisation, and it spent without
    // measure where a replay is detailed: QP 20 made 126 Mbit/s of 1440p60 over Stadium's grid
    // floor, QP 17 with 2 x 2 supersampling 275 Mbit/s (my tests, 2026-10-21). On request only.
    if (!vbr && o.rateControl == RateControl::ConstantQP) plans.push_back(qp);
    if (!vbr && o.rateControl == RateControl::Quality) plans.push_back(quality);
    plans.push_back(peak);
    plans.push_back(plain);
    return plans;
}

// One codec API property: set, read back, and a word for the log about what really happened.
struct PropResult {
    bool took = false;
    std::string text;
};

PropResult setProp(ICodecAPI* codec, const GUID& key, const char* name, VARTYPE vt, ULONGLONG value) {
    PropResult r;
    VARIANT v;
    VariantInit(&v);
    v.vt = vt;
    if (vt == VT_UI8) v.ullVal = value;
    else if (vt == VT_BOOL) v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    else v.ulVal = static_cast<ULONG>(value);
    const HRESULT supported = codec->IsSupported(&key);
    const HRESULT set = codec->SetValue(&key, &v);
    VARIANT got;
    VariantInit(&got);
    const HRESULT get = codec->GetValue(&key, &got);
    ULONGLONG actual = 0;
    if (SUCCEEDED(get)) {
        actual = got.vt == VT_UI8 ? got.ullVal : got.vt == VT_BOOL ? (got.boolVal ? 1u : 0u) : got.vt == VT_UI4 || got.vt == VT_UINT ? got.ulVal
                 : got.vt == VT_I4 || got.vt == VT_INT ? static_cast<ULONGLONG>(got.lVal) : value + 1;
        VariantClear(&got);
    }
    // Read back first: a value the sink writer passed before the media types were set may be
    // refused as a change now but be in effect all the same.
    r.took = SUCCEEDED(get) ? actual == value : SUCCEEDED(set);
    r.text = std::string(name) + "=" + std::to_string(value);
    if (r.took) {
        r.text += SUCCEEDED(set) ? " ok" : " ok (already)";
    } else if (supported != S_OK && FAILED(set)) {
        r.text += " unsupported";
    } else {
        r.text += " refused (set " + hex(set) + (SUCCEEDED(get) ? ", is " + std::to_string(actual) : std::string()) + ")";
    }
    return r;
}

class MfSink : public Sink {
public:
    ~MfSink() override {
        abort();
        for (IMFMediaBuffer* b : m_pool) b->Release();
    }

    // Tries the codec chain and, per codec, the rate controls from best to plainest. `o` comes
    // back with the codec and depth that took effect.
    bool open(const std::wstring& path, Options& o, Pixels& wants, std::string& error) {
        Api& mf = api();
        if (!mf.ok) {
            error = mf.error;
            return false;
        }
        m_path = path;
        m_fps = o.fps;
        rational(o.fps, m_num, m_den);
        struct Attempt {
            Codec codec;
            bool tenBit;
            bool hardware;
        };
        // 10 bits are kept over the codec (they matter more for the renderer's gradients).
        std::vector<Attempt> chain;
        const bool ten = o.tenBit && o.codec != Codec::H264;
        const bool hw = o.hardware;
        if (o.codec == Codec::AV1) {
            if (ten) chain.insert(chain.end(), {{Codec::AV1, true, hw}, {Codec::HEVC, true, hw}});
            chain.insert(chain.end(), {{Codec::AV1, false, hw}, {Codec::HEVC, false, hw}});
        } else if (o.codec == Codec::HEVC) {
            if (ten) chain.push_back({Codec::HEVC, true, hw});
            chain.push_back({Codec::HEVC, false, hw});
        }
        chain.push_back({Codec::H264, false, hw});
        // Last resort: H.264 from Windows' own software encoder, should the graphics card's
        // refuse (a size beyond its limits, a broken driver). A file in any case.
        if (hw) chain.push_back({Codec::H264, false, false});
        if (o.tenBit && o.codec == Codec::H264) TMVS_LOG("venc: H.264 stays 8-bit in Media Foundation");

        bool refusedAll[4] = {};
        for (const Attempt& a : chain) {
            const std::string name = profileName(a.codec, a.tenBit) + (a.hardware == hw ? "" : " (software only)");
            if (refusedAll[static_cast<int>(a.codec)]) continue;
            if (encoderCount(subtypeOf(a.codec)) == 0) {
                TMVS_LOG("venc: %s: Windows has no %s encoder", name.c_str(), codecName(a.codec));
                error += std::string(error.empty() ? "" : "; ") + codecName(a.codec) + ": no encoder";
                refusedAll[static_cast<int>(a.codec)] = true;
                continue;
            }
            Options tried = o;
            tried.codec = a.codec;
            tried.tenBit = a.tenBit;
            tried.hardware = a.hardware;
            const std::vector<Plan> plans = plansFor(tried, a.codec);
            for (size_t i = 0; i < plans.size(); i++) {
                const Plan& plan = plans[i];
                const bool last = i + 1 == plans.size();
                HRESULT hr = create(tried, plan, true);
                if (FAILED(hr)) {
                    // Maybe the encoder refused one of the parameters: once more with its
                    // defaults, which the codec API calls below then change where they can.
                    const HRESULT plain = create(tried, plan, false);
                    if (FAILED(plain)) {
                        TMVS_LOG("venc: %s refused (%s%s)", name.c_str(), hex(plain).c_str(),
                                 plain == MF_E_TOPO_CODEC_NOT_FOUND || plain == MF_E_INVALIDMEDIATYPE ? ": none for this size or format" : "");
                        error += std::string(error.empty() ? "" : "; ") + name + ": " + hex(plain);
                        break;
                    }
                }
                std::string log;
                const bool took = configure(plan, tried, log);
                TMVS_LOG("venc: %s, %s: %s", name.c_str(), m_encoder.c_str(), log.c_str());
                if (!took && !last) {
                    TMVS_LOG("venc: %s: %s not accepted, next", name.c_str(), plan.name());
                    drop();
                    continue;
                }
                hr = m_writer->BeginWriting();
                if (FAILED(hr)) {
                    TMVS_LOG("venc: %s: BeginWriting %s", name.c_str(), hex(hr).c_str());
                    error += std::string(error.empty() ? "" : "; ") + name + ": BeginWriting " + hex(hr);
                    drop();
                    if (last) break;
                    continue;
                }
                o = tried;
                wants = a.tenBit ? Pixels::P010 : Pixels::NV12;
                m_bytes = static_cast<DWORD>(Encoder::frameBytes(wants, o.width, o.height));
                if (o.hdr10 && !a.tenBit) TMVS_LOG("venc: HDR10 in 8 bits: tagged PQ, but expect banding");
                description = strf("%s %dx%d at %.4g fps, ", profileName(a.codec, a.tenBit).c_str(), o.width, o.height, o.fps);
                if (o.bitrate > 0.0) description += strf("%.1f Mbit/s (%s", plan.mean / 1e6, plan.name());
                else description += strf("quality %d (%s", o.quality, plan.name());
                if (plan.mode == Mode::Quality) description += strf(", encoder quality %u", plan.quality);
                else if (plan.mode == Mode::QP) description += strf(", QP %u", plan.qp);
                else if (o.bitrate <= 0.0) description += strf(" %.1f Mbit/s", plan.mean / 1e6);
                if (plan.mode == Mode::PeakVBR) description += strf(", peak %.1f", plan.peak / 1e6);
                description += "), " + m_encoder;
                if (o.hdr10) description += ", HDR10";
                return true;
            }
        }
        return false;
    }

    uint8_t* begin() override {
        release(m_buffer);
        const double t0 = nowMs();
        m_buffer = pooledBuffer();
        m_poolWaitMs = nowMs() - t0;
        BYTE* p = nullptr;
        if (!m_buffer || FAILED(m_buffer->Lock(&p, nullptr, nullptr))) {
            release(m_buffer);
            lastError = strf("out of memory for a frame (%u buffers of %.1f MB in use)", static_cast<unsigned>(m_pool.size()), m_bytes / 1048576.0);
            return nullptr;
        }
        return p;
    }

    bool commit(double& encodeMs) override {
        if (!m_buffer) return false;
        m_buffer->Unlock();
        m_buffer->SetCurrentLength(m_bytes);
        IMFSample* sample = nullptr;
        // Times from the frame number (100 ns units): no drift at 29.97 or 59.94.
        const LONGLONG t0 = static_cast<LONGLONG>(std::llround(m_frames * 1e7 / m_fps));
        const LONGLONG t1 = static_cast<LONGLONG>(std::llround((m_frames + 1) * 1e7 / m_fps));
        HRESULT hr = api().createSample(&sample);
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(m_buffer);
        if (SUCCEEDED(hr)) hr = sample->SetSampleTime(t0);
        if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(t1 - t0);
        if (SUCCEEDED(hr)) {
            const double start = nowMs();
            hr = m_writer->WriteSample(m_stream, sample);
            encodeMs = nowMs() - start + m_poolWaitMs; // waiting for a buffer the encoder still held counts too
        }
        release(sample);
        release(m_buffer);
        if (FAILED(hr)) {
            lastError = strf("frame %d: WriteSample %s", m_frames, hex(hr).c_str());
            return false;
        }
        m_frames++;
        return true;
    }

    bool finish(Sound& sound, int frames, std::string& report) override {
        bool ok = true;
        if (m_writer) {
            const HRESULT hr = frames > 0 ? m_writer->Finalize() : E_FAIL;
            release(m_writer);
            if (FAILED(hr)) {
                ok = false;
                report = strf("no video written (%d frames, finalize %s)", frames, hex(hr).c_str());
                DeleteFileW(m_path.c_str());
            } else {
                report = strf("%d frames (%.2f s), %.1f MB", frames, frames / m_fps, fileSize(m_path) / 1048576.0);
            }
        } else {
            ok = false;
            report = "no video";
        }
        if (!sound.open || sound.file == INVALID_HANDLE_VALUE) return ok;
        const std::wstring base = stem(m_path);
        if (sound.bytes == 0) {
            report += ", no sound came";
        } else if (ok && sound.aacReady()) {
            const std::wstring video = base + L".video.tmp.mp4";
            DeleteFileW(video.c_str());
            std::string why;
            if (MoveFileW(m_path.c_str(), video.c_str()) && remux(video, sound, frames, why)) {
                DeleteFileW(video.c_str());
                report += strf(", sound %.2f s (AAC %u Hz, %u channels)", sound.seconds(), sound.format.nSamplesPerSec, sound.format.nChannels);
            } else {
                ok = false;
                DeleteFileW(m_path.c_str());
                MoveFileW(video.c_str(), m_path.c_str());
                sound.saveWav(base + L".wav");
                report += ", the sound stays beside it as .wav (" + (why.empty() ? std::string("cannot move the video") : why) + ")";
            }
        } else {
            sound.saveWav(base + L".wav");
            report += ", sound " + sound.describe() + " as .wav beside it";
            ok = false;
        }
        return ok;
    }

    void abort() override {
        release(m_buffer);
        if (m_writer) {
            release(m_writer);
            DeleteFileW(m_path.c_str());
        }
    }

private:
    // A sink writer with the encoder for `o` in front, configured for `plan` through the
    // encoding parameters (the sink writer hands them to the encoder before its media types,
    // which some encoders need for the rate control mode) when `params`.
    HRESULT create(const Options& o, const Plan& plan, bool params) {
        drop();
        Api& mf = api();
        IMFAttributes* attributes = nullptr;
        IMFAttributes* encoding = nullptr;
        IMFMediaType* out = nullptr;
        IMFMediaType* in = nullptr;
        HRESULT hr = mf.createAttributes(&attributes, 2);
        if (SUCCEEDED(hr)) {
            attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, o.hardware ? TRUE : FALSE);
            hr = mf.createSinkWriter(m_path.c_str(), nullptr, attributes, &m_writer);
        }
        if (SUCCEEDED(hr)) {
            out = videoType(subtypeOf(o.codec), o, m_num, m_den);
            in = videoType(o.tenBit ? MFVideoFormat_P010 : MFVideoFormat_NV12, o, m_num, m_den);
            hr = out && in ? S_OK : E_OUTOFMEMORY;
        }
        if (SUCCEEDED(hr)) {
            // Some encoders want a bitrate on the output type even in a quality mode.
            out->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(plan.mean));
            UINT32 profile = eAVEncH264VProfile_High;
            if (o.codec == Codec::HEVC) profile = o.tenBit ? eAVEncH265VProfile_Main_420_10 : eAVEncH265VProfile_Main_420_8;
            if (o.codec == Codec::AV1) profile = o.tenBit ? eAVEncAV1VProfile_Main_420_10 : eAVEncAV1VProfile_Main_420_8;
            out->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
            in->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(o.tenBit ? o.width * 2 : o.width));
            hr = m_writer->AddStream(out, &m_stream);
        }
        if (SUCCEEDED(hr) && params && SUCCEEDED(mf.createAttributes(&encoding, 8))) {
            encoding->SetUINT32(CODECAPI_AVEncCommonRateControlMode, plan.rcMode());
            if (plan.mode == Mode::Quality) encoding->SetUINT32(CODECAPI_AVEncCommonQuality, plan.quality);
            if (plan.mode == Mode::QP) encoding->SetUINT64(CODECAPI_AVEncVideoEncodeQP, plan.qp);
            if (plan.mode == Mode::QP) encoding->SetUINT32(CODECAPI_AVEncCommonQuality, plan.quality);
            if (plan.mode == Mode::PeakVBR || plan.mode == Mode::VBR) encoding->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, static_cast<UINT32>(plan.mean));
            if (plan.mode == Mode::PeakVBR) encoding->SetUINT32(CODECAPI_AVEncCommonMaxBitRate, static_cast<UINT32>(plan.peak));
            encoding->SetUINT32(CODECAPI_AVEncMPVGOPSize, gop(o));
            encoding->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 2);
        }
        if (SUCCEEDED(hr)) hr = m_writer->SetInputMediaType(m_stream, in, encoding);
        release(encoding);
        release(in);
        release(out);
        release(attributes);
        if (FAILED(hr)) {
            drop();
            return hr;
        }
        m_encoder = encoderOf(m_writer, m_stream);
        return S_OK;
    }

    // A frame buffer the sink writer and the encoder are done with. A fresh 25 MB buffer per 4K
    // P010 frame ran a 32-bit process out of address space after 94 frames (my test): the
    // sink writer queues samples faster than the encoder takes them. So a fixed set, reused
    // once only the pool holds a reference (the sample and the encoder have let go), which
    // also bounds how far the sink writer runs ahead.
    IMFMediaBuffer* pooledBuffer() {
        if (!m_poolCap) m_poolCap = std::clamp<size_t>((96u << 20) / std::max<DWORD>(m_bytes, 1), 3, 8);
        const double start = nowMs();
        for (int spin = 0;; spin++) {
            for (IMFMediaBuffer* b : m_pool) {
                b->AddRef();
                if (b->Release() == 1) {
                    b->AddRef();
                    return b;
                }
            }
            if (m_pool.size() < m_poolCap) {
                IMFMediaBuffer* b = nullptr;
                if (FAILED(api().createMemoryBuffer(m_bytes, &b))) return nullptr;
                m_pool.push_back(b);
                b->AddRef();
                return b;
            }
            // An encoder that holds every input until it has more (a deep lookahead) would
            // wait forever: after 2 s one buffer more, up to twice the set.
            if (nowMs() - start > 2000.0 && m_poolCap < 16) {
                m_poolCap++;
                TMVS_LOG("venc: the encoder holds every frame buffer, now %u", static_cast<unsigned>(m_poolCap));
                continue;
            }
            Sleep(spin < 10 ? 0 : 1);
        }
    }

    static UINT32 gop(const Options& o) { return static_cast<UINT32>(std::max(1L, std::lround(o.fps * 2.0))); }

    // The rate control and the quality knobs through the encoder's codec API, each checked
    // (set, then read back). False: the plan's rate control did not take.
    bool configure(const Plan& plan, const Options& o, std::string& log) {
        ICodecAPI* codec = nullptr;
        if (FAILED(m_writer->GetServiceForStream(m_stream, GUID_NULL, IID_PPV_ARGS(&codec))) || !codec) {
            log = "no codec API";
            return plan.mode == Mode::VBR; // nothing to check: only the plainest plan is trusted
        }
        std::vector<std::string> parts;
        const auto add = [&](const PropResult& r) {
            parts.push_back(r.text);
            return r.took;
        };
        bool took = add(setProp(codec, CODECAPI_AVEncCommonRateControlMode, "RateControlMode", VT_UI4, plan.rcMode()));
        if (plan.mode == Mode::Quality) took = add(setProp(codec, CODECAPI_AVEncCommonQuality, "Quality", VT_UI4, plan.quality)) && took;
        if (plan.mode == Mode::QP) {
            // The quality value too (not checked), for an encoder that ignores the QP in this
            // mode; the QP last, so it wins where both count.
            add(setProp(codec, CODECAPI_AVEncCommonQuality, "Quality", VT_UI4, plan.quality));
            took = add(setProp(codec, CODECAPI_AVEncVideoEncodeQP, "QP", VT_UI8, plan.qp)) && took;
        }
        if (plan.mode == Mode::PeakVBR || plan.mode == Mode::VBR) add(setProp(codec, CODECAPI_AVEncCommonMeanBitRate, "MeanBitRate", VT_UI4, static_cast<ULONGLONG>(plan.mean)));
        if (plan.mode == Mode::PeakVBR) add(setProp(codec, CODECAPI_AVEncCommonMaxBitRate, "MaxBitRate", VT_UI4, static_cast<ULONGLONG>(plan.peak)));
        // Quality knobs: logged, never fatal (each encoder knows a different subset).
        add(setProp(codec, CODECAPI_AVEncMPVGOPSize, "GOP", VT_UI4, gop(o)));
        add(setProp(codec, CODECAPI_AVEncMPVDefaultBPictureCount, "BFrames", VT_UI4, 2));
        add(setProp(codec, CODECAPI_AVEncCommonLowLatency, "LowLatency", VT_BOOL, 0));
        add(setProp(codec, CODECAPI_AVEncCommonQualityVsSpeed, "QualityVsSpeed", VT_UI4, 100));
        add(setProp(codec, kSpatialAQ, "SpatialAQ", VT_BOOL, 1));
        codec->Release();
        for (size_t i = 0; i < parts.size(); i++) log += (i ? ", " : "") + parts[i];
        // A VBR plan stands even when the encoder keeps its own mode: it is the last resort.
        return took || plan.mode == Mode::VBR;
    }

    void drop() {
        if (m_writer) {
            release(m_writer);
            DeleteFileW(m_path.c_str());
        }
    }

    // The final MP4: the video copied from `video` (no new encode) and the sound encoded to
    // AAC, interleaved by time.
    bool remux(const std::wstring& video, Sound& sound, int frames, std::string& error) {
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
            aac = aacType(sound.format);
            pcm = pcmType(sound.format);
            hr = aac && pcm ? writer->AddStream(aac, &audioStream) : E_OUTOFMEMORY;
        }
        if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(audioStream, pcm, nullptr);
        if (SUCCEEDED(hr)) hr = writer->BeginWriting();
        // Sound in pieces of 1/10 s, always a little ahead of the video.
        SoundFeed feed(sound, writer, audioStream);
        const auto writeAudioUntil = [&](LONGLONG until) {
            if (SUCCEEDED(hr) && !feed.until(until)) hr = E_FAIL;
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
        if (copied < frames) {
            error = "muxing the sound in: only " + std::to_string(copied) + " of " + std::to_string(frames) + " frames copied";
            return false;
        }
        return true;
    }

    std::wstring m_path;
    double m_fps = 0.0;
    UINT32 m_num = 0, m_den = 1;
    IMFSinkWriter* m_writer = nullptr;
    DWORD m_stream = 0;
    DWORD m_bytes = 0;
    IMFMediaBuffer* m_buffer = nullptr;
    std::vector<IMFMediaBuffer*> m_pool;
    size_t m_poolCap = 0;
    double m_poolWaitMs = 0.0;
    int m_frames = 0;
    std::string m_encoder;
};

// ---------------------------------------------------------------------------------------------
// NVENC driven directly (src/nvenc.cpp). The packets go into a temporary elementary stream
// (Annex B, playable on its own should the game die mid-export) with an index in memory, and
// finish() writes the MP4 through Media Foundation's sink writer: the video copied as it came,
// with decode and presentation times (B-frames), the sound encoded to AAC between it.

class NvencSink : public Sink {
public:
    ~NvencSink() override { abort(); }

    // HEVC 10 -> HEVC 8 -> H.264 as the card allows. `o` comes back with what took effect.
    bool open(const std::wstring& path, Options& o, Pixels& wants, std::string& error) {
        const nvenc::Support& s = nvenc::support();
        if (!s.ok) {
            error = s.why;
            return false;
        }
        if (o.codec == Codec::AV1 && s.av1) {
            // The card has AV1, which this backend does not drive: NVIDIA's AV1 MFT does it.
            error = "AV1 goes to the card's Media Foundation encoder";
            return false;
        }
        m_path = path;
        m_fps = o.fps;
        rational(o.fps, m_num, m_den);
        struct Attempt {
            Codec codec;
            bool tenBit;
        };
        std::vector<Attempt> chain;
        if (o.codec != Codec::H264 && s.hevc) {
            if (o.tenBit) chain.push_back({Codec::HEVC, true});
            chain.push_back({Codec::HEVC, false});
        }
        if (s.h264) chain.push_back({Codec::H264, false});
        for (const Attempt& a : chain) {
            Options tried = o;
            tried.codec = a.codec;
            tried.tenBit = a.tenBit;
            const double cq = cqFor(tried, a.codec);
            const double cap = capFor(tried, a.codec);
            auto session = std::make_unique<nvenc::Session>();
            std::string why;
            if (!session->open(tried, cq, qpFor(tried.quality, a.codec), cap, why)) {
                TMVS_LOG("venc: NVENC %s refused: %s", profileName(a.codec, a.tenBit).c_str(), why.c_str());
                error += std::string(error.empty() ? "" : "; ") + "NVENC " + profileName(a.codec, a.tenBit) + ": " + why;
                continue;
            }
            m_temp = stem(path) + (a.codec == Codec::HEVC ? L".video.tmp.hevc" : L".video.tmp.h264");
            m_stream = CreateFileW(m_temp.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (m_stream == INVALID_HANDLE_VALUE) {
                error = strf("cannot write %s (%lu)", narrow(m_temp.c_str()).c_str(), GetLastError());
                return false;
            }
            m_session = std::move(session);
            m_options = tried;
            o = tried;
            wants = a.tenBit ? Pixels::P010 : Pixels::NV12;
            m_bytes = Encoder::frameBytes(wants, o.width, o.height);
            m_scratch.resize(m_bytes);
            description = strf("%s %dx%d at %.4g fps, ", profileName(a.codec, a.tenBit).c_str(), o.width, o.height, o.fps);
            description += o.bitrate > 0.0 ? strf("%.1f Mbit/s (", o.bitrate / 1e6) : strf("quality %d (", o.quality);
            description += m_session->summary() + "), " + s.adapter + strf(" (NVENC direct, API %u.%u)", s.builtApi >> 4, s.builtApi & 15);
            if (o.hdr10) description += a.tenBit ? ", HDR10" : ", HDR10 in 8 bits";
            return true;
        }
        if (chain.empty()) error = "NVENC offers no codec for this";
        return false;
    }

    uint8_t* begin() override { return m_scratch.data(); }

    bool commit(double& encodeMs) override { return encode(m_scratch.data(), encodeMs); }

    bool writeDirect(const uint8_t* frame, size_t, double& encodeMs, double& copyMs) override {
        copyMs = 0.0; // the copy into the encoder's buffer is part of the encode
        return encode(frame, encodeMs);
    }

    bool finish(Sound& sound, int frames, std::string& report) override {
        bool ok = true;
        std::string why;
        if (m_session) {
            if (!m_session->flush([this](const nvenc::Packet& p) { return store(p); }, why)) {
                ok = false;
                report = "the encoder's last frames are lost (" + why + "), ";
            }
            m_header = m_session->sequenceHeader();
            m_session.reset(); // the card's session and buffers go now
        }
        if (m_stream == INVALID_HANDLE_VALUE || m_index.empty() || frames == 0) {
            closeStream(true);
            report += strf("no video written (%d frames)", frames);
            return false;
        }
        const std::wstring base = stem(m_path);
        const bool withSound = sound.open && sound.file != INVALID_HANDLE_VALUE && sound.bytes > 0;
        const bool aac = withSound && sound.aacReady();
        DeleteFileW(m_path.c_str());
        if (!mux(aac ? &sound : nullptr, why)) {
            // The elementary stream stays: ffmpeg or VLC play it, ffmpeg -c copy makes it an MP4.
            DeleteFileW(m_path.c_str());
            const std::wstring raw = base + (m_options.codec == Codec::HEVC ? L".hevc" : L".h264");
            closeStream(false);
            DeleteFileW(raw.c_str());
            const bool kept = MoveFileW(m_temp.c_str(), raw.c_str()) != FALSE;
            report += "the MP4 could not be written (" + why + ")" + (kept ? ", the video stays as " + narrow(raw.c_str()) : std::string());
            if (withSound && sound.saveWav(base + L".wav")) report += ", the sound as .wav beside it";
            return false;
        }
        closeStream(true);
        report += strf("%d frames (%.2f s), %.1f MB", static_cast<int>(m_index.size()), m_index.size() / m_fps, fileSize(m_path) / 1048576.0);
        if (static_cast<int>(m_index.size()) != frames) {
            ok = false;
            report += strf(" (%d frames came in)", frames);
        }
        if (aac) {
            report += strf(", sound %.2f s (AAC %u Hz, %u channels)", sound.seconds(), sound.format.nSamplesPerSec, sound.format.nChannels);
        } else if (withSound) {
            sound.saveWav(base + L".wav");
            report += ", sound " + sound.describe() + " as .wav beside it";
            ok = false;
        } else if (sound.open) {
            report += ", no sound came";
        }
        return ok;
    }

    void abort() override {
        m_session.reset();
        closeStream(true);
    }

private:
    struct Entry {
        uint64_t offset;
        uint32_t size;
        bool key;
        int64_t pts;
    };

    bool encode(const uint8_t* frame, double& encodeMs) {
        const double t0 = nowMs();
        std::string why;
        const bool ok = m_session && m_session->encode(frame, [this](const nvenc::Packet& p) { return store(p); }, why);
        encodeMs = nowMs() - t0;
        if (!ok) {
            lastError = strf("frame %d: %s", m_frames, why.empty() ? "no encoder" : why.c_str());
            return false;
        }
        m_frames++;
        return true;
    }

    bool store(const nvenc::Packet& p) {
        DWORD written = 0;
        if (!WriteFile(m_stream, p.data, p.size, &written, nullptr) || written != p.size) return false;
        m_index.push_back({m_streamBytes, p.size, p.key, p.pts});
        m_streamBytes += p.size;
        return true;
    }

    void closeStream(bool remove) {
        if (m_stream != INVALID_HANDLE_VALUE) {
            CloseHandle(m_stream);
            m_stream = INVALID_HANDLE_VALUE;
        }
        if (remove && !m_temp.empty()) DeleteFileW(m_temp.c_str());
    }

    LONGLONG timeOf(int64_t frame) const { return static_cast<LONGLONG>(std::llround(static_cast<double>(frame) * 1e7 / m_fps)); }

    // The MP4 from the elementary stream (and the sound when given).
    bool mux(Sound* sound, std::string& error) {
        Api& mf = api();
        if (!mf.ok) {
            error = mf.error;
            return false;
        }
        const Codec codec = m_options.codec;
        IMFSinkWriter* writer = nullptr;
        IMFAttributes* attributes = nullptr;
        IMFMediaType* video = nullptr;
        IMFMediaType* aac = nullptr;
        IMFMediaType* pcm = nullptr;
        DWORD videoStream = 0, audioStream = 0;
        HRESULT hr = mf.createAttributes(&attributes, 1);
        // Everything is at hand: no throttling of WriteSample to the pace of the file.
        if (SUCCEEDED(hr)) attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        if (SUCCEEDED(hr)) hr = mf.createSinkWriter(m_path.c_str(), nullptr, attributes, &writer);
        if (SUCCEEDED(hr)) {
            video = videoType(subtypeOf(codec), m_options, m_num, m_den);
            hr = video ? S_OK : E_OUTOFMEMORY;
        }
        if (SUCCEEDED(hr)) {
            if (!m_header.empty()) video->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, m_header.data(), static_cast<UINT32>(m_header.size()));
            const double seconds = std::max(1e-3, m_index.size() / m_fps);
            video->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(std::min(4.0e9, m_streamBytes * 8.0 / seconds)));
            UINT32 profile = eAVEncH264VProfile_High;
            if (codec == Codec::HEVC) profile = m_options.tenBit ? eAVEncH265VProfile_Main_420_10 : eAVEncH265VProfile_Main_420_8;
            video->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
            hr = writer->AddStream(video, &videoStream);
        }
        if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(videoStream, video, nullptr);
        if (SUCCEEDED(hr) && sound) {
            aac = aacType(sound->format);
            pcm = pcmType(sound->format);
            hr = aac && pcm ? writer->AddStream(aac, &audioStream) : E_OUTOFMEMORY;
            if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(audioStream, pcm, nullptr);
        }
        if (SUCCEEDED(hr)) hr = writer->BeginWriting();
        // Decode times run behind the presentation times by the deepest reordering the
        // B-frames made (the k-th picture in decode order shows as frame pts). Media
        // Foundation drops a sample with a negative decode time, so the decode times start at
        // zero and everything shown, the sound too, starts `delay` frames later.
        int64_t delay = 0;
        for (size_t k = 0; k < m_index.size(); k++) delay = std::max<int64_t>(delay, static_cast<int64_t>(k) - m_index[k].pts);
        std::unique_ptr<SoundFeed> feed;
        if (SUCCEEDED(hr) && sound) feed = std::make_unique<SoundFeed>(*sound, writer, audioStream, timeOf(delay));
        std::vector<uint8_t> chunk;
        size_t k = 0;
        for (; SUCCEEDED(hr) && k < m_index.size(); k++) {
            const Entry& e = m_index[k];
            const LONGLONG time = timeOf(e.pts + delay);
            if (feed && !feed->until(time + 5000000)) {
                hr = E_FAIL;
                break;
            }
            chunk.resize(e.size);
            LARGE_INTEGER at;
            at.QuadPart = static_cast<LONGLONG>(e.offset);
            DWORD got = 0;
            if (!SetFilePointerEx(m_stream, at, nullptr, FILE_BEGIN) || !ReadFile(m_stream, chunk.data(), e.size, &got, nullptr) || got != e.size) {
                hr = E_FAIL;
                break;
            }
            IMFMediaBuffer* buffer = nullptr;
            IMFSample* sample = nullptr;
            BYTE* p = nullptr;
            hr = mf.createMemoryBuffer(e.size, &buffer);
            if (SUCCEEDED(hr)) hr = buffer->Lock(&p, nullptr, nullptr);
            if (SUCCEEDED(hr)) {
                memcpy(p, chunk.data(), e.size);
                buffer->Unlock();
                buffer->SetCurrentLength(e.size);
                hr = mf.createSample(&sample);
            }
            if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
            if (SUCCEEDED(hr)) hr = sample->SetSampleTime(time);
            if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(timeOf(e.pts + delay + 1) - time);
            if (SUCCEEDED(hr)) hr = sample->SetUINT32(MFSampleExtension_CleanPoint, e.key ? TRUE : FALSE);
            if (SUCCEEDED(hr)) hr = sample->SetUINT64(MFSampleExtension_DecodeTimestamp, static_cast<UINT64>(timeOf(static_cast<int64_t>(k))));
            if (SUCCEEDED(hr)) hr = writer->WriteSample(videoStream, sample);
            release(sample);
            release(buffer);
        }
        const bool samplesOk = SUCCEEDED(hr);
        if (SUCCEEDED(hr) && feed && !feed->until(LLONG_MAX)) hr = E_FAIL;
        if (SUCCEEDED(hr)) hr = writer->Finalize();
        release(pcm);
        release(aac);
        release(video);
        release(attributes);
        release(writer);
        if (FAILED(hr)) {
            error = "Media Foundation's MP4 writer: " + hex(hr);
            if (!samplesOk && k > 0 && k <= m_index.size()) {
                error += strf(" at picture %d of %d (frame %lld, decode delay %lld)", static_cast<int>(k - 1), static_cast<int>(m_index.size()),
                              static_cast<long long>(m_index[k - 1].pts), static_cast<long long>(delay));
            } else if (samplesOk) {
                error += " (finishing the file)";
            }
            return false;
        }
        return true;
    }

    std::wstring m_path, m_temp;
    double m_fps = 0.0;
    UINT32 m_num = 0, m_den = 1;
    Options m_options;
    std::unique_ptr<nvenc::Session> m_session;
    HANDLE m_stream = INVALID_HANDLE_VALUE;
    uint64_t m_streamBytes = 0;
    std::vector<Entry> m_index;
    std::vector<uint8_t> m_header;
    size_t m_bytes = 0;
    std::vector<uint8_t> m_scratch;
    int m_frames = 0;
};

// ---------------------------------------------------------------------------------------------
// ffmpeg backend: the user's own ffmpeg.exe, fed raw frames through a pipe.

// Starts `cmd` with exactly these handles inherited (std handles may be nullptr: then NUL), no
// console window, below normal priority so the encoder's threads give way to the game's.
bool spawn(const std::wstring& cmd, HANDLE in, HANDLE out, HANDLE err, HANDLE& process) {
    process = nullptr;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (!in) in = nul;
    if (!out) out = nul;
    if (!err) err = nul;
    HANDLE handles[3];
    int n = 0;
    for (HANDLE h : {in, out, err}) {
        if (h && h != INVALID_HANDLE_VALUE && std::find(handles, handles + n, h) == handles + n) handles[n++] = h;
    }
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> buffer(size);
    auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    bool ok = InitializeProcThreadAttributeList(list, 1, 0, &size) &&
              UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, n * sizeof(HANDLE), nullptr, nullptr);
    if (ok) {
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = in;
        si.StartupInfo.hStdOutput = out;
        si.StartupInfo.hStdError = err;
        si.lpAttributeList = list;
        PROCESS_INFORMATION pi{};
        std::wstring line = cmd;
        ok = CreateProcessW(nullptr, &line[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | BELOW_NORMAL_PRIORITY_CLASS,
                            nullptr, nullptr, &si.StartupInfo, &pi) != FALSE;
        if (ok) {
            CloseHandle(pi.hThread);
            process = pi.hProcess;
        }
        DeleteProcThreadAttributeList(list);
    }
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    return ok;
}

// Runs `cmd` to its end; its output (stdout and stderr) into `output` when given, else into
// the file `log` when given. Exit code, or -1 when it could not start or took too long.
int run(const std::wstring& cmd, std::string* output, HANDLE log, DWORD timeoutMs) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (output) {
        if (!CreatePipe(&readEnd, &writeEnd, &sa, 1 << 16)) return -1;
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    }
    HANDLE process = nullptr;
    const bool started = spawn(cmd, nullptr, output ? writeEnd : log, output ? writeEnd : log, process);
    if (writeEnd) CloseHandle(writeEnd);
    if (!started) {
        if (readEnd) CloseHandle(readEnd);
        return -1;
    }
    if (output) {
        char chunk[4096];
        DWORD got = 0;
        while (ReadFile(readEnd, chunk, sizeof(chunk), &got, nullptr) && got) output->append(chunk, got);
        CloseHandle(readEnd);
    }
    DWORD code = static_cast<DWORD>(-1);
    if (WaitForSingleObject(process, timeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(process, &code);
    else TerminateProcess(process, 1);
    CloseHandle(process);
    return static_cast<int>(code);
}

std::wstring quote(const std::wstring& s) {
    return L"\"" + s + L"\"";
}

// The last few lines of ffmpeg's log, for the report when it failed.
std::string logTail(const std::wstring& path, int lines = 4) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "no log";
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    const LONGLONG start = std::max<LONGLONG>(0, size.QuadPart - 4096);
    LARGE_INTEGER pos;
    pos.QuadPart = start;
    SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
    std::string text(static_cast<size_t>(size.QuadPart - start), '\0');
    DWORD got = 0;
    ReadFile(f, &text[0], static_cast<DWORD>(text.size()), &got, nullptr);
    CloseHandle(f);
    text.resize(got);
    std::vector<std::string> found;
    size_t end = text.size();
    while (end > 0 && static_cast<int>(found.size()) < lines) {
        size_t begin = text.find_last_of("\r\n", end - 1);
        begin = begin == std::string::npos ? 0 : begin + 1;
        if (end > begin) found.insert(found.begin(), text.substr(begin, end - begin));
        if (begin == 0) break;
        end = begin - 1;
    }
    std::string out;
    for (const std::string& line : found) out += (out.empty() ? "" : " | ") + line;
    return out.empty() ? "empty log" : out;
}

std::mutex g_ffmpegMutex;
std::map<std::wstring, FfmpegInfo> g_ffmpegCache;

// How quality maps to each software encoder's CRF (the same targets as sliderStep's, 8-bit, the
// "balanced" preset; Mbit/s and VMAF mean / worst frame from my measurement):
//  - x264 medium: CRF 18 -> 100.2, 98.80 / 93.7; 20 -> 75.3, 97.09 / 92.0; 21 -> 64.6,
//    95.81 / 90.4; 22 -> 55.1, 94.23 / 88.5; 24 -> 39.6, 90.23 / 84.5; 27 -> 24.0, 82.25 / 76.3.
//    Quality 80 = CRF 20.5 (about 70 Mbit/s, VMAF 96.4), 92 = 18.7 (about 92, 98.4), 62 = 26.8
//    (about 25, 83). The old scale (80 = CRF 18) spent 100 Mbit/s.
//  - x265 fast: CRF 22 -> 63.7, 90.02 / 82.1; 24 -> 44.7, 85.72 / 77.2. Its psycho-visual
//    tuning costs it dearly in VMAF; 80 = CRF 19 by extrapolation (two points only).
//  - SVT-AV1 preset 7: CRF 32 -> 84.8, 96.66 / 89.5; 36 -> 57.8, 93.86 / 85.9. 80 = CRF 33.
//    The old scale (80 = CRF 24) would have spent well over 100 Mbit/s.
double crfFor(int quality, Codec c) {
    if (c == Codec::H264) return std::clamp(20.5 + sliderStep(quality, 0.15, 0.35), 0.0, 51.0);
    if (c == Codec::HEVC) return std::clamp(19.0 + sliderStep(quality, 0.15, 0.35), 0.0, 51.0);
    return std::clamp(33.0 + sliderStep(quality, 0.33, 0.5), 1.0, 63.0);
}

class FfmpegSink : public Sink {
public:
    ~FfmpegSink() override { abort(); }

    bool open(const std::wstring& path, const FfmpegInfo& ff, Options& o, Pixels& wants, std::string& error) {
        m_exe = ff.path;
        m_path = path;
        m_fps = o.fps;
        const std::wstring base = stem(path);
        // Fragmented MP4 in between (a fragment a second): playable up to the last fragment if
        // ffmpeg dies, and exact frame times (Matroska would round them to milliseconds).
        m_video = base + L".video.tmp.mp4";
        m_log = base + L".ffmpeg.log";
        struct Attempt {
            Codec codec;
            const char* library;
            bool present;
        };
        std::vector<Attempt> chain;
        if (o.codec == Codec::AV1) chain.push_back({Codec::AV1, "libsvtav1", ff.svtav1});
        if (o.codec != Codec::H264) chain.push_back({Codec::HEVC, "libx265", ff.x265});
        chain.push_back({Codec::H264, "libx264", ff.x264});
        for (const Attempt& a : chain) {
            if (!a.present) {
                TMVS_LOG("venc: ffmpeg has no %s (%s)", a.library, codecName(a.codec));
                error += std::string(error.empty() ? "" : "; ") + "ffmpeg: no " + a.library;
                continue;
            }
            for (int depth = 0; depth < (o.tenBit ? 2 : 1); depth++) {
                Options tried = o;
                tried.codec = a.codec;
                tried.tenBit = o.tenBit && depth == 0;
                std::string summary;
                const std::wstring args = encoderArgs(tried, a.library, summary);
                // A short test encode with the same settings first: an unknown option or a
                // libx264 without 10 bits shows here, not after the first real frame.
                UINT32 num = 0, den = 1;
                rational(o.fps, num, den);
                const std::wstring test = quote(m_exe) + L" -hide_banner -nostats -loglevel error -f lavfi -i " +
                                          wstrf(L"color=c=gray:s=%dx%d:r=%u/%u", o.width, o.height, num, den) + L" -frames:v 3 " + args + L" -f null -";
                std::string output;
                const int code = run(test, &output, nullptr, 60000);
                if (code != 0) {
                    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
                    TMVS_LOG("venc: ffmpeg %s refused (exit %d): %s", profileName(a.codec, tried.tenBit).c_str(), code, output.c_str());
                    error += std::string(error.empty() ? "" : "; ") + "ffmpeg " + profileName(a.codec, tried.tenBit) + ": exit " + std::to_string(code);
                    continue;
                }
                if (!start(tried, args, error)) return false;
                o = tried;
                wants = tried.tenBit ? Pixels::P010 : Pixels::NV12;
                m_bytes = Encoder::frameBytes(wants, o.width, o.height);
                m_scratch.resize(m_bytes);
                description = strf("%s %dx%d at %.4g fps, %s, ffmpeg %s (software)", profileName(a.codec, tried.tenBit).c_str(), o.width, o.height, o.fps,
                                     summary.c_str(), a.library);
                if (o.hdr10) description += ", HDR10";
                return true;
            }
        }
        return false;
    }

    uint8_t* begin() override { return m_scratch.data(); }

    bool commit(double& encodeMs) override { return pipeWrite(m_scratch.data(), m_bytes, encodeMs); }

    bool writeDirect(const uint8_t* frame, size_t bytes, double& encodeMs, double& copyMs) override {
        copyMs = 0.0;
        return pipeWrite(frame, bytes, encodeMs);
    }

    bool finish(Sound& sound, int frames, std::string& report) override {
        DWORD code = static_cast<DWORD>(-1);
        if (m_stdin) {
            CloseHandle(m_stdin); // end of input: ffmpeg flushes its encoder and closes the file
            m_stdin = nullptr;
        }
        if (m_process) {
            if (WaitForSingleObject(m_process, 10 * 60 * 1000) != WAIT_OBJECT_0) TerminateProcess(m_process, 1);
            GetExitCodeProcess(m_process, &code);
            CloseHandle(m_process);
            m_process = nullptr;
        }
        closeLog();
        const std::wstring base = stem(m_path);
        if (code != 0 || frames == 0) {
            // Keep what there is: the fragmented MP4 plays up to its last complete fragment.
            report = strf("ffmpeg failed (exit %ld, %d frames): %s", static_cast<long>(code), frames, logTail(m_log).c_str());
            DeleteFileW(m_path.c_str());
            if (frames > 0 && MoveFileW(m_video.c_str(), m_path.c_str())) report += ", the partial video is kept";
            else DeleteFileW(m_video.c_str());
            report += ", ffmpeg's log: " + narrow(m_log.c_str());
            if (sound.open && sound.bytes) {
                sound.saveWav(base + L".wav");
                report += ", the sound as .wav beside it";
            }
            return false;
        }
        // The final file: the video copied (no new encode), faststart, and the sound as AAC.
        // ffmpeg's AAC takes any PCM, so no .wav fallback for odd formats here.
        const bool withSound = sound.open && sound.bytes > 0;
        const std::wstring wav = base + L".sound.tmp.wav";
        if (withSound && !sound.saveWav(wav)) {
            report = "cannot write the sound for ffmpeg";
            MoveFileW(m_video.c_str(), m_path.c_str());
            return false;
        }
        std::wstring cmd = quote(m_exe) + L" -hide_banner -nostats -loglevel warning -y -i " + quote(m_video);
        if (withSound) cmd += L" -i " + quote(wav) + L" -map 0:v:0 -map 1:a:0 -c:v copy -c:a aac -b:a 256k";
        else cmd += L" -map 0:v:0 -c copy";
        if (m_codec == Codec::HEVC) cmd += L" -tag:v hvc1"; // what Apple players and editors look for
        cmd += L" -movflags +faststart " + quote(m_path);
        HANDLE log = openLog(true);
        const int mux = run(cmd, nullptr, log, 10 * 60 * 1000);
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        if (mux != 0) {
            report = strf("%d frames, but muxing failed (exit %d): %s; the video stays as it came", frames, mux, logTail(m_log).c_str());
            DeleteFileW(m_path.c_str());
            MoveFileW(m_video.c_str(), m_path.c_str());
            if (withSound) {
                DeleteFileW((base + L".wav").c_str());
                MoveFileW(wav.c_str(), (base + L".wav").c_str());
                report += ", the sound as .wav beside it";
            }
            return false;
        }
        DeleteFileW(m_video.c_str());
        DeleteFileW(wav.c_str());
        DeleteFileW(m_log.c_str());
        report = strf("%d frames (%.2f s), %.1f MB", frames, frames / m_fps, fileSize(m_path) / 1048576.0);
        if (withSound) report += strf(", sound %.2f s (AAC 256 kbit/s)", sound.seconds());
        else if (sound.open) report += ", no sound came";
        return true;
    }

    void abort() override {
        if (m_stdin) {
            CloseHandle(m_stdin);
            m_stdin = nullptr;
        }
        if (m_process) {
            TerminateProcess(m_process, 1);
            WaitForSingleObject(m_process, 5000);
            CloseHandle(m_process);
            m_process = nullptr;
            closeLog(); // before deleting it: an open handle keeps the file
            DeleteFileW(m_video.c_str());
            DeleteFileW(m_log.c_str());
        }
        closeLog();
    }

private:
    // The encoder half of the command line (after the input), and a summary for the log.
    std::wstring encoderArgs(const Options& o, const char* library, std::string& summary) {
        m_codec = o.codec;
        const int preset = std::clamp(o.ffmpegPreset, 0, 2);
        std::wstring args = L" -an";
        // The frames come untagged through the pipe: setparams says what they are, so no
        // conversion guesses a range or matrix, and the encoder writes the same into the file.
        args += o.hdr10 ? L" -vf setparams=range=tv:color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc"
                        : L" -vf setparams=range=tv:color_primaries=bt709:color_trc=bt709:colorspace=bt709";
        args += L" -c:v " + widen(library);
        if (o.codec == Codec::H264) {
            // My measurement at CRF 20 (1440p60, 12 threads): veryfast 74.6 Mbit/s / VMAF 93.78
            // at 65 fps, faster 74.8 / 96.47 at 39 fps, medium 75.3 / 97.09 at 18-22 fps, slow
            // 75.7 / 97.22 at 12 fps (on a quiet machine; with other work running the speeds
            // halved). The export renders at about 19 fps (2 x 2 samples), so medium about keeps
            // up, slow would slow it for little and slower even more. -tune film gave 66.6 /
            // 96.24 at CRF 21 against 64.6 / 95.81, but its psy-trellis costs speed: left out.
            static const wchar_t* presets[3] = {L"faster", L"medium", L"slow"};
            args += std::wstring(L" -preset ") + presets[preset];
        } else if (o.codec == Codec::HEVC) {
            // x265 is several times slower than x264 at the same preset name (medium: 12 fps
            // at 1080p 10-bit in my test), hence one step faster for "balanced".
            static const wchar_t* presets[3] = {L"veryfast", L"fast", L"slow"};
            args += std::wstring(L" -preset ") + presets[preset];
        } else {
            static const int presets[3] = {10, 7, 5};
            args += wstrf(L" -preset %d", presets[preset]);
        }
        static const char* presetNames[3] = {"fast", "balanced", "best"};
        if (o.bitrate > 0.0) {
            args += wstrf(L" -b:v %lld -maxrate %lld -bufsize %lld", static_cast<long long>(o.bitrate), static_cast<long long>(o.bitrate * 1.5),
                           static_cast<long long>(o.bitrate * 2.0));
            summary = strf("%.1f Mbit/s (VBR, %s preset)", o.bitrate / 1e6, presetNames[preset]);
        } else {
            const double crf = o.encoderQ > 0.0 ? o.encoderQ : crfFor(o.quality, o.codec);
            args += o.codec == Codec::AV1 ? wstrf(L" -crf %d", static_cast<int>(std::lround(crf))) : wstrf(L" -crf %.1f", crf);
            summary = strf("quality %d (CRF %.4g, %s preset)", o.quality, o.codec == Codec::AV1 ? std::round(crf) : crf, presetNames[preset]);
        }
        args += wstrf(L" -g %ld", std::max(1L, std::lround(o.fps * 2.0)));
        args += o.tenBit ? L" -pix_fmt yuv420p10le" : L" -pix_fmt yuv420p";
        args += o.hdr10 ? L" -colorspace bt2020nc -color_primaries bt2020 -color_trc smpte2084 -color_range tv"
                        : L" -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv";
        // HDR10 metadata: BT.2020 primaries, D65, the peak and 0.005 nits.
        if (o.codec == Codec::HEVC) {
            std::wstring params = L"log-level=warning";
            if (o.hdr10) {
                params += wstrf(L":hdr10=1:repeat-headers=1:master-display=G(8500,39850)B(6550,2300)R(35400,14600)WP(15635,16450)L(%d,50):max-cll=%d,%d",
                                 o.hdrPeakNits * 10000, o.hdrPeakNits, o.hdrAverageNits);
            }
            args += L" -x265-params " + params;
        } else if (o.codec == Codec::AV1 && o.hdr10) {
            args += wstrf(L" -svtav1-params mastering-display=G(0.170,0.797)B(0.131,0.046)R(0.708,0.292)WP(0.3127,0.3290)L(%d,0.005):content-light=%d,%d",
                           o.hdrPeakNits, o.hdrPeakNits, o.hdrAverageNits);
        }
        if (!o.ffmpegExtra.empty()) {
            args += L" " + o.ffmpegExtra;
            summary += ", " + narrow(o.ffmpegExtra.c_str());
        }
        return args;
    }

    HANDLE openLog(bool append) {
        SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
        HANDLE h = CreateFileW(m_log.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, &sa, append ? OPEN_ALWAYS : CREATE_ALWAYS, 0, nullptr);
        return h;
    }

    void closeLog() {
        if (m_logFile != INVALID_HANDLE_VALUE) {
            CloseHandle(m_logFile);
            m_logFile = INVALID_HANDLE_VALUE;
        }
    }

    bool start(const Options& o, const std::wstring& args, std::string& error) {
        UINT32 num = 0, den = 1;
        rational(o.fps, num, den);
        const std::wstring cmd = quote(m_exe) + L" -hide_banner -nostats -loglevel info -y -f rawvideo -pix_fmt " + (o.tenBit ? L"p010le" : L"nv12") +
                                 wstrf(L" -video_size %dx%d -framerate %u/%u", o.width, o.height, num, den) + L" -i -" + args +
                                 L" -f mp4 -movflags +frag_keyframe+empty_moov+default_base_moof -frag_duration 1000000 " + quote(m_video);
        SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
        HANDLE readEnd = nullptr;
        // A big pipe: fewer switches between the worker and ffmpeg per frame.
        if (!CreatePipe(&readEnd, &m_stdin, &sa, 4 << 20)) {
            error = "ffmpeg: no pipe";
            return false;
        }
        SetHandleInformation(m_stdin, HANDLE_FLAG_INHERIT, 0);
        m_logFile = openLog(false);
        const bool ok = spawn(cmd, readEnd, m_logFile, m_logFile, m_process);
        CloseHandle(readEnd);
        if (!ok) {
            error = strf("ffmpeg did not start (%lu)", GetLastError());
            CloseHandle(m_stdin);
            m_stdin = nullptr;
            closeLog();
            return false;
        }
        TMVS_LOG("venc: ffmpeg started: %s", narrow(cmd.c_str()).c_str());
        return true;
    }

    bool pipeWrite(const uint8_t* data, size_t bytes, double& encodeMs) {
        if (!m_stdin) return false;
        const double t0 = nowMs();
        while (bytes > 0) {
            DWORD written = 0;
            const DWORD piece = static_cast<DWORD>(std::min<size_t>(bytes, 1u << 30));
            if (!WriteFile(m_stdin, data, piece, &written, nullptr) || written == 0) {
                // The pipe broke: ffmpeg ended. Its last words say why.
                const DWORD why = GetLastError();
                lastError = strf("ffmpeg stopped taking frames (%lu): %s", why, logTail(m_log).c_str());
                CloseHandle(m_stdin);
                m_stdin = nullptr;
                return false;
            }
            data += written;
            bytes -= written;
        }
        encodeMs = nowMs() - t0;
        return true;
    }

    std::wstring m_exe, m_path, m_video, m_log;
    double m_fps = 0.0;
    Codec m_codec = Codec::H264;
    HANDLE m_process = nullptr;
    HANDLE m_stdin = nullptr;
    HANDLE m_logFile = INVALID_HANDLE_VALUE;
    size_t m_bytes = 0;
    std::vector<uint8_t> m_scratch;
};

} // namespace

// ---------------------------------------------------------------------------------------------

FfmpegInfo findFfmpeg(const std::wstring& hint) {
    std::lock_guard<std::mutex> lock(g_ffmpegMutex);
    const auto cached = g_ffmpegCache.find(hint);
    if (cached != g_ffmpegCache.end()) return cached->second;
    FfmpegInfo info;
    std::vector<std::wstring> candidates;
    if (!hint.empty()) candidates.push_back(hint);
    // The data folder: ffmpeg.exe itself, or an unpacked ffmpeg build in a folder there.
    const std::wstring data = log::dataDir();
    candidates.push_back(data + L"\\ffmpeg.exe");
    candidates.push_back(data + L"\\ffmpeg\\ffmpeg.exe");
    candidates.push_back(data + L"\\ffmpeg\\bin\\ffmpeg.exe");
    for (const std::wstring& c : candidates) {
        if (fileExists(c)) {
            info.path = c;
            break;
        }
    }
    if (info.path.empty()) {
        wchar_t found[MAX_PATH];
        if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr)) info.path = found;
    }
    if (!info.path.empty()) {
        // Without -hide_banner: the banner (stderr) has the version, the list (stdout) the encoders.
        std::string output;
        if (run(quote(info.path) + L" -encoders", &output, nullptr, 15000) == 0) {
            info.x264 = output.find(" libx264 ") != std::string::npos;
            info.x265 = output.find(" libx265 ") != std::string::npos;
            info.svtav1 = output.find(" libsvtav1 ") != std::string::npos;
            const size_t v = output.find("ffmpeg version ");
            if (v != std::string::npos) info.version = output.substr(v + 15, output.find_first_of(" \r\n", v + 15) - (v + 15));
        } else {
            TMVS_LOG("venc: %s does not run", narrow(info.path.c_str()).c_str());
            info = FfmpegInfo();
        }
    }
    g_ffmpegCache[hint] = info;
    return info;
}

// ---------------------------------------------------------------------------------------------

struct Encoder::Impl {
    std::wstring path;
    Options req, eff;
    Pixels wants = Pixels::NV12;
    std::string desc;
    bool open = false;

    std::unique_ptr<Sink> sink;
    std::thread worker;
    mutable std::mutex mu;
    std::condition_variable cvWork, cvFree, cvOpen;
    struct Slot {
        std::unique_ptr<uint8_t[]> data;
        size_t capacity = 0;
        Pixels format = Pixels::NV12;
    };
    std::vector<Slot> slots;
    std::deque<int> freeSlots, ready;
    int acquired = -1;
    bool stopping = false, aborting = false;
    bool openDone = false, openOk = false;
    std::string openError;
    std::atomic<bool> failed{false};
    std::string failureText;
    Stats st;
    double firstAt = 0.0; // the worker took the first frame (worker only)
    bool closeOk = false;
    std::string closeReport;

    std::mutex audioMu;
    Sound sound;

    // The worker: opens the backend, encodes what comes, finishes on close().
    void run() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::string error;
        const bool ok = openSink(error);
        {
            std::lock_guard<std::mutex> lock(mu);
            openDone = true;
            openOk = ok;
            openError = error;
        }
        cvOpen.notify_all();
        if (ok) {
            for (;;) {
                int index = -1;
                {
                    std::unique_lock<std::mutex> lock(mu);
                    cvWork.wait(lock, [&] { return !ready.empty() || stopping || aborting; });
                    if (aborting || ready.empty()) break;
                    index = ready.front();
                    ready.pop_front();
                }
                encode(slots[index]);
                {
                    std::lock_guard<std::mutex> lock(mu);
                    freeSlots.push_back(index);
                }
                cvFree.notify_one();
            }
            if (aborting) {
                sink->abort();
            } else {
                const double t0 = nowMs();
                std::string report;
                bool done;
                {
                    std::lock_guard<std::mutex> lock(audioMu);
                    done = sink->finish(sound, st.frames, report);
                }
                std::lock_guard<std::mutex> lock(mu);
                st.finishMs = nowMs() - t0;
                if (failed) {
                    done = false;
                    report = "stopped after " + failureText + "; " + report;
                }
                closeOk = done;
                closeReport = report;
            }
            sink.reset();
        }
        if (com == S_OK || com == S_FALSE) CoUninitialize();
    }

    bool openSink(std::string& error) {
        Options o = req;
        if (o.backend == Backend::Ffmpeg) {
            const FfmpegInfo ff = findFfmpeg(o.ffmpegPath);
            if (ff.path.empty()) {
                TMVS_LOG("venc: no ffmpeg.exe (data folder or PATH): Media Foundation instead");
                error = "no ffmpeg; ";
            } else {
                auto s = std::make_unique<FfmpegSink>();
                std::string why;
                if (s->open(path, ff, o, wants, why)) {
                    sink = std::move(s);
                    eff = o;
                    desc = sink->description;
                    return true;
                }
                TMVS_LOG("venc: ffmpeg could not encode (%s): Media Foundation instead", why.c_str());
                error = "ffmpeg: " + why + "; ";
            }
            o = req;
            o.backend = Backend::MediaFoundation;
        }
        if (o.hardware && o.nvenc) {
            // NVIDIA's encoder directly: B-frames, adaptive quantisation and lookahead, which
            // its MFT does not take. Anything wrong (no NVIDIA card, an old driver, no free
            // session) and the MFT below takes over; the reason is in the log once.
            auto nv = std::make_unique<NvencSink>();
            Options tried = o;
            std::string why;
            if (nv->open(path, tried, wants, why)) {
                sink = std::move(nv);
                eff = tried;
                desc = sink->description;
                return true;
            }
            TMVS_LOG("venc: NVENC direct: %s; Media Foundation's encoder instead", why.c_str());
        }
        auto s = std::make_unique<MfSink>();
        std::string why;
        if (!s->open(path, o, wants, why)) {
            error += why;
            return false;
        }
        sink = std::move(s);
        eff = o;
        desc = sink->description;
        return true;
    }

    void encode(Slot& s) {
        if (firstAt == 0.0) firstAt = nowMs();
        double convertMs = 0.0, encodeMs = 0.0;
        bool ok = false;
        if (!failed) {
            const int w = eff.width, h = eff.height;
            if (s.format == wants) {
                ok = sink->writeDirect(s.data.get(), Encoder::frameBytes(wants, w, h), encodeMs, convertMs);
            } else {
                uint8_t* dst = sink->begin();
                const double t0 = nowMs();
                if (dst) {
                    convert(s.data.get(), s.format, dst, wants, w, h);
                    convertMs = nowMs() - t0;
                    ok = sink->commit(encodeMs);
                }
            }
        }
        std::lock_guard<std::mutex> lock(mu);
        if (ok) {
            st.frames++;
            st.encodeMs += encodeMs;
            st.convertMs += convertMs;
            st.encodeMaxMs = std::max(st.encodeMaxMs, encodeMs);
            st.wallMs = nowMs() - firstAt;
        } else {
            st.lostFrames++;
            if (!failed) {
                failureText = sink->lastError.empty() ? "an encoder error" : sink->lastError;
                failed = true;
                TMVS_LOG("venc: the encoder stopped: %s", failureText.c_str());
            }
        }
    }

    // Sound that came, with no video to put it in: kept as .wav as before.
    std::string soundOnly() {
        std::lock_guard<std::mutex> lock(audioMu);
        std::string out;
        if (sound.open && sound.bytes > 0 && !path.empty() && sound.saveWav(stem(path) + L".wav")) out = ", the sound kept as .wav";
        sound.closeAndDelete();
        return out;
    }

    std::string summary() const {
        const Stats& s = st;
        const double n = std::max(1, s.frames);
        std::string out = strf("%d frames in %.1f s (%.1f fps), encoder %.2f ms/frame (slowest %.1f), conversion %.2f ms/frame, ", s.frames,
                                 s.wallMs / 1000.0, s.wallMs > 0.0 ? s.frames * 1000.0 / s.wallMs : 0.0, s.encodeMs / n, s.encodeMaxMs, s.convertMs / n);
        if (s.waits) out += strf("the game waited %d times (%.0f ms in all, longest %.1f)", s.waits, s.waitMs, s.waitMaxMs);
        else out += "the game never waited";
        out += strf(", queue peak %d of %d, opening %.2f s, finishing %.2f s", s.queuePeak, s.queueSlots, s.openMs / 1000.0, s.finishMs / 1000.0);
        if (s.lostFrames) out += strf(", %d frames lost", s.lostFrames);
        return out;
    }
};

Encoder::Encoder() : m(std::make_unique<Impl>()) {}

Encoder::~Encoder() {
    if (m->open) {
        std::string report;
        close(report);
        TMVS_LOG("venc: closed on destruction: %s", report.c_str());
    } else {
        std::lock_guard<std::mutex> lock(m->audioMu);
        m->sound.closeAndDelete();
    }
}

void Encoder::setPath(const std::wstring& path) {
    m->path = path;
}

const std::wstring& Encoder::path() const {
    return m->path;
}

size_t Encoder::frameBytes(Pixels format, int width, int height) {
    const size_t pixels = static_cast<size_t>(width) * height;
    switch (format) {
    case Pixels::NV12: return pixels * 3 / 2;
    case Pixels::P010: return pixels * 3;
    case Pixels::BGR24: return pixels * 3;
    case Pixels::BGR32: return pixels * 4;
    }
    return 0;
}

size_t Encoder::frameBytes(Pixels format) const {
    return frameBytes(format, m->eff.width, m->eff.height);
}

bool Encoder::open(const std::wstring& path, const Options& o, std::string& error) {
    if (m->open) {
        error = "already open";
        return false;
    }
    if (o.width < 16 || o.height < 16 || (o.width & 1) || (o.height & 1) || !(o.fps > 0.0) || o.fps > 1000.0) {
        error = "unsupported size or frame rate";
        return false;
    }
    if (o.codec != Codec::H264 && o.codec != Codec::HEVC && o.codec != Codec::AV1) {
        error = "unknown codec";
        return false;
    }
    if (m->worker.joinable()) m->worker.join();
    m->path = path;
    m->req = o;
    m->eff = o;
    m->wants = Pixels::NV12;
    m->desc.clear();
    m->slots.clear();
    m->freeSlots.clear();
    m->ready.clear();
    m->acquired = -1;
    m->stopping = m->aborting = false;
    m->openDone = m->openOk = false;
    m->openError.clear();
    m->failed = false;
    m->failureText.clear();
    m->st = Stats();
    m->closeOk = false;
    m->closeReport.clear();
    m->firstAt = 0.0;
    const double t0 = nowMs();
    m->worker = std::thread([this] { m->run(); });
    {
        std::unique_lock<std::mutex> lock(m->mu);
        m->cvOpen.wait(lock, [&] { return m->openDone; });
        m->st.openMs = nowMs() - t0;
    }
    if (!m->openOk) {
        m->worker.join();
        error = m->openError;
        return false;
    }
    // The pool: up to 8 frames, fewer when large (a 32-bit game has little address space; at 4K
    // 3, sized for a BGRA frame from the game). Buffers are allocated on first use.
    const size_t largest = std::max(frameBytes(m->wants, o.width, o.height), frameBytes(Pixels::BGR32, o.width, o.height));
    const int count = static_cast<int>(std::clamp<size_t>((96u << 20) / largest, 3, 8));
    {
        std::lock_guard<std::mutex> lock(m->mu);
        m->slots.resize(count);
        for (int i = 0; i < count; i++) m->freeSlots.push_back(i);
        m->st.queueSlots = count;
    }
    m->open = true;
    TMVS_LOG("venc: %s -> %s (%d buffers)", m->desc.c_str(), narrow(path.c_str()).c_str(), count);
    return true;
}

bool Encoder::isOpen() const {
    return m->open;
}

Pixels Encoder::wants() const {
    return m->wants;
}

const Options& Encoder::options() const {
    return m->eff;
}

uint8_t* Encoder::acquire(Pixels format) {
    if (!m->open || m->failed) return nullptr;
    const size_t bytes = frameBytes(format, m->eff.width, m->eff.height);
    int index = m->acquired;
    if (index < 0) {
        std::unique_lock<std::mutex> lock(m->mu);
        if (m->freeSlots.empty()) {
            // Back-pressure: the encoder is behind by the whole pool.
            const double t0 = nowMs();
            m->cvFree.wait(lock, [&] { return !m->freeSlots.empty(); });
            const double waited = nowMs() - t0;
            m->st.waits++;
            m->st.waitMs += waited;
            m->st.waitMaxMs = std::max(m->st.waitMaxMs, waited);
        }
        index = m->freeSlots.front();
        m->freeSlots.pop_front();
        m->acquired = index;
    }
    Impl::Slot& s = m->slots[index];
    if (s.capacity < bytes) {
        s.data.reset(new (std::nothrow) uint8_t[bytes]);
        s.capacity = s.data ? bytes : 0;
        if (!s.data) {
            std::lock_guard<std::mutex> lock(m->mu);
            m->freeSlots.push_back(index);
            m->acquired = -1;
            m->st.lostFrames++;
            TMVS_LOG("venc: out of memory for a frame buffer (%zu bytes)", bytes);
            return nullptr;
        }
    }
    s.format = format;
    return s.data.get();
}

void Encoder::submit() {
    if (m->acquired < 0) return;
    {
        std::lock_guard<std::mutex> lock(m->mu);
        m->ready.push_back(m->acquired);
        m->acquired = -1;
        m->st.queuePeak = std::max(m->st.queuePeak, static_cast<int>(m->ready.size()));
    }
    m->cvWork.notify_one();
}

bool Encoder::pushBgr(const uint8_t* pixels, int bytesPerPixel) {
    if (!pixels || (bytesPerPixel != 3 && bytesPerPixel != 4)) return false;
    const Pixels format = bytesPerPixel == 4 ? Pixels::BGR32 : Pixels::BGR24;
    uint8_t* p = acquire(format);
    if (!p) return false;
    memcpy(p, pixels, frameBytes(format, m->eff.width, m->eff.height));
    submit();
    return true;
}

void Encoder::openAudio(const WAVEFORMATEX& format) {
    std::lock_guard<std::mutex> lock(m->audioMu);
    Sound& s = m->sound;
    if (s.open) return;
    s.format = format;
    s.format.cbSize = 0;
    s.open = true;
    s.bytes = 0;
    // A unique temporary file beside the video (or in %TEMP% before the path is known): two
    // exports or two game instances never share one.
    std::wstring dir = m->path;
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        wchar_t temp[MAX_PATH];
        dir = GetTempPathW(MAX_PATH, temp) ? temp : L".";
    } else {
        dir = dir.substr(0, slash);
    }
    wchar_t name[MAX_PATH];
    if (GetTempFileNameW(dir.c_str(), L"tmv", 0, name)) {
        s.tempPath = name;
        s.file = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    }
    if (s.file == INVALID_HANDLE_VALUE) TMVS_LOG("venc: cannot keep the sound (%lu), the video stays silent", GetLastError());
}

void Encoder::writeAudio(const void* data, size_t bytes) {
    std::lock_guard<std::mutex> lock(m->audioMu);
    Sound& s = m->sound;
    if (s.file == INVALID_HANDLE_VALUE || !data || !bytes) return;
    DWORD written = 0;
    if (WriteFile(s.file, data, static_cast<DWORD>(bytes), &written, nullptr)) s.bytes += written;
}

bool Encoder::close(std::string& report) {
    if (!m->open) {
        report = "no video" + m->soundOnly();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m->mu);
        if (m->acquired >= 0) {
            m->freeSlots.push_back(m->acquired); // acquired, never submitted
            m->acquired = -1;
        }
        m->stopping = true;
    }
    m->cvWork.notify_all();
    m->worker.join();
    m->open = false;
    {
        std::lock_guard<std::mutex> lock(m->audioMu);
        m->sound.closeAndDelete();
    }
    std::lock_guard<std::mutex> lock(m->mu);
    report = m->closeReport + "; " + m->desc + "; " + m->summary();
    return m->closeOk;
}

void Encoder::discard() {
    if (m->open) {
        {
            std::lock_guard<std::mutex> lock(m->mu);
            m->aborting = true;
        }
        m->cvWork.notify_all();
        m->worker.join();
        m->open = false;
    }
    if (!m->path.empty()) DeleteFileW(m->path.c_str());
    std::lock_guard<std::mutex> lock(m->audioMu);
    m->sound.closeAndDelete();
}

bool Encoder::failed() const {
    return m->failed;
}

std::string Encoder::failure() const {
    std::lock_guard<std::mutex> lock(m->mu);
    return m->failureText;
}

std::string Encoder::description() const {
    return m->desc;
}

Stats Encoder::stats() const {
    std::lock_guard<std::mutex> lock(m->mu);
    return m->st;
}

int Encoder::framesWritten() const {
    std::lock_guard<std::mutex> lock(m->mu);
    return m->st.frames;
}

std::string describeEncoders(const std::wstring& ffmpegPath) {
    std::string out;
    Api& mf = api();
    if (!mf.ok) {
        out = mf.error;
    } else {
        // RPC_E_CHANGED_MODE: COM is already there in another mode, fine for enumerating.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const Codec codecs[3] = {Codec::H264, Codec::HEVC, Codec::AV1};
        for (Codec c : codecs) {
            MFT_REGISTER_TYPE_INFO info = {MFMediaType_Video, subtypeOf(c)};
            IMFActivate** list = nullptr;
            UINT32 count = 0;
            out += out.empty() ? "" : "; ";
            out += codecName(c);
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
    }
    const nvenc::Support& nv = nvenc::support();
    if (nv.ok) {
        out += strf("; NVENC direct: %s, API %u.%u (the driver's %u.%u):%s%s%s", nv.adapter.c_str(), nv.builtApi >> 4, nv.builtApi & 15, nv.driverApi >> 4,
                    nv.driverApi & 15, nv.h264 ? " H.264" : "", nv.hevc ? " HEVC" : "", nv.av1 ? " AV1 (left to the MFT)" : "");
    } else {
        out += "; NVENC direct: no (" + nv.why + ")";
    }
    const FfmpegInfo ff = findFfmpeg(ffmpegPath);
    if (ff.path.empty()) {
        out += "; ffmpeg: not found";
    } else {
        out += "; ffmpeg " + ff.version + " (" + narrow(ff.path.c_str()) + "):";
        if (ff.x264) out += " libx264";
        if (ff.x265) out += " libx265";
        if (ff.svtav1) out += " libsvtav1";
        if (!ff.x264 && !ff.x265 && !ff.svtav1) out += " none of libx264, libx265, libsvtav1";
    }
    return out;
}

} // namespace venc
} // namespace tmshaders
