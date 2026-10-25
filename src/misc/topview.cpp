#include "misc/internal.h"
#include "capwriter.h"
#include "casters.h"
#include "engine.h"
#include "gfx.h"
#include "log.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <cwchar>
#include <cctype>
#include <cstdio>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The minimap's picture from above ("Real look"): the meshes the game draws for the map, drawn again
// once each into a texture seen straight from above, with their own texture and a plain light from
// above. Every mesh gets the same treatment: nothing is told apart by what it does (no block is
// named, no waypoint field is read; a checkpoint gate is its geometry like in the editor).
//
// Which draws: every indexed draw of the main camera that is opaque (depth and colour written, no
// blending, no alpha test), from the managed pool (the game's static meshes), not a car's or glass,
// with a world matrix in the game's constants (casters::drawWorld, the recovery the long shadows
// use), whose mesh lies inside the map's grid area and below the highest track plus a margin: the
// sky, the clouds and the stadium around the grid stay out. Blocks are not asked for by identity:
// the game's corpus for a draw is a block's in only a few percent of the draws (my log on A01).
// Each distinct draw (buffers, index range, place and turn rounded) once per map; instances of a
// block share their buffers and differ by place. With what: the game's vertex and index buffers,
// its declaration's POSITION and TEXCOORD0, the texture in sampler 0. The draws are queued with
// references to all of that and drawn at Present (before the overlay), at most kPerFrame a frame,
// inside a saved and restored device state: never inside one of the game's passes.
//
// Meshes the game hasn't drawn yet stay the plain squares of mapview.cpp under the picture; they
// fill in as the camera sees them. Once nothing new came for a while, only one draw in kStride is
// looked at per frame (each one every kStride frames), so the checks cost little after the build-up.
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
constexpr float kMargin = 16.0f;        // m a mesh may reach past the grid area
constexpr unsigned kStride = 16;        // after the build-up: one draw in 16 looked at per frame
constexpr unsigned kNewPerFrame = 48;  // new meshes measured per frame (their vertices are read: ~20 us each)
constexpr float kCell = 4.0f;          // m: the place's rounding in the key (the camera's jitter moves it ~1 m)
constexpr float kNewPerSecond = 5.0f;  // after the build-up: new meshes admitted per second at most
constexpr DWORD kSettleMs = 5000;       // nothing new for this long: the build-up is over
constexpr uint8_t kRejected = 0xFF;     // a placement outside the map (in `keys`)
constexpr DWORD kReportMs = 60000;      // the cost line while the minimap is on

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
float4 u_Params : register(c0); // x: 0 the tint alone, 1 a colour texture, 2 a grey texture times the tint;
                                // y: lowest height (m), z: 1 / height range
float4 u_Tint : register(c1);   // the material's colour (grass, dirt, water, the rest grey)
float4 main(float2 uv : TEXCOORD0, float3 world : TEXCOORD1) : COLOR0 {
    float3 base = u_Tint.rgb;
    if (u_Params.x > 1.5) base = u_Tint.rgb * (0.55 + 0.9 * tex2D(s_Texture, uv).r);
    else if (u_Params.x > 0.5) base = tex2D(s_Texture, uv).rgb;
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
    WORD positionOffset = 0;
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
    int mode = 0;          // 0 the tint, 1 a colour texture, 2 a grey texture times the tint
    float tint[4] = {0.55f, 0.55f, 0.58f, 1.0f};
    D3DPRIMITIVETYPE type = D3DPT_TRIANGLELIST;
    INT base = 0;
    UINT minIndex = 0, vertices = 0, start = 0, count = 0;
    float world[4][3] = {};
    uint64_t key = 0; // its placement's key (g.keys)
};

// GPU time of the redraws: timestamps around each frame's batch, read a few frames later.
struct GpuTimer {
    IDirect3DQuery9* disjoint = nullptr;
    IDirect3DQuery9* frequency = nullptr;
    IDirect3DQuery9* begin = nullptr;
    IDirect3DQuery9* end = nullptr;
    bool pending = false;
};

// Time sums over a stretch (the build-up, or the last report).
struct Sums {
    unsigned frames = 0;     // frames with the minimap drawn
    double drawMs = 0.0;     // CPU: the minimap's drawing (mapview::draw, the redraws included)
    double checkMs = 0.0;    // CPU: the per-draw checks (onDraw)
    double renderMs = 0.0;   // CPU: the redraws alone (part of drawMs)
    double gpuMs = 0.0;      // GPU: the redraws
    unsigned gpuFrames = 0;  // frames whose GPU time was read
    unsigned checked = 0;    // draws looked at past the quick checks
    int meshes = 0;          // drawn into the picture
    unsigned added = 0;      // new meshes queued
    unsigned capped = 0;     // new meshes refused by the cap after the build-up
    unsigned colour = 0;     // queued with a colour texture
    unsigned grey = 0;       // with a grey texture (tinted)
    unsigned upgraded = 0;   // placements drawn again with a better texture
    unsigned noShader = 0;   // draws outside the game's own shader calls (the mod's passes) left out
    unsigned noTexture = 0;  // the game's draws without any texture bound (depth, shadow) left out
    unsigned ownMap = 0;     // with the material's own colour map, bound for the draw
};

