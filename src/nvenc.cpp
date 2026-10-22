#include "nvenc.h"
#include "log.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <nvEncodeAPI.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

namespace tmshaders {
namespace venc {
namespace nvenc {
namespace {

std::string strf(const char* fmt, ...) {
    char text[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    return text;
}

std::string narrow(const wchar_t* text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], n, nullptr, nullptr);
    return out;
}

const char* statusName(NVENCSTATUS s) {
    switch (s) {
    case NV_ENC_SUCCESS: return "ok";
    case NV_ENC_ERR_NO_ENCODE_DEVICE: return "no encode device";
    case NV_ENC_ERR_UNSUPPORTED_DEVICE: return "unsupported device";
    case NV_ENC_ERR_INVALID_DEVICE: return "invalid device";
    case NV_ENC_ERR_INVALID_PARAM: return "invalid parameter";
    case NV_ENC_ERR_INVALID_CALL: return "invalid call";
    case NV_ENC_ERR_OUT_OF_MEMORY: return "out of memory (or no free encode session)";
    case NV_ENC_ERR_UNSUPPORTED_PARAM: return "unsupported parameter";
    case NV_ENC_ERR_INVALID_VERSION: return "invalid version";
    case NV_ENC_ERR_NEED_MORE_INPUT: return "needs more input";
    case NV_ENC_ERR_ENCODER_BUSY: return "encoder busy";
    case NV_ENC_ERR_GENERIC: return "generic error";
    case NV_ENC_ERR_UNIMPLEMENTED: return "not implemented";
    default: return "error";
    }
}

// A system DLL by its full path (no search order, so no planted copy next to the game). A
// 32-bit process asking for System32 gets SysWOW64.
HMODULE loadSystem(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return nullptr;
    return LoadLibraryW((std::wstring(dir) + L"\\" + name).c_str());
}

struct Lib {
    bool tried = false;
    NV_ENCODE_API_FUNCTION_LIST fn{};
    Support support;
};
Lib g_lib;
std::mutex g_libMutex;

// A D3D11 device on the first NVIDIA adapter: the encoder's own, nothing shared with the game.
ID3D11Device* createDevice(std::string& name, std::string& error) {
    static HMODULE dxgi = loadSystem(L"dxgi.dll");
    static HMODULE d3d11 = loadSystem(L"d3d11.dll");
    if (!dxgi || !d3d11) {
        error = "no D3D11";
        return nullptr;
    }
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    const auto createFactory = reinterpret_cast<CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    const auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d11, "D3D11CreateDevice"));
    IDXGIFactory1* factory = nullptr;
    if (!createFactory || !create || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        error = "no DXGI factory";
        return nullptr;
    }
    ID3D11Device* device = nullptr;
    error = "no NVIDIA graphics card";
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; !device && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x10DE && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            const HRESULT hr = create(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr);
            if (SUCCEEDED(hr)) name = narrow(desc.Description);
            else error = strf("D3D11CreateDevice on the NVIDIA card failed (0x%08lx)", static_cast<unsigned long>(hr));
        }
        adapter->Release();
    }
    factory->Release();
    return device;
}

void* openSession(ID3D11Device* device, NVENCSTATUS& status) {
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS p{};
    p.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    p.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    p.device = device;
    p.apiVersion = NVENCAPI_VERSION;
    void* encoder = nullptr;
    status = g_lib.fn.nvEncOpenEncodeSessionEx(&p, &encoder);
    if (status != NV_ENC_SUCCESS) {
        // A failed open still hands out a session that must be destroyed.
        if (encoder) g_lib.fn.nvEncDestroyEncoder(encoder);
        return nullptr;
    }
    return encoder;
}

int caps(void* encoder, const GUID& codec, NV_ENC_CAPS which) {
    NV_ENC_CAPS_PARAM p{};
    p.version = NV_ENC_CAPS_PARAM_VER;
    p.capsToQuery = which;
    int value = 0;
    return g_lib.fn.nvEncGetEncodeCaps(encoder, codec, &p, &value) == NV_ENC_SUCCESS ? value : 0;
}

