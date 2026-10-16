#include "casters.h"
#include "gfx.h"
#include "hook.h"
#include "log.h"
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tmshaders {
namespace casters {
namespace {

// Three INTZ maps of 4096^2 (64 MB of video memory each, no colour target: NULL format): the
// whole track, and two around the camera (one in use while the other is drawn).
constexpr UINT kMapSize = 4096;
constexpr size_t kMaxCasters = 30000;   // a Stadium map keeps a few thousand
constexpr size_t kMaxPending = 60000;
constexpr UINT kDrawBudget = 1000;      // meshes drawn into the maps per frame (measured: ~0.3 ms of CPU)
constexpr UINT kCheckBudget = 40000;    // meshes looked at per frame (the near map's selection)
// The main camera's draws checked per frame, about (each ~0.7 us of CPU): with more, every n-th
// draw is checked, the next frame the ones after them. A big Stadium map draws 8000 meshes a
// frame and checking them all took 10 ms of CPU every frame (60 FPS where the GPU had time for
// more). Spread out, every draw is still checked every few frames (at most every 16th): a mesh is
// kept a fraction of a second later than it would have been, long before its shadow is near.
constexpr unsigned kChecksPerFrame = 700;
constexpr unsigned kMaxStride = 16;
constexpr DWORD kStaticAfterMs = 500;   // seen again this much later at the same place: static
constexpr float kNearHalf = 160.0f;     // the near map: +-160 m around the camera (horizontally),
constexpr float kNearBelow = 50.0f;     // this far below it
constexpr float kNearAbove = 80.0f;     // and above it,
constexpr float kNearMove = 40.0f;      // drawn again once the camera is this far from its centre
constexpr float kNearLightBefore = 1500.0f; // depth window: casters up to this far towards the light
constexpr float kNearLightAfter = 600.0f;
constexpr float kFarMarginDepth = 300.0f;
constexpr float kUnboundedReach = 64.0f; // a mesh whose vertices can't be read: this far around its origin
// The same buffers this close are one mesh at one place: float noise of the camera's matrices,
// and TAA jitter should the projection SetTransform gives lack it (a shear of up to 0.5 m at 1 km).
constexpr double kSamePlace = 1.0, kSameAxes = 0.01;
// Alpha-tested meshes kept (CasterAlphaTest): each holds the state to draw it again, ~1 KB.
constexpr size_t kMaxAlpha = 8000;
constexpr DWORD kStages = 16;     // sampler stages of ps_2_0 and up
constexpr DWORD kMaxStreams = 16;

struct Caster {
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT offset = 0, stride = 0;
    IDirect3DIndexBuffer9* ib = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr; // ours: the position alone (not owned here)
    D3DPRIMITIVETYPE type = D3DPT_TRIANGLELIST;
    INT base = 0;
    UINT minIndex = 0, vertices = 0, start = 0, count = 0;
    float world[4][3] = {}; // mesh -> world, row vectors (rows 0..2 the axes, row 3 the place)
    float lo[3] = {}, hi[3] = {}; // world bounds
    float box[4] = {};            // light space bounds for the current light: across min/max, up min/max
    int alpha = -1;               // alpha-tested: its state in g_alpha; -1 opaque (our own shaders)
};

// An alpha-tested mesh is drawn into the maps with the game's own shaders, constants and
// textures as they were at its draw, and the game's alpha test: its holes stay holes whatever
// the material does to its alpha (which texture, which texture coordinates, a coordinate
// transform in the vertex shader, Tech3's planar projected ones). Only the camera's matrix
// (GbxVisualPrCamera) is swapped for the map's. Everything here holds a reference.
struct AlphaPart {
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr; // the game's (every element the shader reads)
    int camera = -1;                             // GbxVisualPrCamera's first register
    DWORD streams = 0;                           // the streams the declaration reads (bit per stream)
    IDirect3DVertexBuffer9* vb[kMaxStreams] = {};
    UINT offset[kMaxStreams] = {}, stride[kMaxStreams] = {};
    DWORD samplers = 0; // the pixel shader's samplers: bound to the kept texture, or to none
    IDirect3DBaseTexture9* texture[kStages] = {};
    DWORD sampler[kStages][8] = {}; // kSamplerStates
    DWORD alphaRef = 0, alphaFunc = D3DCMP_ALWAYS;
    // The shaders' constants: (kind, register, count) blocks into `data`.
    struct Block {
        BYTE kind; // 0 vertex float4, 1 vertex int4, 2 vertex bool, 3 pixel float4, 4 pixel int4, 5 pixel bool
        WORD reg, count;
        uint32_t at;
    };
    std::vector<Block> blocks;
    std::vector<uint32_t> data;
};

// What a shader reads, from its constant table: (set, register, count) of the constants
// (set 0 bool, 1 int4, 2 float4) and its samplers.
struct ShaderUse {
    struct Range {
        BYTE set;
        WORD reg, count;
    };
    std::vector<Range> ranges;
    DWORD samplers = 0;
};

const D3DSAMPLERSTATETYPE kSamplerStates[8] = {D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,  D3DSAMP_ADDRESSW,  D3DSAMP_BORDERCOLOR,
                                               D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_MAXMIPLEVEL};

// A mesh seen at a place, not yet for long enough.
struct Pending {
    float world[4][3];
    DWORD firstSeen;
    DWORD lastSeen;
};

// The game's vertex declarations: the stream and our declaration with its position alone.
struct DeclInfo {
    IDirect3DVertexDeclaration9* own = nullptr; // null: no usable position
    WORD stream = 0;
    WORD offset = 0;
    BYTE type = 0;
};

struct LightMap {
    IDirect3DTexture9* texture = nullptr;
    IDirect3DSurface9* surface = nullptr;
    float a0 = 0, sa = 1, b0 = 0, sb = 1, d0 = 0, sd = 1; // light space: across (u), up (v), along (f)
    float centre[3] = {};  // the camera the near map was laid around
    UINT cursor = 0;       // meshes before this one are in the map
    bool cleared = false;
    bool complete = false; // drawn through once: the shaders may use it
    bool active = false;   // a region is set
};

struct Stats {
    unsigned frames = 0, draws = 0, noCamera = 0, otherProjection = 0, noDecl = 0, stream = 0, pool = 0, pending = 0, known = 0;
    unsigned promoted = 0, duplicates = 0, unbounded = 0, mapDraws = 0, nearBuilds = 0;
    unsigned checked = 0, paused = 0; // draws checked (the others wait for a later frame); draws while paused
    // Alpha-tested draws checked; kept (with their state); not kept (no pixel shader, a stream
    // instanced or not managed, kMaxAlpha reached); render-target textures left out; drawn into the maps.
    unsigned alphaChecked = 0, alphaKept = 0, alphaRefused = 0, alphaTextures = 0, alphaMapDraws = 0;
    double recordMs = 0.0, updateMs = 0.0;
};

std::vector<Caster> g_casters;
// The keys are hashes already (mixKey): the standard library's hash would hash them again,
// byte by byte (FNV, 64-bit multiplies a 32-bit build does in three steps).
struct KeyHash {
    size_t operator()(uint64_t key) const { return static_cast<size_t>(key ^ (key >> 32)); }
};
std::unordered_set<uint64_t, KeyHash> g_known;                         // exact keys of kept meshes (the quick way)
std::unordered_map<uint64_t, std::vector<Pending>, KeyHash> g_pending; // buffers, draw call and 16 m cell -> places waiting
size_t g_pendingCount = 0;
std::unordered_map<uint64_t, std::vector<uint32_t>, KeyHash> g_byMesh; // buffers, draw call and 16 m cell -> the kept meshes there
std::unordered_map<const void*, int> g_vsRegister;            // the game's vertex shader -> GbxVisualPrCamera's register, -1 none
std::unordered_map<const void*, DeclInfo> g_decls;            // the game's declaration -> ours
// The last shader and declaration looked up: the game draws by material, most draws repeat them.
const void* g_lastVS = nullptr;
int g_lastRegister = -1;
const void* g_lastDecl = nullptr;
DeclInfo g_lastDeclInfo;
std::map<std::pair<WORD, BYTE>, IDirect3DVertexDeclaration9*> g_ownDecls; // (offset, type) -> position-only declaration
std::vector<AlphaPart> g_alpha;                               // the alpha-tested meshes' state (Caster::alpha)
std::unordered_map<const void*, ShaderUse> g_uses;            // the game's shaders -> what they read
Stats g_stats;
unsigned g_frameDraws = 0;           // draws handed over this frame
unsigned g_stride = 1, g_phase = 0;  // this frame checks the draws whose index + phase is a multiple of stride
bool g_paused = false;               // no pipeline wants the maps: nothing is checked
DWORD g_lastSweep = 0;
DWORD g_lastLog = 0;
bool g_firstLog = false;
bool g_full = false;
double g_toMs = 0.0;
double g_cpuTotal = 0.0; // the recording's and the maps' CPU time since the start (perflog)

// The inverse of the camera's view (rigid), for the frame's view.
float g_ivView[16] = {};
double g_invView[4][4] = {};
bool g_invValid = false;

// World bounds of every kept mesh: what the whole-track map must cover.
float g_boundsLo[3] = {1e30f, 1e30f, 1e30f}, g_boundsHi[3] = {-1e30f, -1e30f, -1e30f};

IDirect3DSurface9* g_nullTarget = nullptr; // NULL format colour target (no memory), else R32F
IDirect3DVertexShader9* g_vs = nullptr;
IDirect3DPixelShader9* g_ps = nullptr;
bool g_unsupported = false;
bool g_loggedResources = false;
LightMap g_far, g_near[2];
int g_nearFront = -1; // the near map the shaders use, -1 none yet
int g_nearBuild = -1; // the near map being drawn, -1 none
bool g_lightSet = false;
float g_f[3] = {}, g_u[3] = {}, g_v[3] = {};
unsigned g_farBuilds = 0;

const char* kCasterVS = R"(
float4x4 u_MeshToMap : register(c0); // mesh -> the map's clip space (row vectors, columns in c0..c3)
float4 main(float4 pos : POSITION) : POSITION {
    return mul(pos, u_MeshToMap);
}
)";

const char* kCasterPS = R"(
float4 main() : COLOR0 { return 0; }
)";

// The keys' hash, four bytes at a step (every draw of the main camera goes through it: byte by
// byte it was half of the recording's CPU time). `bytes` a multiple of 4.
uint64_t mixKey(uint64_t h, const void* data, size_t bytes) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i + 4 <= bytes; i += 4) {
        uint32_t word;
        memcpy(&word, p + i, 4);
        h = (h ^ word) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    return h;
}

