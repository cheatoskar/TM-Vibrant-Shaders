#pragma once
#include "effects.h"
#include "engine_frame.h"
#include "options.h"
#include <d3d11.h>
#include <string>

namespace tmshaders {
struct Settings;
}

namespace tmvs_mp {

// Everything a pass changes on the immediate context, saved before and put back after, so the
// engine continues exactly where it was (same idea as the ImGui D3D11 backend's backup).
struct StateBackup {
    void save(ID3D11DeviceContext* ctx);
    void restore(ID3D11DeviceContext* ctx);

private:
    static constexpr UINT kSrvs = 16, kCbs = 4, kSamplers = 4; // as many as the mod's passes use
    UINT scissorCount = 0, viewportCount = 0;
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4] = {};
    UINT sampleMask = 0;
    UINT stencilRef = 0;
    ID3D11DepthStencilState* dss = nullptr;
    ID3D11ShaderResourceView* psSrv[kSrvs] = {};
    ID3D11SamplerState* psSamplers[kSamplers] = {};
    ID3D11Buffer* psCb[kCbs] = {};
    ID3D11Buffer* vsCb[kCbs] = {};
    ID3D11ShaderResourceView* vsSrv[kSrvs] = {};
    ID3D11PixelShader* ps = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* ds = nullptr;
    ID3D11ClassInstance* psInst[256] = {};
    ID3D11ClassInstance* vsInst[256] = {};
    ID3D11ClassInstance* gsInst[256] = {};
    ID3D11ClassInstance* hsInst[256] = {};
    ID3D11ClassInstance* dsInst[256] = {};
    UINT psInstCount = 256, vsInstCount = 256, gsInstCount = 256, hsInstCount = 256, dsInstCount = 256;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
    UINT ibOffset = 0;
    ID3D11Buffer* vb = nullptr;
    UINT vbStride = 0, vbOffset = 0;
    ID3D11InputLayout* layout = nullptr;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
};

// What the engine handed over this frame (hooks.cpp): its textures (any may be null) and camera.
struct EngineView {
    ID3D11Resource* const* tex = nullptr; // kTexCount entries
    const Camera* camera = nullptr;       // null: not found this frame
    double time = 0.0;                    // seconds (animated effects)
    bool photo = false;
};

// The look of this frame: the shared TMNF settings plus what goes with them.
struct Look {
    const tmshaders::Settings* settings = nullptr;
    std::string lut;          // .cube file in the luts folder, "" = none
    float lutAmount = 1.0f;
    float sun[3] = {1, 1, 1}; // the engine's light colour (split toning)
    unsigned frame = 0;       // grain seed
    float split = 0.0f;       // compare: the game's image left of this (0 = off)
};

class Renderer {
public:
    bool init(ID3D11Device* device);
    void release();
    // Size-dependent resources (copies, bloom, engine views): dropped on ResizeBuffers.
    void releaseTargets();

    // The HDR stage in place of the engine's tone map: reads the HDR image and exposure the engine
    // bound for its own draw and its tone-map constants, runs the scene effects on them (effects.cpp)
    // and writes the picture into its target (exposure, bloom, grade, curve, vignette, look,
    // grain). False: the engine's draw runs.
    bool toneMap(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, ID3D11ShaderResourceView* hdr,
                 ID3D11ShaderResourceView* exposure, ID3D11Buffer* engineConstants, const Look& look, const EngineView& engine);
    // The scene effects ran in the last toneMap().
    bool effectsRan() const { return m_effectsRan; }
    Effects& effects() { return m_effects; }

    // The pass on the finished image of `target`, in place: chromatic aberration and sharpening,
    // the whole look when the HDR stage didn't run (hdrDone false), or a check view.
    bool apply(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, const EngineView& view, const Look& look,
               const Options& options, bool hdrDone);

