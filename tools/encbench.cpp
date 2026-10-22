// Encoder benchmark for the video export (src/videoencoder.cpp): a reference video decoded once
// by ffmpeg to raw NV12, sent through venc::Encoder with the options of each variant, and every
// result measured: encoder speed, size, Mbit/s and VMAF (mean and the worst frame) against the
// reference, through ffmpeg's libvmaf. A 32-bit program like the DLL, so the same 32-bit
// encoders run (nvEncodeAPI.dll from SysWOW64, Media Foundation's x86 MFTs).
//
//   tmvs_encbench <reference.mp4> <outdir> [--seconds 8] [--ffmpeg ffmpeg.exe] [--no-vmaf]
//                 [--keep] [--threads n] [--batch file] [variant ...]
//
// A variant: a name and key=value pairs, on the command line as one argument ("name k=v k=v")
// or one per line in the batch file (# starts a comment):
//   backend=mf|ffmpeg  codec=h264|hevc|av1  ten=1  hdr=1  quality=0..100  bitrate=<Mbit/s>
//   q=<encoder number: NVENC CQ / x264 CRF>  rc=auto|quality|qp|vbr  hw=0  nvenc=0
//   preset=1..7  bframes=n  lookahead=n  aq=0..2  aqs=0..15  multipass=0..2  maxrate=<Mbit/s>
//   ffpreset=0..2  extra="<more ffmpeg encoder options>"
//
// Output: one line per variant on the console and in <outdir>\results.tsv. The encoder's speed
// is the throughput of its worker (frames / the time from the first frame to the last one
// through the encoder); "total" includes reading the frames, flushing and writing the MP4.
#include "log.h"
#include "videoencoder.h"
#include <windows.h>
#include <objbase.h>
#include <psapi.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")

using namespace tmshaders;

namespace {

std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], n);
    return out;
}

std::string narrow(const std::wstring& s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &out[0], n, nullptr, nullptr);
    return out;
}

double nowMs() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / f.QuadPart;
}

uint64_t fileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    return (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

// Runs a command line, its stdout (and stderr) into `out`. Exit code, -1 when it did not start.
int run(const std::wstring& cmd, std::string* out) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 1 << 16)) return -1;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring line = cmd;
    const bool ok = CreateProcessW(nullptr, &line[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != FALSE;
    CloseHandle(writeEnd);
    if (!ok) {
        CloseHandle(readEnd);
        return -1;
    }
    char chunk[4096];
    DWORD got = 0;
    while (ReadFile(readEnd, chunk, sizeof(chunk), &got, nullptr) && got) {
        if (out) out->append(chunk, got);
    }
    CloseHandle(readEnd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

std::wstring quote(const std::wstring& s) {
    return L"\"" + s + L"\"";
}

// Address space this process has reserved or committed, MB: what a 32-bit game has to spare.
double addressSpaceMb() {
    double total = 0.0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t p = 0; VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)); p = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_FREE) total += static_cast<double>(mbi.RegionSize);
        if (reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize < p) break;
    }
    return total / 1048576.0;
}

struct Variant {
    std::string name;
    venc::Options o;
};