// floor(x) as an integer (the CRT's floor is a call in a 32-bit build; the same result for
// every |x| < 2^63).
long long floorToInt(double x) {
    long long q = static_cast<long long>(x);
    if (static_cast<double>(q) > x) q--;
    return q;
}

double milliseconds(const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
    if (g_toMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_toMs = 1000.0 / static_cast<double>(f.QuadPart);
    }
    return static_cast<double>(b.QuadPart - a.QuadPart) * g_toMs;
}

// A shader's bytecode (DWORDs). False: none, or nonsense.
template <typename Shader>
bool shaderCode(Shader* shader, std::vector<DWORD>& code) {
    UINT size = 0;
    if (FAILED(shader->GetFunction(nullptr, &size)) || size < 8 || size >= (1u << 20)) return false;
    code.resize((size + 3) / 4);
    if (FAILED(shader->GetFunction(code.data(), &size))) return false;
    code.resize(size / 4);
    return true;
}

// The constant table in a shader's bytecode (shaders the game compiled from HLSL keep it):
// entry(set, register, count, table, bytes, name offset) for every constant. False: none.
template <typename Entry>
bool forEachConstant(const std::vector<DWORD>& code, Entry entry) {
    const size_t n = code.size();
    bool found = false;
    // Comment tokens: low word 0xFFFE, length in DWORDs in bits 16..30. The table
    // starts with "CTAB"; its offsets count from just after that.
    for (size_t i = 1; i < n;) {
        const DWORD token = code[i];
        if ((token & 0xFFFF) != 0xFFFE) break; // the table comes before the first instruction
        const size_t length = (token >> 16) & 0x7FFF;
        if (i + 1 + length > n) break;
        if (length >= 8 && code[i + 1] == MAKEFOURCC('C', 'T', 'A', 'B')) {
            found = true;
            const unsigned char* table = reinterpret_cast<const unsigned char*>(&code[i + 2]);
            const size_t bytes = (length - 1) * 4;
            auto u32 = [&](size_t at) -> DWORD { return at + 4 <= bytes ? *reinterpret_cast<const DWORD*>(table + at) : 0; };
            auto u16 = [&](size_t at) -> WORD { return at + 2 <= bytes ? *reinterpret_cast<const WORD*>(table + at) : 0; };
            const DWORD constants = u32(12), info = u32(16);
            for (DWORD c = 0; c < constants && c < 512; c++) {
                const size_t at = info + c * 20;
                if (at + 20 > bytes) break;
                entry(u16(at + 4), u16(at + 6), u16(at + 8), table, bytes, u32(at));
            }
        }
        i += 1 + length;
    }
    return found;
}

// GbxVisualPrCamera's first register, from the constant table in the shader's bytecode
// (shaders the game compiled from HLSL keep it). -1: the shader has none (assembled ones,
// the sky, 2D). c0 in every material shader seen so far (62 of 74 dumped vertex shaders).
int cameraRegister(IDirect3DVertexShader9* vs) {
    if (vs == g_lastVS) return g_lastRegister;
    auto it = g_vsRegister.find(vs);
    if (it != g_vsRegister.end()) {
        g_lastVS = vs;
        g_lastRegister = it->second;
        return it->second;
    }
    int result = -1;
    std::vector<DWORD> code;
    if (shaderCode(vs, code)) {
        forEachConstant(code, [&](WORD set, WORD reg, WORD count, const unsigned char* table, size_t bytes, DWORD name) {
            static const char kName[] = "GbxVisualPrCamera";
            if (name + sizeof(kName) <= bytes && !memcmp(table + name, kName, sizeof(kName)) && set == 2 /* D3DXRS_FLOAT4 */ && count == 4) {
                result = reg;
            }
        });
    }
    g_vsRegister[vs] = result;
    g_lastVS = vs;
    g_lastRegister = result;
    return result;
}

DeclInfo declInfo(IDirect3DDevice9* device, IDirect3DVertexDeclaration9* decl) {
    if (decl == g_lastDecl) return g_lastDeclInfo;
    auto it = g_decls.find(decl);
    if (it != g_decls.end()) {
        g_lastDecl = decl;
        g_lastDeclInfo = it->second;
        return it->second;
    }
    DeclInfo info;
    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1];
    UINT count = MAXD3DDECLLENGTH + 1;
    if (SUCCEEDED(decl->GetDeclaration(elements, &count))) {
        for (UINT i = 0; i < count && elements[i].Stream != 0xFF; i++) {
            const D3DVERTEXELEMENT9& e = elements[i];
            if (e.Usage != D3DDECLUSAGE_POSITION || e.UsageIndex != 0 || e.Method != D3DDECLMETHOD_DEFAULT) continue;
            if (e.Type != D3DDECLTYPE_FLOAT3 && e.Type != D3DDECLTYPE_FLOAT4) break;
            const auto key = std::make_pair(e.Offset, e.Type);
            IDirect3DVertexDeclaration9*& own = g_ownDecls[key];
            if (!own) {
                const D3DVERTEXELEMENT9 mine[] = {
                    {0, e.Offset, e.Type, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                    D3DDECL_END(),
                };
                device->CreateVertexDeclaration(mine, &own);
            }
            info.own = own;
            info.stream = e.Stream;
            info.offset = e.Offset;
            info.type = e.Type;
            break;
        }
    }
    g_decls[decl] = info;
    g_lastDecl = decl;
    g_lastDeclInfo = info;
    return info;
}

bool invert(const double m[4][4], double out[4][4]) {
    double a[4][8];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            a[r][c] = m[r][c];
            a[r][c + 4] = r == c ? 1.0 : 0.0;
        }
    }
    for (int c = 0; c < 4; c++) {
        int pivot = c;
        for (int r = c + 1; r < 4; r++) {
            if (fabs(a[r][c]) > fabs(a[pivot][c])) pivot = r;
        }
        if (fabs(a[pivot][c]) < 1e-12) return false;
        if (pivot != c) {
            for (int k = 0; k < 8; k++) std::swap(a[c][k], a[pivot][k]);
        }
        const double d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            const double f = a[r][c];
            if (f == 0.0) continue;
            for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) out[r][c] = a[r][c + 4];
    }
    return true;
}

