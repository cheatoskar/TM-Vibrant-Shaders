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

// One map over the whole track. 4096^2 over a Stadium map (~1.3 km with its margins) is about
// 0.3 m a texel across the light and 0.1 m along it (a low light stretches that over the
// ground by 1 / sin(elevation)). INTZ: 64 MB of video memory, no colour target (NULL format).
constexpr UINT kMapSize = 4096;
constexpr size_t kMaxCasters = 40000;  // a Stadium map shows a few thousand
constexpr size_t kMaxPending = 60000;
constexpr UINT kDrawBudget = 1500;     // meshes drawn into the map per frame (a rebuild spreads)
constexpr DWORD kStaticAfterMs = 500;  // seen again this much later at the same place: static
constexpr float kMarginH = 128.0f;     // meshes reach this far from their origin (m)
constexpr float kMarginDepth = 300.0f; // along the light (m)

struct Caster {
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT offset = 0, stride = 0;
    IDirect3DIndexBuffer9* ib = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr; // ours: the position alone (not owned here)
    D3DPRIMITIVETYPE type = D3DPT_TRIANGLELIST;
    INT base = 0;
    UINT minIndex = 0, vertices = 0, start = 0, count = 0;
    float world[4][3] = {}; // mesh -> world, row vectors (rows 0..2 the axes, row 3 the place)
};

struct Pending {
    DWORD firstSeen = 0;
    DWORD lastSeen = 0;
};

// The game's vertex declarations: the stream and our declaration with its position alone.
struct DeclInfo {
    IDirect3DVertexDeclaration9* own = nullptr; // null: no usable position
    WORD stream = 0;
};

struct Stats {
    unsigned draws = 0, noShader = 0, noCamera = 0, notAffine = 0, noDecl = 0, stream = 0, pool = 0, pending = 0, known = 0, promoted = 0;
    unsigned frames = 0;
};

std::vector<Caster> g_casters;
std::unordered_set<uint64_t> g_known;
std::unordered_map<uint64_t, Pending> g_pending;
std::unordered_map<const void*, int> g_vsRegister;           // the game's vertex shader -> GbxVisualPrCamera's register, -1 none
std::unordered_map<const void*, DeclInfo> g_decls;           // the game's declaration -> ours
std::map<std::pair<WORD, BYTE>, IDirect3DVertexDeclaration9*> g_ownDecls; // (offset, type) -> position-only declaration
Stats g_stats;
DWORD g_lastSweep = 0;
DWORD g_lastLog = 0;
int g_logs = 0;

// Inverse of the camera's view-projection, for the frame's view and projection.
float g_ivView[16] = {}, g_ivProj[16] = {};
double g_invVP[4][4] = {};
bool g_invValid = false;

// Translations of the static meshes (world): what the map must cover.
float g_placeMin[3] = {1e30f, 1e30f, 1e30f}, g_placeMax[3] = {-1e30f, -1e30f, -1e30f};

// The map.
IDirect3DTexture9* g_depth = nullptr;
IDirect3DSurface9* g_depthSurface = nullptr;
IDirect3DSurface9* g_nullTarget = nullptr; // NULL format colour target (no memory), else R32F
IDirect3DVertexShader9* g_vs = nullptr;
IDirect3DPixelShader9* g_ps = nullptr;
bool g_unsupported = false;
struct Region {
    float a0 = 0, sa = 1, b0 = 0, sb = 1, d0 = 0, sd = 1; // light space: across (U), up (V), along (F)
    float light[3] = {0, 0, 0};
    float u[3] = {}, v[3] = {}, f[3] = {};
    UINT cursor = 0;       // casters before this one are in the map
    bool complete = false; // drawn through once: the shaders may use it
    bool cleared = false;
    bool valid = false;
} g_map;
unsigned g_rebuilds = 0;

const char* kCasterVS = R"(
float4x4 u_MeshToMap : register(c0); // mesh -> the map's clip space (row vectors, columns in c0..c3)
float4 main(float4 pos : POSITION) : POSITION {
    return mul(pos, u_MeshToMap);
}
)";

