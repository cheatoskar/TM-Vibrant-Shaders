#include "misc/internal.h"
#include "engine.h"
#include "log.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// The Real look picture complete as soon as a map is open: every block's meshes read from the
// game's memory and queued for the picture (topview::addBlockMesh), with the same buffers, draw
// and place the game's renderer uses for them, so what the camera draws later is known already
// (the same placement key). Read only, inside SEH guards; nothing in the game is called or written.
//
// The chain (Ghidra, TMNF names; the same offsets in TMUF: every function used for them ported
// over 256 bytes with tools/port_address.py):
//   CGameCtnBlock +0x30 its CSceneMobil, +0x38 CFastBufferRef<CSceneMobil> (a clip's), see
//     CGameCtnBlock::SetMobilAndHelper; CSceneMobil +0x28 its CHmsItem (whose +0x40 points back),
//     +0x38 its links (CSceneMobil::AddObject), each link's object at +0x18 (the checkpoint part of a
//     gate is such a child mobil).
//   CHmsItem +0x14 CPlugSolid (CHmsItem::SetSolid), +0x34 CFastBuffer<CHmsCorpus*> (AddCorpus);
//     CPlugSolid +0x64 the CPlugTree (CHmsZoneVPacker::AddNewSolid); CHmsCorpus +0x18 its GmIso4
//     place in the world (SetLocation, GetLocation when +0x58, a CHmsDyna, is null), +0x48 its item (SetItem).
//   CPlugTree +0x28 its children (CFastBuffer, CPlugTree::GetChild), +0x5c its GmIso4 (used when
//     +0x9c has bit 4; bit 8: shown), +0x90 CPlugVisual, +0x94 CPlugShader, +0x54/+0x56 its split
//     range (CPlugTree::GetThisToRootTransfo, CHmsZoneVPacker::AddTree, RenderVisual).
//     CPlugTreeVisualMip: the levels at +0xac after the children (GetChild); level 0 is taken.
//   GmIso4: a 3 x 3 matrix by rows, then the translation: p' = R p + t; a child's world is its
//     parent's times its own (GmIso4::SetMult).
//   CPlugVisual +0x14 its CDx9VisualKeeper (RenderVisual), +0x1c flags (bit 0x20: static indices),
//     +0x64 its poly groups (12 bytes: first vertex, first index, index count).
//   CDx9VisualKeeper (CDx9VisualKeeper::LoadAndPrepareGeometry_*, LoadAndPrepareIndexation,
//     RenderVisualGeometry): +0x4 its visual, +0xc flags (bits 16..21 the primitive kind), +0x10
//     the split count, +0x14 its SStreamDecl, +0x2c the geometry loader, +0x30 + 12 p (p = +0x98, the
//     vertex processing) {CDx9VertexBuffer*, first vertex, vertex count}, +0x78 CDx9IndexBuffer*,
//     +0x88 the split table (SPackModel, 0x28 bytes: +0x18 first index, +0x1c first vertex).
//   CDx9VertexBuffer +0x0 stride, +0x14 IDirect3DVertexBuffer9* (managed: CDx9VertexBuffer::Create);
//   CDx9IndexBuffer +0x8 + 12 p {index count, bytes, IDirect3DIndexBuffer9*} (CDx9IndexBuffer::Create).
//   The declaration: CDx9VertexDeclaration::s_Declarations {SStreamDecl, SStreamDecl, IDirect3DVertexDeclaration9*}
//     (StaticSetStream01Decl), found for the keeper's SStreamDecl and none on stream 1.
// Keepers, buffers and declarations are the game's own, made when it loads the map's visuals: a
// visual it hasn't made them for is skipped (counted), the camera fills it in as before.
namespace tmshaders {
namespace misc {
namespace blockmesh {
namespace {

constexpr size_t kBlockInfo = 0x24, kInfoId = 0x24, kBlockCoord = 0x48;
constexpr size_t kBlockMobil = 0x30, kBlockClipMobils = 0x38;
constexpr size_t kMobilItem = 0x28, kMobilLinks = 0x38, kLinkObject = 0x18;
constexpr size_t kItemSolid = 0x14, kItemCorpora = 0x34, kItemMobil = 0x40;
constexpr size_t kSolidTree = 0x64;
constexpr size_t kCorpusLocation = 0x18, kCorpusItem = 0x48, kCorpusDyna = 0x58;
constexpr size_t kTreeChildren = 0x28, kTreeSplit = 0x54, kTreeLocation = 0x5c, kTreeVisual = 0x90, kTreeShader = 0x94, kTreeFlags = 0x9c;
constexpr size_t kMipLevels = 0xac;
constexpr uint32_t kTreeHasLocation = 0x4, kTreeShown = 0x8;
constexpr size_t kVisualKeeper = 0x14, kVisualFlags = 0x1c, kVisualPolyGroups = 0x64;
constexpr uint32_t kVisualStaticIndices = 0x20;
constexpr size_t kKeeperVisual = 0x4, kKeeperFlags = 0xc, kKeeperSplits = 0x10, kKeeperDecl = 0x14, kKeeperLoader = 0x2c;
constexpr size_t kKeeperGeometry = 0x30, kKeeperIndexation = 0x78, kKeeperSplitTable = 0x88, kKeeperProcess = 0x98, kKeeperLines = 0x9c;
constexpr uint32_t kKeeperIndexKinds = 0x1c00000, kKeeperQuads = 0x1000000, kKeeperListKind0 = 0x8000000, kKeeperPolyGroupRange = 0x40000000;
constexpr size_t kVertexBufferStride = 0x0, kVertexBufferD3D = 0x14;
constexpr size_t kIndexBufferSlots = 0x8;
constexpr size_t kSplitSize = 0x28, kSplitFirstIndex = 0x18, kSplitFirstVertex = 0x1c;
constexpr size_t kPolyGroupSize = 12;

constexpr int kMaxMobils = 24;          // per block: its mobil, a clip's, their children
constexpr uint32_t kMaxCorpora = 16;    // per item
constexpr uint32_t kMaxChildren = 512;  // per tree node
constexpr int kMaxNodes = 4096;         // per mobil and corpus
constexpr int kMaxMeshes = 1024;        // per block
constexpr int kPerFrame = 300;          // meshes queued per frame at most
constexpr double kFrameMs = 1.5;        // reading time per frame at most
constexpr int kPasses = 4;              // passes over a list at most (the first and three more)
constexpr DWORD kPassMs = 5000;         // between them

struct Iso {
    float m[9]; // rows
    float t[3];
};

Iso compose(const Iso& parent, const Iso& local) {
    Iso out;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) out.m[3 * r + c] = parent.m[3 * r] * local.m[c] + parent.m[3 * r + 1] * local.m[3 + c] + parent.m[3 * r + 2] * local.m[6 + c];
        out.t[r] = parent.m[3 * r] * local.t[0] + parent.m[3 * r + 1] * local.t[1] + parent.m[3 * r + 2] * local.t[2] + parent.t[r];
    }
    return out;
}

bool sane(const Iso& iso) {
    for (float v : iso.m) {
        if (!std::isfinite(v) || fabsf(v) > 64.0f) return false;
    }
    for (float v : iso.t) {
        if (!std::isfinite(v) || fabsf(v) > 100000.0f) return false;
    }
    return true;
}

// What happened to the meshes of the blocks read so far (for the log).
struct Counts {
    unsigned blocks = 0, unreadable = 0, noMesh = 0;   // blocks: read, not readable, without any mesh found
    unsigned meshes = 0, queued = 0, known = 0, outside = 0, unusable = 0;
    unsigned notLoaded = 0;   // visuals without the game's keeper or buffers yet
    unsigned otherKind = 0;   // geometry or primitive kinds the reader doesn't take (lines, sprites, dynamic, quads)
    unsigned noDecl = 0;      // no declaration made by the game for its vertex format yet
    unsigned otherTree = 0;   // tree classes whose children aren't read (generators and the like)
    void add(const Counts& o) {
        blocks += o.blocks, unreadable += o.unreadable, noMesh += o.noMesh, meshes += o.meshes, queued += o.queued, known += o.known;
        outside += o.outside, unusable += o.unusable, notLoaded += o.notLoaded, otherKind += o.otherKind, noDecl += o.noDecl, otherTree += o.otherTree;
    }
};

uint32_t rd(uintptr_t a) {
    return *reinterpret_cast<const uint32_t*>(a);
}
uint16_t rd16(uintptr_t a) {
    return *reinterpret_cast<const uint16_t*>(a);
}

struct Reader {
    engine::MeshReadAddresses a{};
    Counts counts; // this block's
    topview::BlockMesh* out = nullptr;
    int max = 0, n = 0;
    uintptr_t lastDecl = 0;
    IDirect3DVertexDeclaration9* lastD3DDecl = nullptr;
};

// The game's IDirect3DVertexDeclaration9 for a vertex format on stream 0 alone (null: not made yet).
IDirect3DVertexDeclaration9* declarationOf(Reader& r, uint32_t format) {
    if (format == r.lastDecl && r.lastD3DDecl) return r.lastD3DDecl;
    const uint32_t count = rd(r.a.declarations), data = rd(r.a.declarations + 4);
    if (!data || count > 4096) return nullptr;
    for (uint32_t i = 0; i < count; i++) {
        const uintptr_t e = data + 12 * i;
        if (rd(e) == format && rd(e + 4) == 0) {
            r.lastDecl = format;
            r.lastD3DDecl = reinterpret_cast<IDirect3DVertexDeclaration9*>(static_cast<uintptr_t>(rd(e + 8)));
            return r.lastD3DDecl;
        }
    }
    return nullptr;
}

// One tree node's visual as the game's renderer draws it (PrepareVisualGeometry, the loaders,
// LoadAndPrepareIndexation, RenderVisualGeometry), at `world`.
void takeVisual(Reader& r, uintptr_t tree, uintptr_t visual, uintptr_t shader, const Iso& world) {
    const uintptr_t keeper = rd(visual + kVisualKeeper);
    if (!keeper || rd(keeper + kKeeperVisual) != visual) {
        r.counts.notLoaded++;
        return;
    }
    const uint32_t flags = rd(keeper + kKeeperFlags), process = rd(keeper + kKeeperProcess), loader = rd(keeper + kKeeperLoader);
    if (process > 1 || rd(keeper + kKeeperLines)) {
        r.counts.otherKind++;
        return;
    }
    const uintptr_t geometry = keeper + kKeeperGeometry + 12 * process;
    const uintptr_t vbObject = rd(geometry);
    if (!vbObject) {
        r.counts.notLoaded++;
        return;
    }
    const uint32_t stride = rd(vbObject + kVertexBufferStride);
    auto* vb = reinterpret_cast<IDirect3DVertexBuffer9*>(static_cast<uintptr_t>(rd(vbObject + kVertexBufferD3D)));
    const uint32_t allVertices = rd(geometry + 8);
    uint32_t vertexOffset = rd(geometry + 4), vertexMin = 0, vertexCount = allVertices;
    if (!vb || stride == 0 || stride > 256 || allVertices == 0 || allVertices > (1u << 20)) {
        r.counts.notLoaded++;
        return;
    }
    const uint32_t first = rd16(tree + kTreeSplit), splitCount = rd16(tree + kTreeSplit + 2), splits = rd16(keeper + kKeeperSplits);
    const uintptr_t table = rd(keeper + kKeeperSplitTable);
    uintptr_t splitData = 0;
    if (splits) {
        if (!table || first + splitCount > splits) {
            r.counts.otherKind++;
            return;
        }
        splitData = rd(table + 4);
        if (!splitData) {
            r.counts.otherKind++;
            return;
        }
    }
    const uint32_t polyGroups = rd(visual + kVisualPolyGroups), polyData = rd(visual + kVisualPolyGroups + 4);
    const uint32_t group = rd(tree + kTreeSplit) & 0xfff;
    if (loader == r.a.geometryStatic || loader == r.a.geometrySplits) {
        if (splits) { // the tree's share of the visual's vertices (LoadAndPrepareGeometry_StaticVisualSplits)
            vertexMin = first ? rd(splitData + kSplitSize * first + kSplitFirstVertex) : 0;
            vertexCount = (first + splitCount < splits ? rd(splitData + kSplitSize * (first + splitCount) + kSplitFirstVertex) : allVertices) - vertexMin;
        }
    } else if (loader == r.a.geometrySubVisuals) { // its poly group's vertices (LoadAndPrepareGeometry_StaticSubVisuals)
        if (!polyData || group >= polyGroups) {
            r.counts.otherKind++;
            return;
        }
        const uint32_t groupVertex = rd(polyData + kPolyGroupSize * group);
        vertexOffset += groupVertex;
        vertexCount = polyGroups >= 2 && allVertices % polyGroups == 0 ? allVertices / polyGroups : allVertices - groupVertex;
    } else {
        r.counts.otherKind++; // dynamic, skinned, sprites, streams
        return;
    }
    // The indices: only the static ones the visual keeps (a dynamic index buffer is filled per draw,
    // quads share one buffer; neither is a block's usual mesh).
    if (!(flags & kKeeperIndexKinds) || (flags & kKeeperQuads) || !(rd(visual + kVisualFlags) & kVisualStaticIndices)) {
        r.counts.otherKind++;
        return;
    }
    const uintptr_t ibObject = rd(keeper + kKeeperIndexation);
    if (!ibObject) {
        r.counts.notLoaded++;
        return;
    }
    const uint32_t allIndices = rd(ibObject + kIndexBufferSlots + 12 * process);
    auto* ib = reinterpret_cast<IDirect3DIndexBuffer9*>(static_cast<uintptr_t>(rd(ibObject + kIndexBufferSlots + 12 * process + 8)));
    if (!ib || allIndices == 0) {
        r.counts.notLoaded++;
        return;
    }
    uint32_t indexStart = 0, indexCount = allIndices;
    if (flags & kKeeperPolyGroupRange) {
        if (!polyData || group >= polyGroups) {
            r.counts.otherKind++;
            return;
        }
        indexStart = rd(polyData + kPolyGroupSize * group + 4);
        indexCount = rd(polyData + kPolyGroupSize * group + 8);
    } else if (splits) {
        indexStart = first ? rd(splitData + kSplitSize * first + kSplitFirstIndex) : 0;
        indexCount = (first + splitCount < splits ? rd(splitData + kSplitSize * (first + splitCount) + kSplitFirstIndex) : allIndices) - indexStart;
    }
    if (indexCount == 0 || indexStart >= allIndices || indexCount > allIndices - indexStart || vertexCount == 0 || vertexCount > allVertices) {
        r.counts.otherKind++;
        return;
    }
    topview::BlockMesh m;
    switch ((flags >> 16) & 0x3f) { // RenderVisualGeometry's switch
    case 2:
    case 6:
    case 0xd:
    case 0xf:
        m.type = D3DPT_TRIANGLELIST, m.minIndex = vertexMin, m.count = indexCount / 3;
        break;
    case 0:
        if (flags & kKeeperListKind0) m.type = D3DPT_TRIANGLELIST, m.minIndex = 0, m.count = indexCount / 3;
        else m.type = D3DPT_TRIANGLESTRIP, m.minIndex = 0, m.count = indexCount - 2;
        break;
    case 3:
        m.type = D3DPT_TRIANGLESTRIP, m.minIndex = vertexMin, m.count = indexCount - 2;
        break;
    default:
        r.counts.otherKind++; // lines, points, non-indexed
        return;
    }
    if (indexCount < 3) {
        r.counts.otherKind++;
        return;
    }
    m.decl = declarationOf(r, rd(keeper + kKeeperDecl));
    if (!m.decl) {
        r.counts.noDecl++;
        return;
    }
    m.vb = vb;
    m.stride = stride;
    m.ib = ib;
    m.base = static_cast<INT>(vertexOffset);
    m.vertices = vertexCount;
    m.start = indexStart;
    m.shader = reinterpret_cast<const void*>(shader);
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < 3; i++) m.world[i][k] = world.m[3 * k + i];
        m.world[3][k] = world.t[k];
    }
    if (r.n < r.max) {
        r.out[r.n++] = m;
        r.counts.meshes++;
    }
}