bool inverseView(const float view[16]) {
    if (g_invValid && !memcmp(view, g_ivView, sizeof(g_ivView))) return true;
    double v[4][4];
    for (int i = 0; i < 16; i++) v[i / 4][i % 4] = view[i];
    memcpy(g_ivView, view, sizeof(g_ivView));
    g_invValid = invert(v, g_invView);
    return g_invValid;
}

// Mesh -> world from the game's mesh -> clip (c[j] = column j, row vectors) and the camera.
// The view-space position comes from clip x, y and w alone: clip z carries the near plane,
// which multiplied the float noise of the game's constants by 1 / near (up to 0.2 m of drift
// with the camera, every drift a new copy of the mesh). Clip z is used only to check that the
// draw used this projection (the sky's has another near plane). False: not this camera.
bool meshToWorld(const float c[4][4], const float* p, double w[4][4], bool& otherProjection) {
    otherProjection = false;
    // Perspective: w = view z (P03 = P13 = 0, P23 = 1, P33 = 0).
    if (p[3] != 0.0f || p[7] != 0.0f || p[11] != 1.0f || p[15] != 0.0f) {
        otherProjection = true;
        return false;
    }
    const double det = static_cast<double>(p[0]) * p[5] - static_cast<double>(p[4]) * p[1];
    if (fabs(det) < 1e-9) return false;
    double mv[4][4];
    for (int r = 0; r < 4; r++) {
        const double e = r == 3 ? 1.0 : 0.0;
        const double zv = c[3][r];
        const double x = c[0][r] - zv * p[8] - e * p[12];
        const double y = c[1][r] - zv * p[9] - e * p[13];
        const double xv = (x * p[5] - y * p[4]) / det;
        const double yv = (y * p[0] - x * p[1]) / det;
        const double z = xv * p[2] + yv * p[6] + zv * p[10] + e * p[14];
        if (fabs(c[2][r] - z) > 2e-5 * (1.0 + fabs(c[2][r]) + fabs(zv)) + 1e-4) {
            otherProjection = true;
            return false;
        }
        mv[r][0] = xv;
        mv[r][1] = yv;
        mv[r][2] = zv;
        mv[r][3] = e;
    }
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            double sum = 0.0;
            for (int k = 0; k < 4; k++) sum += mv[r][k] * g_invView[k][col];
            w[r][col] = sum;
        }
    }
    return true;
}

bool managedStatic(IDirect3DVertexBuffer9* vb) {
    D3DVERTEXBUFFER_DESC d{};
    return SUCCEEDED(vb->GetDesc(&d)) && d.Pool == D3DPOOL_MANAGED && !(d.Usage & D3DUSAGE_DYNAMIC);
}

bool managedStatic(IDirect3DIndexBuffer9* ib) {
    D3DINDEXBUFFER_DESC d{};
    return SUCCEEDED(ib->GetDesc(&d)) && d.Pool == D3DPOOL_MANAGED && !(d.Usage & D3DUSAGE_DYNAMIC);
}

// A texture an alpha-tested mesh may keep: not a render target or depth texture (their content
// changes every frame: the game's shadow buffer, reflections; a material's alpha isn't there).
bool keepableTexture(IDirect3DBaseTexture9* t) {
    DWORD usage = D3DUSAGE_RENDERTARGET;
    switch (t->GetType()) {
    case D3DRTYPE_TEXTURE: {
        D3DSURFACE_DESC d{};
        if (SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d))) usage = d.Usage;
        break;
    }
    case D3DRTYPE_CUBETEXTURE: {
        D3DSURFACE_DESC d{};
        if (SUCCEEDED(static_cast<IDirect3DCubeTexture9*>(t)->GetLevelDesc(0, &d))) usage = d.Usage;
        break;
    }
    case D3DRTYPE_VOLUMETEXTURE: {
        D3DVOLUME_DESC d{};
        if (SUCCEEDED(static_cast<IDirect3DVolumeTexture9*>(t)->GetLevelDesc(0, &d))) usage = d.Usage;
        break;
    }
    default:
        break;
    }
    return !(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL));
}

// What a shader reads (cached per shader until the meshes are let go).
template <typename Shader>
const ShaderUse& shaderUse(Shader* shader, bool pixel) {
    auto it = g_uses.find(shader);
    if (it != g_uses.end()) return it->second;
    ShaderUse use;
    std::vector<DWORD> code;
    bool table = false;
    if (shaderCode(shader, code)) {
        table = forEachConstant(code, [&](WORD set, WORD reg, WORD count, const unsigned char*, size_t, DWORD) {
            if (set == 3 /* D3DXRS_SAMPLER */) {
                for (DWORD s = reg; s < static_cast<DWORD>(reg) + count && s < kStages; s++) use.samplers |= 1u << s;
            } else if (set <= 2 && count > 0) {
                use.ranges.push_back({static_cast<BYTE>(set), reg, count});
            }
        });
    }
    if (!table && pixel) {
        // Assembled (the game's ps_1_x materials keep no table): every constant and stage it can read.
        const bool v1 = code.empty() || ((code[0] >> 8) & 0xFF) <= 1;
        use.ranges.push_back({2, 0, static_cast<WORD>(v1 ? 8 : 32)});
        use.samplers = v1 ? 0x3F : 0xFFFF;
    }
    return g_uses[shader] = use;
}

void releaseAlpha(AlphaPart& a) {
    gfx::release(a.vs);
    gfx::release(a.ps);
    gfx::release(a.decl);
    for (DWORD s = 0; s < kMaxStreams; s++) gfx::release(a.vb[s]);
    for (DWORD s = 0; s < kStages; s++) gfx::release(a.texture[s]);
}

// The state of the alpha-tested draw bound now, to draw it again into the maps. False: it
// can't be (no pixel shader, a stream instanced or not in the managed pool).
bool captureAlpha(IDirect3DDevice9* device, int camera, AlphaPart& a) {
    device->GetPixelShader(&a.ps);
    device->GetVertexShader(&a.vs);
    device->GetVertexDeclaration(&a.decl);
    if (!a.ps || !a.vs || !a.decl) return false;
    a.camera = camera;
    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1];
    UINT count = MAXD3DDECLLENGTH + 1;
    if (FAILED(a.decl->GetDeclaration(elements, &count))) return false;
    for (UINT i = 0; i < count && elements[i].Stream != 0xFF; i++) {
        if (elements[i].Stream >= kMaxStreams) return false;
        a.streams |= 1u << elements[i].Stream;
    }
    for (DWORD s = 0; s < kMaxStreams; s++) {
        if (!(a.streams & (1u << s))) continue;
        UINT freq = 1;
        device->GetStreamSourceFreq(s, &freq);
        device->GetStreamSource(s, &a.vb[s], &a.offset[s], &a.stride[s]);
        if (!a.vb[s] || freq != 1 || a.stride[s] == 0 || !managedStatic(a.vb[s])) return false;
    }
    auto snapshot = [&](const ShaderUse& use, bool pixel) {
        for (const ShaderUse::Range& r : use.ranges) {
            const UINT limit = r.set == 2 ? (pixel ? 224u : 256u) : 16u;
            if (r.reg >= limit) continue;
            const WORD n = static_cast<WORD>(r.count < limit - r.reg ? r.count : limit - r.reg);
            AlphaPart::Block b{static_cast<BYTE>((pixel ? 3 : 0) + (2 - r.set)), r.reg, n, static_cast<uint32_t>(a.data.size())};
            a.data.resize(a.data.size() + (r.set == 0 ? n : n * 4u), 0);
            void* p = a.data.data() + b.at;
            switch (b.kind) {
            case 0: device->GetVertexShaderConstantF(b.reg, static_cast<float*>(p), n); break;
            case 1: device->GetVertexShaderConstantI(b.reg, static_cast<int*>(p), n); break;
            case 2: device->GetVertexShaderConstantB(b.reg, static_cast<BOOL*>(p), n); break;
            case 3: device->GetPixelShaderConstantF(b.reg, static_cast<float*>(p), n); break;
            case 4: device->GetPixelShaderConstantI(b.reg, static_cast<int*>(p), n); break;
            default: device->GetPixelShaderConstantB(b.reg, static_cast<BOOL*>(p), n); break;
            }
            a.blocks.push_back(b);
        }
    };
    snapshot(shaderUse(a.vs, false), false);
    const ShaderUse& pixelUse = shaderUse(a.ps, true);
    snapshot(pixelUse, true);
    a.samplers = pixelUse.samplers;
    for (DWORD s = 0; s < kStages; s++) {
        if (!(pixelUse.samplers & (1u << s))) continue;
        IDirect3DBaseTexture9* t = nullptr;
        device->GetTexture(s, &t);
        if (!t) continue;
        if (!keepableTexture(t)) {
            t->Release();
            g_stats.alphaTextures++;
            continue;
        }
        a.texture[s] = t;
        for (int k = 0; k < 8; k++) device->GetSamplerState(s, kSamplerStates[k], &a.sampler[s][k]);
    }
    device->GetRenderState(D3DRS_ALPHAREF, &a.alphaRef);
    device->GetRenderState(D3DRS_ALPHAFUNC, &a.alphaFunc);
    return true;
}

