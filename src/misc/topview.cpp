#include "misc/internal.h"
#include "blockorder.h"
#include "casters.h"
#include "engine.h"
#include "gfx.h"
#include "log.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The minimap's picture from above ("Real look"): the meshes the game draws for the map's blocks,
// drawn again once each into a texture seen straight from above, with their own texture and a
// plain light from above. Every block gets the same treatment: nothing is told apart by what it
// does (no waypoint field is read; a checkpoint gate is its geometry like in the editor).
//
// Which draws: indexed draws of the main camera that blockorder.cpp maps to a block of the open map
// (corpus -> item -> block), plus the grass material's (the ground), opaque (depth written, no
// blending, no alpha test). Each distinct one (item, buffers, index range) once per map. Where:
// the world matrix from the game's own constants (casters::drawWorld, the same recovery the long
// shadows use). With what: the game's vertex and index buffers, its declaration's POSITION and
// TEXCOORD0, the texture in sampler 0. The draws are queued with references to all of that and
// drawn at Present (before the overlay), at most kPerFrame a frame, inside a saved and restored
// device state: never inside one of the game's passes.
//
// Blocks the game hasn't drawn yet stay the plain squares of mapview.cpp under the picture; they
// fill in as the camera sees them.
namespace tmshaders {
namespace misc {
namespace topview {
namespace {

constexpr int kPixelsPerCell = 32;      // 1 px per metre: the auto zoom shows 150 m across
constexpr int kMaxSize = 4096;
constexpr size_t kMaxQueue = 4096;      // draws waiting (each holds the game's buffers and texture)
constexpr UINT kPerFrame = 400;         // drawn into the picture per frame
constexpr size_t kMaxKeys = 200000;
constexpr float kLowest = -64.0f;       // m: the depth range of the picture (higher covers lower)
constexpr float kHighest = 2048.0f;
constexpr int kQuietFrames = 30;        // frames an item's draws brought nothing new: it is done

const char* kTopVS = R"(
float4x4 u_MeshToMap : register(c0);   // mesh -> the picture's clip space (row vectors, columns)
float4x4 u_MeshToWorld : register(c4);
struct Out { float4 pos : POSITION; float2 uv : TEXCOORD0; float3 world : TEXCOORD1; };
Out main(float4 pos : POSITION, float2 uv : TEXCOORD0) {
    Out o;
    o.pos = mul(pos, u_MeshToMap);
    o.uv = uv;
    o.world = mul(pos, u_MeshToWorld).xyz;
    return o;
}
)";

const char* kTopPS = R"(
sampler2D s_Texture : register(s0);
float4 u_Params : register(c0); // x: a texture is bound, y: lowest height (m), z: 1 / height range
float4 main(float2 uv : TEXCOORD0, float3 world : TEXCOORD1) : COLOR0 {
    float3 base = u_Params.x > 0.5 ? tex2D(s_Texture, uv).rgb : float3(0.55, 0.55, 0.58);
    float3 n = normalize(cross(ddx(world), ddy(world)));
    if (n.y < 0.0) n = -n;
    float light = 0.62 + 0.38 * saturate(dot(n, normalize(float3(0.35, 1.0, 0.25))));
    float height = saturate((world.y - u_Params.y) * u_Params.z);
    return float4(base * light * (0.85 + 0.3 * height), 1.0);
}
)";

// Our declaration for one of the game's: its POSITION and TEXCOORD0, in up to two of its streams
// (ours: 0 and 1).
struct DeclInfo {
    IDirect3DVertexDeclaration9* own = nullptr; // null: no usable position
    WORD streams[2] = {0, 0};
    int streamCount = 0;
    bool texcoord = false;
};

struct Job {
    IDirect3DVertexDeclaration9* decl = nullptr; // ours (kept in g_ownDecls)
    IDirect3DVertexBuffer9* vb[2] = {};
    UINT offset[2] = {}, stride[2] = {};
    int streams = 0;
    IDirect3DIndexBuffer9* ib = nullptr;
    IDirect3DBaseTexture9* texture = nullptr;
    bool texcoord = false;
    D3DPRIMITIVETYPE type = D3DPT_TRIANGLELIST;
    INT base = 0;
    UINT minIndex = 0, vertices = 0, start = 0, count = 0;
    float world[4][3] = {};
};

struct ItemState {
    unsigned frame = 0;
    bool fresh = false; // a new draw of it this frame
    int quiet = 0;
};

struct State {
    bool wanted = false;
    const void* map = nullptr;
    bool region = false;
    float x0 = 0, z0 = 0, x1 = 1, z1 = 1;
    int width = 0, height = 0;
    IDirect3DTexture9* texture = nullptr;
    IDirect3DSurface9* surface = nullptr;
    IDirect3DSurface9* depth = nullptr;
    bool cleared = false;
    IDirect3DStateBlock9* saved = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    bool failed = false; // resources could not be made: the squares stay
    std::vector<Job> queue;
    std::unordered_set<uint64_t> keys;
    std::unordered_map<uintptr_t, ItemState> items;
    std::unordered_set<uintptr_t> doneItems;
    unsigned frame = 1; // render() counts them (0: an item not seen yet)
    int drawn = 0;
    // The height shading's base: the lowest mesh seen before the first drawing (the ground).
    float lowestSeen = 1e9f, ground = 0.0f;
    bool groundFixed = false;
    // The log: once per map, after the queue was empty for a while.
    double worstMs = 0.0, totalMs = 0.0;
    double lookMs = 0.0; // onDraw's time on the draws it looked at (past the quick checks)
    unsigned looked = 0;
    int framesDrawn = 0;
    DWORD lastWork = 0;
    bool logged = false;
};
State g;
std::map<std::tuple<WORD, BYTE, WORD, BYTE, bool, bool>, IDirect3DVertexDeclaration9*> g_ownDecls;
std::unordered_map<IDirect3DVertexDeclaration9*, DeclInfo> g_decls; // the game's (held) -> ours

uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

void releaseJob(Job& j) {
    gfx::release(j.vb[0]);
    gfx::release(j.vb[1]);
    gfx::release(j.ib);
    gfx::release(j.texture);
}

void clearQueue() {
    for (Job& j : g.queue) releaseJob(j);
    g.queue.clear();
}

void forgetDecls() {
    for (auto& e : g_decls) e.first->Release();
    g_decls.clear();
}

DeclInfo declInfo(IDirect3DDevice9* device, IDirect3DVertexDeclaration9* decl) {
    auto it = g_decls.find(decl);
    if (it != g_decls.end()) return it->second;
    DeclInfo info;
    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1];
    UINT count = MAXD3DDECLLENGTH + 1;
    const D3DVERTEXELEMENT9* position = nullptr;
    const D3DVERTEXELEMENT9* texcoord = nullptr;
    if (SUCCEEDED(decl->GetDeclaration(elements, &count))) {
        for (UINT i = 0; i < count && elements[i].Stream != 0xFF; i++) {
            const D3DVERTEXELEMENT9& e = elements[i];
            if (e.Method != D3DDECLMETHOD_DEFAULT || e.UsageIndex != 0) continue;
            if (e.Usage == D3DDECLUSAGE_POSITION && !position && (e.Type == D3DDECLTYPE_FLOAT3 || e.Type == D3DDECLTYPE_FLOAT4)) position = &e;
            if (e.Usage == D3DDECLUSAGE_TEXCOORD && !texcoord && e.Type != D3DDECLTYPE_UNUSED) texcoord = &e;
        }
    }
    if (position) {
        const bool twoStreams = texcoord && texcoord->Stream != position->Stream;
        info.streams[0] = position->Stream;
        info.streamCount = 1;
        if (twoStreams) {
            info.streams[1] = texcoord->Stream;
            info.streamCount = 2;
        }
        info.texcoord = texcoord != nullptr;
        const auto key = std::make_tuple(position->Offset, position->Type, texcoord ? texcoord->Offset : WORD(0),
                                         texcoord ? texcoord->Type : BYTE(0), info.texcoord, twoStreams);
        IDirect3DVertexDeclaration9*& own = g_ownDecls[key];
        if (!own) {
            D3DVERTEXELEMENT9 mine[3] = {{0, position->Offset, position->Type, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END(), D3DDECL_END()};
            if (texcoord) {
                mine[1] = {static_cast<WORD>(twoStreams ? 1 : 0), texcoord->Offset, texcoord->Type, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0};
            }
            device->CreateVertexDeclaration(mine, &own);
        }
        info.own = own;
    }
    if (g_decls.size() >= 4096) forgetDecls();
    decl->AddRef();
    g_decls[decl] = info;
    return info;
}

// A texture the picture may use: a plain 2D one, not a render target or depth texture (the game's
// shadow buffer, reflections: their content changes every frame).
bool usableTexture(IDirect3DBaseTexture9* t) {
    if (!t || t->GetType() != D3DRTYPE_TEXTURE) return false;
    D3DSURFACE_DESC d{};
    if (FAILED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d))) return false;
    return !(d.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL));
}