std::string describeCaps(void* encoder, const GUID& codec) {
    const int rc = caps(encoder, codec, NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES);
    const int bref = caps(encoder, codec, NV_ENC_CAPS_SUPPORT_BFRAME_REF_MODE);
    const auto yes = [&](NV_ENC_CAPS c) { return caps(encoder, codec, c) ? "yes" : "no"; };
    return strf("B-frames %d (as reference: %s), lookahead %s (levels %s), temporal AQ %s, 10-bit %s, several reference frames %s, "
                "temporal filter %s, weighted prediction %s, %d engine(s), up to %dx%d, rate control modes 0x%x",
                caps(encoder, codec, NV_ENC_CAPS_NUM_MAX_BFRAMES), bref == 1 ? "each" : bref == 2 ? "middle" : "no", yes(NV_ENC_CAPS_SUPPORT_LOOKAHEAD),
                yes(NV_ENC_CAPS_SUPPORT_LOOKAHEAD_LEVEL), yes(NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ), yes(NV_ENC_CAPS_SUPPORT_10BIT_ENCODE),
                yes(NV_ENC_CAPS_SUPPORT_MULTIPLE_REF_FRAMES), yes(NV_ENC_CAPS_SUPPORT_TEMPORAL_FILTER), yes(NV_ENC_CAPS_SUPPORT_WEIGHTED_PREDICTION),
                std::max(1, caps(encoder, codec, NV_ENC_CAPS_NUM_ENCODER_ENGINES)), caps(encoder, codec, NV_ENC_CAPS_WIDTH_MAX),
                caps(encoder, codec, NV_ENC_CAPS_HEIGHT_MAX), rc);
}

// The video's colours in the bitstream (VUI): BT.709 studio range, or HDR10 (PQ, BT.2020).
void setVui(NV_ENC_CONFIG_H264_VUI_PARAMETERS& v, bool hdr) {
    v.videoSignalTypePresentFlag = 1;
    v.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    v.videoFullRangeFlag = 0;
    v.colourDescriptionPresentFlag = 1;
    v.colourPrimaries = hdr ? NV_ENC_VUI_COLOR_PRIMARIES_BT2020 : NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    v.transferCharacteristics = hdr ? NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SMPTE2084 : NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    v.colourMatrix = hdr ? NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL : NV_ENC_VUI_MATRIX_COEFFS_BT709;
}

const GUID* presetGuid(int preset) {
    static const GUID* presets[7] = {&NV_ENC_PRESET_P1_GUID, &NV_ENC_PRESET_P2_GUID, &NV_ENC_PRESET_P3_GUID, &NV_ENC_PRESET_P4_GUID,
                                     &NV_ENC_PRESET_P5_GUID, &NV_ENC_PRESET_P6_GUID, &NV_ENC_PRESET_P7_GUID};
    return presets[std::clamp(preset, 1, 7) - 1];
}

// My defaults, from my measurement (tools/encbench, 8 s of a 1440p60 replay over Stadium's grid
// with film grain, VMAF against a 275 Mbit/s export, GTX 1080 Ti, H.264 at CQ 23 unless said):
//  - P7: 75.3 Mbit/s, VMAF 97.95 at 117 fps; P4 76.1 / 97.77 at 249 fps; P1 79.1 / 97.01.
//    The export renders at about 19 fps (2 x 2 samples), so the slowest preset costs nothing.
//  - No B-frames: three B-frames gave 81.6 Mbit/s / 97.68 at CQ 21, where no B-frames give
//    about 98.4 at that rate; every B-frame setting lost (the grain makes B-frames expensive).
//  - No AQ: spatial AQ 74.1 / 96.90 against 75.3 / 97.95 without; temporal AQ changed nothing
//    (75.4 / 97.98). VMAF punishes AQ; whether it looks better in flat dark areas is not judged
//    yet by eye (Options::nvencAQ = 1 to try).
//  - No lookahead: 0 frames 74.2 / 97.89, 8 frames 75.3 / 97.95, 24 frames 75.5 / 97.96, and
//    every frame of lookahead costs a buffer pair (10 MB of the game's address space at 1440p).
//  - One pass: 75.3 / 97.95 at 117 fps, two passes (quarter resolution) 75.1 / 97.78 at 161 fps.
//  - Everything on (3 B-frames, spatial + temporal AQ, lookahead 24, two passes): CQ 23 gave
//    64.3 Mbit/s / 94.61, CQ 21 81.7 / 96.83: well below the plain setting at the same rate.
// Today's path, the NVENC MFT, gave 45.9 Mbit/s / 91.37 (quality 80) and needs 82 Mbit/s for 97.76.
const int kPreset = 7;
const int kBFrames = 0;
const int kLookahead = 0;
const int kAQ = 0;
const int kAQStrength = 0;
const int kMultipass = 0;

} // namespace