// The mesh's own bounds from its vertices (the managed pool keeps a copy in system memory:
// reading it doesn't touch the GPU). False: not readable, or nonsense.
bool meshBounds(const Caster& m, const DeclInfo& info, float lo[3], float hi[3]) {
    const long long first = static_cast<long long>(m.base) + m.minIndex;
    if (first < 0 || m.vertices == 0 || m.stride < info.offset + 12u) return false;
    void* data = nullptr;
    const UINT from = m.offset + static_cast<UINT>(first) * m.stride;
    if (FAILED(m.vb->Lock(from, m.vertices * m.stride, &data, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) || !data) return false;
    for (int j = 0; j < 3; j++) {
        lo[j] = 1e30f;
        hi[j] = -1e30f;
    }
    bool ok = true;
    __try {
        const unsigned char* v = static_cast<const unsigned char*>(data) + info.offset;
        for (UINT i = 0; i < m.vertices; i++, v += m.stride) {
            const float* p = reinterpret_cast<const float*>(v);
            for (int j = 0; j < 3; j++) {
                lo[j] = fminf(lo[j], p[j]);
                hi[j] = fmaxf(hi[j], p[j]);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    m.vb->Unlock();
    for (int j = 0; j < 3 && ok; j++) ok = std::isfinite(lo[j]) && std::isfinite(hi[j]) && hi[j] >= lo[j] && hi[j] - lo[j] < 5000.0f;
    return ok;
}

float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void normalize(float v[3]) {
    const float l = sqrtf(dot3(v, v));
    if (l > 1e-8f) {
        for (int j = 0; j < 3; j++) v[j] /= l;
    }
}

// A world box's bounds in the light's frame: across, up, along.
void lightBox(const float lo[3], const float hi[3], float out[6]) {
    out[0] = out[2] = out[4] = 1e30f;
    out[1] = out[3] = out[5] = -1e30f;
    for (int corner = 0; corner < 8; corner++) {
        const float p[3] = {(corner & 1) ? hi[0] : lo[0], (corner & 2) ? hi[1] : lo[1], (corner & 4) ? hi[2] : lo[2]};
        const float l[3] = {dot3(p, g_u), dot3(p, g_v), dot3(p, g_f)};
        for (int j = 0; j < 3; j++) {
            out[j * 2] = fminf(out[j * 2], l[j]);
            out[j * 2 + 1] = fmaxf(out[j * 2 + 1], l[j]);
        }
    }
}

void casterBox(Caster& m) {
    float b[6];
    lightBox(m.lo, m.hi, b);
    m.box[0] = b[0];
    m.box[1] = b[1];
    m.box[2] = b[2];
    m.box[3] = b[3];
}

void forgetMeshes() {
    for (Caster& c : g_casters) {
        gfx::release(c.vb);
        gfx::release(c.ib);
    }
    g_casters.clear();
    for (AlphaPart& a : g_alpha) releaseAlpha(a);
    g_alpha.clear();
    g_uses.clear();
    g_known.clear();
    g_pending.clear();
    g_pendingCount = 0;
    g_byMesh.clear();
    g_vsRegister.clear();
    g_decls.clear();
    g_lastVS = g_lastDecl = nullptr;
    g_invValid = false;
    for (int j = 0; j < 3; j++) {
        g_boundsLo[j] = 1e30f;
        g_boundsHi[j] = -1e30f;
    }
    g_far.active = g_far.complete = false;
    g_near[0].active = g_near[0].complete = false;
    g_near[1].active = g_near[1].complete = false;
    g_nearFront = g_nearBuild = -1;
    g_lightSet = false;
    g_full = false;
}

void logStats(const char* when) {
    const Stats& s = g_stats;
    const double f = s.frames ? static_cast<double>(s.frames) : 1.0;
    TMVS_LOG("casters (%s): %u meshes kept (%u new, %u copies merged, %u without readable bounds); per frame %.0f indexed draws, "
             "%.0f opaque ones checked (%.0f known, %.0f waiting, %.0f other shader, %.0f other projection, %.0f no float position, %.0f instanced, "
             "%.0f not managed), %.0f while paused; CPU %.3f ms recording + %.3f ms drawing, %.0f meshes drawn into the maps; near map "
             "drawn %u times, whole-track map %u times; %u frames",
             when, static_cast<unsigned>(g_casters.size()), s.promoted, s.duplicates, s.unbounded, s.draws / f, s.checked / f, s.known / f,
             s.pending / f, s.noCamera / f, s.otherProjection / f, s.noDecl / f, s.stream / f, s.pool / f, s.paused / f, s.recordMs / f,
             s.updateMs / f, s.mapDraws / f, s.nearBuilds, g_farBuilds, s.frames);
    // CasterAlphaTest: only once alpha-tested draws came (none: the line is as before).
    if (s.alphaChecked || !g_alpha.empty()) {
        TMVS_LOG("casters (%s): alpha-tested: %u meshes kept (%u new, %u refused), %.0f draws checked per frame, %.0f drawn into the maps "
                 "per frame, %u render-target textures left out",
                 when, static_cast<unsigned>(g_alpha.size()), s.alphaKept, s.alphaRefused, s.alphaChecked / f, s.alphaMapDraws / f,
                 s.alphaTextures);
    }
    g_stats = Stats();
}

bool createMap(IDirect3DDevice9* device, LightMap& m, D3DFORMAT intz) {
    if (m.texture) return true;
    return SUCCEEDED(device->CreateTexture(kMapSize, kMapSize, 1, D3DUSAGE_DEPTHSTENCIL, intz, D3DPOOL_DEFAULT, &m.texture, nullptr)) &&
           SUCCEEDED(m.texture->GetSurfaceLevel(0, &m.surface));
}

void releaseMap(LightMap& m) {
    gfx::release(m.surface);
    gfx::release(m.texture);
    m.active = m.complete = false;
}

bool ensureResources(IDirect3DDevice9* device) {
    if (g_unsupported) return false;
    if (g_far.texture && g_near[0].texture && g_near[1].texture && g_nullTarget && g_vs && g_ps) return true;
    IDirect3D9* d3d = nullptr;
    device->GetDirect3D(&d3d);
    D3DDEVICE_CREATION_PARAMETERS cp{};
    device->GetCreationParameters(&cp);
    D3DDISPLAYMODE mode{};
    device->GetDisplayMode(0, &mode);
    const D3DFORMAT intz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
    const D3DFORMAT nullFormat = static_cast<D3DFORMAT>(MAKEFOURCC('N', 'U', 'L', 'L'));
    const bool haveIntz = d3d && SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format, D3DUSAGE_DEPTHSTENCIL,
                                                                  D3DRTYPE_TEXTURE, intz));
    const bool haveNull = d3d && SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format, D3DUSAGE_RENDERTARGET,
                                                                  D3DRTYPE_SURFACE, nullFormat));
    if (d3d) d3d->Release();
    if (!haveIntz) {
        TMVS_LOG("casters: no INTZ depth textures - CasterShadows unavailable");
        g_unsupported = true;
        return false;
    }
    bool ok = createMap(device, g_far, intz) && createMap(device, g_near[0], intz) && createMap(device, g_near[1], intz);
    if (ok && !g_nullTarget) {
        // A colour target must be bound; the NULL format takes no memory.
        ok = SUCCEEDED(device->CreateRenderTarget(kMapSize, kMapSize, haveNull ? nullFormat : D3DFMT_R32F, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                  &g_nullTarget, nullptr));
    }
    if (ok && !g_vs) g_vs = gfx::compileVertexShader(device, kCasterVS, "main", "CasterVS");
    if (ok && !g_ps) g_ps = gfx::compilePixelShader(device, kCasterPS, "main", "CasterPS");
    if (!ok || !g_vs || !g_ps) {
        TMVS_LOG("casters: map resources failed (3 x %ux%u INTZ%s) - CasterShadows unavailable", kMapSize, kMapSize,
                 haveNull ? ", NULL target" : ", R32F target");
        releaseDevice();
        g_unsupported = true;
        return false;
    }
    if (!g_loggedResources) {
        g_loggedResources = true;
        TMVS_LOG("casters: 3 maps %ux%u INTZ (%u MB of video memory)%s", kMapSize, kMapSize, 3 * kMapSize * kMapSize * 4 / (1024 * 1024),
                 haveNull ? "" : " + an R32F colour target");
        hook::logMemory("after the caster maps");
    }
    return true;
}