struct Node {
    uintptr_t tree;
    Iso world; // the parent's (the node's own place is applied when it is taken)
};

// A mesh tree under a corpus' place: each shown node's visual, its children (and a mip's level 0).
void walkTree(Reader& r, uintptr_t root, const Iso& place, Node* stack) {
    int top = 0, visited = 0;
    stack[top++] = {root, place};
    while (top > 0 && visited++ < kMaxNodes) {
        const Node node = stack[--top];
        const uintptr_t tree = node.tree;
        const uint32_t flags = rd(tree + kTreeFlags);
        if (!(flags & kTreeShown)) continue;
        Iso world = node.world;
        if (flags & kTreeHasLocation) {
            Iso local;
            memcpy(&local, reinterpret_cast<const void*>(tree + kTreeLocation), sizeof(local));
            if (!sane(local)) continue;
            world = compose(node.world, local);
        }
        const uintptr_t visual = rd(tree + kTreeVisual), shader = rd(tree + kTreeShader);
        if (visual && shader) takeVisual(r, tree, visual, shader, world);
        const uintptr_t vtable = rd(tree);
        const uint32_t childCount = rd(vtable + 31 * 4), getChild = rd(vtable + 32 * 4);
        const bool plain = childCount == r.a.treeChildCount && getChild == r.a.treeGetChild;
        const bool mip = childCount == r.a.mipChildCount && getChild == r.a.mipGetChild;
        if (!plain && !mip) {
            r.counts.otherTree++;
            continue;
        }
        const uint32_t count = rd(tree + kTreeChildren), data = rd(tree + kTreeChildren + 4);
        if (count && data && count <= kMaxChildren) {
            for (uint32_t i = count; i-- > 0 && top < kMaxNodes;) {
                const uintptr_t child = rd(data + 4 * i);
                if (child) stack[top++] = {child, world};
            }
        }
        if (mip && top < kMaxNodes) {
            const uint32_t levels = rd(tree + kMipLevels), levelData = rd(tree + kMipLevels + 4);
            const uintptr_t level0 = levels && levelData ? rd(levelData) : 0;
            if (level0) stack[top++] = {level0, world};
        }
    }
}