namespace {

void probe(Support& s) {
    s.builtApi = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    HMODULE dll = loadSystem(L"nvEncodeAPI.dll");
    if (!dll) {
        s.why = "no nvEncodeAPI.dll (no NVIDIA driver)";
        return;
    }
    using GetMax = NVENCSTATUS(NVENCAPI*)(uint32_t*);
    using Create = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    const auto getMax = reinterpret_cast<GetMax>(GetProcAddress(dll, "NvEncodeAPIGetMaxSupportedVersion"));
    const auto create = reinterpret_cast<Create>(GetProcAddress(dll, "NvEncodeAPICreateInstance"));
    uint32_t version = 0;
    if (!getMax || !create || getMax(&version) != NV_ENC_SUCCESS) {
        s.why = "nvEncodeAPI.dll without its entry points";
        return;
    }
    s.driverApi = version;
    if (version < s.builtApi) {
        s.why = strf("the driver offers NVENC API %u.%u, the mod needs %u.%u (driver R570 or newer)", version >> 4, version & 15, s.builtApi >> 4,
                     s.builtApi & 15);
        return;
    }
    g_lib.fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (create(&g_lib.fn) != NV_ENC_SUCCESS || !g_lib.fn.nvEncOpenEncodeSessionEx) {
        s.why = "NvEncodeAPICreateInstance failed";
        return;
    }
    std::string error;
    ID3D11Device* device = createDevice(s.adapter, error);
    if (!device) {
        s.why = error;
        return;
    }
    NVENCSTATUS status = NV_ENC_SUCCESS;
    void* encoder = openSession(device, status);
    if (!encoder) {
        s.why = strf("no encode session: %s (%d)", statusName(status), static_cast<int>(status));
        device->Release();
        return;
    }
    uint32_t count = 0;
    g_lib.fn.nvEncGetEncodeGUIDCount(encoder, &count);
    std::vector<GUID> guids(count);
    if (count) g_lib.fn.nvEncGetEncodeGUIDs(encoder, guids.data(), count, &count);
    for (uint32_t i = 0; i < count; i++) {
        const char* name = nullptr;
        if (guids[i] == NV_ENC_CODEC_H264_GUID) s.h264 = true, name = "H.264";
        else if (guids[i] == NV_ENC_CODEC_HEVC_GUID) s.hevc = true, name = "HEVC";
        else if (guids[i] == NV_ENC_CODEC_AV1_GUID) s.av1 = true, name = "AV1";
        if (name) s.caps += std::string(s.caps.empty() ? "" : "; ") + name + ": " + describeCaps(encoder, guids[i]);
    }
    g_lib.fn.nvEncDestroyEncoder(encoder);
    device->Release();
    s.ok = s.h264 || s.hevc;
    if (!s.ok) s.why = "NVENC offers neither H.264 nor HEVC";
}

} // namespace

const Support& support() {
    std::lock_guard<std::mutex> lock(g_libMutex);
    Support& s = g_lib.support;
    if (g_lib.tried) return s;
    g_lib.tried = true;
    probe(s);
    if (s.ok) {
        TMVS_LOG("venc: NVENC: %s, API %u.%u (the driver offers %u.%u); %s", s.adapter.c_str(), s.builtApi >> 4, s.builtApi & 15, s.driverApi >> 4,
                 s.driverApi & 15, s.caps.c_str());
    } else {
        TMVS_LOG("venc: NVENC not used: %s", s.why.c_str());
    }
    return s;
}

// ---------------------------------------------------------------------------------------------

struct Session::Impl {
    ID3D11Device* device = nullptr;
    void* encoder = nullptr;
    int width = 0, height = 0;
    bool tenBit = false, hevc = false;
    NV_ENC_BUFFER_FORMAT format = NV_ENC_BUFFER_FORMAT_NV12;
    // Input and output buffers in pairs. A pair is busy from the frame that went in with it
    // until its output buffer was read: the encoder fills the output buffers in the order they
    // came, with the pictures in decode order, and holds frames for B-frames and lookahead
    // longer than their numbers say (P7 with three B-frames and no lookahead: eight frames;
    // with lookahead the first success comes after the lookahead, then one frame a call). So
    // no fixed delay is trusted: in asynchronous mode each pair has an event the driver sets
    // when its output is complete, and only those are read, oldest first.
    bool async = false;
    std::vector<NV_ENC_INPUT_PTR> inputs;
    std::vector<NV_ENC_OUTPUT_PTR> outputs;
    std::vector<HANDLE> events;
    HANDLE eosEvent = nullptr;
    std::deque<size_t> free, pending;
    size_t ready = 0;        // synchronous mode: the first `ready` of `pending` were released by a success
    size_t maxPairs = 0;     // the pool's limit
    size_t maxPending = 0;   // the most frames the encoder held at once (for the log)
    int64_t sent = 0;
    std::vector<uint8_t> header;
    int reorder = 0;
    std::string summary;
    bool hdr = false;
    MASTERING_DISPLAY_INFO mastering{};
    CONTENT_LIGHT_LEVEL light{};

    NV_ENCODE_API_FUNCTION_LIST& fn() { return g_lib.fn; }

    std::string fail(const char* what, NVENCSTATUS status) {
        std::string text = strf("%s: %s (%d)", what, statusName(status), static_cast<int>(status));
        if (encoder && fn().nvEncGetLastErrorString) {
            const char* detail = fn().nvEncGetLastErrorString(encoder);
            if (detail && *detail) text += std::string(", ") + detail;
        }
        return text;
    }