struct State {
    bool wanted = false;
    uint64_t map = 0; // the map's identity (its UID's hash): the same map loaded again keeps the picture
    bool region = false;
    float x0 = 0, z0 = 0, x1 = 1, z1 = 1, top = kHighest;
    int width = 0, height = 0;
    IDirect3DTexture9* texture = nullptr;
    IDirect3DSurface9* surface = nullptr;
    IDirect3DSurface9* depth = nullptr;
    bool mipmaps = false;
    bool cleared = false;
    bool depthCleared = false;
    // A copy of the picture in memory (level 0, ARGB rows), made a second after it changed: a device
    // reset (the video export's start, a focus or mode change) loses the video memory, and the copy
    // brings it back with what was drawn (the placements stay known, so nothing is drawn twice).
    std::vector<uint32_t> copy;
    int copyDrawn = -1;    // g.drawn when it was made
    DWORD changedTick = 0; // the picture last drawn into
    DWORD copyTick = 0;    // the copy made
    bool restore = false;  // after a reset: the copy goes back in before anything else
    // Placements taken since the copy (drawn or still queued): not in it, so a reset forgets them and
    // they come again (the rest stays known: nothing is drawn twice over the copy, whose heights are gone).
    std::vector<uint64_t> sinceCopy;
    int savedDrawn = -1;   // g.drawn when the picture went to disk (Minimap\<map>.bin)
    IDirect3DStateBlock9* saved = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    bool failed = false; // resources could not be made: the squares stay
    std::vector<Job> queue;
    // Per placement: how it went into the picture (kRejected: outside the map; then 1 the material's
    // colour, 2 a grey texture, 3 a colour texture, 4 the material's own colour map). A better draw
    // of the same placement later (the pass with the colour map after one with only the lightmap)
    // draws it again over the first. Above the low byte: the textures bound the last two times it
    // was looked at (boundTextures, 28 bits each), so a pass with the same ones isn't looked at again.
    std::unordered_map<uint64_t, uint64_t> keys;
    unsigned frame = 1;
    unsigned drawIndex = 0; // the main camera's indexed draws this frame (the stride)
    unsigned newThisFrame = 0; // new meshes measured this frame (kNewPerFrame)
    int drawn = 0;
    // The height shading's base: the lowest mesh seen before the first drawing (the ground).
    float lowestSeen = 1e9f, ground = 0.0f;
    bool groundFixed = false;
    // The build-up: from the first mesh queued until nothing new came for kSettleMs.
    DWORD firstNew = 0, lastNew = 0;
    bool settled = false;
    int buildLines = 0; // build-up lines this map (a camera that keeps finding more: at most 3)
    float allowance = 0.0f; // after the build-up: new meshes that may still come (kNewPerSecond)
    DWORD allowanceTick = 0;
    int materialLines = 0;  // materials logged this map (where their colour came from)
    std::unordered_set<uintptr_t> loggedMaterials; // material and how its colour came (once each)
    Sums build, window;
    DWORD windowStart = 0;
    GpuTimer gpu[4];
};
State g;
std::map<std::tuple<WORD, BYTE, WORD, BYTE, bool, bool>, IDirect3DVertexDeclaration9*> g_ownDecls;
std::unordered_map<IDirect3DVertexDeclaration9*, DeclInfo> g_decls; // the game's (held) -> ours
// Per pixel shader of the game (held): the sampler its colour comes from (-1: none known) and the
// names of its samplers, from the constant table in its bytecode.
struct ShaderSamplers {
    int colour = -1;
    std::string names;
};
std::unordered_map<IDirect3DPixelShader9*, ShaderSamplers> g_samplers;

double now() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

