#include "softshadow.h"
#include "hooks.h"
#include "log.h"
#include "shaders/PS_SoftShadow.h"
#include "shaders/PS_SoftShadowOpacity.h"
#include "shaders/PS_SoftShadowSprite.h"
#include "shaders/PS_SoftShadowSpriteOpacity.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace tmvs_mp {
namespace softshadow {
namespace {

template <typename T> void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// The engine's variants of Tech3/DeferredShadowPssm_p that pick the split by its rectangle
// (ShaderP = 11 + 5 * splits registers), by DXBC checksum (bytes 4..19), from ManiaPlanet's
// GpuCache_D3D11_SM5.zip and _SM4.zip (2019-11-19). The ones that pick by view depth
// (SplitZcMax4s) and the one without splits stay the engine's.
struct Known {
    const char* checksum;
    int splits;
    int extras; // bit 0 PSplitOpacity, bit 1 StaticSprite
};
const Known kKnown[] = {
    // shader model 5
    {"d0fd228d4bbc69f6e3dcc6711e7bd8c6", 1, 0}, {"73996e29d95290570d1b6b31186554a3", 1, 1},
    {"e166f388e8570c0369b07b955be966f3", 1, 2}, {"f586671d11cb6a5a3fa9411616d66a05", 1, 3},
    {"22a6d2c1faf3c6a4c7181be2cb0156e6", 2, 0}, {"409c6a03aa8d66dd4050730e130333d2", 2, 1},
    {"fac2e1ced861c13c47ab9e96e939f17f", 2, 2}, {"351d973d7fb87ae4d1b67b33532f244a", 2, 3},
    {"6fbe4ddf61566cc75a5089c5a2253f1b", 3, 0}, {"da2097904b9be9dcaf5300b6c7cabe09", 3, 1},
    {"36b7957c1cebb087a4ad9d408a527296", 3, 2}, {"5572715a4c1c2456c08f1eed90fe4095", 3, 3},
    {"5a8ecb3df72fb758734ee11e643b22d2", 4, 0}, {"01309343e53e5c0ef3cbbd4dff1cb356", 4, 1},
    {"9b86d81141e93a1e73405ee828c0d0ad", 4, 2}, {"4e3daad060554fbe7047ba3873e2329b", 4, 3},
    {"30a644920ba187540e942242e47e28b5", 5, 0}, {"2bae4e3895d4ec0d3871cfea6d62397e", 5, 1},
    {"be37a1313a9aba2834e11b34d2f212ba", 5, 2}, {"015fa9f0b9b34176f22d90743d10faf0", 5, 3},
    // shader model 4
    {"3c2d4319a93947f0693f36df5eea8f02", 1, 0}, {"80a268c051bb55dca4a87fef468f25bd", 1, 1},
    {"0e4e9e708305806177c37029b18c263c", 1, 2}, {"dfc7e546f091139c1fccc60464278db1", 1, 3},
    {"7c8c36f2b30cbad53a30ebe920170983", 2, 0}, {"15638019a85b3b7f096f3c29fec45547", 2, 1},
    {"c1e22e062bc7796ba98fde7ff1d234ea", 2, 2}, {"410a10c7b98ba51f1cc84d1e69a8319b", 2, 3},
    {"7aab8acf7d659d011036b5ba461d7f7d", 3, 0}, {"235f56b2af9a418039d466a951de9589", 3, 1},
    {"de4d7c33423dcd328e2cbd90f5a9d16b", 3, 2}, {"39d6a00a5d570056caa5bdd619f15b62", 3, 3},
    {"f6438ce97a5621230d29305f4d605f3d", 4, 0}, {"098cd6efa644c0228c0fa6eb8b7ad8cb", 4, 1},
    {"fff3b74a6a07a8f1859c3a6878e2522f", 4, 2}, {"08d2c43fb05c5b8e05e844ef72c8f4c4", 4, 3},
    {"52c4939bfc0e609a5dbe9fe536269643", 5, 0}, {"9c9bedeaabaf7ffaf7df098a3cef46a9", 5, 1},
    {"ed7ac58586b53e0187294a9c3f2fa767", 5, 2}, {"e6943e78dbade2d4e838423b78acddee", 5, 3},
};

const char* const kExtrasName[4] = {"", ", opacity map", ", sprite map", ", sprite and opacity maps"};

// The replacement's constants (b5 in softshadow.hlsli).
struct Constants {
    float soft0[4]; // tan(half the light's angle), reach (m), splits, mode
    float soft1[4]; // smallest filter radius, largest search radius, largest filter radius (texels), depth epsilon
};
constexpr UINT kSlot = 5; // b5 and s5: the engine's pass uses b0, b13 and s0-s3

std::mutex g_lock;
std::unordered_map<ID3D11PixelShader*, Variant> g_variants;

bool g_ready = false;
ID3D11PixelShader* g_shaders[4] = {}; // by extras
ID3D11SamplerState* g_point = nullptr;
ID3D11Buffer* g_constants = nullptr;
Constants g_uploaded = {};
bool g_uploadedValid = false;

// What end() puts back.
struct Bound {
    bool active = false;
    ID3D11PixelShader* engine = nullptr;
    ID3D11Buffer* buffer = nullptr;
    ID3D11SamplerState* sampler = nullptr;
};
Bound g_bound;

char g_status[160] = "off";
std::string g_logged; // reasons logged so far (each once)

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool sameChecksum(const unsigned char* sum, const char* hex) {
    for (int i = 0; i < 16; i++) {
        const int hi = hexValue(hex[i * 2]), lo = hexValue(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0 || sum[i] != static_cast<unsigned char>(hi * 16 + lo)) return false;
    }
    return true;
}

// The engine's shader stays: says why in the menu, and in the log once per reason.
bool keepEngine(const char* why) {
    snprintf(g_status, sizeof(g_status), "the engine's: %s", why);
    const std::string key = std::string("|") + why + "|";
    if (g_logged.find(key) == std::string::npos) {
        g_logged += key;
        TMVS_LOG("soft shadows: the engine's sun shadow shader stays (%s)", why);
    }
    return false;
}

bool depthFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R16_UNORM || f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
           f == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
}

} // namespace

