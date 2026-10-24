#include "blockorder.h"
#include "log.h"
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace blockorder {
namespace {

// Offsets of the engine's classes (TMNF, see blockorder.h). TMUF is taken to be the same: every
// scan checks the chain block -> mobil -> item -> mobil -> block and gives up (rank 0 everywhere,
// logged) when it does not hold.
constexpr size_t kChallengeBlocks = 0x54;   // CFastBuffer<CGameCtnBlock*>: count, then the data pointer
constexpr size_t kBlockUnits = 0x28;        // CFastArray<CGameCtnBlockUnit*>: count, data
constexpr size_t kBlockMobil = 0x30;        // CSceneMobil*
constexpr size_t kBlockCoord = 0x48;        // x, y, z (ints)
constexpr size_t kUnitOffset = 0x28;        // x, y, z (ints), added to the block's coordinate
constexpr size_t kMobilBlock = 0x1c;        // CSceneMobil -> its CGameCtnBlock (CGameCtnBlock::SetMobilAndHelper)
constexpr size_t kMobilItem = 0x28;         // CSceneMobil -> CHmsItem
constexpr size_t kItemMobil = 0x40;         // CHmsItem -> CSceneMobil
constexpr size_t kCorpusItem = 0x48;        // CHmsCorpus -> CHmsItem
constexpr size_t kDelayedCorpus = 0x00;     // SDelayedRender: the corpus current when it was queued
constexpr int kMaxRank = 7;
constexpr uint32_t kMaxBlocks = 200000;
constexpr uint32_t kMaxUnits = 128;         // per block (a block spans a handful)
constexpr DWORD kPollMs = 250;              // how often the block array is looked at for changes

// The render thread alone calls beginDraw, scans and reads; invalidate() may come from the game's
// main thread and only sets a flag.
volatile LONG g_dirty = 1;
const void* g_challenge = nullptr;
bool g_scanned = false;
uint32_t g_scanCount = 0;
uintptr_t g_scanData = 0;
DWORD g_lastPoll = 0;
std::unordered_map<uintptr_t, uint8_t> g_itemRank; // CHmsItem -> rank (0 included: the block is known)
int g_rank = 0;
// What the draws found, logged once per map after enough of them.
uint32_t g_draws = 0, g_known = 0, g_ranked = 0, g_delayedDraws = 0, g_delayedKnown = 0;
bool g_reported = false;

struct BlockRead {
    uintptr_t mobil = 0;
    uintptr_t item = 0;
    bool chainOk = false;
    int x = 0, y = 0, z = 0;
    uint32_t nUnits = 0;
    int units[kMaxUnits][3];
};

bool read32(uintptr_t address, uintptr_t& out) {
    __try {
        out = *reinterpret_cast<const uint32_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readBuffer(const void* challenge, uint32_t& count, uintptr_t& data) {
    uintptr_t c = 0, d = 0;
    if (!challenge || !read32(reinterpret_cast<uintptr_t>(challenge) + kChallengeBlocks, c) || !read32(reinterpret_cast<uintptr_t>(challenge) + kChallengeBlocks + 4, d)) return false;
    count = static_cast<uint32_t>(c);
    data = d;
    return true;
}

// One block's mobil, item, coordinate and units. False: unreadable.
bool readBlock(uintptr_t block, BlockRead& out) {
    __try {
        const BYTE* b = reinterpret_cast<const BYTE*>(block);
        out.mobil = *reinterpret_cast<const uint32_t*>(b + kBlockMobil);
        out.x = *reinterpret_cast<const int*>(b + kBlockCoord);
        out.y = *reinterpret_cast<const int*>(b + kBlockCoord + 4);
        out.z = *reinterpret_cast<const int*>(b + kBlockCoord + 8);
        if (out.mobil) {
            const BYTE* m = reinterpret_cast<const BYTE*>(out.mobil);
            out.item = *reinterpret_cast<const uint32_t*>(m + kMobilItem);
            const uintptr_t back = *reinterpret_cast<const uint32_t*>(m + kMobilBlock);
            if (out.item) {
                const uintptr_t itemMobil = *reinterpret_cast<const uint32_t*>(out.item + kItemMobil);
                out.chainOk = back == block && itemMobil == out.mobil;
            }
        }
        const uint32_t count = *reinterpret_cast<const uint32_t*>(b + kBlockUnits);
        const BYTE* const* data = *reinterpret_cast<const BYTE* const* const*>(b + kBlockUnits + 4);
        if (count > kMaxUnits || (count && !data)) return false;
        out.nUnits = count;
        for (uint32_t i = 0; i < count; i++) {
            const BYTE* u = data[i];
            out.units[i][0] = out.x + *reinterpret_cast<const int*>(u + kUnitOffset);
            out.units[i][1] = out.y + *reinterpret_cast<const int*>(u + kUnitOffset + 4);
            out.units[i][2] = out.z + *reinterpret_cast<const int*>(u + kUnitOffset + 8);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t unitKey(int x, int y, int z) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x) & 0x1fffff) << 42) | (static_cast<uint64_t>(static_cast<uint32_t>(y) & 0x1fffff) << 21) | (static_cast<uint64_t>(static_cast<uint32_t>(z) & 0x1fffff));
}

void scan() {
    g_dirty = 0;
    g_scanned = true;
    g_itemRank.clear();
    g_draws = g_known = g_ranked = g_delayedDraws = g_delayedKnown = 0;
    g_reported = false;
    uint32_t count = 0;
    uintptr_t data = 0;
    if (!readBuffer(g_challenge, count, data)) {
        g_scanCount = 0;
        g_scanData = 0;
        return;
    }
    g_scanCount = count;
    g_scanData = data;
    if (count == 0 || count > kMaxBlocks || !data) return;

    // The blocks of each grid unit, in array (placement) order.
    std::vector<uintptr_t> items(count, 0);
    std::vector<std::vector<uint64_t>> cells(count);
    std::unordered_map<uint64_t, std::vector<uint32_t>> groups;
    groups.reserve(count * 2);
    uint32_t unreadable = 0, withMobil = 0, chainOk = 0;
    BlockRead r;
    for (uint32_t i = 0; i < count; i++) {
        uintptr_t block = 0;
        r = BlockRead();
        if (!read32(data + 4 * i, block) || !block || !readBlock(block, r)) {
            unreadable++;
            continue;
        }
        if (r.mobil) withMobil++;
        if (!r.chainOk) continue;
        chainOk++;
        items[i] = r.item;
        for (uint32_t u = 0; u < r.nUnits; u++) {
            const uint64_t key = unitKey(r.units[u][0], r.units[u][1], r.units[u][2]);
            std::vector<uint32_t>& g = groups[key];
            if (g.empty() || g.back() != i) {
                g.push_back(i);
                cells[i].push_back(key);
            }
        }
    }
    if (withMobil == 0 || chainOk * 10 < withMobil * 9) {
        TMVS_LOG("blockorder: %u blocks, the engine's block -> mobil -> item chain does not hold (%u of %u mobils, %u unreadable): no depth ranks", count, chainOk, withMobil, unreadable);
        return;
    }
    // Each block one above every earlier block it shares a unit with (the first placed: 0, no
    // bias), so in every overlapping pair the later one ranks higher, also when a large block
    // shares only one of its units (its place in each unit's list alone would not say that).
    std::vector<uint8_t> rank(count, 0);
    std::vector<uint8_t> overlapping(count, 0);
    size_t largest = 0;
    for (const auto& entry : groups) {
        if (entry.second.size() < 2) continue;
        largest = std::max(largest, entry.second.size());
        for (uint32_t i : entry.second) overlapping[i] = 1;
    }
    for (uint32_t i = 0; i < count; i++) {
        for (uint64_t key : cells[i]) {
            for (uint32_t j : groups[key]) {
                if (j >= i) break; // the list is in placement order
                rank[i] = static_cast<uint8_t>(std::max<int>(rank[i], std::min<int>(rank[j] + 1, kMaxRank)));
            }
        }
    }
    uint32_t inGroups = 0;
    g_itemRank.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        if (!items[i]) continue;
        if (overlapping[i]) inGroups++;
        g_itemRank[items[i]] = rank[i];
    }
    TMVS_LOG("blockorder: %u blocks, %u in overlap groups (largest %zu)", count, inGroups, largest);
}

// A change in the block array (a block placed or removed in the editor) or an invalidate().
void pollChanges() {
    if (!g_challenge) return;
    const DWORD now = GetTickCount();
    if (!g_dirty && g_scanned && now - g_lastPoll < kPollMs) return;
    g_lastPoll = now;
    if (!g_dirty && g_scanned) {
        uint32_t count = 0;
        uintptr_t data = 0;
        if (readBuffer(g_challenge, count, data) && count == g_scanCount && data == g_scanData) return;
    }
    scan();
}

bool itemOf(uintptr_t corpus, uintptr_t& item) {
    return corpus && read32(corpus + kCorpusItem, item);
}

void report() {
    g_reported = true;
    TMVS_LOG("blockorder: after %u draws: %u of them from a known block, %u of those with a depth rank; the sorted passes: %u draws, %u known", g_draws, g_known, g_ranked, g_delayedDraws, g_delayedKnown);
}

} // namespace

int currentDrawRank() { return g_rank; }

void invalidate() { InterlockedExchange(&g_dirty, 1); }

void setChallenge(const void* challenge) {
    if (challenge == g_challenge) return;
    g_challenge = challenge;
    g_rank = 0;
    g_scanned = false;
    g_scanCount = 0;
    g_scanData = 0;
    g_itemRank.clear();
    invalidate();
}

int beginDraw(const void* corpus, bool delayedPass) {
    const int previous = g_rank;
    if (!g_challenge) {
        g_rank = 0;
        return previous;
    }
    pollChanges();
    if (g_itemRank.empty()) {
        g_rank = 0;
        return previous;
    }
    uintptr_t item = 0;
    if (delayedPass) {
        // The sorted passes get no corpus, only what was queued: its first word is the corpus
        // that was current then (an unverified guess: the stats in the log tell).
        uintptr_t queued = 0;
        if (corpus && read32(reinterpret_cast<uintptr_t>(corpus) + kDelayedCorpus, queued)) itemOf(queued, item);
        g_delayedDraws++;
    } else {
        itemOf(reinterpret_cast<uintptr_t>(corpus), item);
        g_draws++;
    }
    int rank = 0;
    if (item) {
        const auto it = g_itemRank.find(item);
        if (it != g_itemRank.end()) {
            rank = it->second;
            if (delayedPass) {
                g_delayedKnown++;
            } else {
                g_known++;
                if (rank) g_ranked++;
            }
        }
    }
    g_rank = rank;
    if (!g_reported && g_draws >= 20000) report();
    return previous;
}

void endDraw(int previous) { g_rank = previous; }

} // namespace blockorder