const char* kCasterPS = R"(
float4 main() : COLOR0 { return 0; }
)";

uint64_t fnv(uint64_t h, const void* data, size_t bytes) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < bytes; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// GbxVisualPrCamera's first register, from the constant table in the shader's bytecode
// (shaders the game compiled from HLSL keep it). -1: the shader has none (assembled ones,
// the sky, 2D). c0 in every material shader seen so far (62 of 74 dumped vertex shaders).
int cameraRegister(IDirect3DVertexShader9* vs) {
    auto it = g_vsRegister.find(vs);
    if (it != g_vsRegister.end()) return it->second;
    int result = -1;
    UINT size = 0;
    if (SUCCEEDED(vs->GetFunction(nullptr, &size)) && size >= 8 && size < (1u << 20)) {
        std::vector<DWORD> code((size + 3) / 4);
        if (SUCCEEDED(vs->GetFunction(code.data(), &size))) {
            const size_t n = size / 4;
            // Comment tokens: low word 0xFFFE, length in DWORDs in bits 16..30. The table
            // starts with "CTAB"; its offsets count from just after that.
            for (size_t i = 1; i < n;) {
                const DWORD token = code[i];
                if ((token & 0xFFFF) != 0xFFFE) break; // the table comes before the first instruction
                const size_t length = (token >> 16) & 0x7FFF;
                if (i + 1 + length > n) break;
                if (length >= 8 && code[i + 1] == MAKEFOURCC('C', 'T', 'A', 'B')) {
                    const unsigned char* table = reinterpret_cast<const unsigned char*>(&code[i + 2]);
                    const size_t bytes = (length - 1) * 4;
                    auto u32 = [&](size_t at) -> DWORD { return at + 4 <= bytes ? *reinterpret_cast<const DWORD*>(table + at) : 0; };
                    auto u16 = [&](size_t at) -> WORD { return at + 2 <= bytes ? *reinterpret_cast<const WORD*>(table + at) : 0; };
                    const DWORD constants = u32(12), info = u32(16);
                    for (DWORD c = 0; c < constants && c < 512; c++) {
                        const size_t at = info + c * 20;
                        if (at + 20 > bytes) break;
                        const DWORD name = u32(at);
                        const WORD set = u16(at + 4), reg = u16(at + 6), count = u16(at + 8);
                        static const char kName[] = "GbxVisualPrCamera";
                        if (name + sizeof(kName) <= bytes && !memcmp(table + name, kName, sizeof(kName)) && set == 2 /* D3DXRS_FLOAT4 */ &&
                            count == 4) {
                            result = reg;
                        }
                    }
                }
                i += 1 + length;
            }
        }
    }
    g_vsRegister[vs] = result;
    return result;
}

DeclInfo declInfo(IDirect3DDevice9* device, IDirect3DVertexDeclaration9* decl) {
    auto it = g_decls.find(decl);
    if (it != g_decls.end()) return it->second;
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
            break;
        }
    }
    g_decls[decl] = info;
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

bool inverseViewProjection(const float view[16], const float projection[16]) {
    if (g_invValid && !memcmp(view, g_ivView, sizeof(g_ivView)) && !memcmp(projection, g_ivProj, sizeof(g_ivProj))) return true;
    double vp[4][4];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            double sum = 0.0;
            for (int k = 0; k < 4; k++) sum += static_cast<double>(view[r * 4 + k]) * projection[k * 4 + c];
            vp[r][c] = sum;
        }
    }
    memcpy(g_ivView, view, sizeof(g_ivView));
    memcpy(g_ivProj, projection, sizeof(g_ivProj));
    g_invValid = invert(vp, g_invVP);
    return g_invValid;
}