// The region of the whole-track map: every kept mesh, with room to grow.
bool fitFar() {
    float b[6];
    lightBox(g_boundsLo, g_boundsHi, b);
    b[4] -= kFarMarginDepth;
    b[5] += kFarMarginDepth;
    LightMap& m = g_far;
    const bool outside = !m.active || b[0] < m.a0 || b[1] > m.a0 + m.sa || b[2] < m.b0 || b[3] > m.b0 + m.sb || b[4] < m.d0 ||
                         b[5] > m.d0 + m.sd;
    if (!outside) return false;
    // A fifth more room on every side: a growing track doesn't start it over with every block.
    const float ga = (b[1] - b[0]) * 0.2f, gb = (b[3] - b[2]) * 0.2f, gd = (b[5] - b[4]) * 0.2f;
    m.a0 = b[0] - ga;
    m.sa = b[1] - b[0] + 2.0f * ga;
    m.b0 = b[2] - gb;
    m.sb = b[3] - b[2] + 2.0f * gb;
    m.d0 = b[4] - gd;
    m.sd = b[5] - b[4] + 2.0f * gd;
    m.cursor = 0;
    m.cleared = m.complete = false;
    m.active = true;
    if (++g_farBuilds <= 12) {
        TMVS_LOG("casters: whole-track map %.0f x %.0f m (%.2f x %.2f m a texel), %u meshes", m.sa, m.sb, m.sa / kMapSize, m.sb / kMapSize,
                 static_cast<unsigned>(g_casters.size()));
    }
    return true;
}

// The near map's region around the camera.
void fitNear(LightMap& m, const float camera[3]) {
    const float lo[3] = {camera[0] - kNearHalf, camera[1] - kNearBelow, camera[2] - kNearHalf};
    const float hi[3] = {camera[0] + kNearHalf, camera[1] + kNearAbove, camera[2] + kNearHalf};
    float b[6];
    lightBox(lo, hi, b);
    const float d = dot3(camera, g_f);
    m.a0 = b[0];
    m.sa = b[1] - b[0];
    m.b0 = b[2];
    m.sb = b[3] - b[2];
    m.d0 = d - kNearLightBefore;
    m.sd = kNearLightBefore + kNearLightAfter;
    memcpy(m.centre, camera, sizeof(m.centre));
    m.cursor = 0;
    m.cleared = m.complete = false;
    m.active = true;
}

bool overlaps(const Caster& c, const LightMap& m) {
    return c.box[1] >= m.a0 && c.box[0] <= m.a0 + m.sa && c.box[3] >= m.b0 && c.box[2] <= m.b0 + m.sb;
}

// What the alpha-tested meshes' draws change beyond the opaque ones' (constants, samplers,
// streams 1..15): saved at the first such draw of an update, given back at its end. The
// pipeline's passes after the maps rely on constants they set before.
struct AlphaSave {
    bool constants = false;
    float vsF[256][4], psF[224][4];
    int vsI[16][4], psI[16][4];
    BOOL vsB[16], psB[16];
    DWORD alphaRef, alphaFunc;
    DWORD stages = 0;
    IDirect3DBaseTexture9* texture[kStages];
    DWORD sampler[kStages][8];
    DWORD streams = 0;
    IDirect3DVertexBuffer9* vb[kMaxStreams];
    UINT offset[kMaxStreams], stride[kMaxStreams];
};
AlphaSave g_save;
UINT g_vsConstants = 0, g_psConstants = 0; // the device's float registers

void saveForAlpha(IDirect3DDevice9* device, const AlphaPart& a) {
    AlphaSave& s = g_save;
    if (!s.constants) {
        s.constants = true;
        if (!g_vsConstants) {
            D3DCAPS9 caps{};
            device->GetDeviceCaps(&caps);
            g_vsConstants = caps.MaxVertexShaderConst < 96 ? 96 : (caps.MaxVertexShaderConst > 256 ? 256 : caps.MaxVertexShaderConst);
            g_psConstants = caps.PixelShaderVersion >= D3DPS_VERSION(3, 0) ? 224 : 32;
        }
        device->GetVertexShaderConstantF(0, &s.vsF[0][0], g_vsConstants);
        device->GetPixelShaderConstantF(0, &s.psF[0][0], g_psConstants);
        device->GetVertexShaderConstantI(0, &s.vsI[0][0], 16);
        device->GetPixelShaderConstantI(0, &s.psI[0][0], 16);
        device->GetVertexShaderConstantB(0, s.vsB, 16);
        device->GetPixelShaderConstantB(0, s.psB, 16);
        device->GetRenderState(D3DRS_ALPHAREF, &s.alphaRef);
        device->GetRenderState(D3DRS_ALPHAFUNC, &s.alphaFunc);
    }
    const DWORD streams = a.streams & ~1u & ~s.streams; // stream 0: update() gives it back
    for (DWORD i = 1; i < kMaxStreams; i++) {
        if (streams & (1u << i)) device->GetStreamSource(i, &s.vb[i], &s.offset[i], &s.stride[i]);
    }
    s.streams |= streams;
    const DWORD stages = a.samplers & ~s.stages;
    for (DWORD i = 0; i < kStages; i++) {
        if (!(stages & (1u << i))) continue;
        device->GetTexture(i, &s.texture[i]);
        for (int k = 0; k < 8; k++) device->GetSamplerState(i, kSamplerStates[k], &s.sampler[i][k]);
    }
    s.stages |= stages;
}

void restoreAfterAlpha(IDirect3DDevice9* device) {
    AlphaSave& s = g_save;
    if (s.constants) {
        device->SetVertexShaderConstantF(0, &s.vsF[0][0], g_vsConstants);
        device->SetPixelShaderConstantF(0, &s.psF[0][0], g_psConstants);
        device->SetVertexShaderConstantI(0, &s.vsI[0][0], 16);
        device->SetPixelShaderConstantI(0, &s.psI[0][0], 16);
        device->SetVertexShaderConstantB(0, s.vsB, 16);
        device->SetPixelShaderConstantB(0, s.psB, 16);
        device->SetRenderState(D3DRS_ALPHAREF, s.alphaRef);
        device->SetRenderState(D3DRS_ALPHAFUNC, s.alphaFunc);
    }
    for (DWORD i = 1; i < kMaxStreams; i++) {
        if (!(s.streams & (1u << i))) continue;
        device->SetStreamSource(i, s.vb[i], s.offset[i], s.stride[i]);
        gfx::release(s.vb[i]);
    }
    for (DWORD i = 0; i < kStages; i++) {
        if (!(s.stages & (1u << i))) continue;
        device->SetTexture(i, s.texture[i]);
        for (int k = 0; k < 8; k++) device->SetSamplerState(i, kSamplerStates[k], s.sampler[i][k]);
        gfx::release(s.texture[i]);
    }
    s.constants = false;
    s.streams = s.stages = 0;
}