void releaseTargets() {
    gfx::release(g.surface);
    gfx::release(g.texture);
    gfx::release(g.depth);
    gfx::release(g.saved);
    g.cleared = false;
}

// Everything drawn is gone (a new map, a device reset, the setting off): the draws come again
// as the game draws them.
void forget() {
    clearQueue();
    g.keys.clear();
    g.items.clear();
    g.doneItems.clear();
    g.cleared = false;
    g.drawn = 0;
    g.lowestSeen = 1e9f;
    g.groundFixed = false;
    g.worstMs = g.totalMs = g.lookMs = 0.0;
    g.looked = 0;
    g.framesDrawn = 0;
    g.logged = false;
}

bool ensureResources(IDirect3DDevice9* device) {
    if (g.failed) return false;
    if (!g.vs) g.vs = gfx::compileVertexShader(device, kTopVS, "main", "MinimapTopVS");
    if (!g.ps) g.ps = gfx::compilePixelShader(device, kTopPS, "main", "MinimapTopPS");
    if (!g.texture) {
        // With mipmaps made by the card: the whole map in a small window doesn't shimmer.
        if (FAILED(device->CreateTexture(g.width, g.height, 0, D3DUSAGE_RENDERTARGET | D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                         &g.texture, nullptr))) {
            g.texture = nullptr;
            device->CreateTexture(g.width, g.height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g.texture, nullptr);
        }
        if (g.texture && FAILED(g.texture->GetSurfaceLevel(0, &g.surface))) gfx::release(g.texture);
    }
    if (!g.depth && FAILED(device->CreateDepthStencilSurface(g.width, g.height, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE, &g.depth, nullptr))) {
        g.depth = nullptr;
    }
    if (!g.saved && FAILED(device->CreateStateBlock(D3DSBT_ALL, &g.saved))) g.saved = nullptr;
    if (!g.vs || !g.ps || !g.texture || !g.surface || !g.depth || !g.saved) {
        TMVS_LOG("misc: minimap: the picture from above can't be made (%d x %d; shaders %d %d, texture %d, depth %d, state %d): the squares stay",
                 g.width, g.height, g.vs != nullptr, g.ps != nullptr, g.texture != nullptr, g.depth != nullptr, g.saved != nullptr);
        releaseTargets();
        g.failed = true;
        return false;
    }
    return true;
}