    // One more pair of buffers. False: the limit, or the driver refused.
    bool addPair(std::string& error) {
        if (inputs.size() >= maxPairs) {
            error = strf("the encoder holds more than %u frames", static_cast<unsigned>(maxPairs));
            return false;
        }
        NV_ENC_CREATE_INPUT_BUFFER in{};
        in.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
        in.width = static_cast<uint32_t>(width);
        in.height = static_cast<uint32_t>(height);
        in.bufferFmt = format;
        NVENCSTATUS st = fn().nvEncCreateInputBuffer(encoder, &in);
        if (st != NV_ENC_SUCCESS) {
            error = fail("nvEncCreateInputBuffer", st);
            return false;
        }
        NV_ENC_CREATE_BITSTREAM_BUFFER out{};
        out.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        st = fn().nvEncCreateBitstreamBuffer(encoder, &out);
        if (st != NV_ENC_SUCCESS) {
            fn().nvEncDestroyInputBuffer(encoder, in.inputBuffer);
            error = fail("nvEncCreateBitstreamBuffer", st);
            return false;
        }
        HANDLE event = nullptr;
        if (async && !(event = registeredEvent(error))) {
            fn().nvEncDestroyInputBuffer(encoder, in.inputBuffer);
            fn().nvEncDestroyBitstreamBuffer(encoder, out.bitstreamBuffer);
            return false;
        }
        inputs.push_back(in.inputBuffer);
        outputs.push_back(out.bitstreamBuffer);
        events.push_back(event);
        free.push_back(inputs.size() - 1);
        return true;
    }

    HANDLE registeredEvent(std::string& error) {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) {
            error = "no event";
            return nullptr;
        }
        NV_ENC_EVENT_PARAMS p{};
        p.version = NV_ENC_EVENT_PARAMS_VER;
        p.completionEvent = event;
        const NVENCSTATUS st = fn().nvEncRegisterAsyncEvent(encoder, &p);
        if (st != NV_ENC_SUCCESS) {
            CloseHandle(event);
            error = fail("nvEncRegisterAsyncEvent", st);
            return nullptr;
        }
        return event;
    }

    void unregister(HANDLE& event) {
        if (!event) return;
        NV_ENC_EVENT_PARAMS p{};
        p.version = NV_ENC_EVENT_PARAMS_VER;
        p.completionEvent = event;
        if (encoder) fn().nvEncUnregisterAsyncEvent(encoder, &p);
        CloseHandle(event);
        event = nullptr;
    }

    // Reads finished output buffers, oldest first. Asynchronous: those whose event fired, and
    // the first `waitFor` waited for (20 s at most each). Synchronous: those a success
    // released, but the newest `keep` (the card may still work on them while the CPU copies
    // the next frame; a lock waits for it).
    bool collect(size_t waitFor, size_t keep, const PacketSink& sink, std::string& error) {
        while (!pending.empty()) {
            const size_t pair = pending.front();
            if (async) {
                const DWORD r = WaitForSingleObject(events[pair], waitFor > 0 ? 20000 : 0);
                if (r == WAIT_TIMEOUT && waitFor == 0) break;
                if (r != WAIT_OBJECT_0) {
                    error = r == WAIT_TIMEOUT ? "the encoder did not finish a frame in 20 s" : "waiting for the encoder failed";
                    return false;
                }
                if (waitFor > 0) waitFor--;
            } else {
                if (ready <= keep) break;
                ready--;
            }
            NV_ENC_LOCK_BITSTREAM lock{};
            lock.version = NV_ENC_LOCK_BITSTREAM_VER;
            lock.outputBitstream = outputs[pair];
            const NVENCSTATUS st = fn().nvEncLockBitstream(encoder, &lock);
            if (st != NV_ENC_SUCCESS) {
                error = fail("nvEncLockBitstream", st);
                return false;
            }
            Packet p;
            p.data = static_cast<const uint8_t*>(lock.bitstreamBufferPtr);
            p.size = lock.bitstreamSizeInBytes;
            p.pts = static_cast<int64_t>(lock.outputTimeStamp);
            p.key = lock.pictureType == NV_ENC_PIC_TYPE_IDR;
            const bool ok = sink(p);
            fn().nvEncUnlockBitstream(encoder, lock.outputBitstream);
            pending.pop_front();
            free.push_back(pair);
            if (!ok) {
                error = "the packet could not be written";
                return false;
            }
        }
        return true;
    }

    void destroy() {
        if (encoder) {
            for (NV_ENC_INPUT_PTR p : inputs) fn().nvEncDestroyInputBuffer(encoder, p);
            for (NV_ENC_OUTPUT_PTR p : outputs) fn().nvEncDestroyBitstreamBuffer(encoder, p);
        }
        for (HANDLE& e : events) unregister(e);
        unregister(eosEvent);
        if (encoder) {
            fn().nvEncDestroyEncoder(encoder);
            encoder = nullptr;
        }
        inputs.clear();
        outputs.clear();
        events.clear();
        free.clear();
        pending.clear();
        ready = 0;
        if (device) {
            device->Release();
            device = nullptr;
        }
    }
};

