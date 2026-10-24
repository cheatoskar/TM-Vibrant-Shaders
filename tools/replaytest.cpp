// Replay reader test: the LZO1X decompressor on made-up streams, then real replays (the files
// given, or the .Replay.Gbx in Documents\TrackMania\Tracks\Replays): the header's map UID and the
// ghost's race and checkpoint times (never falling, the last one the race time).
//   tmvs_replaytest [file.Replay.Gbx ...]
#include "misc/replay.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace tmshaders::misc;

namespace {

int g_failed = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        g_failed++;
        printf("FAIL %s\n", what);
    }
}

// Streams built by hand from the format: a literal run with the end marker, a match that
// repeats the run (overlapping copy), and garbage that must be refused.
void lzoUnits() {
    {
        // 18 + 3 = 21 > 17: three literals, then the end marker (M4 with distance 0).
        const unsigned char in[] = {17 + 3, 'a', 'b', 'c', 0x11, 0x00, 0x00};
        unsigned char out[3] = {};
        check(lzoDecompress(in, sizeof(in), out, 3) && memcmp(out, "abc", 3) == 0, "lzo: literals and end marker");
        unsigned char longer[4] = {};
        check(!lzoDecompress(in, sizeof(in), longer, 4), "lzo: refuses a size that doesn't match");
    }
    {
        // "abcd" then an M2 match: length 3+... copies "abcd" again from distance 4 with a trailing
        // literal count of 0: byte = ((len-1) << 5) | ((dist-1) & 7) << 2, next = (dist-1) >> 3.
        // len 4 -> (4-1)=3 -> 3<<5 = 0x60; dist 4 -> 3 -> (3&7)<<2 = 0x0c; next 0.
        const unsigned char in[] = {17 + 4, 'a', 'b', 'c', 'd', 0x6c, 0x00, 0x11, 0x00, 0x00};
        unsigned char out[8] = {};
        check(lzoDecompress(in, sizeof(in), out, 8) && memcmp(out, "abcdabcd", 8) == 0, "lzo: M2 match");
    }
    {
        // A match reaching before the start of the output.
        const unsigned char in[] = {17 + 1, 'a', 0x6c, 0x05, 0x11, 0x00, 0x00};
        unsigned char out[8] = {};
        check(!lzoDecompress(in, sizeof(in), out, 8), "lzo: refuses a match before the output");
        const unsigned char cut[] = {17 + 4, 'a', 'b'};
        check(!lzoDecompress(cut, sizeof(cut), out, 4), "lzo: refuses a cut stream");
    }
}

bool readAll(const std::wstring& path, std::vector<unsigned char>& data) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data.resize(n > 0 ? n : 0);
    const bool ok = n > 0 && fread(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

int g_files = 0, g_parsed = 0;

void testFile(const std::wstring& path, bool verbose) {
    g_files++;
    Replay header, full;
    std::string error, headerError;
    const bool h = readReplayHeader(path, header, headerError);
    const bool ok = readReplay(path, full, error);
    const wchar_t* base = wcsrchr(path.c_str(), L'\\');
    base = base ? base + 1 : path.c_str();
    char name[512] = {}; // UTF-8: printf's %ls stops at the first character the console can't show
    WideCharToMultiByte(CP_UTF8, 0, base, -1, name, sizeof(name) - 1, nullptr, nullptr);
    if (!ok) {
        printf("  %-60s  %s\n", name, error.c_str());
        return;
    }
    g_parsed++;
    check(h && header.mapUid == full.mapUid, "header alone gives the same map UID");
    bool rising = true;
    for (size_t i = 1; i < full.checkpoints.size(); i++) rising = rising && full.checkpoints[i] >= full.checkpoints[i - 1]; // two in one tick: equal
    const bool lastIsTime = full.checkpoints.back() == full.raceTime;
    if (verbose || !rising || !lastIsTime) {
        std::string cps;
        for (size_t i = 0; i < full.checkpoints.size() && i < 6; i++) cps += (i ? " " : "") + std::to_string(full.checkpoints[i]);
        printf("  %-60s  map %s, %d ms, %zu CPs (%s%s)%s%s\n", name, full.mapUid.c_str(), full.raceTime,
               full.checkpoints.size(), cps.c_str(), full.checkpoints.size() > 6 ? " ..." : "", rising ? "" : " [FALLING]",
               lastIsTime ? "" : " [LAST CP != TIME]");
    }
    check(rising, "checkpoint times never fall");
    check(lastIsTime, "last checkpoint is the race time");
}

void scanFolder(const std::wstring& folder, int depth, int& budget) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (budget <= 0) break;
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring path = folder + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0) scanFolder(path, depth - 1, budget);
        } else if (name.size() > 11 && _wcsicmp(name.c_str() + name.size() - 11, L".Replay.Gbx") == 0) {
            // Only TrackMania Forever's (class 0x03093000): the folder may hold TrackMania 2020's too.
            Replay header;
            std::string error;
            if (!readReplayHeader(path, header, error) && error.compare(0, 12, "not a replay") == 0) continue;
            budget--;
            testFile(path, budget > 290);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    lzoUnits();
    if (argc > 1) {
        for (int i = 1; i < argc; i++) testFile(argv[i], true);
    } else {
        wchar_t docs[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) {
            int budget = 300;
            scanFolder(std::wstring(docs) + L"\\TrackMania\\Tracks\\Replays", 2, budget);
        }
    }
    printf("%d replays, %d read with their times; %d checks failed\n", g_files, g_parsed, g_failed);
    // A file that can't be read is reported above, not a failure (old formats, corrupt files);
    // most must read.
    return g_failed == 0 && (g_files == 0 || g_parsed * 10 >= g_files * 8) ? 0 : 1;
}