Variant identify(const void* code, size_t size) {
    Variant v;
    if (!code || size < 20 || memcmp(code, "DXBC", 4) != 0) return v;
    const unsigned char* sum = static_cast<const unsigned char*>(code) + 4;
    for (const Known& k : kKnown) {
        if (!sameChecksum(sum, k.checksum)) continue;
        v.known = true;
        v.splits = k.splits;
        v.extras = k.extras;
        break;
    }
    return v;
}

void noteEngineShader(ID3D11PixelShader* shader, const void* code, size_t size) {
    if (!shader) return;
    const Variant v = identify(code, size);
    std::lock_guard<std::mutex> lock(g_lock);
    g_variants[shader] = v;
}

Variant variantOf(ID3D11PixelShader* shader) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_variants.find(shader);
    return it != g_variants.end() ? it->second : Variant{};
}

bool init(ID3D11Device* device) {
    if (g_ready) return true;
    if (!device || device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        TMVS_LOG("soft shadows: unavailable (they need feature level 11.0)");
        return false;
    }
    hooks::Inside inside;
    struct Code {
        const BYTE* data;
        size_t size;
    } code[4] = {{g_PS_SoftShadow, sizeof(g_PS_SoftShadow)},
                 {g_PS_SoftShadowOpacity, sizeof(g_PS_SoftShadowOpacity)},
                 {g_PS_SoftShadowSprite, sizeof(g_PS_SoftShadowSprite)},
                 {g_PS_SoftShadowSpriteOpacity, sizeof(g_PS_SoftShadowSpriteOpacity)}};
    bool ok = true;
    for (int i = 0; i < 4; i++) ok &= SUCCEEDED(device->CreatePixelShader(code[i].data, code[i].size, nullptr, &g_shaders[i]));
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ok &= SUCCEEDED(device->CreateSamplerState(&sd, &g_point));
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(Constants);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ok &= SUCCEEDED(device->CreateBuffer(&bd, nullptr, &g_constants));
    if (!ok) {
        TMVS_LOG("soft shadows: unavailable (the shaders could not be created)");
        release();
        return false;
    }
    g_ready = true;
    return true;
}

void release() {
    for (auto*& s : g_shaders) safeRelease(s);
    safeRelease(g_point);
    safeRelease(g_constants);
    g_uploadedValid = false;
    g_ready = false;
}