// "name k=v k=v" with values in double quotes allowed.
bool parseVariant(const std::string& text, Variant& v, std::string& error) {
    std::vector<std::string> tokens;
    std::string cur;
    bool inQuote = false, any = false;
    for (char c : text) {
        if (c == '"') {
            inQuote = !inQuote;
            any = true;
        } else if ((c == ' ' || c == '\t') && !inQuote) {
            if (any || !cur.empty()) tokens.push_back(cur);
            cur.clear();
            any = false;
        } else {
            cur += c;
        }
    }
    if (any || !cur.empty()) tokens.push_back(cur);
    if (tokens.empty()) return false;
    v.name = tokens[0];
    venc::Options& o = v.o;
    for (size_t i = 1; i < tokens.size(); i++) {
        const size_t eq = tokens[i].find('=');
        if (eq == std::string::npos) {
            error = "no '=' in " + tokens[i];
            return false;
        }
        const std::string k = tokens[i].substr(0, eq), val = tokens[i].substr(eq + 1);
        const double d = atof(val.c_str());
        const int n = atoi(val.c_str());
        if (k == "backend") o.backend = val == "ffmpeg" ? venc::Backend::Ffmpeg : venc::Backend::MediaFoundation;
        else if (k == "codec") o.codec = val == "hevc" ? venc::Codec::HEVC : val == "av1" ? venc::Codec::AV1 : venc::Codec::H264;
        else if (k == "ten") o.tenBit = n != 0;
        else if (k == "hdr") o.hdr10 = n != 0;
        else if (k == "quality") o.quality = n;
        else if (k == "bitrate") o.bitrate = d * 1e6;
        else if (k == "q") o.encoderQ = d;
        else if (k == "rc") o.rateControl = val == "qp" ? venc::RateControl::ConstantQP : val == "quality" ? venc::RateControl::Quality : val == "vbr" ? venc::RateControl::VBR : venc::RateControl::Auto;
        else if (k == "hw") o.hardware = n != 0;
        else if (k == "nvenc") o.nvenc = n != 0;
        else if (k == "preset") o.nvencPreset = n;
        else if (k == "bframes") o.nvencBFrames = n;
        else if (k == "lookahead") o.nvencLookahead = n;
        else if (k == "aq") o.nvencAQ = n;
        else if (k == "aqs") o.nvencAQStrength = n;
        else if (k == "multipass") o.nvencMultipass = n;
        else if (k == "maxrate") o.nvencMaxRate = d;
        else if (k == "ffpreset") o.ffmpegPreset = n;
        else if (k == "extra") o.ffmpegExtra = widen(val);
        else {
            error = "unknown key " + k;
            return false;
        }
    }
    return true;
}

// VMAF of `encoded` against the raw reference: mean and minimum (-1 when it failed).
bool vmaf(const std::wstring& ffmpeg, const std::wstring& encoded, const std::wstring& raw, int w, int h, double fps, int threads, double& mean,
          double& minimum) {
    mean = minimum = -1.0;
    DeleteFileW(L"vmaf.json");
    wchar_t size[64];
    swprintf(size, 64, L"%dx%d", w, h);
    wchar_t rate[32];
    swprintf(rate, 32, L"%g", fps);
    wchar_t filter[512];
    // Both to 8-bit 4:2:0 planar (a 10-bit encode is rounded), both timed from zero.
    swprintf(filter, 512,
             L"[0:v]format=yuv420p,setpts=PTS-STARTPTS[a];[1:v]format=yuv420p,setpts=PTS-STARTPTS[b];[a][b]libvmaf=n_threads=%d:log_fmt=json:log_path=vmaf.json",
             threads);
    const std::wstring cmd = quote(ffmpeg) + L" -hide_banner -v error -i " + quote(encoded) + L" -f rawvideo -pix_fmt nv12 -video_size " + size +
                             L" -framerate " + rate + L" -i " + quote(raw) + L" -lavfi \"" + filter + L"\" -f null -";
    std::string out;
    if (run(cmd, &out) != 0) {
        fprintf(stderr, "vmaf failed: %s\n", out.c_str());
        return false;
    }
    FILE* f = _wfopen(L"vmaf.json", L"rb");
    if (!f) return false;
    std::string json;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) json.append(buf, n);
    fclose(f);
    const size_t pooled = json.find("\"pooled_metrics\"");
    if (pooled == std::string::npos) return false;
    const size_t v = json.find("\"vmaf\"", pooled);
    if (v == std::string::npos) return false;
    const size_t mn = json.find("\"min\"", v), me = json.find("\"mean\"", v);
    if (mn == std::string::npos || me == std::string::npos) return false;
    minimum = atof(json.c_str() + json.find(':', mn) + 1);
    mean = atof(json.c_str() + json.find(':', me) + 1);
    return true;
}

