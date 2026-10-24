#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Replay files (.Replay.Gbx, TrackMania Nations/United Forever): the map they were driven on and
// the ghost's race and checkpoint times, for the split times. Nothing else of the ghost is read
// (not its path). No game code: the file alone (the GBX format as GBX.NET and the community docs
// describe it, written from scratch).
//
// A GBX file: "GBX", version 6, the format letters (B = binary; U/C = reference table and body
// compressed or not; R), the class (0x03093000 CGameCtnReplayRecord), the header chunks (the
// map's UID, the time, the driver: read without the body), the node count, the reference table,
// then the body: LZO1X compressed (uncompressed size, compressed size, data). In the body the
// ghost (CGameCtnGhost) keeps its race time (skippable chunk 0x03092005) and its checkpoint times
// (skippable chunk 0x0309200B).
namespace tmshaders {
namespace misc {

struct Replay {
    std::string mapUid;      // the map's UID (its header)
    std::string driver;      // nickname ("" unknown)
    int raceTime = -1;       // ms (the ghost's, else the header's)
    std::vector<int> checkpoints; // race time at each checkpoint (ms), the last one the finish
};

// Only the header (fast: for the file list). False: not a replay, or unreadable.
bool readReplayHeader(const std::wstring& path, Replay& out, std::string& error);
// Header and body: the times. False with the reason.
bool readReplay(const std::wstring& path, Replay& out, std::string& error);
// The same from bytes in memory (the test tool, a download).
bool parseReplay(const std::vector<unsigned char>& file, bool body, Replay& out, std::string& error);

// LZO1X decompression (the GBX body). False: corrupt or not exactly `size` bytes out.
bool lzoDecompress(const unsigned char* in, size_t inSize, unsigned char* out, size_t size);

} // namespace misc
} // namespace tmshaders