double now() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

} // namespace

void setWanted(bool wanted) {
    if (g.wanted && !wanted) {
        forget();
        releaseTargets();
    }
    g.wanted = wanted;
}

void setRegion(const void* map, float x0, float z0, float x1, float z1, int cellsX, int cellsZ) {
    int width = cellsX * kPixelsPerCell, height = cellsZ * kPixelsPerCell;
    const int largest = std::max(width, height);
    if (largest > kMaxSize) {
        width = std::max(1, width * kMaxSize / largest);
        height = std::max(1, height * kMaxSize / largest);
    }
    const bool same = g.region && map == g.map && x0 == g.x0 && z0 == g.z0 && x1 == g.x1 && z1 == g.z1;
    if (same) return;
    forget();
    if (width != g.width || height != g.height) releaseTargets();
    g.map = map;
    g.region = width > 0 && height > 0;
    g.x0 = x0;
    g.z0 = z0;
    g.x1 = x1;
    g.z1 = z1;
    g.width = width;
    g.height = height;
    g.failed = false;
}

void clearRegion() {
    forget();
    g.region = false;
    g.map = nullptr;
}

void onDraw(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start, UINT count,
            const float view[16], const float projection[16]) {
    if (!g.wanted || !g.region || g.failed || count == 0 || g.queue.size() >= kMaxQueue) return;
    uintptr_t item = blockorder::currentDrawItem();
    if (item) {
        if (g.doneItems.count(item)) return; // every draw of this block is in the picture
        ItemState& s = g.items[item];
        if (s.frame != g.frame) {
            // A new frame for this block: nothing new the last time it was drawn -> one quiet frame more.
            if (s.frame != 0) s.quiet = s.fresh ? 0 : s.quiet + 1;
            s.frame = g.frame;
            s.fresh = false;
            if (s.quiet >= kQuietFrames) {
                g.doneItems.insert(item);
                g.items.erase(item);
                return;
            }
        }
    } else if (engine::currentMaterial() != engine::Material::Grass) {
        return; // not a block of the map (the car, the stadium around it, the sky) and not the ground
    }
    struct Timer {
        double from = now();
        ~Timer() {
            g.lookMs += now() - from;
            g.looked++;
        }
    } timer;
    DWORD z = FALSE, zwrite = FALSE, blend = FALSE, alphaTest = FALSE, colorWrite = 0;
    device->GetRenderState(D3DRS_ZENABLE, &z);
    device->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    device->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
    device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
    if (!z || !zwrite || blend || alphaTest || !colorWrite) return;
    IDirect3DVertexDeclaration9* decl = nullptr;
    device->GetVertexDeclaration(&decl);
    if (!decl) return;
    const DeclInfo info = declInfo(device, decl);
    decl->Release();
    if (!info.own) return;
    Job j;
    j.decl = info.own;
    j.streams = info.streamCount;
    j.texcoord = info.texcoord;
    bool ok = true;
    for (int i = 0; i < info.streamCount; i++) {
        UINT freq = 1;
        device->GetStreamSourceFreq(info.streams[i], &freq);
        device->GetStreamSource(info.streams[i], &j.vb[i], &j.offset[i], &j.stride[i]);
        if (!j.vb[i] || freq != 1 || j.stride[i] == 0) ok = false;
    }
    device->GetIndices(&j.ib);
    if (!ok || !j.ib) {
        releaseJob(j);
        return;
    }
    uint64_t key = mix(mix(mix(mix(14695981039346656037ull, item), reinterpret_cast<uintptr_t>(j.vb[0])), reinterpret_cast<uintptr_t>(j.ib)),
                       (static_cast<uint64_t>(start) << 32) | count);
    key = mix(key, (static_cast<uint64_t>(static_cast<uint32_t>(base)) << 32) | minIndex);
    if (g.keys.count(key) || g.keys.size() >= kMaxKeys) {
        releaseJob(j);
        return;
    }
    g.keys.insert(key);
    if (item) g.items[item].fresh = true;
    // Its place: from the game's constants. Not this camera's, or outside the map's area: left out.
    if (!casters::drawWorld(device, view, projection, j.world)) {
        releaseJob(j);
        return;
    }
    const float wx = j.world[3][0], wz = j.world[3][2];
    const float marginX = (g.x1 - g.x0) * 0.25f, marginZ = (g.z1 - g.z0) * 0.25f;
    if (wx < g.x0 - marginX || wx > g.x1 + marginX || wz < g.z0 - marginZ || wz > g.z1 + marginZ) {
        releaseJob(j);
        return;
    }
    device->GetTexture(0, &j.texture);
    if (j.texture && !usableTexture(j.texture)) gfx::release(j.texture);
    g.lowestSeen = std::min(g.lowestSeen, j.world[3][1]);
    j.type = type;
    j.base = base;
    j.minIndex = minIndex;
    j.vertices = vertices;
    j.start = start;
    j.count = count;
    g.queue.push_back(j);
}