Session::Session() : m(std::make_unique<Impl>()) {}

Session::~Session() {
    m->destroy();
}

bool Session::open(const Options& o, double cq, int qp, double maxRate, std::string& error) {
    const Support& s = support();
    if (!s.ok) {
        error = s.why;
        return false;
    }
    if (o.codec == Codec::AV1) {
        error = "AV1 is not driven directly";
        return false;
    }
    m->hevc = o.codec == Codec::HEVC;
    m->tenBit = o.tenBit && m->hevc;
    if (m->hevc ? !s.hevc : !s.h264) {
        error = m->hevc ? "no HEVC on this card" : "no H.264 on this card";
        return false;
    }
    m->width = o.width;
    m->height = o.height;
    m->format = m->tenBit ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;
    m->hdr = o.hdr10 && m->hevc;
    std::string adapter;
    m->device = createDevice(adapter, error);
    if (!m->device) return false;
    NVENCSTATUS st = NV_ENC_SUCCESS;
    m->encoder = openSession(m->device, st);
    if (!m->encoder) {
        error = strf("no encode session: %s (%d)", statusName(st), static_cast<int>(st));
        m->destroy();
        return false;
    }
    const GUID& codec = m->hevc ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
    void* enc = m->encoder;
    if (m->tenBit && !caps(enc, codec, NV_ENC_CAPS_SUPPORT_10BIT_ENCODE)) {
        error = "no 10-bit HEVC on this card";
        m->destroy();
        return false;
    }
    if (o.width > caps(enc, codec, NV_ENC_CAPS_WIDTH_MAX) || o.height > caps(enc, codec, NV_ENC_CAPS_HEIGHT_MAX)) {
        error = strf("%dx%d is beyond the card's %dx%d", o.width, o.height, caps(enc, codec, NV_ENC_CAPS_WIDTH_MAX), caps(enc, codec, NV_ENC_CAPS_HEIGHT_MAX));
        m->destroy();
        return false;
    }
    const int maxB = caps(enc, codec, NV_ENC_CAPS_NUM_MAX_BFRAMES);
    const bool canLookahead = caps(enc, codec, NV_ENC_CAPS_SUPPORT_LOOKAHEAD) != 0;
    const bool canTemporalAQ = caps(enc, codec, NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ) != 0;
    const int brefMode = caps(enc, codec, NV_ENC_CAPS_SUPPORT_BFRAME_REF_MODE);
    // Without events (asynchronous mode) only B-frames are safe to collect: a success
    // releases every frame before it. The lookahead's delay is not knowable then.
    m->async = caps(enc, codec, NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT) != 0;

    const int preset = o.nvencPreset >= 1 ? std::min(o.nvencPreset, 7) : kPreset;
    int bframes = std::clamp(o.nvencBFrames >= 0 ? o.nvencBFrames : kBFrames, 0, std::max(0, maxB));
    int lookahead = canLookahead && m->async ? std::clamp(o.nvencLookahead >= 0 ? o.nvencLookahead : kLookahead, 0, 31 - bframes) : 0;
    int aq = std::clamp(o.nvencAQ >= 0 ? o.nvencAQ : kAQ, 0, 3);
    if (aq >= 2 && !canTemporalAQ) aq = aq == 2 ? 1 : 0;
    const int aqStrength = std::clamp(o.nvencAQStrength >= 0 ? o.nvencAQStrength : kAQStrength, 0, 15);
    int multipass = std::clamp(o.nvencMultipass >= 0 ? o.nvencMultipass : kMultipass, 0, 2);
    // The buffers live in the game's 32-bit address space (2 GB, TmForever is not large
    // address aware): every frame the encoder holds (B-frames, lookahead, a few in flight)
    // needs a pair, about twice a frame (1440p NV12: 10 MB a pair in my measurement; the MFT
    // path took 222 MB of address space in all, NVENC with lookahead 8 about 360, with 24 570).
    // About 160 MB for the pairs, the lookahead shortened to fit (4K: 2, 4K 10-bit: none).
    const size_t frameBytes = static_cast<size_t>(o.width) * o.height * 3 / (m->tenBit ? 1 : 2);
    const int budget = static_cast<int>(std::clamp<size_t>((160u << 20) / std::max<size_t>(2 * frameBytes, 1), 6, 48));
    lookahead = std::max(0, std::min(lookahead, budget - bframes - 4));

    const GUID& presetId = *presetGuid(preset);
    NV_ENC_PRESET_CONFIG presetConfig{};
    presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
    st = m->fn().nvEncGetEncodePresetConfigEx(enc, codec, presetId, NV_ENC_TUNING_INFO_HIGH_QUALITY, &presetConfig);
    if (st != NV_ENC_SUCCESS) {
        error = m->fail("nvEncGetEncodePresetConfigEx", st);
        m->destroy();
        return false;
    }
    UINT32 num = 0, den = 1;
    if (std::fabs(o.fps - std::round(o.fps)) < 1e-4) {
        num = static_cast<UINT32>(std::lround(o.fps));
    } else {
        num = static_cast<UINT32>(std::lround(o.fps * 1000.0));
        den = 1000;
    }
    const uint32_t gop = static_cast<uint32_t>(std::max(1L, std::lround(o.fps * 2.0)));
    if (m->hdr) {
        // BT.2020 primaries and D65 in 0.00002 steps, the peak and 0.005 nits in 0.0001 steps.
        m->mastering.g = {8500, 39850};
        m->mastering.b = {6550, 2300};
        m->mastering.r = {35400, 14600};
        m->mastering.whitePoint = {15635, 16450};
        m->mastering.maxLuma = static_cast<uint32_t>(std::max(1, o.hdrPeakNits)) * 10000u;
        m->mastering.minLuma = 50;
        m->light.maxContentLightLevel = static_cast<uint16_t>(std::clamp(o.hdrPeakNits, 1, 65535));
        m->light.maxPicAverageLightLevel = static_cast<uint16_t>(std::clamp(o.hdrAverageNits, 1, 65535));
    }

    // Initialise with everything, and if the driver refuses, once more with less (the tools
    // that are most often missing first), each refusal logged.
    bool initialized = false;
    std::string refusal;
    for (int attempt = 0; attempt <= 4 && !initialized; attempt++) {
        if (attempt == 1) {
            if (aq < 2) continue;
            aq = aq == 2 ? 1 : 0;
        } else if (attempt == 2) {
            if (multipass == 0) continue;
            multipass = 0;
        } else if (attempt == 3) {
            if (lookahead == 0) continue;
            lookahead = 0;
        } else if (attempt == 4) {
            if (bframes == 0) continue;
            bframes = 0;
        }
        NV_ENC_CONFIG cfg = presetConfig.presetCfg;
        cfg.version = NV_ENC_CONFIG_VER;
        cfg.profileGUID = m->hevc ? (m->tenBit ? NV_ENC_HEVC_PROFILE_MAIN10_GUID : NV_ENC_HEVC_PROFILE_MAIN_GUID) : NV_ENC_H264_PROFILE_HIGH_GUID;
        cfg.gopLength = gop;
        cfg.frameIntervalP = bframes + 1;
        NV_ENC_RC_PARAMS& rc = cfg.rcParams;
        rc.version = NV_ENC_RC_PARAMS_VER;
        rc.averageBitRate = 0;
        rc.maxBitRate = 0;
        rc.vbvBufferSize = 0;
        rc.vbvInitialDelay = 0;
        rc.targetQuality = 0;
        rc.targetQualityLSB = 0;
        rc.enableMinQP = rc.enableMaxQP = rc.enableInitialRCQP = 0;
        if (o.bitrate > 0.0) {
            rc.rateControlMode = NV_ENC_PARAMS_RC_VBR;
            rc.averageBitRate = static_cast<uint32_t>(o.bitrate);
            rc.maxBitRate = static_cast<uint32_t>(std::min(o.bitrate * 1.5, 4.0e9));
            rc.vbvBufferSize = static_cast<uint32_t>(std::min(o.bitrate * 2.0, 4.0e9));
        } else if (o.rateControl == RateControl::ConstantQP) {
            rc.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
            const uint32_t q = static_cast<uint32_t>(std::clamp(qp, 0, 51));
            rc.constQP.qpIntra = q > 2 ? q - 2 : 0;
            rc.constQP.qpInterP = q;
            rc.constQP.qpInterB = std::min(51u, q + 2);
        } else {
            // Constant quality: VBR without an average, the CQ level and a cap. The VBV is two
            // seconds of the cap: a file, not a stream, may spend a burst on a detailed moment.
            rc.rateControlMode = NV_ENC_PARAMS_RC_VBR;
            const double level = std::clamp(cq, 1.0, 51.0);
            rc.targetQuality = static_cast<uint8_t>(std::floor(level));
            rc.targetQualityLSB = static_cast<uint8_t>(std::lround((level - std::floor(level)) * 256.0) & 255);
            if (maxRate > 0.0) {
                rc.maxBitRate = static_cast<uint32_t>(std::min(maxRate, 4.0e9));
                rc.vbvBufferSize = static_cast<uint32_t>(std::min(maxRate * 2.0, 4.0e9));
            }
        }
        rc.enableAQ = aq == 1 || aq == 2 ? 1 : 0;
        rc.enableTemporalAQ = aq >= 2 ? 1 : 0;
        rc.aqStrength = rc.enableAQ ? static_cast<uint32_t>(aqStrength) : 0;
        rc.enableLookahead = lookahead > 0 ? 1 : 0;
        rc.lookaheadDepth = static_cast<uint16_t>(lookahead);
        rc.multiPass = multipass == 2 ? NV_ENC_TWO_PASS_FULL_RESOLUTION : multipass == 1 ? NV_ENC_TWO_PASS_QUARTER_RESOLUTION : NV_ENC_MULTI_PASS_DISABLED;
        rc.zeroReorderDelay = 0;
        const NV_ENC_BFRAME_REF_MODE bref = bframes >= 2 && brefMode == 2 ? NV_ENC_BFRAME_REF_MODE_MIDDLE
                                          : bframes >= 2 && brefMode == 1 ? NV_ENC_BFRAME_REF_MODE_MIDDLE
                                                                          : NV_ENC_BFRAME_REF_MODE_DISABLED;
        if (m->hevc) {
            NV_ENC_CONFIG_HEVC& c = cfg.encodeCodecConfig.hevcConfig;
            c.idrPeriod = gop;
            c.repeatSPSPPS = 1;
            c.chromaFormatIDC = 1;
            c.inputBitDepth = c.outputBitDepth = m->tenBit ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
            c.useBFramesAsRef = bref;
            setVui(c.hevcVUIParameters, m->hdr);
            c.outputMasteringDisplay = m->hdr ? 1 : 0;
            c.outputMaxCll = m->hdr ? 1 : 0;
        } else {
            NV_ENC_CONFIG_H264& c = cfg.encodeCodecConfig.h264Config;
            c.idrPeriod = gop;
            c.repeatSPSPPS = 1;
            c.chromaFormatIDC = 1;
            c.inputBitDepth = c.outputBitDepth = NV_ENC_BIT_DEPTH_8;
            c.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;
            c.useBFramesAsRef = bref;
            setVui(c.h264VUIParameters, false);
        }
        NV_ENC_INITIALIZE_PARAMS init{};
        init.version = NV_ENC_INITIALIZE_PARAMS_VER;
        init.encodeGUID = codec;
        init.presetGUID = presetId;
        init.encodeWidth = init.darWidth = init.maxEncodeWidth = static_cast<uint32_t>(o.width);
        init.encodeHeight = init.darHeight = init.maxEncodeHeight = static_cast<uint32_t>(o.height);
        init.frameRateNum = num;
        init.frameRateDen = den;
        init.enablePTD = 1;
        init.enableEncodeAsync = m->async ? 1 : 0;
        init.encodeConfig = &cfg;
        init.tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;
        st = m->fn().nvEncInitializeEncoder(enc, &init);
        const std::string tools = strf("P%d, %d B-frames, lookahead %d, AQ %s, %s", preset, bframes, lookahead,
                                       aq == 3 ? "temporal" : aq == 2 ? "spatial + temporal" : aq == 1 ? "spatial" : "off",
                                       multipass == 2 ? "two full passes" : multipass == 1 ? "two passes (quarter)" : "one pass");
        if (st == NV_ENC_SUCCESS) {
            std::string rate;
            if (rc.rateControlMode == NV_ENC_PARAMS_RC_CONSTQP) rate = strf("constant QP %u", rc.constQP.qpInterP);
            else if (o.bitrate > 0.0) rate = strf("VBR %.1f Mbit/s, peak %.1f", rc.averageBitRate / 1e6, rc.maxBitRate / 1e6);
            else rate = strf("CQ %.4g", rc.targetQuality + rc.targetQualityLSB / 256.0) + (rc.maxBitRate ? strf(", cap %.0f Mbit/s", rc.maxBitRate / 1e6) : std::string(", no cap"));
            m->summary = rate + ", " + tools;
            m->reorder = bframes;
            initialized = true;
            break;
        }
        refusal = m->fail("nvEncInitializeEncoder", st);
        TMVS_LOG("venc: NVENC refused %s: %s", tools.c_str(), refusal.c_str());
    }
    if (!initialized) {
        error = refusal;
        m->destroy();
        return false;
    }

    // The pairs the encoder should need, more on demand (it holds a few frames more than its
    // B-frames and lookahead) up to a limit that only a misbehaving driver reaches.
    m->maxPairs = static_cast<size_t>(bframes + lookahead + 16);
    if (m->async && !(m->eosEvent = m->registeredEvent(error))) {
        m->destroy();
        return false;
    }
    for (int i = 0; i < bframes + lookahead + 3; i++) {
        if (!m->addPair(error)) {
            m->destroy();
            return false;
        }
    }

    std::vector<uint8_t> header(1024);
    uint32_t headerSize = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD payload{};
    payload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    payload.inBufferSize = static_cast<uint32_t>(header.size());
    payload.spsppsBuffer = header.data();
    payload.outSPSPPSPayloadSize = &headerSize;
    st = m->fn().nvEncGetSequenceParams(enc, &payload);
    if (st != NV_ENC_SUCCESS) {
        error = m->fail("nvEncGetSequenceParams", st);
        m->destroy();
        return false;
    }
    header.resize(headerSize);
    m->header = header;
    m->sent = 0;
    return true;
}