    // The copy of the image the last apply() worked on (for captures).
    ID3D11Texture2D* lastInput() const { return m_copy; }
    DXGI_FORMAT lastViewFormat() const { return m_copyViewFormat; }
    bool hasDepthView() const { return m_texCurrent[kTexDepth] >= 0 && m_tex[kTexDepth][m_texCurrent[kTexDepth]].srv; }
    DXGI_FORMAT depthFormat() const {
        return m_texCurrent[kTexDepth] >= 0 ? m_tex[kTexDepth][m_texCurrent[kTexDepth]].format : DXGI_FORMAT_UNKNOWN;
    }
    const std::string& lutError() const { return m_lutError; }

private:
    bool ensureCopy(ID3D11Texture2D* source, const D3D11_TEXTURE2D_DESC& desc, DXGI_FORMAT viewFormat);
    bool ensureBloom(UINT width, UINT height);
    void setCommonState(ID3D11DeviceContext* ctx);
    void uploadLook(ID3D11DeviceContext* ctx, const Look& look);
    ID3D11ShaderResourceView* textureView(int index, ID3D11Resource* res);
    void updateLut(const std::string& name);
    void pass(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv, UINT width, UINT height,
              ID3D11ShaderResourceView* const* srvs, UINT srvCount, float texelX, float texelY, float z = 0.0f,
              float w = 0.0f);

    ID3D11Device* m_device = nullptr;
    ID3D11VertexShader* m_vs = nullptr;
    ID3D11PixelShader* m_grade = nullptr;
    ID3D11PixelShader* m_bloomDown = nullptr;
    ID3D11PixelShader* m_bloomUp = nullptr;
    ID3D11PixelShader* m_toneMap = nullptr;
    ID3D11Buffer* m_toneCb = nullptr;
    ID3D11Buffer* m_gradeCb = nullptr;
    ID3D11Buffer* m_passCb = nullptr;
    ID3D11Buffer* m_lookCb = nullptr;
    ID3D11SamplerState* m_linear = nullptr;
    ID3D11SamplerState* m_point = nullptr;
    ID3D11RasterizerState* m_rs = nullptr;
    ID3D11DepthStencilState* m_dss = nullptr;
    ID3D11BlendState* m_blend = nullptr;

    ID3D11Texture2D* m_copy = nullptr;
    ID3D11ShaderResourceView* m_copySrv = nullptr;
    D3D11_TEXTURE2D_DESC m_copyDesc = {};
    DXGI_FORMAT m_copyViewFormat = DXGI_FORMAT_UNKNOWN;

    // HDR bloom pyramid: [0] down levels, [1] up levels (half size and smaller).
    static constexpr int kBloomLevels = 6;
    ID3D11Texture2D* m_bloomTex[2][kBloomLevels] = {};
    ID3D11RenderTargetView* m_bloomRtv[2][kBloomLevels] = {};
    ID3D11ShaderResourceView* m_bloomSrv[2][kBloomLevels] = {};
    UINT m_bloomW[kBloomLevels] = {}, m_bloomH[kBloomLevels] = {};
    UINT m_bloomBaseW = 0, m_bloomBaseH = 0;

    // Views of the engine's textures, a few per kind: the engine alternates some of them between
    // frames (the exposure ping-pongs), and making a view every frame would be wasted work. A view
    // holds its texture, so a cached pointer can't be reused by another texture.
    struct TexView {
        ID3D11Resource* res = nullptr;
        ID3D11ShaderResourceView* srv = nullptr; // null: not readable
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };
    static constexpr int kViewsPerTexture = 4;
    TexView m_tex[kTexCount][kViewsPerTexture] = {};
    int m_texCurrent[kTexCount] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int m_texNext[kTexCount] = {};
    int m_texLogged[kTexCount] = {};

    Effects m_effects;
    bool m_effectsReady = false;
    bool m_effectsRan = false;

    std::string m_lutName;
    std::string m_lutError;
    ID3D11ShaderResourceView* m_lutSrv = nullptr;
    int m_lutSize = 0;
};

} // namespace tmvs_mp