int64_t rounded(float v, float step) {
    return static_cast<int64_t>(floorf(v / step + 0.5f));
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
        info.positionOffset = position->Offset;
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

bool managed(IDirect3DVertexBuffer9* vb) {
    D3DVERTEXBUFFER_DESC d{};
    return SUCCEEDED(vb->GetDesc(&d)) && d.Pool == D3DPOOL_MANAGED && !(d.Usage & D3DUSAGE_DYNAMIC);
}

bool managed(IDirect3DIndexBuffer9* ib) {
    D3DINDEXBUFFER_DESC d{};
    return SUCCEEDED(ib->GetDesc(&d)) && d.Pool == D3DPOOL_MANAGED && !(d.Usage & D3DUSAGE_DYNAMIC);
}

// The mesh's own bounds from its vertices (the managed pool keeps a copy in system memory: reading
// it doesn't touch the GPU). False: not readable, or nonsense.
bool meshBounds(const Job& j, WORD positionOffset, float lo[3], float hi[3]) {
    const long long first = static_cast<long long>(j.base) + j.minIndex;
    if (first < 0 || j.vertices == 0 || j.stride[0] < positionOffset + 12u) return false;
    void* data = nullptr;
    const UINT from = j.offset[0] + static_cast<UINT>(first) * j.stride[0];
    if (FAILED(j.vb[0]->Lock(from, j.vertices * j.stride[0], &data, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) || !data) return false;
    for (int k = 0; k < 3; k++) {
        lo[k] = 1e30f;
        hi[k] = -1e30f;
    }
    bool ok = true;
    __try {
        const unsigned char* v = static_cast<const unsigned char*>(data) + positionOffset;
        for (UINT i = 0; i < j.vertices; i++, v += j.stride[0]) {
            const float* p = reinterpret_cast<const float*>(v);
            for (int k = 0; k < 3; k++) {
                lo[k] = fminf(lo[k], p[k]);
                hi[k] = fmaxf(hi[k], p[k]);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    j.vb[0]->Unlock();
    for (int k = 0; k < 3 && ok; k++) ok = std::isfinite(lo[k]) && std::isfinite(hi[k]) && hi[k] >= lo[k];
    return ok;
}

// Inside the map's area: the mesh's world box within the grid (plus a margin) and below the highest
// track plus a margin. The sky, the clouds and the stadium around the grid are not.
bool insideMap(const Job& j, const float lo[3], const float hi[3]) {
    float wlo[3] = {1e30f, 1e30f, 1e30f}, whi[3] = {-1e30f, -1e30f, -1e30f};
    for (int c = 0; c < 8; c++) {
        const float p[3] = {(c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1], (c & 4) ? hi[2] : lo[2]};
        for (int k = 0; k < 3; k++) {
            const float w = p[0] * j.world[0][k] + p[1] * j.world[1][k] + p[2] * j.world[2][k] + j.world[3][k];
            wlo[k] = std::min(wlo[k], w);
            whi[k] = std::max(whi[k], w);
        }
    }
    return wlo[0] >= g.x0 - kMargin && whi[0] <= g.x1 + kMargin && wlo[2] >= g.z0 - kMargin && whi[2] <= g.z1 + kMargin && whi[1] <= g.top &&
           whi[1] > kLowest;
}

// The samplers in a pixel shader's constant table (shaders the game compiled from HLSL keep it): the
// one named for the colour ("Diffuse", "Diff..."), else the first not named for light, normals,
// specular, reflections, clouds, masks or shadows. -1: none.
ShaderSamplers samplersOf(IDirect3DPixelShader9* ps) {
    ShaderSamplers out;
    UINT size = 0;
    if (FAILED(ps->GetFunction(nullptr, &size)) || size < 8 || size > (1u << 20)) return out;
    std::vector<DWORD> code(size / 4);
    if (FAILED(ps->GetFunction(code.data(), &size))) return out;
    int named = -1, plain = -1;
    for (size_t i = 1; i < code.size();) {
        const DWORD token = code[i];
        if ((token & 0xFFFF) != 0xFFFE) break; // comments come right after the version token
        const size_t length = (token >> 16) & 0x7FFF;
        if (length >= 8 && i + 1 + length <= code.size() && code[i + 1] == 0x42415443 /* CTAB */) {
            const unsigned char* table = reinterpret_cast<const unsigned char*>(&code[i + 2]);
            const size_t bytes = (length - 1) * 4;
            const DWORD count = *reinterpret_cast<const DWORD*>(table + 12), info = *reinterpret_cast<const DWORD*>(table + 16);
            for (DWORD c = 0; c < count && info + (c + 1) * 20 <= bytes; c++) {
                const unsigned char* e = table + info + c * 20;
                const DWORD name = *reinterpret_cast<const DWORD*>(e);
                const WORD set = *reinterpret_cast<const WORD*>(e + 4), reg = *reinterpret_cast<const WORD*>(e + 6);
                if (set != 3 || name >= bytes) continue; // samplers only
                std::string n;
                for (size_t k = name; k < bytes && table[k] && n.size() < 40; k++) n += static_cast<char>(table[k]);
                char entry[64];
                snprintf(entry, sizeof(entry), "%ss%u %s", out.names.empty() ? "" : ", ", reg, n.c_str());
                out.names += entry;
                std::string lower = n;
                for (char& ch : lower) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                if (named < 0 && lower.find("diff") != std::string::npos) named = reg;
                static const char* const kOther[] = {"light", "lm", "norm", "nrm", "spec", "cube", "env", "refl", "fresnel", "cloud", "mask", "shadow", "occ", "warp", "fog", "noise"};
                bool other = false;
                for (const char* w : kOther) other |= lower.find(w) != std::string::npos;
                if (!other && (plain < 0 || reg < plain)) plain = reg;
            }
        }
        i += 1 + length;
    }
    out.colour = named >= 0 ? named : plain;
    return out;
}

// Formats that hold no colour (luminance, alpha): such a texture tints the material's colour.
bool greyFormat(D3DFORMAT f) {
    return f == D3DFMT_L8 || f == D3DFMT_A8L8 || f == D3DFMT_A4L4 || f == D3DFMT_L16 || f == D3DFMT_A8;
}

// The material's colour when no colour texture is known: grass green, dirt brown, water blue, else grey.
void materialTint(float out[4]) {
    switch (engine::currentMaterial()) {
    case engine::Material::Grass: out[0] = 0.30f, out[1] = 0.46f, out[2] = 0.22f; break;
    case engine::Material::Dirt: out[0] = 0.48f, out[1] = 0.35f, out[2] = 0.22f; break;
    case engine::Material::Water: out[0] = 0.20f, out[1] = 0.36f, out[2] = 0.55f; break;
    default: out[0] = 0.55f, out[1] = 0.55f, out[2] = 0.58f; break;
    }
    out[3] = 1.0f;
}

const char* formatName(D3DFORMAT f) {
    switch (f) {
    case D3DFMT_A8R8G8B8: return "A8R8G8B8";
    case D3DFMT_X8R8G8B8: return "X8R8G8B8";
    case D3DFMT_DXT1: return "DXT1";
    case D3DFMT_DXT3: return "DXT3";
    case D3DFMT_DXT5: return "DXT5";
    case D3DFMT_L8: return "L8";
    case D3DFMT_A8L8: return "A8L8";
    case D3DFMT_A8: return "A8";
    case D3DFMT_R5G6B5: return "R5G6B5";
    case D3DFMT_A1R5G5B5: return "A1R5G5B5";
    case D3DFMT_A4R4G4B4: return "A4R4G4B4";
    default: return "other";
    }
}

bool usableTexture(IDirect3DBaseTexture9* t);

// The textures bound on stages 0..7 as a 28-bit number (pointers only, nothing read from them).
uint64_t boundTextures(IDirect3DDevice9* device) {
    uint64_t h = 1469598103934665603ull;
    for (int st = 0; st < 8; st++) {
        IDirect3DBaseTexture9* t = nullptr;
        device->GetTexture(static_cast<DWORD>(st), &t);
        h = (h ^ reinterpret_cast<uintptr_t>(t)) * 1099511628211ull;
        if (t) t->Release();
    }
    return (h ^ (h >> 28) ^ (h >> 56)) & 0xFFFFFFFull;
}

// The texture the colour comes from, among those bound for this draw (stages 0..7). The game binds
// a pass's textures before it draws: CDx9ShaderKeeper::UpdateAndRender (TMNF 0x00971b10, TMUF
// 0x009719e0) runs CDx9StateBlock::Apply (TMNF 0x00980740, TMUF 0x00980440: SetTexture for each
// stage) and then CDx9VisualKeeper::RenderVisualGeometry (TMNF 0x0097c660, TMUF 0x0097c350:
// DrawIndexedPrimitive); the sorted passes (ShadeVisualForCurrentPass) draw with the pass's state
// applied before them. So what the device holds here, before the game's draw, is that draw's.
// First the material's own colour map (engine::materialDiffuse) when it is bound; then the sampler
// named for the colour when the shader keeps its names; else the largest colour texture (not
// luminance, not a render target), compressed colour (DXT) of 256 px or more first. A grey one only
// when no colour texture is bound. Returns the rank (0 no texture bound at all, 1 none usable,
// 2 grey, 3 colour, 4 the material's own colour map) and holds the texture in `out`.
int chooseTexture(IDirect3DDevice9* device, const void* diffuse, int named, IDirect3DBaseTexture9*& out, int& stage,
                  D3DSURFACE_DESC& desc, bool& own) {
    out = nullptr;
    stage = -1;
    own = false;
    double best = 0.0;
    int bestRank = 1;
    int bound = 0;
    for (int st = 0; st < 8; st++) {
        IDirect3DBaseTexture9* t = nullptr;
        device->GetTexture(static_cast<DWORD>(st), &t);
        if (!t) continue;
        bound++;
        D3DSURFACE_DESC d{};
        if (!usableTexture(t) || FAILED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d))) {
            t->Release();
            continue;
        }
        const bool grey = greyFormat(d.Format);
        const bool dxt = d.Format == D3DFMT_DXT1 || d.Format == D3DFMT_DXT3 || d.Format == D3DFMT_DXT5;
        const int rank = grey ? 2 : 3;
        double score = static_cast<double>(d.Width) * d.Height * (dxt && d.Width >= 256 ? 4.0 : 1.0);
        if (st == named && rank == 3) score = 1e18; // the shader names it
        const bool isOwn = diffuse && t == diffuse;
        if (isOwn) score = 1e19;
        if (isOwn || (bestRank < 4 && (rank > bestRank || (rank == bestRank && score > best)))) {
            if (out) out->Release();
            out = t;
            stage = st;
            best = score;
            bestRank = isOwn ? 4 : rank;
            desc = d;
            own = isOwn;
        } else {
            t->Release();
        }
    }
    if (!bound) return 0;
    return bestRank;
}

// The textures bound on stages 0..3 for the log: "s0 DXT1 512x512, s1 L8 256x256, s2 -, s3 -".
std::string stagesText(IDirect3DDevice9* device) {
    std::string out;
    for (int st = 0; st < 4; st++) {
        IDirect3DBaseTexture9* t = nullptr;
        device->GetTexture(static_cast<DWORD>(st), &t);
        char entry[64];
        if (!t) {
            snprintf(entry, sizeof(entry), "%ss%d -", st ? ", " : "", st);
        } else if (t->GetType() != D3DRTYPE_TEXTURE) {
            snprintf(entry, sizeof(entry), "%ss%d %s", st ? ", " : "", st, t->GetType() == D3DRTYPE_CUBETEXTURE ? "cube" : "volume");
        } else {
            D3DSURFACE_DESC d{};
            static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d);
            snprintf(entry, sizeof(entry), "%ss%d %s %ux%u%s", st ? ", " : "", st, formatName(d.Format), d.Width, d.Height,
                     (d.Usage & D3DUSAGE_RENDERTARGET) ? " (target)" : "");
        }
        if (t) t->Release();
        out += entry;
    }
    return out;
}

// A texture the picture may use: a plain 2D one, not a render target or depth texture (the game's
// shadow buffer, reflections: their content changes every frame).
bool usableTexture(IDirect3DBaseTexture9* t) {
    if (!t || t->GetType() != D3DRTYPE_TEXTURE) return false;
    D3DSURFACE_DESC d{};
    if (FAILED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d))) return false;
    return !(d.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL));
}