int countFrames(const std::wstring& ffprobe, const std::wstring& file) {
    std::string out;
    if (run(quote(ffprobe) + L" -v error -select_streams v:0 -count_packets -show_entries stream=nb_read_packets -of csv=p=0 " + quote(file), &out) != 0) return -1;
    return atoi(out.c_str());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: tmvs_encbench <reference.mp4> <outdir> [--seconds 8] [--ffmpeg ffmpeg.exe] [--no-vmaf] [--keep] [--threads n] "
                        "[--batch file] [\"name key=value ...\" ...]\n");
        return 2;
    }
    log::setFileName(L"encbench.log");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    wchar_t full[MAX_PATH];
    GetFullPathNameW(widen(argv[1]).c_str(), MAX_PATH, full, nullptr);
    const std::wstring reference = full;
    const std::wstring outdir = widen(argv[2]);
    CreateDirectoryW(outdir.c_str(), nullptr);
    double seconds = 8.0;
    std::wstring ffmpeg;
    bool doVmaf = true, keep = false;
    int threads = 12;
    std::vector<Variant> variants;
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        std::string error;
        if (a == "--seconds" && i + 1 < argc) seconds = atof(argv[++i]);
        else if (a == "--ffmpeg" && i + 1 < argc) ffmpeg = widen(argv[++i]);
        else if (a == "--no-vmaf") doVmaf = false;
        else if (a == "--keep") keep = true;
        else if (a == "--threads" && i + 1 < argc) threads = atoi(argv[++i]);
        else if (a == "--batch" && i + 1 < argc) {
            FILE* f = fopen(argv[++i], "r");
            if (!f) {
                fprintf(stderr, "cannot read %s\n", argv[i]);
                return 2;
            }
            char line[2048];
            while (fgets(line, sizeof(line), f)) {
                std::string s = line;
                const size_t hash = s.find('#');
                if (hash != std::string::npos) s.resize(hash);
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
                Variant v;
                if (s.empty()) continue;
                if (!parseVariant(s, v, error)) {
                    fprintf(stderr, "%s: %s\n", s.c_str(), error.c_str());
                    return 2;
                }
                variants.push_back(v);
            }
            fclose(f);
        } else {
            Variant v;
            if (!parseVariant(a, v, error)) {
                fprintf(stderr, "%s: %s\n", a.c_str(), error.c_str());
                return 2;
            }
            variants.push_back(v);
        }
    }
    SetCurrentDirectoryW(outdir.c_str());
    const venc::FfmpegInfo ff = venc::findFfmpeg(ffmpeg);
    if (ff.path.empty()) {
        fprintf(stderr, "no ffmpeg.exe (--ffmpeg)\n");
        return 2;
    }
    ffmpeg = ff.path;
    std::wstring ffprobe = ffmpeg.substr(0, ffmpeg.find_last_of(L"\\/") + 1) + L"ffprobe.exe";
    printf("encoders: %s\n", venc::describeEncoders(ffmpeg).c_str());

    // The reference's size and rate, then the raw frames (once; kept between runs).
    std::string probe;
    if (run(quote(ffprobe) + L" -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate -of csv=p=0 " + quote(reference), &probe) != 0) {
        fprintf(stderr, "ffprobe failed: %s\n", probe.c_str());
        return 1;
    }
    int w = 0, h = 0, rn = 0, rd = 1;
    if (sscanf(probe.c_str(), "%d,%d,%d/%d", &w, &h, &rn, &rd) < 3 || w <= 0 || h <= 0 || rn <= 0) {
        fprintf(stderr, "cannot read the reference's format: %s\n", probe.c_str());
        return 1;
    }
    const double fps = static_cast<double>(rn) / std::max(1, rd);
    wchar_t rawName[128];
    swprintf(rawName, 128, L"ref_%gs_%dx%d.nv12", seconds, w, h);
    const std::wstring raw = rawName;
    const size_t frameBytes = venc::Encoder::frameBytes(venc::Pixels::NV12, w, h);
    if (fileSize(raw) == 0 || fileSize(raw) % frameBytes) {
        printf("decoding %g s of the reference to %s ...\n", seconds, narrow(raw).c_str());
        wchar_t t[32];
        swprintf(t, 32, L"%g", seconds);
        std::string out;
        if (run(quote(ffmpeg) + L" -hide_banner -v error -y -i " + quote(reference) + L" -t " + t + L" -an -f rawvideo -pix_fmt nv12 " + quote(raw), &out) != 0) {
            fprintf(stderr, "decoding failed: %s\n", out.c_str());
            return 1;
        }
    }
    const int frames = static_cast<int>(fileSize(raw) / frameBytes);
    printf("reference: %dx%d at %g fps, %d frames\n", w, h, fps, frames);

    FILE* tsv = _wfopen(L"results.tsv", L"a");
    if (tsv && ftell(tsv) == 0) fprintf(tsv, "name\tMbit/s\tencoder fps\ttotal fps\tVMAF mean\tVMAF min\tframes\taddress MB\tdescription\n");
    printf("%-28s %8s %8s %8s %7s %7s %6s %6s  %s\n", "variant", "Mbit/s", "enc fps", "tot fps", "VMAF", "min", "frames", "addrMB", "encoder");
    for (Variant& v : variants) {
        v.o.width = w;
        v.o.height = h;
        v.o.fps = fps;
        if (v.o.backend == venc::Backend::Ffmpeg) v.o.ffmpegPath = ffmpeg;
        const std::wstring out = widen(v.name) + L".mp4";
        DeleteFileW(out.c_str());
        HANDLE in = CreateFileW(raw.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (in == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "cannot read %s\n", narrow(raw).c_str());
            return 1;
        }
        const double space0 = addressSpaceMb();
        double spacePeak = space0;
        venc::Encoder enc;
        std::string error;
        const double t0 = nowMs();
        if (!enc.open(out, v.o, error)) {
            printf("%-28s failed to open: %s\n", v.name.c_str(), error.c_str());
            CloseHandle(in);
            continue;
        }
        int sent = 0;
        for (; sent < frames; sent++) {
            uint8_t* p = enc.acquire(venc::Pixels::NV12);
            if (!p) break;
            DWORD got = 0;
            if (!ReadFile(in, p, static_cast<DWORD>(frameBytes), &got, nullptr) || got != frameBytes) break;
            enc.submit();
            if (sent % 30 == 0) spacePeak = std::max(spacePeak, addressSpaceMb());
        }
        CloseHandle(in);
        const std::string desc = enc.description();
        const venc::Stats st = enc.stats();
        std::string report;
        const bool closed = enc.close(report);
        const double total = nowMs() - t0;
        const venc::Stats done = enc.stats();
        const double encFps = done.wallMs > 0.0 ? done.frames * 1000.0 / done.wallMs : 0.0;
        const double totFps = sent * 1000.0 / total;
        const double mbps = fileSize(out) * 8.0 / (sent / fps) / 1e6;
        const int got = countFrames(ffprobe, out);
        double mean = -1.0, minimum = -1.0;
        if (doVmaf && closed) vmaf(ffmpeg, out, raw, w, h, fps, threads, mean, minimum);
        printf("%-28s %8.1f %8.1f %8.1f %7.2f %7.2f %6d %6.0f  %s\n", v.name.c_str(), mbps, encFps, totFps, mean, minimum, got, spacePeak - space0, desc.c_str());
        if (!closed) printf("    close: %s\n", report.c_str());
        fflush(stdout);
        if (tsv) {
            fprintf(tsv, "%s\t%.2f\t%.1f\t%.1f\t%.2f\t%.2f\t%d\t%.0f\t%s\n", v.name.c_str(), mbps, encFps, totFps, mean, minimum, got, spacePeak - space0, desc.c_str());
            fflush(tsv);
        }
        TMVS_LOG("encbench %s: %s", v.name.c_str(), report.c_str());
        (void)st;
        if (!keep) DeleteFileW(out.c_str());
    }
    if (tsv) fclose(tsv);
    return 0;
}
