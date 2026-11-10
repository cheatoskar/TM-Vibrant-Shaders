// The clips' encoder for cards without NVENC (AMD, Intel): Media Foundation's H.264 encoder MFT of
// the graphics card, driven directly so its pictures land in the clips' buffer (the video export's
// sink writer writes a file). Hardware MFTs are asynchronous (they say when they want a frame and
// when a picture is ready), Windows' software one is synchronous: both are handled.
#include "clips/encoder.h"
#include "log.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <codecapi.h>
#include <strmif.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "strmiids.lib")

namespace tmshaders {
namespace clips {
namespace {

struct Mf {
    bool tried = false, ok = false;
    decltype(&MFStartup) startup = nullptr;
    decltype(&MFCreateMediaType) createMediaType = nullptr;
    decltype(&MFCreateSample) createSample = nullptr;
    decltype(&MFCreateMemoryBuffer) createMemoryBuffer = nullptr;
    decltype(&MFTEnumEx) enumEx = nullptr;
    std::string error;
};

Mf& mf() {
    static Mf s;
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);
    if (s.tried) return s;
    s.tried = true;
    HMODULE plat = LoadLibraryW(L"mfplat.dll");
    if (!plat) {
        s.error = "Media Foundation is not installed (Windows N: the Media Feature Pack)";
        return s;
    }
    s.startup = reinterpret_cast<decltype(s.startup)>(GetProcAddress(plat, "MFStartup"));
    s.createMediaType = reinterpret_cast<decltype(s.createMediaType)>(GetProcAddress(plat, "MFCreateMediaType"));
    s.createSample = reinterpret_cast<decltype(s.createSample)>(GetProcAddress(plat, "MFCreateSample"));
    s.createMemoryBuffer = reinterpret_cast<decltype(s.createMemoryBuffer)>(GetProcAddress(plat, "MFCreateMemoryBuffer"));
    s.enumEx = reinterpret_cast<decltype(s.enumEx)>(GetProcAddress(plat, "MFTEnumEx"));
    if (!s.startup || !s.createMediaType || !s.createSample || !s.createMemoryBuffer || !s.enumEx) {
        s.error = "Media Foundation functions missing";
        return s;
    }
    if (FAILED(s.startup(MF_VERSION, MFSTARTUP_FULL))) {
        s.error = "MFStartup failed";
        return s;
    }
    s.ok = true;
    return s;
}

template <typename T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

std::string hex(HRESULT hr) {
    char text[16];
    snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(hr));
    return text;
}

class MftEncoder : public PictureEncoder {
public:
    ~MftEncoder() override {
        if (m_mft) {
            m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        }
        release(m_events);
        release(m_mft);
        if (m_activate) m_activate->ShutdownObject();
        release(m_activate);
        if (m_com) CoUninitialize();
    }

    bool open(const EncoderSetup& s, std::string& error) {
        m_com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        Mf& api = mf();
        if (!api.ok) {
            error = api.error;
            return false;
        }
        m_width = s.width;
        m_height = s.height;
        m_fps = s.fps;
        MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** list = nullptr;
        UINT32 count = 0;
        UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
        if (s.softwareOnly) flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;
        HRESULT hr = api.enumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &out, &list, &count);
        if (FAILED(hr) || count == 0) {
            error = s.softwareOnly ? "no software H.264 encoder" : "no hardware H.264 encoder";
            if (list) CoTaskMemFree(list);
            return false;
        }
        std::string refusals;
        for (UINT32 i = 0; i < count && !m_mft; i++) {
            std::string why;
            if (tryActivate(list[i], s, why)) {
                m_activate = list[i];
                m_activate->AddRef();
            } else {
                refusals += (refusals.empty() ? "" : "; ") + why;
            }
        }
        for (UINT32 i = 0; i < count; i++) list[i]->Release();
        CoTaskMemFree(list);
        if (!m_mft) {
            error = refusals;
            return false;
        }
        return true;
    }

    bool encode(const uint8_t* nv12, const PictureSink& sink, std::string& error) override {
        const DWORD bytes = static_cast<DWORD>(m_width * m_height * 3 / 2);
        IMFMediaBuffer* buffer = nullptr;
        IMFSample* sample = nullptr;
        BYTE* dst = nullptr;
        HRESULT hr = mf().createMemoryBuffer(bytes, &buffer);
        if (SUCCEEDED(hr)) hr = buffer->Lock(&dst, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            memcpy(dst, nv12, bytes);
            buffer->Unlock();
            buffer->SetCurrentLength(bytes);
            hr = mf().createSample(&sample);
        }
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
        const LONGLONG duration = static_cast<LONGLONG>(1e7 / m_fps);
        if (SUCCEEDED(hr)) hr = sample->SetSampleTime(m_sent * duration);
        if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(duration);
        bool ok = SUCCEEDED(hr);
        if (!ok) error = "a sample: " + hex(hr);
        if (ok && m_async) {
            // Its wish for a frame first; pictures that come meanwhile go out.
            while (ok && m_needInput == 0) ok = pump(true, sink, error);
            if (ok) m_needInput--;
        }
        if (ok) {
            hr = m_mft->ProcessInput(0, sample, 0);
            if (hr == MF_E_NOTACCEPTING && !m_async) {
                ok = drainSync(sink, error);
                if (ok) hr = m_mft->ProcessInput(0, sample, 0);
            }
            if (ok && FAILED(hr)) {
                error = "ProcessInput: " + hex(hr);
                ok = false;
            }
        }
        if (ok) m_sent++;
        if (ok) ok = m_async ? pump(false, sink, error) : drainSync(sink, error);
        release(sample);
        release(buffer);
        return ok;
    }