void releaseGpuTimers() {
    for (GpuTimer& t : g.gpu) {
        gfx::release(t.disjoint);
        gfx::release(t.frequency);
        gfx::release(t.begin);
        gfx::release(t.end);
        t.pending = false;
    }
}

void releaseTargets() {
    gfx::release(g.surface);
    gfx::release(g.texture);
    gfx::release(g.depth);
    gfx::release(g.saved);
    releaseGpuTimers();
    g.cleared = false;
    g.depthCleared = false;
}

// Everything drawn is gone (a new map, a device reset, the setting off): the draws come again
// as the game draws them.
void forget() {
    clearQueue();
    g.keys.clear();
    g.copy.clear();
    g.copyDrawn = -1;
    g.restore = false;
    g.sinceCopy.clear();
    g.savedDrawn = -1;
    g.cleared = false;
    g.drawn = 0;
    g.lowestSeen = 1e9f;
    g.groundFixed = false;
    g.firstNew = g.lastNew = 0;
    g.settled = false;
    g.buildLines = 0;
    g.materialLines = 0;
    g.loggedMaterials.clear();
    g.allowance = 0.0f;
    g.build = Sums();
}

// Video memory of the picture: the colour target with its mipmaps and the depth buffer.
double megabytes() {
    if (!g.texture) return 0.0;
    const double pixels = static_cast<double>(g.width) * g.height;
    return (pixels * 4.0 * (g.mipmaps ? 4.0 / 3.0 : 1.0) + pixels * 4.0) / (1024.0 * 1024.0);
}

bool ensureResources(IDirect3DDevice9* device) {
    if (g.failed) return false;
    if (!g.vs) g.vs = gfx::compileVertexShader(device, kTopVS, "main", "MinimapTopVS");
    if (!g.ps) g.ps = gfx::compilePixelShader(device, kTopPS, "main", "MinimapTopPS");
    if (!g.texture) {
        // With mipmaps made by the card: the whole map in a small window doesn't shimmer.
        g.mipmaps = SUCCEEDED(device->CreateTexture(g.width, g.height, 0, D3DUSAGE_RENDERTARGET | D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8,
                                                    D3DPOOL_DEFAULT, &g.texture, nullptr)) && g.texture;
        if (!g.mipmaps) {
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
    // The GPU timers (none on a card without timestamp queries: the log says "n/a").
    for (GpuTimer& t : g.gpu) {
        if (t.end) continue;
        if (FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &t.disjoint)) || FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &t.frequency)) ||
            FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &t.begin)) || FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &t.end))) {
            gfx::release(t.disjoint);
            gfx::release(t.frequency);
            gfx::release(t.begin);
            gfx::release(t.end);
        }
    }
    return true;
}

// The redraws' GPU times that have come in (never waits).
void collectGpu() {
    for (GpuTimer& t : g.gpu) {
        if (!t.pending) continue;
        BOOL disjoint = FALSE;
        UINT64 frequency = 0, begin = 0, end = 0;
        if (t.disjoint->GetData(&disjoint, sizeof(disjoint), 0) != S_OK || t.frequency->GetData(&frequency, sizeof(frequency), 0) != S_OK ||
            t.begin->GetData(&begin, sizeof(begin), 0) != S_OK || t.end->GetData(&end, sizeof(end), 0) != S_OK) {
            continue;
        }
        t.pending = false;
        if (disjoint || frequency == 0 || end < begin) continue;
        const double ms = static_cast<double>(end - begin) * 1000.0 / static_cast<double>(frequency);
        g.window.gpuMs += ms;
        g.window.gpuFrames++;
        if (!g.settled) {
            g.build.gpuMs += ms;
            g.build.gpuFrames++;
        }
    }
}

void add(Sums& to, const Sums& from) {
    to.frames += from.frames;
    to.drawMs += from.drawMs;
    to.checkMs += from.checkMs;
    to.renderMs += from.renderMs;
    to.checked += from.checked;
    to.meshes += from.meshes;
    to.added += from.added;
    to.capped += from.capped;
    to.colour += from.colour;
    to.grey += from.grey;
    to.upgraded += from.upgraded;
    to.noShader += from.noShader;
    to.noTexture += from.noTexture;
    to.ownMap += from.ownMap;
}