// An alpha-tested mesh: the game's draw again with the map's matrix (see AlphaPart).
void drawAlpha(IDirect3DDevice9* device, const Caster& c, const AlphaPart& a, const float columns[4][4]) {
    saveForAlpha(device, a);
    device->SetVertexShader(a.vs);
    device->SetPixelShader(a.ps);
    device->SetVertexDeclaration(a.decl);
    for (const AlphaPart::Block& b : a.blocks) {
        const void* p = a.data.data() + b.at;
        switch (b.kind) {
        case 0: device->SetVertexShaderConstantF(b.reg, static_cast<const float*>(p), b.count); break;
        case 1: device->SetVertexShaderConstantI(b.reg, static_cast<const int*>(p), b.count); break;
        case 2: device->SetVertexShaderConstantB(b.reg, static_cast<const BOOL*>(p), b.count); break;
        case 3: device->SetPixelShaderConstantF(b.reg, static_cast<const float*>(p), b.count); break;
        case 4: device->SetPixelShaderConstantI(b.reg, static_cast<const int*>(p), b.count); break;
        default: device->SetPixelShaderConstantB(b.reg, static_cast<const BOOL*>(p), b.count); break;
        }
    }
    device->SetVertexShaderConstantF(a.camera, &columns[0][0], 4);
    for (DWORD s = 0; s < kMaxStreams; s++) {
        if (a.streams & (1u << s)) device->SetStreamSource(s, a.vb[s], a.offset[s], a.stride[s]);
    }
    for (DWORD s = 0; s < kStages; s++) {
        if (!(a.samplers & (1u << s))) continue;
        device->SetTexture(s, a.texture[s]);
        if (!a.texture[s]) continue;
        for (int k = 0; k < 8; k++) device->SetSamplerState(s, kSamplerStates[k], a.sampler[s][k]);
    }
    device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHAREF, a.alphaRef);
    device->SetRenderState(D3DRS_ALPHAFUNC, a.alphaFunc);
    device->SetIndices(c.ib);
    device->DrawIndexedPrimitive(c.type, c.base, c.minIndex, c.vertices, c.start, c.count);
}

// Draws the meshes from the map's cursor on (only those over its region), within the budget.
// `alphaBound`: the last draw was an alpha-tested mesh's (the game's shaders are bound).
void drawInto(IDirect3DDevice9* device, LightMap& m, UINT& budget, UINT& checks, bool select, bool& alphaBound) {
    const UINT total = static_cast<UINT>(g_casters.size());
    if (!m.active || (m.cleared && m.cursor >= total) || budget == 0 || checks == 0) return;
    device->SetDepthStencilSurface(m.surface);
    if (!m.cleared) {
        device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
        m.cleared = true;
    }
    // World -> the map's clip space (row vectors): x across, y up, z along the light.
    double lm[4][4] = {};
    for (int r = 0; r < 3; r++) {
        lm[r][0] = 2.0 * g_u[r] / m.sa;
        lm[r][1] = 2.0 * g_v[r] / m.sb;
        lm[r][2] = g_f[r] / m.sd;
    }
    lm[3][0] = -2.0 * m.a0 / m.sa - 1.0;
    lm[3][1] = -2.0 * m.b0 / m.sb - 1.0;
    lm[3][2] = -m.d0 / m.sd;
    lm[3][3] = 1.0;
    IDirect3DVertexDeclaration9* lastDecl = nullptr;
    UINT i = m.cursor;
    for (; i < total && budget > 0 && checks > 0; i++) {
        checks--;
        const Caster& c = g_casters[i];
        if (select && !overlaps(c, m)) continue;
        float columns[4][4];
        for (int col = 0; col < 4; col++) {
            for (int r = 0; r < 4; r++) {
                double sum = r == 3 ? lm[3][col] : 0.0;
                for (int k = 0; k < 3; k++) sum += static_cast<double>(c.world[r][k]) * lm[k][col];
                columns[col][r] = static_cast<float>(sum);
            }
        }
        if (c.alpha >= 0) {
            drawAlpha(device, c, g_alpha[c.alpha], columns);
            alphaBound = true;
            lastDecl = nullptr;
            budget--;
            g_stats.mapDraws++;
            g_stats.alphaMapDraws++;
            continue;
        }
        if (alphaBound) {
            device->SetVertexShader(g_vs);
            device->SetPixelShader(g_ps);
            device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            alphaBound = false;
        }
        if (c.decl != lastDecl) {
            device->SetVertexDeclaration(c.decl);
            lastDecl = c.decl;
        }
        device->SetStreamSource(0, c.vb, c.offset, c.stride);
        device->SetIndices(c.ib);
        device->SetVertexShaderConstantF(0, &columns[0][0], 4);
        device->DrawIndexedPrimitive(c.type, c.base, c.minIndex, c.vertices, c.start, c.count);
        budget--;
        g_stats.mapDraws++;
    }
    m.cursor = i;
    if (i >= total) m.complete = true;
}

void mapInfo(const LightMap& m, MapInfo& out) {
    // World -> the map's texture coordinates. D3D9 rasterizes texel i at screen x = i, which
    // the texture holds at (i + 0.5) / size: half a texel on top of the projection.
    const float half = 0.5f / kMapSize;
    for (int j = 0; j < 3; j++) {
        out.rows[0][j] = g_u[j] / m.sa;
        out.rows[1][j] = -g_v[j] / m.sb;
        out.rows[2][j] = g_f[j] / m.sd;
    }
    out.rows[0][3] = -m.a0 / m.sa + half;
    out.rows[1][3] = 1.0f + m.b0 / m.sb + half;
    out.rows[2][3] = -m.d0 / m.sd;
    out.texel[0] = out.texel[1] = 1.0f / kMapSize;
    // A receiver must lie this far behind the stored depth. One texel up the map covers
    // texel / sin(elevation) of flat ground, whose depth changes by cos(elevation) per metre:
    // the four taps of the filter see up to that much, plus a constant for the depth's noise.
    const float sinEl = fmaxf(-g_f[1], 0.05f);
    const float cosEl = sqrtf(fmaxf(1.0f - sinEl * sinEl, 0.0f));
    const float ground = (m.sb / kMapSize) / sinEl;
    out.bias = (0.15f + 1.5f * ground * cosEl) / m.sd;
    out.texture = m.texture;
}

} // namespace

bool takeTurn() {
    g_stats.draws++;
    const unsigned drawIndex = g_frameDraws++;
    if (g_paused) {
        g_stats.paused++;
        return false;
    }
    return (drawIndex + g_phase) % g_stride == 0; // else a later frame's turn
}