bool begin(ID3D11DeviceContext* ctx, const Params& params) {
    if (g_bound.active) end(ctx);
    if (!(params.lightSize > 0.0f)) {
        snprintf(g_status, sizeof(g_status), "off (the engine's)");
        return false;
    }
    if (!g_ready) return keepEngine("the replacement shaders are not available");
    hooks::Inside inside;
    ID3D11PixelShader* engine = nullptr;
    ctx->PSGetShader(&engine, nullptr, nullptr);
    const Variant v = variantOf(engine);
    if (!v.known) {
        if (engine) engine->Release();
        return keepEngine("a variant of it that the replacement doesn't know");
    }

    // The variant's bindings: ShaderP big enough for its splits, SceneP, the depth, the maps,
    // the face normal and the extra maps; their samplers.
    bool ok = true;
    const char* why = nullptr;
    ID3D11Buffer* cb[14] = {};
    ctx->PSGetConstantBuffers(0, 14, cb);
    D3D11_BUFFER_DESC bd = {};
    if (cb[0]) cb[0]->GetDesc(&bd);
    if (!cb[0] || bd.ByteWidth < static_cast<UINT>(11 + 5 * v.splits) * 16) {
        ok = false;
        why = "its constants are not the size its variant needs";
    } else if (!cb[13]) {
        ok = false;
        why = "no SceneP constants bound";
    }
    for (auto* b : cb)
        if (b) b->Release();
    ID3D11ShaderResourceView* srv[8] = {};
    ctx->PSGetShaderResources(0, 8, srv);
    const int needed = v.normalSlot() + 1;
    for (int i = 0; ok && i < needed; i++) {
        if (!srv[i]) {
            ok = false;
            why = "a texture of its variant is not bound";
        }
    }
    if (ok) {
        D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
        srv[3]->GetDesc(&d);
        if (d.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || !depthFormat(d.Format)) {
            ok = false;
            why = "its split atlas is not a 2D depth texture";
        }
    }
    for (auto* s : srv)
        if (s) s->Release();
    ID3D11SamplerState* samplers[4] = {};
    ctx->PSGetSamplers(0, 4, samplers);
    const int samplersNeeded = 2 + ((v.extras & 1) ? 1 : 0) + ((v.extras & 2) ? 1 : 0);
    for (int i = 0; ok && i < samplersNeeded; i++) {
        if (!samplers[i]) {
            ok = false;
            why = "a sampler of its variant is not bound";
        }
    }
    for (auto* s : samplers)
        if (s) s->Release();
    if (!ok) {
        engine->Release();
        return keepEngine(why);
    }

    Constants c = {};
    const float half = params.lightSize * 0.5f * 3.14159265f / 180.0f;
    c.soft0[0] = std::tan(half);
    c.soft0[1] = params.reach;
    c.soft0[2] = static_cast<float>(v.splits);
    c.soft0[3] = static_cast<float>(params.mode);
    c.soft1[0] = 0.5f;            // smallest filter radius: half a texel more than the engine's bilinear tap
    c.soft1[1] = 12.0f;           // search radius at most 12 texels (else a coarser split: faster, as good)
    c.soft1[2] = 8.0f;            // filter radius at most 8 texels (else a coarser split)
    // Depth epsilon: 0.75 steps of the 16-bit maps (the filter; the search takes twice). The maps
    // are drawn a step away from the light already: more lets light under casters near contact.
    c.soft1[3] = 0.75f / 65535.0f;
    if (!g_uploadedValid || memcmp(&c, &g_uploaded, sizeof(c)) != 0) {
        ctx->UpdateSubresource(g_constants, 0, nullptr, &c, 0, 0);
        g_uploaded = c;
        g_uploadedValid = true;
    }

    g_bound.active = true;
    g_bound.engine = engine; // the reference PSGetShader gave
    ctx->PSGetConstantBuffers(kSlot, 1, &g_bound.buffer);
    ctx->PSGetSamplers(kSlot, 1, &g_bound.sampler);
    ctx->PSSetShader(g_shaders[v.extras & 3], nullptr, 0);
    ctx->PSSetConstantBuffers(kSlot, 1, &g_constants);
    ctx->PSSetSamplers(kSlot, 1, &g_point);

    char line[160];
    snprintf(line, sizeof(line), "on: %d splits%s, light %.2f degrees", v.splits, kExtrasName[v.extras & 3], params.lightSize);
    if (strcmp(line, g_status) != 0) {
        const std::string key = std::string("|variant ") + std::to_string(v.splits) + "/" + std::to_string(v.extras) + "|";
        if (g_logged.find(key) == std::string::npos) {
            g_logged += key;
            TMVS_LOG("soft shadows: the mod's PCSS shader draws the engine's sun shadow pass (Tech3/DeferredShadowPssm_p, "
                     "%d splits%s), light %.2f degrees",
                     v.splits, kExtrasName[v.extras & 3], params.lightSize);
        }
        memcpy(g_status, line, sizeof(line));
    }
    return true;
}

void end(ID3D11DeviceContext* ctx) {
    if (!g_bound.active) return;
    hooks::Inside inside;
    ctx->PSSetShader(g_bound.engine, nullptr, 0);
    ctx->PSSetConstantBuffers(kSlot, 1, &g_bound.buffer);
    ctx->PSSetSamplers(kSlot, 1, &g_bound.sampler);
    safeRelease(g_bound.engine);
    safeRelease(g_bound.buffer);
    safeRelease(g_bound.sampler);
    g_bound.active = false;
}

const char* status() {
    return g_status;
}

} // namespace softshadow
} // namespace tmvs_mp