void logSums(const char* what, const Sums& s, double seconds) {
    const double frames = std::max(1u, s.frames);
    char gpu[64] = "n/a (no timestamp queries)";
    if (s.gpuFrames) snprintf(gpu, sizeof(gpu), "%.3f ms a frame with redraws (%.1f ms in all)", s.gpuMs / s.gpuFrames, s.gpuMs);
    else if (!g.wanted) snprintf(gpu, sizeof(gpu), "none (Real look off)");
    else if (s.meshes == 0) snprintf(gpu, sizeof(gpu), "none (no redraws)");
    const double share = s.added ? 100.0 * s.colour / s.added : 0.0, greyShare = s.added ? 100.0 * s.grey / s.added : 0.0;
    TMVS_LOG("misc: minimap cost, %s (%.0f s, %u frames, Real look %s): CPU %.3f ms a frame (drawing %.3f, of it redraws %.3f; checks of the "
             "game's draws %.3f, %u looked at); GPU %s; %d meshes drawn into the picture, %u new (%.1f a second, %u refused by the cap); "
             "%.0f %% with a colour texture (%u of them the material's own colour map), %.0f %% grey (tinted), the rest the material's colour, "
             "%u drawn again with a better one; left out: %u draws outside the game's shader calls, %u without a texture; "
             "picture %d x %d px, %.1f MB of video memory",
             what, seconds, s.frames, g.wanted ? "on" : "off", (s.drawMs + s.checkMs) / frames, s.drawMs / frames, s.renderMs / frames,
             s.checkMs / frames, s.checked, gpu, s.meshes, s.added, seconds > 0.0 ? s.added / seconds : 0.0, s.capped, share, s.ownMap, greyShare,
             s.upgraded, s.noShader, s.noTexture, g.texture ? g.width : 0, g.texture ? g.height : 0, megabytes());
}

} // namespace

void setWanted(bool wanted) {
    if (g.wanted && !wanted) {
        forget();
        releaseTargets();
    }
    g.wanted = wanted;
}

void loadFromDisk();

void setRegion(uint64_t map, float x0, float z0, float x1, float z1, float top, int cellsX, int cellsZ) {
    int width = cellsX * kPixelsPerCell, height = cellsZ * kPixelsPerCell;
    const int largest = std::max(width, height);
    if (largest > kMaxSize) {
        width = std::max(1, width * kMaxSize / largest);
        height = std::max(1, height * kMaxSize / largest);
    }
    const bool same = g.region && map == g.map && x0 == g.x0 && z0 == g.z0 && x1 == g.x1 && z1 == g.z1 && top == g.top;
    if (same) return;
    forget();
    if (width != g.width || height != g.height) releaseTargets();
    g.map = map;
    g.region = width > 0 && height > 0;
    g.x0 = x0;
    g.z0 = z0;
    g.x1 = x1;
    g.z1 = z1;
    g.top = top;
    g.width = width;
    g.height = height;
    g.failed = false;
    loadFromDisk();
}

void clearRegion() {
    forget();
    g.region = false;
    g.map = 0;
}