bool managedStatic(IDirect3DVertexBuffer9* vb) {
    D3DVERTEXBUFFER_DESC d{};
    return SUCCEEDED(vb->GetDesc(&d)) && d.Pool == D3DPOOL_MANAGED && !(d.Usage & D3DUSAGE_DYNAMIC);
}

bool managedStatic(IDirect3DIndexBuffer9* ib) {
    D3DINDEXBUFFER_DESC d{};
    return SUCCEEDED(ib->GetDesc(&d)) && d.Pool == D3DPOOL_MANAGED && !(d.Usage & D3DUSAGE_DYNAMIC);
}

void forgetMeshes() {
    for (Caster& c : g_casters) {
        gfx::release(c.vb);
        gfx::release(c.ib);
    }
    g_casters.clear();
    g_known.clear();
    g_pending.clear();
    g_vsRegister.clear();
    g_decls.clear();
    g_invValid = false;
    for (int j = 0; j < 3; j++) {
        g_placeMin[j] = 1e30f;
        g_placeMax[j] = -1e30f;
    }
    g_map.valid = false;
    g_map.complete = false;
    g_map.cursor = 0;
}

void logStats(const char* when) {
    const Stats& s = g_stats;
    const double f = s.frames ? static_cast<double>(s.frames) : 1.0;
    TMVS_LOG("casters (%s): per frame %.0f opaque draws: %.0f without GbxVisualPrCamera, %.0f not mesh -> world, %.0f no float "
             "position, %.0f instanced/no stream, %.0f not managed, %.0f waiting, %.0f known; %u meshes kept (%u new), map %s, %u rebuilds, "
             "%u frames",
             when, s.draws / f, s.noCamera / f, s.notAffine / f, s.noDecl / f, s.stream / f, s.pool / f, s.pending / f, s.known / f,
             static_cast<unsigned>(g_casters.size()), s.promoted, g_map.complete ? "complete" : (g_map.valid ? "building" : "none"),
             g_rebuilds, s.frames);
    g_stats = Stats();
}

bool ensureResources(IDirect3DDevice9* device) {
    if (g_unsupported) return false;
    if (g_depth && g_nullTarget && g_vs && g_ps) return true;
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
    bool ok = true;
    if (!g_depth) {
        ok = SUCCEEDED(device->CreateTexture(kMapSize, kMapSize, 1, D3DUSAGE_DEPTHSTENCIL, intz, D3DPOOL_DEFAULT, &g_depth, nullptr)) &&
             SUCCEEDED(g_depth->GetSurfaceLevel(0, &g_depthSurface));
    }
    if (ok && !g_nullTarget) {
        // A colour target must be bound; the NULL format takes no memory.
        ok = SUCCEEDED(device->CreateRenderTarget(kMapSize, kMapSize, haveNull ? nullFormat : D3DFMT_R32F, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                  &g_nullTarget, nullptr));
    }
    if (ok && !g_vs) g_vs = gfx::compileVertexShader(device, kCasterVS, "main", "CasterVS");
    if (ok && !g_ps) g_ps = gfx::compilePixelShader(device, kCasterPS, "main", "CasterPS");
    if (!ok || !g_vs || !g_ps) {
        TMVS_LOG("casters: map resources failed (%ux%u INTZ%s) - CasterShadows unavailable", kMapSize, kMapSize,
                 haveNull ? ", NULL target" : ", R32F target");
        releaseDevice();
        g_unsupported = true;
        return false;
    }
    TMVS_LOG("casters: map %ux%u INTZ%s", kMapSize, kMapSize, haveNull ? " (NULL colour target)" : " (R32F colour target)");
    hook::logMemory("after the caster map");
    return true;
}

void normalize(float v[3]) {
    const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-8f) {
        for (int j = 0; j < 3; j++) v[j] /= l;
    }
}