    bool flush(const PictureSink& sink, std::string& error) override {
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        HRESULT hr = m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        if (FAILED(hr)) {
            error = "drain: " + hex(hr);
            return false;
        }
        if (!m_async) return drainSync(sink, error);
        m_drained = false;
        const DWORD until = GetTickCount() + 3000;
        while (!m_drained && GetTickCount() < until) {
            if (!pump(true, sink, error)) return false;
        }
        return true;
    }

    std::string description() const override { return m_description; }
    std::vector<uint8_t> header() const override {
        std::vector<uint8_t> out;
        IMFMediaType* type = nullptr;
        if (m_mft && SUCCEEDED(m_mft->GetOutputCurrentType(0, &type)) && type) {
            UINT32 size = 0;
            if (SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size) {
                out.resize(size);
                if (FAILED(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, out.data(), size, nullptr))) out.clear();
            }
            type->Release();
        }
        return out;
    }

private:
    bool tryActivate(IMFActivate* activate, const EncoderSetup& s, std::string& why) {
        wchar_t* friendly = nullptr;
        UINT32 length = 0;
        std::string name = "an encoder";
        if (SUCCEEDED(activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &length)) && friendly) {
            char narrow[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, friendly, -1, narrow, sizeof(narrow), nullptr, nullptr);
            name = narrow;
            CoTaskMemFree(friendly);
        }
        UINT32 enumFlags = 0;
        activate->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &enumFlags);
        IMFTransform* mft = nullptr;
        HRESULT hr = activate->ActivateObject(IID_PPV_ARGS(&mft));
        if (FAILED(hr)) {
            why = name + ": " + hex(hr);
            return false;
        }
        bool async = false;
        IMFAttributes* attributes = nullptr;
        if (SUCCEEDED(mft->GetAttributes(&attributes)) && attributes) {
            UINT32 isAsync = 0;
            if (SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync)) && isAsync) {
                async = true;
                attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            }
            attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
            release(attributes);
        }
        // Rate control and key frames before the types (some encoders take them only then).
        ICodecAPI* codec = nullptr;
        std::string rate = "the encoder's own rate";
        if (SUCCEEDED(mft->QueryInterface(IID_PPV_ARGS(&codec))) && codec) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_UI4;
            v.ulVal = eAVEncCommonRateControlMode_PeakConstrainedVBR;
            const bool peak = SUCCEEDED(codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v));
            if (!peak) {
                v.ulVal = eAVEncCommonRateControlMode_UnconstrainedVBR;
                codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
            }
            v.ulVal = static_cast<ULONG>(s.bitrate);
            if (SUCCEEDED(codec->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v))) {
                char text[64];
                snprintf(text, sizeof(text), "%s %.1f Mbit/s", peak ? "peak-constrained VBR" : "VBR", s.bitrate / 1e6);
                rate = text;
            }
            v.ulVal = static_cast<ULONG>(s.bitrate * 1.5);
            codec->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &v);
            v.ulVal = static_cast<ULONG>(s.fps * s.gopSeconds + 0.5);
            codec->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
            v.ulVal = 0;
            codec->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);
            v.vt = VT_BOOL;
            v.boolVal = VARIANT_TRUE;
            codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
            release(codec);
        }
        UINT32 num = static_cast<UINT32>(s.fps + 0.5), den = 1;
        IMFMediaType* outType = nullptr;
        IMFMediaType* inType = nullptr;
        hr = mf().createMediaType(&outType);
        if (SUCCEEDED(hr)) {
            outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            outType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(s.bitrate));
            outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
            MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, s.width, s.height);
            MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, num, den);
            MFSetAttributeRatio(outType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            hr = mft->SetOutputType(0, outType, 0);
            if (FAILED(hr)) {
                // Main profile then (an older encoder).
                outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
                hr = mft->SetOutputType(0, outType, 0);
            }
        }
        if (SUCCEEDED(hr)) hr = mf().createMediaType(&inType);
        if (SUCCEEDED(hr)) {
            inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, s.width, s.height);
            MFSetAttributeRatio(inType, MF_MT_FRAME_RATE, num, den);
            MFSetAttributeRatio(inType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            hr = mft->SetInputType(0, inType, 0);
        }
        release(inType);
        release(outType);
        IMFMediaEventGenerator* events = nullptr;
        if (SUCCEEDED(hr) && async) hr = mft->QueryInterface(IID_PPV_ARGS(&events));
        if (SUCCEEDED(hr)) hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        if (SUCCEEDED(hr)) hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        if (FAILED(hr)) {
            why = name + ": " + hex(hr);
            release(events);
            release(mft);
            activate->ShutdownObject();
            return false;
        }
        m_mft = mft;
        m_events = events;
        m_async = async;
        char text[256];
        snprintf(text, sizeof(text), "Media Foundation H.264 %dx%d %.0f fps, %s, %s (%s%s)", s.width, s.height, s.fps, rate.c_str(), name.c_str(),
                 (enumFlags & MFT_ENUM_FLAG_HARDWARE) ? "hardware" : "software", async ? ", asynchronous" : "");
        m_description = text;
        return true;
    }

    // Asynchronous: the events that are there (wait: until one came).
    bool pump(bool wait, const PictureSink& sink, std::string& error) {
        for (;;) {
            IMFMediaEvent* event = nullptr;
            HRESULT hr = m_events->GetEvent(wait ? 0 : MF_EVENT_FLAG_NO_WAIT, &event);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) return true;
            if (FAILED(hr)) {
                error = "GetEvent: " + hex(hr);
                return false;
            }
            MediaEventType type = MEUnknown;
            event->GetType(&type);
            HRESULT status = S_OK;
            event->GetStatus(&status);
            release(event);
            if (FAILED(status)) {
                error = "the encoder reported " + hex(status);
                return false;
            }
            if (type == METransformNeedInput) m_needInput++;
            else if (type == METransformHaveOutput && !takeOutput(sink, error)) return false;
            else if (type == METransformDrainComplete) m_drained = true;
            if (wait) return true;
        }
    }

    // Synchronous: every picture it has.
    bool drainSync(const PictureSink& sink, std::string& error) {
        for (int i = 0; i < 64; i++) {
            bool none = false;
            if (!takeOutput(sink, error, &none)) return false;
            if (none) return true;
        }
        return true;
    }

    bool takeOutput(const PictureSink& sink, std::string& error, bool* none = nullptr) {
        MFT_OUTPUT_STREAM_INFO info{};
        m_mft->GetOutputStreamInfo(0, &info);
        const bool provides = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        IMFSample* own = nullptr;
        IMFMediaBuffer* ownBuffer = nullptr;
        if (!provides) {
            const DWORD size = info.cbSize ? info.cbSize : static_cast<DWORD>(m_width * m_height * 2);
            if (FAILED(mf().createMemoryBuffer(size, &ownBuffer)) || FAILED(mf().createSample(&own)) || FAILED(own->AddBuffer(ownBuffer))) {
                release(own);
                release(ownBuffer);
                error = "an output sample";
                return false;
            }
        }
        MFT_OUTPUT_DATA_BUFFER out{};
        out.pSample = own;
        DWORD status = 0;
        HRESULT hr = m_mft->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();
        bool ok = true;
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (none) *none = true;
        } else if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // The encoder settled its output type (some do at the first frame): take what it offers.
            IMFMediaType* type = nullptr;
            if (SUCCEEDED(m_mft->GetOutputAvailableType(0, 0, &type)) && type) {
                m_mft->SetOutputType(0, type, 0);
                type->Release();
            }
        } else if (FAILED(hr)) {
            error = "ProcessOutput: " + hex(hr);
            ok = false;
        } else if (out.pSample) {
            IMFMediaBuffer* buffer = nullptr;
            if (SUCCEEDED(out.pSample->ConvertToContiguousBuffer(&buffer)) && buffer) {
                BYTE* data = nullptr;
                DWORD length = 0;
                if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                    UINT32 clean = 0;
                    out.pSample->GetUINT32(MFSampleExtension_CleanPoint, &clean);
                    EncodedPicture e;
                    e.data = data;
                    e.size = length;
                    e.frame = m_out++;
                    e.key = clean != 0;
                    if (length) sink(e);
                    buffer->Unlock();
                }
                buffer->Release();
            }
        }
        if (out.pSample && out.pSample != own) out.pSample->Release();
        release(own);
        release(ownBuffer);
        return ok;
    }

    IMFActivate* m_activate = nullptr;
    IMFTransform* m_mft = nullptr;
    IMFMediaEventGenerator* m_events = nullptr;
    bool m_async = false;
    bool m_com = false;
    bool m_drained = false;
    int m_needInput = 0;
    int m_width = 0, m_height = 0;
    double m_fps = 60.0;
    LONGLONG m_sent = 0;
    int64_t m_out = 0;
    std::string m_description;
};

} // namespace

std::unique_ptr<PictureEncoder> openMft(const EncoderSetup& setup, std::string& error) {
    auto e = std::make_unique<MftEncoder>();
    if (!e->open(setup, error)) return nullptr;
    return e;
}

} // namespace clips
} // namespace tmshaders
