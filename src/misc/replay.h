#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Replay files (.Replay.Gbx, TrackMania Nations/United Forever): the map they were driven on, the
// ghost's race and checkpoint times and its speed over time, for the split times. Its path is not
// kept: the samples' positions only give the speed between them and are dropped right away. No game
// code: the file alone (the GBX format as GBX.NET and the community docs describe it, written from
// scratch).
//
// A GBX file: "GBX", version 6, the format letters (B = binary; U/C = reference table and body
// compressed or not; R), the class (0x03093000 CGameCtnReplayRecord), the header chunks (the
// map's UID, the time, the driver: read without the body), the node count, the reference table,
// then the body: LZO1X compressed (uncompressed size, compressed size, data). In the body the
// ghost (CGameCtnGhost) keeps its race time (skippable chunk 0x03092005), its checkpoint times
// (skippable chunk 0x0309200B) and its samples (CGameGhost 0x0303F005: zlib-compressed records at a
// fixed period, each one starting with the car's position).
namespace tmshaders {
namespace misc {

struct Replay {
    std::string mapUid;      // the map's UID (its header)
    std::string driver;      // nickname ("" unknown)
    int raceTime = -1;       // ms (the ghost's, else the header's)
    std::vector<int> checkpoints; // race time at each checkpoint (ms), the last one the finish
    int samplePeriod = 0;         // ms between speeds (0: none read)
    std::vector<float> speeds;    // m/s at 0, samplePeriod, 2 samplePeriod, ... ms of the race
};

// Only the header (fast: for the file list). False: not a replay, or unreadable.
bool readReplayHeader(const std::wstring& path, Replay& out, std::string& error);
// Header and body: the times. False with the reason.
bool readReplay(const std::wstring& path, Replay& out, std::string& error);
// The same from bytes in memory (the test tool, a download).
bool parseReplay(const std::vector<unsigned char>& file, bool body, Replay& out, std::string& error);

// The ghost's speed (m/s) at a race time (ms), -1 unknown.
float speedAt(const Replay& replay, int ms);

// LZO1X decompression (the GBX body). False: corrupt or not exactly `size` bytes out.
bool lzoDecompress(const unsigned char* in, size_t inSize, unsigned char* out, size_t size);

} // namespace misc
} // namespace tmshaders