float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// The light-space box (across, up, along the light) that holds every kept mesh.
void wantedBox(const float u[3], const float v[3], const float f[3], float lo[3], float hi[3]) {
    for (int j = 0; j < 3; j++) {
        lo[j] = 1e30f;
        hi[j] = -1e30f;
    }
    for (int corner = 0; corner < 8; corner++) {
        const float p[3] = {(corner & 1) ? g_placeMax[0] + kMarginH : g_placeMin[0] - kMarginH,
                            (corner & 2) ? g_placeMax[1] + kMarginH : g_placeMin[1] - kMarginH,
                            (corner & 4) ? g_placeMax[2] + kMarginH : g_placeMin[2] - kMarginH};
        const float l[3] = {dot3(p, u), dot3(p, v), dot3(p, f)};
        for (int j = 0; j < 3; j++) {
            lo[j] = fminf(lo[j], l[j]);
            hi[j] = fmaxf(hi[j], l[j]);
        }
    }
    lo[2] -= kMarginDepth;
    hi[2] += kMarginDepth;
}

} // namespace

void recordDraw(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start, UINT count,
                const float view[16], const float projection[16]) {
    g_stats.draws++;
    if (g_casters.size() >= kMaxCasters || count == 0) return;
    IDirect3DVertexShader9* vs = nullptr;
    device->GetVertexShader(&vs);
    if (!vs) {
        g_stats.noShader++;
        return;
    }
    const int reg = cameraRegister(vs);
    vs->Release(); // kept as a key only (the game holds it)
    if (reg < 0) {
        g_stats.noCamera++;
        return;
    }
    float c[4][4];
    if (FAILED(device->GetVertexShaderConstantF(reg, &c[0][0], 4)) || !inverseViewProjection(view, projection)) {
        g_stats.noCamera++;
        return;
    }
    // c[j] is column j of mesh -> clip (row vectors: clip.x = dot(p, c0)). Mesh -> world is
    // that times (view * projection)^-1; for a mesh drawn with this camera it is affine.
    double w[4][4];
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            double sum = 0.0;
            for (int k = 0; k < 4; k++) sum += static_cast<double>(c[k][r]) * g_invVP[k][col];
            w[r][col] = sum;
        }
    }
    const double reach = 1.0 + fabs(w[3][0]) + fabs(w[3][1]) + fabs(w[3][2]);
    if (fabs(w[0][3]) > 1e-3 || fabs(w[1][3]) > 1e-3 || fabs(w[2][3]) > 1e-3 || fabs(w[3][3] - 1.0) > 1e-5 * reach + 1e-3) {
        g_stats.notAffine++;
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
    if (!ib || !managedStatic(vb) || !managedStatic(ib)) {
        g_stats.pool++;
        gfx::release(vb);
        gfx::release(ib);
        return;
    }
    // The key: the mesh and its place (rounded: the inverse carries float noise of the
    // camera's matrices, well below a millimetre).
    uint64_t h = 14695981039346656037ull;
    const void* ids[2] = {vb, ib};
    const UINT draw[8] = {offset, stride, static_cast<UINT>(type), static_cast<UINT>(base), minIndex, vertices, start, count};
    h = fnv(h, ids, sizeof(ids));
    h = fnv(h, draw, sizeof(draw));
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 3; col++) {
            const long long q = static_cast<long long>(floor(w[r][col] * (r == 3 ? 50.0 : 2000.0) + 0.5));
            h = fnv(h, &q, sizeof(q));
        }
    }
    if (g_known.count(h)) {
        g_stats.known++;
        gfx::release(vb);
        gfx::release(ib);
        return;
    }
    const DWORD now = GetTickCount();
    Pending& p = g_pending[h];
    if (!p.firstSeen) p.firstSeen = now;
    p.lastSeen = now;
    if (now - p.firstSeen < kStaticAfterMs) {
        g_stats.pending++;
        gfx::release(vb);
        gfx::release(ib);
        return;
    }
    // Static: kept, with the game's buffers (references held until reset()).
    g_pending.erase(h);
    g_known.insert(h);
    Caster m;
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
    for (int j = 0; j < 3; j++) {
        g_placeMin[j] = fminf(g_placeMin[j], m.world[3][j]);
        g_placeMax[j] = fmaxf(g_placeMax[j], m.world[3][j]);
    }
    g_casters.push_back(m);
    g_stats.promoted++;
}