void onDraw(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start, UINT count,
            const float view[16], const float projection[16]) {
    if (!g.wanted || !g.region || g.failed || count == 0) return;
    // After the build-up one draw in kStride is looked at per frame (a different one each frame); the
    // others cost a counter (not timed: the clock would cost more than they do).
    const unsigned index = g.drawIndex++;
    if (g.settled && (index + g.frame) % kStride != 0) return;
    const double t0 = now();
    struct Timer {
        double from;
        ~Timer() { g.window.checkMs += now() - from; }
    } timer{t0};
    if (g.queue.size() >= kMaxQueue) return;
    // Only the game's own draws: inside its shader call (CDx9ShaderKeeper::UpdateAndRender or a sorted
    // pass), where its material is known. The mod's own passes (the long shadows' maps, the G-buffer)
    // draw the game's meshes too, without textures: my first colour test took those (no material,
    // no texture in the log).
    const void* shader = engine::currentShader();
    if (!shader) {
        g.window.noShader++;
        return;
    }
    const engine::Material material = engine::currentMaterial();
    if (material == engine::Material::Car || material == engine::Material::Glass) return; // cars, ghosts, windows
    DWORD z = FALSE, zwrite = FALSE, blend = FALSE, alphaTest = FALSE, colorWrite = 0;
    device->GetRenderState(D3DRS_ZENABLE, &z);
    device->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    device->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
    device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
    if (!z || !zwrite || blend || alphaTest || !colorWrite) return;
    g.window.checked++;
    IDirect3DVertexDeclaration9* decl = nullptr;
    device->GetVertexDeclaration(&decl);
    if (!decl) return;
    const DeclInfo info = declInfo(device, decl);
    decl->Release();
    if (!info.own) return;
    Job j;
    j.decl = info.own;
    j.type = type;
    j.base = base;
    j.minIndex = minIndex;
    j.vertices = vertices; // meshBounds reads this range
    j.start = start;
    j.count = count;
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
    // The game's static meshes only (the managed pool): what is rebuilt every frame (particles,
    // the HUD's 3D parts) is left out.
    if (!ok || !j.ib || !managed(j.vb[0]) || !managed(j.ib)) {
        releaseJob(j);
        return;
    }
    // Its place: from the game's constants. Not this camera's: left out.
    if (!casters::drawWorld(device, view, projection, j.world)) {
        releaseJob(j);
        return;
    }
    // The key: the buffers' sizes (not their addresses: the same map loaded again, as Shoot video does,
    // has new buffers with the same meshes, and the picture and its keys go to disk), the vertex
    // stride, the index range, the turn (1/20) and the place in 4 m cells. The place the constants give
    // moves by up to about a metre from frame to frame (the camera's jitter in the projection, float
    // noise far away): a place near a cell's edge also finds the key of the neighbouring cell on that
    // side, so each placement is taken once.
    D3DVERTEXBUFFER_DESC vbDesc{};
    D3DINDEXBUFFER_DESC ibDesc{};
    j.vb[0]->GetDesc(&vbDesc);
    j.ib->GetDesc(&ibDesc);
    uint64_t mesh = mix(mix(14695981039346656037ull, (static_cast<uint64_t>(vbDesc.Size) << 32) | ibDesc.Size), j.stride[0]);
    mesh = mix(mesh, (static_cast<uint64_t>(start) << 32) | count);
    mesh = mix(mesh, (static_cast<uint64_t>(static_cast<uint32_t>(base)) << 32) | minIndex);
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) mesh = mix(mesh, static_cast<uint64_t>(rounded(j.world[r][k], 0.05f)));
    }
    int64_t cell[3], side[3];
    for (int k = 0; k < 3; k++) {
        const float c = j.world[3][k] / kCell;
        cell[k] = static_cast<int64_t>(floorf(c));
        side[k] = c - static_cast<float>(cell[k]) < 0.5f ? cell[k] - 1 : cell[k] + 1;
    }
    const auto keyAt = [&](int64_t x, int64_t y, int64_t z) { return mix(mix(mix(mesh, static_cast<uint64_t>(x)), static_cast<uint64_t>(y)), static_cast<uint64_t>(z)); };
    uint64_t key = keyAt(cell[0], cell[1], cell[2]);
    uint8_t stored = 0; // 0: not seen
    uint64_t seen[2] = {0, 0}; // the textures of the last two looks
    for (int n = 0; n < 8 && !stored; n++) {
        const uint64_t k = keyAt((n & 1) ? side[0] : cell[0], (n & 2) ? side[1] : cell[1], (n & 4) ? side[2] : cell[2]);
        auto it = g.keys.find(k);
        if (it != g.keys.end()) {
            stored = static_cast<uint8_t>(it->second & 0xFF);
            seen[0] = (it->second >> 8) & 0xFFFFFFFull;
            seen[1] = (it->second >> 36) & 0xFFFFFFFull;
            key = k;
        }
    }
    if (stored == kRejected || stored == 4 || (!stored && g.keys.size() >= kMaxKeys)) {
        releaseJob(j);
        return;
    }
    const uint64_t textures = boundTextures(device);
    if (stored && (textures == seen[0] || textures == seen[1])) { // looked at with these textures already
        releaseJob(j);
        return;
    }
    // Its colour, from the textures bound for this draw (see chooseTexture).
    int named = -1;
    const char* names = "(no pixel shader)";
    IDirect3DPixelShader9* ps = nullptr;
    device->GetPixelShader(&ps);
    if (ps) {
        auto it = g_samplers.find(ps);
        if (it == g_samplers.end()) {
            if (g_samplers.size() >= 1024) {
                for (auto& e : g_samplers) e.first->Release();
                g_samplers.clear();
            }
            ps->AddRef();
            it = g_samplers.emplace(ps, samplersOf(ps)).first;
        }
        named = it->second.colour;
        names = it->second.names.empty() ? "(none)" : it->second.names.c_str();
        ps->Release();
    }
    wchar_t diffuseFile[96] = L"";
    IDirect3DBaseTexture9* diffuse = engine::materialDiffuse(shader, diffuseFile, 96);
    D3DSURFACE_DESC desc{};
    int stage = -1;
    bool own = false;
    int rank = chooseTexture(device, diffuse, named, j.texture, stage, desc, own);
    if (rank == 0) { // nothing bound: not the colour pass (depth, shadows)
        g.window.noTexture++;
        releaseJob(j);
        return;
    }
    if (!j.texcoord && rank > 1) { // no texture coordinates to read it with
        gfx::release(j.texture);
        rank = 1;
        own = false;
    }
    if (stored && rank <= stored) { // drawn already, and this pass has nothing better: remember its textures
        g.keys[key] = (seen[0] << 36) | (textures << 8) | stored;
        releaseJob(j);
        return;
    }
    // New meshes are measured a few dozen a frame (reading their vertices is the costly part): the
    // rest come again in the next frames, so a map's start doesn't stutter. After the build-up at most
    // kNewPerSecond (something that moves would otherwise come as new forever).
    if (g.newThisFrame >= kNewPerFrame) {
        if (!g.settled) g.lastNew = GetTickCount(); // more to come
        releaseJob(j);
        return;
    }
    if (g.settled) {
        if (g.allowance < 1.0f) {
            g.window.capped++;
            releaseJob(j);
            return;
        }
        g.allowance -= 1.0f;
    }
    g.newThisFrame++;
    if (!stored) {
        float lo[3], hi[3];
        if (!meshBounds(j, info.positionOffset, lo, hi) || !insideMap(j, lo, hi)) {
            g.keys[key] = kRejected; // looked at once
            releaseJob(j);
            return;
        }
    } else {
        g.window.upgraded++;
    }
    g.keys[key] = (seen[0] << 36) | (textures << 8) | static_cast<uint64_t>(rank);
    g.sinceCopy.push_back(key);
    j.key = key;
    j.mode = rank >= 3 ? 1 : rank == 2 ? 2 : 0;
    if (!j.mode) gfx::release(j.texture);
    materialTint(j.tint);
    (j.mode == 1 ? g.window.colour : g.window.grey) += j.mode != 0 ? 1 : 0;
    g.window.ownMap += own ? 1 : 0;
    g.window.added++;
    // Each material once per map (the first 16): its file, its colour map, the samplers' names,
    // what is bound on stages 0..3 and the choice (the next game test shows where the colour is).
    if (g.materialLines < 16 && g.loggedMaterials.insert(reinterpret_cast<uintptr_t>(shader) ^ static_cast<uintptr_t>(rank)).second) {
        g.materialLines++;
        wchar_t file[160] = L"(unnamed)";
        engine::materialFile(shader, file, 160);
        const wchar_t* fileName = wcsrchr(file, L'\\');
        char from[48] = "the material's colour";
        if (stage >= 0) snprintf(from, sizeof(from), "s%d%s%s", stage, own ? " (its colour map)" : "", j.mode == 2 ? " (grey, tinted)" : "");
        TMVS_LOG("misc: minimap: material %ls (class %d): colour map %ls%s; samplers %s; bound %s; colour from %s%s",
                 fileName ? fileName + 1 : file, static_cast<int>(material), diffuseFile[0] ? diffuseFile : L"(none)",
                 diffuseFile[0] && !diffuse ? " (not loaded)" : diffuseFile[0] && !own ? " (not bound here)" : "", names,
                 stagesText(device).c_str(), from, stored ? ", drawn again over the first" : "");
    }
    g.lowestSeen = std::min(g.lowestSeen, j.world[3][1]);
    g.queue.push_back(j);
    if (!g.settled) {
        g.lastNew = GetTickCount();
        if (!g.firstNew) g.firstNew = g.lastNew;
    }
}