void render(IDirect3DDevice9* device) {
    g.frame++;
    if (!g.wanted || !g.region || !device) return;
    if (g.queue.empty()) {
        // The log, once per map: what went into the picture and what it cost.
        if (!g.logged && g.drawn > 0 && GetTickCount() - g.lastWork > 5000) {
            g.logged = true;
            TMVS_LOG("misc: minimap: %d block meshes drawn into the map (%d x %d px) over %d frames, at most %.2f ms a frame, %.1f ms in all; "
                     "%u of the game's draws looked at for it, %.1f ms in all",
                     g.drawn, g.width, g.height, g.framesDrawn, g.worstMs, g.totalMs, g.looked, g.lookMs);
        }
        return;
    }
    if (!ensureResources(device)) {
        clearQueue();
        return;
    }
    const double t0 = now();
    if (!g.groundFixed) {
        g.groundFixed = true;
        g.ground = g.lowestSeen < 1e8f ? g.lowestSeen : 0.0f;
    }
    g.saved->Capture();
    IDirect3DSurface9* targets[4] = {};
    IDirect3DSurface9* depth = nullptr;
    for (DWORD i = 0; i < 4; i++) device->GetRenderTarget(i, &targets[i]);
    device->GetDepthStencilSurface(&depth);
    if (SUCCEEDED(device->BeginScene())) {
        device->SetRenderTarget(0, g.surface);
        for (DWORD i = 1; i < 4; i++) device->SetRenderTarget(i, nullptr);
        device->SetDepthStencilSurface(g.depth);
        const D3DVIEWPORT9 viewport = {0, 0, static_cast<DWORD>(g.width), static_cast<DWORD>(g.height), 0.0f, 1.0f};
        device->SetViewport(&viewport);
        if (!g.cleared) {
            device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
            g.cleared = true;
        }
        device->SetRenderState(D3DRS_ZENABLE, TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_DEPTHBIAS, 0);
        device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        device->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
        device->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0);
        device->SetStreamSourceFreq(0, 1);
        device->SetStreamSourceFreq(1, 1);
        device->SetVertexShader(g.vs);
        device->SetPixelShader(g.ps);
        // World -> the picture's clip space (row vectors): x to the right, z down the rows (row 0 at
        // z0, as mapview's squares), depth from the height (higher covers lower); half a pixel for
        // D3D9's pixel centres.
        double lm[4][4] = {};
        const double range = kHighest - kLowest;
        lm[0][0] = 2.0 / (g.x1 - g.x0);
        lm[3][0] = -1.0 - 2.0 * g.x0 / (g.x1 - g.x0) - 1.0 / g.width;
        lm[2][1] = -2.0 / (g.z1 - g.z0);
        lm[3][1] = 1.0 + 2.0 * g.z0 / (g.z1 - g.z0) + 1.0 / g.height;
        lm[1][2] = -1.0 / range;
        lm[3][2] = kHighest / range;
        lm[3][3] = 1.0;
        IDirect3DVertexDeclaration9* lastDecl = nullptr;
        UINT budget = kPerFrame;
        size_t i = 0;
        for (; i < g.queue.size() && budget > 0; i++, budget--) {
            Job& j = g.queue[i];
            float columns[8][4];
            for (int col = 0; col < 4; col++) {
                for (int r = 0; r < 4; r++) {
                    double sum = r == 3 ? lm[3][col] : 0.0;
                    for (int k = 0; k < 3; k++) sum += static_cast<double>(j.world[r][k]) * lm[k][col];
                    columns[col][r] = static_cast<float>(sum);
                    columns[4 + col][r] = col < 3 ? j.world[r][col] : (r == 3 ? 1.0f : 0.0f);
                }
            }
            if (j.decl != lastDecl) {
                device->SetVertexDeclaration(j.decl);
                lastDecl = j.decl;
            }
            for (int s = 0; s < j.streams; s++) device->SetStreamSource(s, j.vb[s], j.offset[s], j.stride[s]);
            device->SetIndices(j.ib);
            device->SetTexture(0, j.texture);
            const float params[4] = {j.texture && j.texcoord ? 1.0f : 0.0f, g.ground, 1.0f / 120.0f, 0.0f};
            device->SetVertexShaderConstantF(0, &columns[0][0], 8);
            device->SetPixelShaderConstantF(0, params, 1);
            device->DrawIndexedPrimitive(j.type, j.base, j.minIndex, j.vertices, j.start, j.count);
            releaseJob(j);
            g.drawn++;
        }
        g.queue.erase(g.queue.begin(), g.queue.begin() + static_cast<std::ptrdiff_t>(i));
        device->SetTexture(0, nullptr);
        device->SetIndices(nullptr);
        device->SetStreamSource(0, nullptr, 0, 0);
        device->SetStreamSource(1, nullptr, 0, 0);
        device->EndScene();
        g.texture->GenerateMipSubLevels();
    }
    for (DWORD i = 0; i < 4; i++) {
        device->SetRenderTarget(i, targets[i]);
        gfx::release(targets[i]);
    }
    device->SetDepthStencilSurface(depth);
    gfx::release(depth);
    g.saved->Apply();
    const double ms = now() - t0;
    g.worstMs = std::max(g.worstMs, ms);
    g.totalMs += ms;
    g.framesDrawn++;
    g.lastWork = GetTickCount();
}

IDirect3DTexture9* picture() {
    return g.wanted && g.region && g.cleared && g.drawn > 0 ? g.texture : nullptr;
}

int meshes() {
    return g.drawn;
}

void releaseDevice() {
    forget();
    releaseTargets();
    forgetDecls();
    for (auto& e : g_ownDecls) gfx::release(e.second);
    g_ownDecls.clear();
    gfx::release(g.vs);
    gfx::release(g.ps);
    g.failed = false;
}

void precompile() {
    std::thread([] {
        gfx::precompile({{kTopVS, "main", "vs_3_0", "MinimapTopVS"}, {kTopPS, "main", "ps_3_0", "MinimapTopPS"}});
    }).detach();
}

} // namespace topview
} // namespace misc
} // namespace tmshaders