// A mobil's meshes: its item's tree under each of the item's corpora.
void takeMobil(Reader& r, uintptr_t mobil, Node* stack) {
    const uintptr_t item = rd(mobil + kMobilItem);
    if (!item || rd(item + kItemMobil) != mobil) return;
    const uintptr_t solid = rd(item + kItemSolid);
    const uintptr_t tree = solid ? rd(solid + kSolidTree) : 0;
    const uint32_t corpora = rd(item + kItemCorpora), data = rd(item + kItemCorpora + 4);
    if (!tree || !data || corpora == 0 || corpora > kMaxCorpora) return;
    for (uint32_t i = 0; i < corpora; i++) {
        const uintptr_t corpus = rd(data + 4 * i);
        // A static corpus (no CHmsDyna: its place is +0x18, CHmsCorpus::GetLocation); a moving one isn't a block's.
        if (!corpus || rd(corpus + kCorpusItem) != item || rd(corpus + kCorpusDyna)) continue;
        Iso place;
        memcpy(&place, reinterpret_cast<const void*>(corpus + kCorpusLocation), sizeof(place));
        if (sane(place)) walkTree(r, tree, place, stack);
    }
}

// The mobils of a block: its own, a clip's, and their children (the gate's checkpoint part).
int blockMobils(uintptr_t block, uintptr_t* mobils) {
    int n = 0;
    const auto addMobil = [&](uintptr_t m) {
        if (!m || n >= kMaxMobils) return;
        for (int i = 0; i < n; i++) {
            if (mobils[i] == m) return;
        }
        mobils[n++] = m;
    };
    addMobil(rd(block + kBlockMobil));
    const uint32_t clips = rd(block + kBlockClipMobils), clipData = rd(block + kBlockClipMobils + 4);
    if (clipData && clips <= 16) {
        for (uint32_t i = 0; i < clips; i++) addMobil(rd(clipData + 4 * i));
    }
    for (int i = 0; i < n; i++) { // children found are looked at too (n grows)
        const uint32_t links = rd(mobils[i] + kMobilLinks), linkData = rd(mobils[i] + kMobilLinks + 4);
        if (!linkData || links > 64) continue;
        for (uint32_t k = 0; k < links; k++) {
            const uintptr_t link = rd(linkData + 4 * k);
            const uintptr_t object = link ? rd(link + kLinkObject) : 0;
            const uintptr_t item = object ? rd(object + kMobilItem) : 0;
            if (item && rd(item + kItemMobil) == object) addMobil(object); // a mobil (its item points back)
        }
    }
    return n;
}