void recordDraw(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start, UINT count,
                const float view[16], const float projection[16], bool alphaTested) {
    g_stats.checked++;
    if (alphaTested) g_stats.alphaChecked++;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    struct Timer {
        LARGE_INTEGER& from;
        ~Timer() {
            LARGE_INTEGER to;
            QueryPerformanceCounter(&to);
            const double ms = milliseconds(from, to);
            g_stats.recordMs += ms;
            g_cpuTotal += ms;
        }
    } timer{t0};
    if (g_full || count == 0) return;
    IDirect3DVertexShader9* vs = nullptr;
    device->GetVertexShader(&vs);
    if (!vs) {
        g_stats.noCamera++;
        return;
    }
    const int reg = cameraRegister(vs);
    vs->Release(); // kept as a key only (the game holds it)
    if (reg < 0) {
        g_stats.noCamera++;
        return;
    }
    float c[4][4];
    if (FAILED(device->GetVertexShaderConstantF(reg, &c[0][0], 4)) || !inverseView(view)) {
        g_stats.noCamera++;
        return;
    }
    double w[4][4];
    bool otherProjection = false;
    if (!meshToWorld(c, projection, w, otherProjection)) {
        g_stats.otherProjection++;
        return;
    }
    IDirect3DVertexDeclaration9* decl = nullptr;
    device->GetVertexDeclaration(&decl);
    if (!decl) {
        g_stats.noDecl++;
        return;
    }
    const DeclInfo info = declInfo(device, decl);
    decl->Release();
    if (!info.own) {
        g_stats.noDecl++;
        return;
    }
    UINT freq = 1;
    device->GetStreamSourceFreq(info.stream, &freq);
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT offset = 0, stride = 0;
    device->GetStreamSource(info.stream, &vb, &offset, &stride);
    if (!vb || freq != 1 || stride == 0) {
        g_stats.stream++;
        gfx::release(vb);
        return;
    }
    IDirect3DIndexBuffer9* ib = nullptr;
    device->GetIndices(&ib);
    if (!ib) {
        g_stats.pool++;
        gfx::release(vb);
        return;
    }
    // The key: the mesh and its place (rounded to 2 cm and 1/2000; the recovery is exact to
    // well below that, see meshToWorld).
    uint64_t mesh = 14695981039346656037ull;
    const void* ids[2] = {vb, ib};
    const UINT draw[8] = {offset, stride, static_cast<UINT>(type), static_cast<UINT>(base), minIndex, vertices, start, count};
    mesh = mixKey(mesh, ids, sizeof(ids));
    mesh = mixKey(mesh, draw, sizeof(draw));
    uint64_t h = mesh;
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 3; col++) {
            const long long q = floorToInt(w[r][col] * (r == 3 ? 50.0 : 2000.0) + 0.5);
            h = mixKey(h, &q, sizeof(q));
        }
    }
    // Known: its buffers are held since it was kept, so they are the managed ones it had then
    // (no need to ask the buffers for their pool on every draw).
    if (g_known.count(h)) {
        g_stats.known++;
        gfx::release(vb);
        gfx::release(ib);
        return;
    }
    if (!managedStatic(vb) || !managedStatic(ib)) {
        g_stats.pool++;
        gfx::release(vb);
        gfx::release(ib);
        return;
    }
    auto samePlace = [&](const float k[4][3]) {
        for (int r = 0; r < 4; r++) {
            for (int col = 0; col < 3; col++) {
                if (fabs(w[r][col] - k[r][col]) >= (r == 3 ? kSamePlace : kSameAxes)) return false;
            }
        }
        return true;
    };
    // Places are looked up by mesh and 16 m cell (the cell and its neighbours): a block mesh
    // the track uses hundreds of times isn't compared with every copy.
    const long long cellX = floorToInt(w[3][0] / 16.0), cellZ = floorToInt(w[3][2] / 16.0);
    auto bucket = [&](long long x, long long z) {
        const long long xz[2] = {x, z};
        return mixKey(mesh, xz, sizeof(xz));
    };
    // Kept already, under another rounding: its key leads there too from now on (bounded: with
    // the jitter missing the keys would change every frame).
    for (long long dz = -1; dz <= 1; dz++) {
        for (long long dx = -1; dx <= 1; dx++) {
            auto it = g_byMesh.find(bucket(cellX + dx, cellZ + dz));
            if (it == g_byMesh.end()) continue;
            for (uint32_t index : it->second) {
                if (!samePlace(g_casters[index].world)) continue;
                g_stats.known++;
                g_stats.duplicates++;
                if (g_known.size() < 4 * kMaxCasters) g_known.insert(h);
                gfx::release(vb);
                gfx::release(ib);
                return;
            }
        }
    }
    // Waiting: kept once it stood at the same place for half a second.
    const DWORD now = GetTickCount();
    bool ready = false, found = false;
    for (long long dz = -1; dz <= 1 && !found; dz++) {
        for (long long dx = -1; dx <= 1 && !found; dx++) {
            auto it = g_pending.find(bucket(cellX + dx, cellZ + dz));
            if (it == g_pending.end()) continue;
            std::vector<Pending>& waiting = it->second;
            for (size_t i = 0; i < waiting.size(); i++) {
                if (!samePlace(waiting[i].world)) continue;
                found = true;
                waiting[i].lastSeen = now;
                if (now - waiting[i].firstSeen >= kStaticAfterMs) {
                    ready = true;
                    waiting.erase(waiting.begin() + i);
                    g_pendingCount--;
                }
                break;
            }
        }
    }
    if (!found && g_pendingCount < kMaxPending) {
        std::vector<Pending>& waiting = g_pending[bucket(cellX, cellZ)];
        Pending fresh;
        for (int r = 0; r < 4; r++) {
            for (int col = 0; col < 3; col++) fresh.world[r][col] = static_cast<float>(w[r][col]);
        }
        fresh.firstSeen = fresh.lastSeen = now;
        waiting.push_back(fresh);
        g_pendingCount++;
    }
    if (!ready) {
        g_stats.pending++;
        gfx::release(vb);
        gfx::release(ib);
        return;
    }
    g_known.insert(h);
    // Alpha-tested: kept with the state to draw it again (once: the key stays known also when
    // it can't be, so it isn't looked at again).
    int alpha = -1;
    if (alphaTested) {
        AlphaPart part;
        if (g_alpha.size() >= kMaxAlpha || !captureAlpha(device, reg, part)) {
            releaseAlpha(part);
            g_stats.alphaRefused++;
            gfx::release(vb);
            gfx::release(ib);
            return;
        }
        alpha = static_cast<int>(g_alpha.size());
        g_alpha.push_back(std::move(part));
        g_stats.alphaKept++;
    }
    // Static: kept, with the game's buffers (references held until reset()).
    Caster m;
    m.alpha = alpha;
    m.vb = vb;
    m.offset = offset;
    m.stride = stride;
    m.ib = ib;
    m.decl = info.own;
    m.type = type;
    m.base = base;
    m.minIndex = minIndex;
    m.vertices = vertices;
    m.start = start;
    m.count = count;
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 3; col++) m.world[r][col] = static_cast<float>(w[r][col]);
    }
    float lo[3], hi[3];
    if (meshBounds(m, info, lo, hi)) {
        for (int j = 0; j < 3; j++) {
            m.lo[j] = m.hi[j] = m.world[3][j];
        }
        for (int corner = 0; corner < 8; corner++) {
            const float q[3] = {(corner & 1) ? hi[0] : lo[0], (corner & 2) ? hi[1] : lo[1], (corner & 4) ? hi[2] : lo[2]};
            for (int j = 0; j < 3; j++) {
                const float v = q[0] * m.world[0][j] + q[1] * m.world[1][j] + q[2] * m.world[2][j] + m.world[3][j];
                m.lo[j] = fminf(m.lo[j], v);
                m.hi[j] = fmaxf(m.hi[j], v);
            }
        }
    } else {
        g_stats.unbounded++;
        for (int j = 0; j < 3; j++) {
            m.lo[j] = m.world[3][j] - kUnboundedReach;
            m.hi[j] = m.world[3][j] + kUnboundedReach;
        }
    }
    for (int j = 0; j < 3; j++) {
        g_boundsLo[j] = fminf(g_boundsLo[j], m.lo[j]);
        g_boundsHi[j] = fmaxf(g_boundsHi[j], m.hi[j]);
    }
    if (g_lightSet) casterBox(m);
    g_byMesh[bucket(cellX, cellZ)].push_back(static_cast<uint32_t>(g_casters.size()));
    g_casters.push_back(m);
    g_stats.promoted++;
    if (g_casters.size() >= kMaxCasters && !g_full) {
        g_full = true;
        TMVS_LOG("casters: %u meshes kept, no more for this map", static_cast<unsigned>(g_casters.size()));
    }
}