// The picture on disk, per map (Documents\TrackMania\TMVS\Minimap\<map>.bin): the next time the map
// loads it is there at once, the meshes in it known. Only for a map told by its UID (the high bit).
constexpr uint32_t kDiskMagic = 0x4D4D5654; // "TVMM"
constexpr uint32_t kDiskVersion = 1;
struct DiskHeader {
    uint32_t magic, version;
    int32_t width, height, drawn;
    uint32_t keys;
    float x0, z0, x1, z1, top;
};

std::wstring diskFile(uint64_t map) {
    if (!(map >> 63)) return std::wstring();
    wchar_t name[32];
    swprintf(name, 32, L"\\%016llx.bin", static_cast<unsigned long long>(map));
    const std::wstring folder = log::dataDir() + L"\\Minimap";
    CreateDirectoryW(folder.c_str(), nullptr);
    return folder + name;
}

void saveToDisk() {
    const std::wstring file = diskFile(g.map);
    if (file.empty() || g.copy.empty()) return;
    g.savedDrawn = g.drawn;
    auto data = std::make_shared<std::vector<char>>();
    std::unordered_set<uint64_t> pending(g.sinceCopy.begin(), g.sinceCopy.end());
    std::vector<std::pair<uint64_t, uint64_t>> keys;
    keys.reserve(g.keys.size());
    for (const auto& e : g.keys) {
        if (!pending.count(e.first)) keys.push_back(e);
    }
    const DiskHeader h = {kDiskMagic, kDiskVersion, g.width, g.height, g.drawn, static_cast<uint32_t>(keys.size()), g.x0, g.z0, g.x1, g.z1, g.top};
    const size_t pixels = g.copy.size() * 4, pairs = keys.size() * sizeof(keys[0]);
    data->resize(sizeof(h) + pixels + pairs);
    memcpy(data->data(), &h, sizeof(h));
    memcpy(data->data() + sizeof(h), g.copy.data(), pixels);
    if (pairs) memcpy(data->data() + sizeof(h) + pixels, keys.data(), pairs);
    const int drawn = g.drawn;
    capwriter::post([data, file, drawn] {
        const std::wstring part = file + L".part";
        HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        const bool ok = WriteFile(f, data->data(), static_cast<DWORD>(data->size()), &written, nullptr) && written == data->size();
        CloseHandle(f);
        if (ok && MoveFileExW(part.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            TMVS_LOG("misc: minimap: the picture from above saved (%d meshes, %.1f MB)", drawn, data->size() / 1048576.0);
        } else {
            DeleteFileW(part.c_str());
        }
    });
}

// A new map: its picture from disk, if one was made for this area (it goes in at the next render).
void loadFromDisk() {
    const std::wstring file = diskFile(g.map);
    if (file.empty()) return;
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DiskHeader h{};
    DWORD read = 0;
    bool ok = ReadFile(f, &h, sizeof(h), &read, nullptr) && read == sizeof(h) && h.magic == kDiskMagic && h.version == kDiskVersion &&
              h.width == g.width && h.height == g.height && h.x0 == g.x0 && h.z0 == g.z0 && h.x1 == g.x1 && h.z1 == g.z1 && h.top == g.top &&
              h.drawn > 0 && h.keys <= kMaxKeys;
    if (ok) {
        g.copy.resize(static_cast<size_t>(h.width) * h.height);
        std::vector<std::pair<uint64_t, uint64_t>> keys(h.keys);
        const DWORD pixels = static_cast<DWORD>(g.copy.size() * 4), pairs = static_cast<DWORD>(keys.size() * sizeof(keys[0]));
        ok = ReadFile(f, g.copy.data(), pixels, &read, nullptr) && read == pixels &&
             (!pairs || (ReadFile(f, keys.data(), pairs, &read, nullptr) && read == pairs));
        if (ok) {
            for (const auto& e : keys) g.keys.insert(e);
            g.drawn = g.copyDrawn = g.savedDrawn = h.drawn;
            g.restore = true;
            g.firstNew = g.lastNew = GetTickCount(); // the build-up ends 5 s after the last new one
            TMVS_LOG("misc: minimap: the picture from above loaded (%d meshes, %u known)", h.drawn, h.keys);
        } else {
            g.copy.clear();
        }
    }
    CloseHandle(f);
}

// The picture into memory, a second after it was last drawn into or every 5 s while it fills (one
// read-back: it waits for the card once, a few milliseconds for a 1024 x 1024 picture), so a device
// reset doesn't lose it.
void keepCopy(IDirect3DDevice9* device) {
    if (!g.texture || !g.surface || !g.cleared || g.drawn <= 0 || g.copyDrawn == g.drawn) return;
    const DWORD tick = GetTickCount();
    if (tick - g.changedTick < 1000 && tick - g.copyTick < 5000) return;
    g.copyTick = tick;
    IDirect3DSurface9* memory = nullptr;
    if (FAILED(device->CreateOffscreenPlainSurface(g.width, g.height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &memory, nullptr)) || !memory) return;
    D3DLOCKED_RECT locked{};
    if (SUCCEEDED(device->GetRenderTargetData(g.surface, memory)) && SUCCEEDED(memory->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        g.copy.resize(static_cast<size_t>(g.width) * g.height);
        for (int y = 0; y < g.height; y++)
            memcpy(&g.copy[static_cast<size_t>(y) * g.width], static_cast<const BYTE*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch, g.width * 4);
        memory->UnlockRect();
        g.copyDrawn = g.drawn;
        // What is still queued isn't in the copy.
        g.sinceCopy.clear();
        for (const Job& j : g.queue) g.sinceCopy.push_back(j.key);
    }
    memory->Release();
    if (g.copyDrawn == g.drawn && g.settled && g.savedDrawn != g.drawn) saveToDisk();
}

// After a device reset: the copy back into a new picture (its mipmaps made again).
void restoreCopy(IDirect3DDevice9* device) {
    g.restore = false;
    if (g.copy.size() != static_cast<size_t>(g.width) * g.height || !ensureResources(device)) {
        forget();
        return;
    }
    IDirect3DSurface9* memory = nullptr;
    bool ok = false;
    D3DLOCKED_RECT locked{};
    if (SUCCEEDED(device->CreateOffscreenPlainSurface(g.width, g.height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &memory, nullptr)) && memory &&
        SUCCEEDED(memory->LockRect(&locked, nullptr, 0))) {
        for (int y = 0; y < g.height; y++)
            memcpy(static_cast<BYTE*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch, &g.copy[static_cast<size_t>(y) * g.width], g.width * 4);
        memory->UnlockRect();
        ok = SUCCEEDED(device->UpdateSurface(memory, nullptr, g.surface, nullptr));
    }
    gfx::release(memory);
    if (!ok) {
        forget();
        return;
    }
    if (g.mipmaps) g.texture->GenerateMipSubLevels();
    g.cleared = true;
    g.depthCleared = false;
    TMVS_LOG("misc: minimap: the picture from above back after the device reset (%d meshes)", g.drawn);
}

void render(IDirect3DDevice9* device) {
    g.frame++;
    g.drawIndex = 0;
    g.newThisFrame = 0;
    if (!g.wanted || !g.region || !device) return;
    if (g.restore) restoreCopy(device);
    else keepCopy(device);
    if (g.queue.empty()) return;
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
    GpuTimer* timer = nullptr;
    for (GpuTimer& t : g.gpu) {
        if (t.end && !t.pending) {
            timer = &t;
            break;
        }
    }
    if (SUCCEEDED(device->BeginScene())) {
        if (timer) {
            timer->disjoint->Issue(D3DISSUE_BEGIN);
            timer->begin->Issue(D3DISSUE_END);
        }
        device->SetRenderTarget(0, g.surface);
        for (DWORD i = 1; i < 4; i++) device->SetRenderTarget(i, nullptr);
        device->SetDepthStencilSurface(g.depth);
        const D3DVIEWPORT9 viewport = {0, 0, static_cast<DWORD>(g.width), static_cast<DWORD>(g.height), 0.0f, 1.0f};
        device->SetViewport(&viewport);
        if (!g.cleared) {
            device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
            g.cleared = true;
            g.depthCleared = true;
        } else if (!g.depthCleared) {
            // The picture came back from its copy, the heights didn't: what comes now covers it.
            device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
            g.depthCleared = true;
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
            const float params[8] = {static_cast<float>(j.texture ? j.mode : 0), g.ground, 1.0f / 120.0f, 0.0f, j.tint[0], j.tint[1], j.tint[2], 1.0f};
            device->SetVertexShaderConstantF(0, &columns[0][0], 8);
            device->SetPixelShaderConstantF(0, params, 2);
            device->DrawIndexedPrimitive(j.type, j.base, j.minIndex, j.vertices, j.start, j.count);
            releaseJob(j);
            g.drawn++;
            g.window.meshes++;
            g.changedTick = GetTickCount();
        }
        g.queue.erase(g.queue.begin(), g.queue.begin() + static_cast<std::ptrdiff_t>(i));
        device->SetTexture(0, nullptr);
        device->SetIndices(nullptr);
        device->SetStreamSource(0, nullptr, 0, 0);
        device->SetStreamSource(1, nullptr, 0, 0);
        if (timer) {
            timer->end->Issue(D3DISSUE_END);
            timer->disjoint->Issue(D3DISSUE_END);
            timer->frequency->Issue(D3DISSUE_END);
            timer->pending = true;
        }
        device->EndScene();
        if (g.mipmaps) g.texture->GenerateMipSubLevels();
    }
    for (DWORD i = 0; i < 4; i++) {
        device->SetRenderTarget(i, targets[i]);
        gfx::release(targets[i]);
    }
    device->SetDepthStencilSurface(depth);
    gfx::release(depth);
    g.saved->Apply();
    g.window.renderMs += now() - t0;
}

void noteFrame(double drawMs) {
    g.window.frames++;
    g.window.drawMs += drawMs;
    collectGpu();
    const DWORD tick = GetTickCount();
    if (!g.windowStart) g.windowStart = tick;
    // The build-up: from the first mesh queued until nothing new came for kSettleMs and the queue is
    // drawn (or 30 s at most). One line, then only one draw in kStride is looked at per frame.
    if (g.wanted && !g.settled && g.firstNew) {
        add(g.build, g.window); // the build-up's share of this frame (window sums are per frame here)
    }
    // The build-up ends when nothing new came for kSettleMs, or after 20 s whatever comes (something
    // that keeps moving); from then on only one draw in kStride is looked at and new meshes are capped.
    if (g.wanted && !g.settled && g.firstNew && g.queue.empty() && (tick - g.lastNew > kSettleMs || tick - g.firstNew > 20000)) {
        g.settled = true;
        g.allowanceTick = tick;
        if (g.buildLines++ < 3) {
            logSums(tick - g.lastNew > kSettleMs ? "the build-up" : "the build-up (ended after 20 s, still finding more)", g.build, (tick - g.firstNew) / 1000.0);
        }
    }
    if (g.settled) {
        g.allowance = std::min(g.allowance + (tick - g.allowanceTick) / 1000.0f * kNewPerSecond, 2.0f * kNewPerSecond);
        g.allowanceTick = tick;
    }
    // The running sums: one line every kReportMs while the minimap is on.
    static Sums s_running;
    add(s_running, g.window);
    s_running.gpuMs += g.window.gpuMs;
    s_running.gpuFrames += g.window.gpuFrames;
    g.window = Sums();
    if (tick - g.windowStart >= kReportMs) {
        logSums("the last 60 s", s_running, (tick - g.windowStart) / 1000.0);
        s_running = Sums();
        g.windowStart = tick;
    }
}

IDirect3DTexture9* picture() {
    return g.wanted && g.region && g.cleared && g.drawn > 0 ? g.texture : nullptr;
}

int meshes() {
    return g.drawn;
}

void releaseDevice() {
    // The game's buffers held by the queue must go before the reset. With a copy of the picture it
    // goes back in after the reset; a copy of all that was drawn keeps the placements known (nothing
    // is drawn twice), an older one lets them come again over it. Without one everything starts over.
    clearQueue();
    if (g.copyDrawn > 0 && !g.copy.empty()) {
        for (uint64_t k : g.sinceCopy) g.keys.erase(k);
        if (!g.sinceCopy.empty()) {
            g.settled = false;
            g.firstNew = g.lastNew = GetTickCount();
        }
        g.sinceCopy.clear();
        g.drawn = g.copyDrawn;
        g.restore = true;
        g.cleared = false;
    } else {
        forget();
    }
    releaseTargets();
    forgetDecls();
    for (auto& e : g_samplers) e.first->Release();
    g_samplers.clear();
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