bool update(IDirect3DDevice9* device, const float sunWorld[3], MapInfo& out) {
    g_stats.frames++;
    const DWORD now = GetTickCount();
    // Things that moved never come back to the same place: forget them after a while.
    if (now - g_lastSweep > 5000 || g_pending.size() > kMaxPending) {
        g_lastSweep = now;
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            if (now - it->second.lastSeen > 3000 || g_pending.size() > kMaxPending) it = g_pending.erase(it);
            else ++it;
        }
    }
    if ((g_logs < 3 && g_stats.frames >= 600) || now - g_lastLog > 120000) {
        g_lastLog = now;
        g_logs++;
        logStats("map so far");
    }
    if (g_casters.empty() || !ensureResources(device)) return false;

    // The light's frame: f along the light (away from it), u across (horizontal), v up.
    float f[3] = {-sunWorld[0], -sunWorld[1], -sunWorld[2]};
    normalize(f);
    if (f[1] > -0.02f) return false; // light at or below the horizon: no map
    const float up[3] = {0.0f, 1.0f, 0.0f};
    float u[3] = {up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0]};
    if (dot3(u, u) < 1e-6f) {
        u[0] = 1.0f;
        u[1] = u[2] = 0.0f;
    }
    normalize(u);
    float v[3] = {f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
    normalize(v);

    float lo[3], hi[3];
    wantedBox(u, v, f, lo, hi);
    const bool turned = !g_map.valid || dot3(f, g_map.light) < 0.999999f;
    const bool outside = lo[0] < g_map.a0 || hi[0] > g_map.a0 + g_map.sa || lo[1] < g_map.b0 || hi[1] > g_map.b0 + g_map.sb ||
                         lo[2] < g_map.d0 || hi[2] > g_map.d0 + g_map.sd;
    if (turned || outside) {
        // A new map: a fifth more room on every side, so a growing track doesn't start it over
        // with every new block.
        float grow[3];
        for (int j = 0; j < 3; j++) grow[j] = (hi[j] - lo[j]) * 0.2f;
        g_map.a0 = lo[0] - grow[0];
        g_map.sa = hi[0] - lo[0] + 2.0f * grow[0];
        g_map.b0 = lo[1] - grow[1];
        g_map.sb = hi[1] - lo[1] + 2.0f * grow[1];
        g_map.d0 = lo[2] - grow[2];
        g_map.sd = hi[2] - lo[2] + 2.0f * grow[2];
        memcpy(g_map.light, f, sizeof(f));
        memcpy(g_map.u, u, sizeof(u));
        memcpy(g_map.v, v, sizeof(v));
        memcpy(g_map.f, f, sizeof(f));
        g_map.cursor = 0;
        g_map.complete = false;
        g_map.cleared = false;
        g_map.valid = true;
        g_rebuilds++;
        if (g_rebuilds <= 20) {
            TMVS_LOG("casters: map over %.0f x %.0f m (%.2f x %.2f m a texel), %.0f m deep, %u meshes", g_map.sa, g_map.sb,
                     g_map.sa / kMapSize, g_map.sb / kMapSize, g_map.sd, static_cast<unsigned>(g_casters.size()));
        }
    }

    const UINT total = static_cast<UINT>(g_casters.size());
    if (g_map.cursor < total || !g_map.cleared) {
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
        device->SetDepthStencilSurface(g_depthSurface);
        D3DVIEWPORT9 viewport = {0, 0, kMapSize, kMapSize, 0.0f, 1.0f};
        device->SetViewport(&viewport);
        if (!g_map.cleared) {
            device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
            g_map.cleared = true;
        }
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

        // World -> the map's clip space (row vectors): x across, y up, z along the light.
        double lm[4][4] = {};
        for (int r = 0; r < 3; r++) {
            lm[r][0] = 2.0 * g_map.u[r] / g_map.sa;
            lm[r][1] = 2.0 * g_map.v[r] / g_map.sb;
            lm[r][2] = g_map.f[r] / g_map.sd;
        }
        lm[3][0] = -2.0 * g_map.a0 / g_map.sa - 1.0;
        lm[3][1] = -2.0 * g_map.b0 / g_map.sb - 1.0;
        lm[3][2] = -g_map.d0 / g_map.sd;
        lm[3][3] = 1.0;

        const UINT end = g_map.cursor + kDrawBudget < total ? g_map.cursor + kDrawBudget : total;
        IDirect3DVertexDeclaration9* lastDecl = nullptr;
        for (UINT i = g_map.cursor; i < end; i++) {
            const Caster& m = g_casters[i];
            float columns[4][4];
            for (int col = 0; col < 4; col++) {
                for (int r = 0; r < 4; r++) {
                    double sum = r == 3 ? lm[3][col] : 0.0;
                    for (int k = 0; k < 3; k++) sum += static_cast<double>(m.world[r][k]) * lm[k][col];
                    columns[col][r] = static_cast<float>(sum);
                }
            }
            if (m.decl != lastDecl) {
                device->SetVertexDeclaration(m.decl);
                lastDecl = m.decl;
            }
            device->SetStreamSource(0, m.vb, m.offset, m.stride);
            device->SetIndices(m.ib);
            device->SetVertexShaderConstantF(0, &columns[0][0], 4);
            device->DrawIndexedPrimitive(m.type, m.base, m.minIndex, m.vertices, m.start, m.count);
        }
        g_map.cursor = end;
        if (end >= total) g_map.complete = true;

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
    if (!g_map.complete) return false;

    // World -> the map's texture coordinates. D3D9 rasterizes texel i at screen x = i, which
    // the texture holds at (i + 0.5) / size: half a texel on top of the projection.
    const float half = 0.5f / kMapSize;
    for (int j = 0; j < 3; j++) {
        out.rows[0][j] = g_map.u[j] / g_map.sa;
        out.rows[1][j] = -g_map.v[j] / g_map.sb;
        out.rows[2][j] = g_map.f[j] / g_map.sd;
    }
    out.rows[0][3] = -g_map.a0 / g_map.sa + half;
    out.rows[1][3] = 1.0f + g_map.b0 / g_map.sb + half;
    out.rows[2][3] = -g_map.d0 / g_map.sd;
    out.texel[0] = out.texel[1] = 1.0f / kMapSize;
    // A receiver must lie this far behind the stored depth. One texel along the light covers
    // texel / sin(elevation) of flat ground, whose depth changes by cos(elevation) per metre:
    // the four taps of the filter see up to that much, plus a constant for the depth's noise.
    const float sinEl = fmaxf(-f[1], 0.05f);
    const float cosEl = sqrtf(fmaxf(1.0f - sinEl * sinEl, 0.0f));
    const float ground = (g_map.sb / kMapSize) / sinEl;
    out.bias = (0.15f + 1.5f * ground * cosEl) / g_map.sd;
    out.texture = g_depth;
    return true;
}

void reset(const char* why) {
    if (!g_casters.empty() || !g_pending.empty()) {
        logStats(why);
        TMVS_LOG("casters: %u meshes let go (%s)", static_cast<unsigned>(g_casters.size()), why);
    }
    forgetMeshes();
    g_logs = 0;
    g_rebuilds = 0;
    g_stats = Stats();
}

void releaseDevice() {
    gfx::release(g_depthSurface);
    gfx::release(g_depth);
    gfx::release(g_nullTarget);
    gfx::release(g_vs);
    gfx::release(g_ps);
    for (auto& d : g_ownDecls) gfx::release(d.second);
    g_ownDecls.clear();
    // The kept meshes point at those declarations, and the map is gone.
    forgetMeshes();
    g_unsupported = false;
}

} // namespace casters
} // namespace tmshaders