// All meshes of one block into r.out; false: its memory couldn't be read.
bool readBlock(Reader& r, uintptr_t block, Node* stack) {
    __try {
        uintptr_t mobils[kMaxMobils];
        const int n = blockMobils(block, mobils);
        for (int i = 0; i < n; i++) takeMobil(r, mobils[i], stack);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readBlockPointer(uintptr_t data, uint32_t index, uintptr_t& block) {
    __try {
        block = rd(data + 4 * index);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readCoord(uintptr_t block, int cell[3], uintptr_t& info) {
    __try {
        for (int k = 0; k < 3; k++) cell[k] = *reinterpret_cast<const int*>(block + kBlockCoord + 4 * k);
        info = rd(block + kBlockInfo);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

double now() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

struct State {
    bool addressesTried = false, addressesKnown = false;
    engine::MeshReadAddresses addresses{};
    uint32_t count = 0;
    uintptr_t data = 0;
    unsigned epoch = ~0u;
    uint32_t next = 0;
    bool done = false;
    bool sampleLogged = false;
    Counts total;
    unsigned frames = 0;
    double ms = 0.0;
    DWORD doneTick = 0;
    bool hitsLogged = false;
    // Read again 5 s after a pass that met meshes the game hadn't made yet (the list is read while
    // the map loads): at most kPasses passes a list; what is in the picture already is skipped.
    int passes = 0;
    unsigned queuedAll = 0;
    std::vector<topview::BlockMesh> meshes;
    std::vector<Node> stack;
};
State g;

void logDone() {
    const Counts& c = g.total;
    TMVS_LOG("misc: minimap: blocks read (pass %d): %u blocks, %u meshes queued (%u unreadable) in %u frames, %.1f ms; of %u meshes found: %u already in the "
             "picture, %u outside the map's area, %u not usable; visuals skipped: %u not loaded by the game yet, %u of other kinds, %u without the "
             "game's declaration; %u blocks without a mesh found, %u tree nodes of other classes",
             g.passes, c.blocks, c.queued, c.unreadable, g.frames, g.ms, c.meshes, c.known, c.outside, c.unusable, c.notLoaded, c.otherKind, c.noDecl, c.noMesh,
             c.otherTree);
}

} // namespace

void read(IDirect3DDevice9* device, uint32_t count, uintptr_t data) {
    if (!g.addressesTried) {
        g.addressesTried = true;
        g.addressesKnown = engine::meshReadAddresses(g.addresses);
        if (!g.addressesKnown) TMVS_LOG("misc: minimap: unknown game build, the blocks' meshes aren't read (the camera fills the picture in)");
    }
    if (!g.addressesKnown || !device) return;
    if (count != g.count || data != g.data || topview::epoch() != g.epoch) {
        // Another list (another map, the editor's change, the list still filling while the map loads):
        // from the start, quietly (the picture keeps what it has).
        g.count = count;
        g.data = data;
        g.epoch = topview::epoch();
        g.next = 0;
        g.done = false;
        g.sampleLogged = false;
        g.total = Counts();
        g.frames = 0;
        g.ms = 0.0;
        g.hitsLogged = false;
        g.passes = 1;
        g.queuedAll = 0;
        topview::takeBlockHits();
    }
    if (g.done && g.passes < kPasses && g.total.notLoaded + g.total.noDecl + g.total.unreadable > 0 && GetTickCount() - g.doneTick > kPassMs) {
        g.passes++;
        g.next = 0;
        g.done = false;
        g.total = Counts();
        g.frames = 0;
        g.ms = 0.0;
    }
    if (g.done) {
        // Once, 30 s after: how many of the placements read the camera has found (the same keys
        // both ways; none while the camera saw them would mean they differ).
        if (!g.hitsLogged && GetTickCount() - g.doneTick > 30000) {
            g.hitsLogged = true;
            TMVS_LOG("misc: minimap: the camera's draws found %u of the %u placements read from the blocks", topview::takeBlockHits(), g.queuedAll);
        }
        return;
    }
    if (!data || count == 0) return;
    if (g.meshes.size() != kMaxMeshes) g.meshes.resize(kMaxMeshes);
    if (g.stack.size() != kMaxNodes) g.stack.resize(kMaxNodes);
    const double t0 = now();
    int queued = 0;
    while (g.next < count) {
        Reader r;
        r.a = g.addresses;
        r.out = g.meshes.data();
        r.max = kMaxMeshes;
        uintptr_t block = 0;
        if (!readBlockPointer(data, g.next, block) || !block || !readBlock(r, block, g.stack.data())) {
            g.total.unreadable++;
            g.next++;
            continue;
        }
        // Into the picture's queue; a full queue stops here and the block comes again next frame
        // (what of it went in is known then).
        bool full = false;
        for (int i = 0; i < r.n && !full; i++) {
            switch (topview::addBlockMesh(device, r.out[i])) {
            case topview::BlockMeshResult::Queued: r.counts.queued++, queued++; break;
            case topview::BlockMeshResult::Known: r.counts.known++; break;
            case topview::BlockMeshResult::Outside: r.counts.outside++; break;
            case topview::BlockMeshResult::Unusable: r.counts.unusable++; break;
            case topview::BlockMeshResult::Full: full = true; break;
            }
        }
        if (full) {
            g.total.queued += r.counts.queued; // those went in; the rest of the block next time
            break;
        }
        r.counts.blocks = 1;
        r.counts.noMesh = r.n == 0 ? 1 : 0;
        g.total.add(r.counts);
        // Once per map: the first block with a mesh, its grid cell against its first mesh's place
        // (32 x 8 x 32 m cells: the mesh should lie in or next to the cell).
        if (!g.sampleLogged && r.n > 0) {
            g.sampleLogged = true;
            int cell[3] = {};
            uintptr_t info = 0;
            if (readCoord(block, cell, info)) {
                const std::string name = info ? engine::idText(reinterpret_cast<const void*>(info + kInfoId)) : std::string();
                TMVS_LOG("misc: minimap: first block read: %s at cell %d, %d, %d (%.0f, %.0f, %.0f m), %d meshes, the first at %.1f, %.1f, %.1f",
                         name.empty() ? "(unnamed)" : name.c_str(), cell[0], cell[1], cell[2], cell[0] * 32.0f, cell[1] * 8.0f, cell[2] * 32.0f, r.n,
                         r.out[0].world[3][0], r.out[0].world[3][1], r.out[0].world[3][2]);
            }
        }
        g.next++;
        if (queued >= kPerFrame || now() - t0 > kFrameMs) break;
    }
    g.frames++;
    g.ms += now() - t0;
    if (g.next >= count) {
        g.done = true;
        g.doneTick = GetTickCount();
        g.queuedAll += g.total.queued;
        logDone();
    }
}

} // namespace blockmesh
} // namespace misc
} // namespace tmshaders