bool Session::encode(const uint8_t* frame, const PacketSink& sink, std::string& error) {
    if (!m->encoder) {
        error = "not open";
        return false;
    }
    if (m->free.empty() && !m->addPair(error)) {
        // The pool is full: wait for the oldest frame (asynchronous mode; synchronous mode
        // has nothing to wait for).
        if (!m->async || m->pending.empty() || !m->collect(1, 0, sink, error)) return false;
        error.clear();
    }
    const size_t slot = m->free.front();
    m->free.pop_front();
    NV_ENC_LOCK_INPUT_BUFFER lock{};
    lock.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
    lock.inputBuffer = m->inputs[slot];
    NVENCSTATUS st = m->fn().nvEncLockInputBuffer(m->encoder, &lock);
    if (st != NV_ENC_SUCCESS) {
        m->free.push_front(slot);
        error = m->fail("nvEncLockInputBuffer", st);
        return false;
    }
    // Y rows, then the interleaved UV rows right after `height` rows of the pitch.
    const size_t row = static_cast<size_t>(m->width) * (m->tenBit ? 2 : 1);
    uint8_t* dst = static_cast<uint8_t*>(lock.bufferDataPtr);
    const uint8_t* src = frame;
    for (int y = 0; y < m->height + m->height / 2; y++) {
        memcpy(dst + static_cast<size_t>(y) * lock.pitch, src, row);
        src += row;
    }
    m->fn().nvEncUnlockInputBuffer(m->encoder, m->inputs[slot]);

    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth = static_cast<uint32_t>(m->width);
    pic.inputHeight = static_cast<uint32_t>(m->height);
    pic.inputPitch = lock.pitch;
    pic.inputBuffer = m->inputs[slot];
    pic.outputBitstream = m->outputs[slot];
    pic.bufferFmt = m->format;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = static_cast<uint64_t>(m->sent);
    pic.inputDuration = 1;
    pic.frameIdx = static_cast<uint32_t>(m->sent);
    pic.completionEvent = m->async ? m->events[slot] : nullptr;
    if (m->hdr) {
        pic.codecPicParams.hevcPicParams.pMasteringDisplay = &m->mastering;
        pic.codecPicParams.hevcPicParams.pMaxCll = &m->light;
    }
    st = m->fn().nvEncEncodePicture(m->encoder, &pic);
    m->pending.push_back(slot);
    m->maxPending = std::max(m->maxPending, m->pending.size());
    m->sent++;
    if (st == NV_ENC_ERR_NEED_MORE_INPUT) return !m->async || m->collect(0, 0, sink, error); // a B-frame or the lookahead: its buffers stay busy
    if (st != NV_ENC_SUCCESS) {
        error = m->fail("nvEncEncodePicture", st);
        return false;
    }
    // Success releases every output buffer that went in so far. All but the newest two are
    // read now: the card keeps working on those while the CPU copies the next frame.
    m->ready = m->pending.size();
    return m->collect(0, 2, sink, error);
}

bool Session::flush(const PacketSink& sink, std::string& error) {
    if (!m->encoder) return true;
    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    pic.completionEvent = m->eosEvent;
    const NVENCSTATUS st = m->fn().nvEncEncodePicture(m->encoder, &pic);
    if (st != NV_ENC_SUCCESS) {
        error = m->fail("nvEncEncodePicture (end of stream)", st);
        return false;
    }
    m->ready = m->pending.size();
    TMVS_LOG("venc: NVENC held up to %u frames at once, %u buffer pairs", static_cast<unsigned>(m->maxPending), static_cast<unsigned>(m->inputs.size()));
    return m->collect(m->pending.size(), 0, sink, error);
}

const std::vector<uint8_t>& Session::sequenceHeader() const {
    return m->header;
}

int Session::reorderDelay() const {
    return m->reorder;
}

const std::string& Session::summary() const {
    return m->summary;
}

} // namespace nvenc
} // namespace venc
} // namespace tmshaders
