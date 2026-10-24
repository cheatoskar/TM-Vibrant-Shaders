#include "misc/internal.h"
#include "engine.h"
#include "log.h"
#include "menu.h"
#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

// The minimap: the map's blocks seen from above, one square per grid cell they take, the track
// lighter the higher it lies, the ground and everything else dimmed; the car as an arrow. Only
// the track's shape: no block is told apart by what it does (a checkpoint, start, finish, booster
// or engine-off block is drawn like any road or platform; their waypoint field is never read).
//
// The blocks (the same as blockorder.cpp reads, Ghidra, TMNF names): CGameCtnChallenge +0x54 is a
// CFastBuffer<CGameCtnBlock*> {count, data}; a block's info (CGameCtnBlockInfo) at +0x24, whose name
// is the CMwId at its +0x24; its coordinate (x, y, z grid units) at +0x48 and its units at +0x28
// (CFastArray of CGameCtnBlockUnit*, each one's offset at +0x28, added to the block's coordinate:
// the cells it takes, turned with it). Stadium's grid: 32 m x 8 m x 32 m, cell (x, z) covering
// world x 32 x .. 32 x + 32 and z likewise (checked once per map against the car on its start,
// see checkCarCell; TMUF's other environments have other cells, so the minimap is Stadium's only).
// The picture is made once per map and when the editor changes the block list, into a texture;
// each frame draws it and the car.
namespace tmshaders {
namespace misc {
namespace mapview {
namespace {

constexpr size_t kChallengeBlocks = 0x54;
constexpr size_t kBlockInfo = 0x24;
constexpr size_t kInfoId = 0x24;
constexpr size_t kBlockCoord = 0x48;
constexpr size_t kBlockUnits = 0x28;
constexpr size_t kUnitOffset = 0x28;
constexpr uint32_t kMaxBlocks = 100000;
constexpr uint32_t kMaxUnits = 128;
constexpr int kMaxGrid = 256;      // cells per side at most (Stadium has 32)
constexpr int kPixelsPerCell = 8;
constexpr float kCellSize = 32.0f; // m (Stadium)
constexpr DWORD kPollMs = 500;

enum Kind : uint8_t { kEmpty = 0, kOther = 1, kGround = 2, kTrack = 3 };

// What a block is for the picture, from its name: road, circuit, platform and the like are track,
// grass, dirt and water ground, everything else (stands, lights, decoration) other. A name that
// says checkpoint, start, finish or turbo is a road or platform like the rest.
Kind kindOf(const std::string& name) {
    static const char* const kTrackWords[] = {"Road", "Circuit", "Platform", "Loop", "Tube", "Ramp", "Bump"};
    static const char* const kGroundWords[] = {"Grass", "Dirt", "Water", "Pool", "Terrain", "Hill"};
    for (const char* w : kTrackWords) {
        if (name.find(w) != std::string::npos) return kTrack;
    }
    for (const char* w : kGroundWords) {
        if (name.find(w) != std::string::npos) return kGround;
    }
    return kOther;
}

struct Picture {
    const void* map = nullptr;
    uint32_t count = 0;
    uintptr_t data = 0;
    DWORD polled = 0;
    bool read = false;
    int minX = 0, minZ = 0, sizeX = 0, sizeZ = 0;
    int blocks = 0, cells = 0, unreadable = 0;
    std::vector<uint32_t> pixels; // ARGB, row r = cell z minZ + r / kPixelsPerCell
    int width = 0, height = 0;
    bool upload = false;
    IDirect3DTexture9* texture = nullptr;
    IDirect3DDevice9* device = nullptr;
    // The start check: the car's cell against the blocks once per map (the log says).
    std::vector<uint8_t> occupied; // sizeX * sizeZ
    bool carChecked = false;
};
Picture g_picture;
// The window as drawn last (the split times go under it).
ImVec2 g_shownAt, g_shownSize;
int g_shownFrame = -10;

bool read32(uintptr_t address, uint32_t& out) {
    __try {
        out = *reinterpret_cast<const uint32_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct BlockCells {
    uintptr_t info = 0;
    uint32_t n = 0;
    int cells[kMaxUnits][3];
};

bool readBlock(uintptr_t block, BlockCells& out) {
    __try {
        const BYTE* b = reinterpret_cast<const BYTE*>(block);
        out.info = *reinterpret_cast<const uint32_t*>(b + kBlockInfo);
        const int x = *reinterpret_cast<const int*>(b + kBlockCoord);
        const int y = *reinterpret_cast<const int*>(b + kBlockCoord + 4);
        const int z = *reinterpret_cast<const int*>(b + kBlockCoord + 8);
        const uint32_t count = *reinterpret_cast<const uint32_t*>(b + kBlockUnits);
        const BYTE* const* units = *reinterpret_cast<const BYTE* const* const*>(b + kBlockUnits + 4);
        if (count > kMaxUnits || (count && !units)) return false;
        out.n = count;
        for (uint32_t i = 0; i < count; i++) {
            const BYTE* u = units[i];
            out.cells[i][0] = x + *reinterpret_cast<const int*>(u + kUnitOffset);
            out.cells[i][1] = y + *reinterpret_cast<const int*>(u + kUnitOffset + 4);
            out.cells[i][2] = z + *reinterpret_cast<const int*>(u + kUnitOffset + 8);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t argb(float r, float g, float b, float a) {
    const auto c = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (c(a) << 24) | (c(r) << 16) | (c(g) << 8) | c(b);
}

void build(const void* map, uint32_t count, uintptr_t data) {
    Picture& p = g_picture;
    p.map = map;
    p.count = count;
    p.data = data;
    p.read = true;
    p.upload = true;
    p.pixels.clear();
    p.width = p.height = 0;
    p.blocks = p.cells = p.unreadable = 0;
    p.carChecked = false;
    if (!data || count == 0 || count > kMaxBlocks) return;
    struct Column {
        uint8_t kind = kEmpty;
        int top = INT32_MIN; // the highest track cell (grid y)
    };
    std::unordered_map<uintptr_t, Kind> kinds;
    std::unordered_map<int64_t, Column> columns;
    int minX = INT32_MAX, maxX = INT32_MIN, minZ = INT32_MAX, maxZ = INT32_MIN, minY = INT32_MAX, maxY = INT32_MIN;
    BlockCells b;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t block = 0;
        if (!read32(data + 4 * i, block) || !block || !readBlock(block, b)) {
            p.unreadable++;
            continue;
        }
        auto it = kinds.find(b.info);
        if (it == kinds.end()) it = kinds.emplace(b.info, kindOf(engine::idText(reinterpret_cast<const BYTE*>(b.info) + kInfoId))).first;
        const Kind kind = it->second;
        p.blocks++;
        for (uint32_t u = 0; u < b.n; u++) {
            const int x = b.cells[u][0], y = b.cells[u][1], z = b.cells[u][2];
            if (x < -1024 || x > 1024 || z < -1024 || z > 1024) continue;
            Column& c = columns[(static_cast<int64_t>(x) << 32) ^ static_cast<uint32_t>(z)];
            if (kind > c.kind) c.kind = kind;
            if (kind == kTrack) {
                c.top = std::max(c.top, y);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minZ = std::min(minZ, z);
            maxZ = std::max(maxZ, z);
        }
    }
    if (columns.empty() || maxX - minX >= kMaxGrid || maxZ - minZ >= kMaxGrid) {
        TMVS_LOG("misc: minimap: %u blocks, %d read, no picture (%zu cells)", count, p.blocks, columns.size());
        return;
    }
    p.minX = minX;
    p.minZ = minZ;
    p.sizeX = maxX - minX + 1;
    p.sizeZ = maxZ - minZ + 1;
    p.cells = static_cast<int>(columns.size());
    p.width = p.sizeX * kPixelsPerCell;
    p.height = p.sizeZ * kPixelsPerCell;
    p.pixels.assign(static_cast<size_t>(p.width) * p.height, 0);
    p.occupied.assign(static_cast<size_t>(p.sizeX) * p.sizeZ, 0);
    const auto at = [&](int x, int z) -> const Column* {
        auto it = columns.find((static_cast<int64_t>(x) << 32) ^ static_cast<uint32_t>(z));
        return it == columns.end() ? nullptr : &it->second;
    };
    const float span = maxY > minY ? static_cast<float>(maxY - minY) : 1.0f;
    for (const auto& entry : columns) {
        const int x = static_cast<int>(entry.first >> 32), z = static_cast<int>(static_cast<int32_t>(entry.first & 0xffffffff));
        const Column& c = entry.second;
        const int cx = x - minX, cz = z - minZ;
        p.occupied[static_cast<size_t>(cz) * p.sizeX + cx] = 1;
        uint32_t fill = 0, edge = 0;
        if (c.kind == kTrack) {
            const float h = static_cast<float>(c.top - minY) / span;
            const float v = 0.52f + 0.45f * h;
            fill = argb(v, v, v * 1.02f, 1.0f);
            edge = argb(v * 0.45f, v * 0.45f, v * 0.5f, 1.0f);
        } else if (c.kind == kGround) {
            fill = edge = argb(0.25f, 0.33f, 0.24f, 0.55f);
        } else {
            fill = edge = argb(0.35f, 0.35f, 0.38f, 0.45f);
        }
        // Track cells get a dark rim where the next cell isn't track: the road's outline.
        const Column* n[4] = {at(x - 1, z), at(x + 1, z), at(x, z - 1), at(x, z + 1)};
        bool open[4];
        for (int k = 0; k < 4; k++) open[k] = c.kind == kTrack && (!n[k] || n[k]->kind != kTrack);
        for (int py = 0; py < kPixelsPerCell; py++) {
            for (int px = 0; px < kPixelsPerCell; px++) {
                const bool rim = (open[0] && px == 0) || (open[1] && px == kPixelsPerCell - 1) || (open[2] && py == 0) ||
                                 (open[3] && py == kPixelsPerCell - 1);
                p.pixels[static_cast<size_t>(cz * kPixelsPerCell + py) * p.width + cx * kPixelsPerCell + px] = rim ? edge : fill;
            }
        }
    }
    TMVS_LOG("misc: minimap: %u blocks (%d unreadable), %d cells over %d x %d, track heights %d..%d", count, p.unreadable, p.cells,
             p.sizeX, p.sizeZ, minY == INT32_MAX ? 0 : minY, maxY == INT32_MIN ? 0 : maxY);
}

// A new map, or the editor added or removed blocks (the list's count or storage changed).
void poll() {
    Picture& p = g_picture;
    const void* map = engine::currentMap();
    const DWORD now = GetTickCount();
    if (map == p.map && p.read && now - p.polled < kPollMs) return;
    p.polled = now;
    uint32_t count = 0, data = 0;
    if (!map || !read32(reinterpret_cast<uintptr_t>(map) + kChallengeBlocks, count) ||
        !read32(reinterpret_cast<uintptr_t>(map) + kChallengeBlocks + 4, data)) {
        if (p.map != map || !p.read) build(map, 0, 0);
        return;
    }
    if (map == p.map && p.read && count == p.count && data == p.data) return;
    build(map, count, data);
}

void upload(IDirect3DDevice9* device) {
    Picture& p = g_picture;
    if (!p.upload && p.device == device) return;
    p.upload = false;
    if (p.texture) p.texture->Release();
    p.texture = nullptr;
    p.device = device;
    if (!device || p.pixels.empty()) return;
    // Managed: lives through a device reset.
    if (FAILED(device->CreateTexture(p.width, p.height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &p.texture, nullptr)) || !p.texture) {
        p.texture = nullptr;
        return;
    }
    D3DLOCKED_RECT locked{};
    if (FAILED(p.texture->LockRect(0, &locked, nullptr, 0))) {
        p.texture->Release();
        p.texture = nullptr;
        return;
    }
    for (int y = 0; y < p.height; y++) memcpy(static_cast<BYTE*>(locked.pBits) + y * locked.Pitch, &p.pixels[static_cast<size_t>(y) * p.width], p.width * 4);
    p.texture->UnlockRect(0);
}

// Once per map: is the car's cell (at the start) one of the block cells? If not, the grid's scale
// or origin here is wrong (the log tells; my reading is 32 m cells from world 0).
void checkCarCell(const engine::CarPose& pose) {
    Picture& p = g_picture;
    if (p.carChecked || pose.source != 1 || p.occupied.empty()) return;
    p.carChecked = true;
    const int cx = static_cast<int>(floorf(pose.position[0] / kCellSize)) - p.minX;
    const int cz = static_cast<int>(floorf(pose.position[2] / kCellSize)) - p.minZ;
    const bool inside = cx >= 0 && cz >= 0 && cx < p.sizeX && cz < p.sizeZ;
    TMVS_LOG("misc: minimap: the car at %.1f %.1f %.1f is in cell %d, %d of the map: %s", pose.position[0], pose.position[1], pose.position[2],
             cx + p.minX, cz + p.minZ,
             !inside ? "outside the blocks' cells (the grid is not what I read)"
                     : (p.occupied[static_cast<size_t>(cz) * p.sizeX + cx] ? "a block's cell, as expected" : "an empty cell (check the grid)"));
}

} // namespace

bool rect(ImVec2& pos, ImVec2& size) {
    pos = g_shownAt;
    size = g_shownSize;
    return ImGui::GetFrameCount() - g_shownFrame <= 1;
}

bool wanted() {
    return settings().minimap && engine::currentMap() != nullptr && engine::stadium();
}

void drawPageInfo() {
    if (!engine::currentMap()) {
        ImGui::TextDisabled("Open a map.");
        return;
    }
    if (!engine::stadium()) {
        ImGui::TextDisabled("Stadium maps only.");
        return;
    }
    const Picture& p = g_picture;
    if (p.read && p.cells > 0) ImGui::TextDisabled("%d blocks, %d x %d cells", p.blocks, p.sizeX, p.sizeZ);
}

void draw(const Frame& frame) {
    poll();
    upload(frame.device);
    Picture& p = g_picture;
    Settings& s = settings();
    const float scale = menu::scale();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float side = s.mapSize * screenScale();
    // The size and place are the settings'; the slider and dragging the corner both change them.
    // (The window's own size used to be written back every frame while the menu was open: the size
    // slider jumped back at once.)
    static WindowPlace s_place;
    const float margin = 16.0f * screenScale();
    s_place.before(frame, ImVec2(s.mapX >= 0.0f ? s.mapX : display.x - side - margin, s.mapY >= 0.0f ? s.mapY : margin), ImVec2(side, side));
    ImGui::SetNextWindowBgAlpha(0.45f * s.mapOpacity);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f * scale, 4.0f * scale));
    ImGui::Begin("Minimap##misc", nullptr, windowFlags(frame, true));
    ImGui::PopStyleVar();
    ImVec2 pos, size;
    if (s_place.after(frame, pos, size)) {
        s.mapX = pos.x;
        s.mapY = pos.y;
        s.mapSize = std::clamp(std::max(size.x, size.y) / screenScale(), 120.0f, 800.0f);
        settingsChanged();
    }
    g_shownAt = ImGui::GetWindowPos();
    g_shownSize = ImGui::GetWindowSize();
    g_shownFrame = ImGui::GetFrameCount();
    windowChrome(frame, "Minimap", true);
    const ImVec2 lo = ImGui::GetCursorScreenPos(), wp = ImGui::GetWindowPos();
    const ImVec2 hi = ImGui::GetWindowContentRegionMax();
    const ImVec2 a(lo.x, lo.y), b(wp.x + hi.x, wp.y + hi.y);
    const ImVec2 centre((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    const float view = std::min(b.x - a.x, b.y - a.y);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    engine::CarPose pose;
    const bool car = engine::carPose(pose);
    if (car) checkCarCell(pose);
    if (p.texture && p.sizeX > 0) {
        // World (x, z) to the window: x to the right, z up (seen from above in the engine's
        // left-handed space); with "turn with the car", the car's heading up.
        const float worldX0 = p.minX * kCellSize, worldZ0 = p.minZ * kCellSize;
        const float worldX1 = worldX0 + p.sizeX * kCellSize, worldZ1 = worldZ0 + p.sizeZ * kCellSize;
        const float extent = std::max(worldX1 - worldX0, worldZ1 - worldZ0);
        const bool follow = car && (s.mapRotate || s.mapZoom > 1.01f);
        const float cx = follow ? pose.position[0] : (worldX0 + worldX1) * 0.5f;
        const float cz = follow ? pose.position[2] : (worldZ0 + worldZ1) * 0.5f;
        const float k = view / (extent / s.mapZoom);
        float fx = 0.0f, fz = 1.0f;
        if (s.mapRotate && car) {
            const float l = sqrtf(pose.forward[0] * pose.forward[0] + pose.forward[2] * pose.forward[2]);
            if (l > 1e-3f) {
                fx = pose.forward[0] / l;
                fz = pose.forward[2] / l;
            }
        }
        // Across the heading: forward turned a quarter clockwise as seen from above (x right, z up).
        const float rx = fz, rz = -fx;
        const auto toScreen = [&](float x, float z) {
            const float dx = x - cx, dz = z - cz;
            return ImVec2(centre.x + (dx * rx + dz * rz) * k, centre.y - (dx * fx + dz * fz) * k);
        };
        draw->PushClipRect(a, b, true);
        const ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(255.0f * s.mapOpacity));
        draw->AddImageQuad(reinterpret_cast<ImTextureID>(p.texture), toScreen(worldX0, worldZ0), toScreen(worldX1, worldZ0),
                           toScreen(worldX1, worldZ1), toScreen(worldX0, worldZ1), ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), tint);
        if (car) {
            const ImVec2 at = toScreen(pose.position[0], pose.position[2]);
            const ImVec2 ahead = toScreen(pose.position[0] + pose.forward[0], pose.position[2] + pose.forward[2]);
            float dx = ahead.x - at.x, dy = ahead.y - at.y;
            const float l = sqrtf(dx * dx + dy * dy);
            dx = l > 1e-4f ? dx / l : 0.0f;
            dy = l > 1e-4f ? dy / l : -1.0f;
            const float r = 7.0f * scale;
            const ImVec2 tip(at.x + dx * r * 1.4f, at.y + dy * r * 1.4f);
            const ImVec2 left(at.x - dx * r * 0.8f + dy * r * 0.8f, at.y - dy * r * 0.8f - dx * r * 0.8f);
            const ImVec2 right(at.x - dx * r * 0.8f - dy * r * 0.8f, at.y - dy * r * 0.8f + dx * r * 0.8f);
            draw->AddTriangleFilled(tip, left, right, IM_COL32(255, 170, 60, 255));
            draw->AddTriangle(tip, left, right, IM_COL32(20, 20, 20, 220), 1.5f * scale);
        }
        draw->PopClipRect();
    } else {
        ImGui::TextDisabled(p.read ? "No blocks" : "Loading...");
    }
    ImGui::End();
}

} // namespace mapview
} // namespace misc
} // namespace tmshaders