bool update(IDirect3DDevice9* device, const float sunWorld[3], const float camera[3], Maps& out) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    out = Maps();
    g_stats.frames++;
    const DWORD now = GetTickCount();
    // Things that moved never come back to the same place: forget them after a while.
    if (now - g_lastSweep > 3000 || g_pendingCount >= kMaxPending) {
        g_lastSweep = now;
        const bool full = g_pendingCount >= kMaxPending;
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            std::vector<Pending>& waiting = it->second;
            for (size_t i = 0; i < waiting.size();) {
                if (full || now - waiting[i].lastSeen > 2000) {
                    waiting[i] = waiting.back();
                    waiting.pop_back();
                    g_pendingCount--;
                } else {
                    i++;
                }
            }
            if (waiting.empty()) it = g_pending.erase(it);
            else ++it;
        }
    }
    // The log: once ten seconds into a map with meshes, then every two minutes (never in menus).
    if (!g_casters.empty() && (g_firstLog ? now - g_lastLog > 120000 : g_stats.frames >= 600)) {
        g_firstLog = true;
        g_lastLog = now;
        logStats("map so far");
    }
    if (g_casters.empty() || !ensureResources(device)) return false;

    // The light's frame: f along the light (away from it), u across (horizontal), v up.
    float f[3] = {-sunWorld[0], -sunWorld[1], -sunWorld[2]};
    normalize(f);
    if (f[1] > -0.02f) return false; // light at or below the horizon: no map
    if (!g_lightSet || dot3(f, g_f) < 0.999999f) {
        const float up[3] = {0.0f, 1.0f, 0.0f};
        float u[3] = {up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0]};
        if (dot3(u, u) < 1e-6f) {
            u[0] = 1.0f;
            u[1] = u[2] = 0.0f;
        }
        normalize(u);
        float v[3] = {f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
        normalize(v);
        memcpy(g_f, f, sizeof(f));
        memcpy(g_u, u, sizeof(u));
        memcpy(g_v, v, sizeof(v));
        g_lightSet = true;
        for (Caster& c : g_casters) casterBox(c);
        g_far.active = g_far.complete = false;
        g_near[0].active = g_near[0].complete = false;
        g_near[1].active = g_near[1].complete = false;
        g_nearFront = g_nearBuild = -1;
    }
    fitFar();
    // The near map follows the camera: the next one is drawn in the background (the current
    // one stays in use) once the camera is far enough from the current one's centre.
    if (g_nearBuild < 0) {
        bool want = g_nearFront < 0;
        if (!want) {
            const float* c = g_near[g_nearFront].centre;
            const float dx = camera[0] - c[0], dz = camera[2] - c[2];
            want = dx * dx + dz * dz > kNearMove * kNearMove || fabsf(camera[1] - c[1]) > kNearMove * 0.5f;
        }
        if (want) {
            g_nearBuild = g_nearFront < 0 ? 0 : 1 - g_nearFront;
            fitNear(g_near[g_nearBuild], camera);
            g_stats.nearBuilds++;
        }
    }

    const UINT total = static_cast<UINT>(g_casters.size());
    const bool work = (g_far.cursor < total || !g_far.cleared) || g_nearBuild >= 0 ||
                      (g_nearFront >= 0 && g_near[g_nearFront].cursor < total);
    if (work) {
        hook::InternalScope internal;
        // State the game and the pipeline may rely on: given back afterwards (the pipeline's
        // state block gives the rest back to the game).
        IDirect3DSurface9* savedTarget = nullptr;
        IDirect3DSurface9* savedDepth = nullptr;
        device->GetRenderTarget(0, &savedTarget);
        device->GetDepthStencilSurface(&savedDepth);
        D3DVIEWPORT9 savedViewport{};
        device->GetViewport(&savedViewport);
        IDirect3DVertexBuffer9* savedVB = nullptr;
        UINT savedOffset = 0, savedStride = 0;
        device->GetStreamSource(0, &savedVB, &savedOffset, &savedStride);
        IDirect3DIndexBuffer9* savedIB = nullptr;
        device->GetIndices(&savedIB);
        IDirect3DVertexDeclaration9* savedDecl = nullptr;
        device->GetVertexDeclaration(&savedDecl);
        IDirect3DVertexShader9* savedVS = nullptr;
        device->GetVertexShader(&savedVS);
        IDirect3DPixelShader9* savedPS = nullptr;
        device->GetPixelShader(&savedPS);
        float savedConstants[4][4];
        device->GetVertexShaderConstantF(0, &savedConstants[0][0], 4);
        static const D3DRENDERSTATETYPE kStates[] = {D3DRS_ZENABLE,        D3DRS_ZWRITEENABLE,         D3DRS_ZFUNC,
                                                     D3DRS_CULLMODE,       D3DRS_ALPHABLENDENABLE,     D3DRS_ALPHATESTENABLE,
                                                     D3DRS_COLORWRITEENABLE, D3DRS_STENCILENABLE,      D3DRS_SCISSORTESTENABLE,
                                                     D3DRS_CLIPPLANEENABLE, D3DRS_FILLMODE,            D3DRS_DEPTHBIAS,
                                                     D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_FOGENABLE};
        DWORD saved[sizeof(kStates) / sizeof(kStates[0])];
        for (size_t i = 0; i < sizeof(kStates) / sizeof(kStates[0]); i++) device->GetRenderState(kStates[i], &saved[i]);

        device->SetRenderTarget(0, g_nullTarget);
        D3DVIEWPORT9 viewport = {0, 0, kMapSize, kMapSize, 0.0f, 1.0f};
        device->SetViewport(&viewport);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); // both faces: thin walls, either winding
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        device->SetRenderState(D3DRS_DEPTHBIAS, 0);
        device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetVertexShader(g_vs);
        device->SetPixelShader(g_ps);

        // What the camera sees first: new meshes into the near map in use, then the next near
        // map, then the whole-track map (all of it, a rebuild spreads over frames).
        UINT budget = kDrawBudget, checks = kCheckBudget;
        bool alphaBound = false;
        if (g_nearFront >= 0) drawInto(device, g_near[g_nearFront], budget, checks, true, alphaBound);
        if (g_nearBuild >= 0) drawInto(device, g_near[g_nearBuild], budget, checks, true, alphaBound);
        drawInto(device, g_far, budget, checks, false, alphaBound);

        restoreAfterAlpha(device); // before c0..c3 below: those are the same values
        for (size_t i = 0; i < sizeof(kStates) / sizeof(kStates[0]); i++) device->SetRenderState(kStates[i], saved[i]);
        device->SetVertexShaderConstantF(0, &savedConstants[0][0], 4);
        device->SetPixelShader(savedPS);
        device->SetVertexShader(savedVS);
        device->SetVertexDeclaration(savedDecl);
        device->SetIndices(savedIB);
        device->SetStreamSource(0, savedVB, savedOffset, savedStride);
        device->SetRenderTarget(0, savedTarget);
        device->SetDepthStencilSurface(savedDepth);
        device->SetViewport(&savedViewport);
        gfx::release(savedPS);
        gfx::release(savedVS);
        gfx::release(savedDecl);
        gfx::release(savedIB);
        gfx::release(savedVB);
        gfx::release(savedDepth);
        gfx::release(savedTarget);
    }
    if (g_nearBuild >= 0 && g_near[g_nearBuild].complete) {
        g_nearFront = g_nearBuild;
        g_nearBuild = -1;
    }
    if (g_nearFront >= 0 && g_near[g_nearFront].complete) mapInfo(g_near[g_nearFront], out.nearMap);
    if (g_far.complete) mapInfo(g_far, out.farMap);
    QueryPerformanceCounter(&t1);
    g_stats.updateMs += milliseconds(t0, t1);
    g_cpuTotal += milliseconds(t0, t1);
    return out.nearMap.texture || out.farMap.texture;
}

void reset(const char* why, bool log) {
    if (log && !g_casters.empty()) {
        logStats(why);
        TMVS_LOG("casters: %u meshes let go (%s)", static_cast<unsigned>(g_casters.size()), why);
    }
    forgetMeshes();
    g_firstLog = false;
    g_lastLog = GetTickCount();
    g_farBuilds = 0;
    g_stats = Stats();
}

double cpuMs() {
    return g_cpuTotal;
}

void frameDone() {
    g_stride = (g_frameDraws + kChecksPerFrame - 1) / kChecksPerFrame;
    g_stride = g_stride < 1 ? 1 : (g_stride > kMaxStride ? kMaxStride : g_stride);
    g_phase = (g_phase + 1) % g_stride;
    g_frameDraws = 0;
}

void pauseRecording(bool paused) {
    g_paused = paused;
}

void runtimeShaders(std::vector<gfx::ShaderSource>& out) {
    out.push_back({kCasterVS, "main", "vs_3_0", "CasterVS"});
    out.push_back({kCasterPS, "main", "ps_3_0", "CasterPS"});
}

unsigned keptMeshes() {
    return static_cast<unsigned>(g_casters.size());
}

unsigned keptAlphaMeshes() {
    return static_cast<unsigned>(g_alpha.size());
}

void releaseDevice() {
    releaseMap(g_far);
    releaseMap(g_near[0]);
    releaseMap(g_near[1]);
    gfx::release(g_nullTarget);
    gfx::release(g_vs);
    gfx::release(g_ps);
    for (auto& d : g_ownDecls) gfx::release(d.second);
    g_ownDecls.clear();
    // The kept meshes point at those declarations, and the maps are gone.
    forgetMeshes();
    g_unsupported = false;
}

} // namespace casters
} // namespace tmshaders
