#pragma once
#include "engine_frame.h"
#include "settings.h"
#include <d3d11.h>
#include <string>

namespace tmvs_mp {

// Everything a pass changes on the immediate context, saved before and put back after, so the
// engine continues exactly where it was (same idea as the ImGui D3D11 backend's backup).
struct StateBackup {
    void save(ID3D11DeviceContext* ctx);
    void restore(ID3D11DeviceContext* ctx);

private:
    UINT scissorCount = 0, viewportCount = 0;
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4] = {};
    UINT sampleMask = 0;
    UINT stencilRef = 0;
    ID3D11DepthStencilState* dss = nullptr;
    ID3D11ShaderResourceView* psSrv[4] = {};
    ID3D11SamplerState* psSamplers[2] = {};
    ID3D11Buffer* psCb[2] = {};
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
};

class Renderer {
public:
    bool init(ID3D11Device* device);
    void release();
    // Size-dependent resources (copies, blur targets, depth view): dropped on ResizeBuffers.
    void releaseTargets();

    // The HDR stage in place of the engine's tone map: reads the HDR image and exposure the engine
    // bound for its own draw and its tone-map constants, writes the picture into its target (bloom
    // and the chosen curve). False: the engine's draw should run as usual.
    bool toneMap(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, ID3D11ShaderResourceView* hdr,
                 ID3D11ShaderResourceView* exposure, ID3D11Buffer* engineConstants, const Settings& s);

    // Grades the image of `target` in place.
    bool apply(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, const EngineView& view, const Settings& s);

    // The copy of the image the last apply() graded (for captures).
    ID3D11Texture2D* lastInput() const { return m_copy; }
    DXGI_FORMAT lastViewFormat() const { return m_copyViewFormat; }
    bool hasDepthView() const { return m_texCurrent[kTexDepth] >= 0 && m_tex[kTexDepth][m_texCurrent[kTexDepth]].srv; }
    DXGI_FORMAT depthFormat() const {
        return m_texCurrent[kTexDepth] >= 0 ? m_tex[kTexDepth][m_texCurrent[kTexDepth]].format : DXGI_FORMAT_UNKNOWN;
    }
    const std::string& lutError() const { return m_lutError; }

private:
    bool ensureCopy(ID3D11Texture2D* source, const D3D11_TEXTURE2D_DESC& desc, DXGI_FORMAT viewFormat);
    bool ensureBlur(UINT width, UINT height);
    bool ensureBloom(UINT width, UINT height);
    void setCommonState(ID3D11DeviceContext* ctx);
    ID3D11ShaderResourceView* textureView(int index, ID3D11Resource* res);
    void updateLut(const std::string& name);
    void pass(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv, UINT width, UINT height,
              ID3D11ShaderResourceView* const* srvs, UINT srvCount, float texelX, float texelY, float z = 0.0f,
              float w = 0.0f);

    ID3D11Device* m_device = nullptr;
    ID3D11VertexShader* m_vs = nullptr;
    ID3D11PixelShader* m_down = nullptr;
    ID3D11PixelShader* m_blurH = nullptr;
    ID3D11PixelShader* m_blurV = nullptr;
    ID3D11PixelShader* m_grade = nullptr;
    ID3D11PixelShader* m_bloomDown = nullptr;
    ID3D11PixelShader* m_bloomUp = nullptr;
    ID3D11PixelShader* m_toneMap = nullptr;
    ID3D11Buffer* m_toneCb = nullptr;
    ID3D11Buffer* m_gradeCb = nullptr;
    ID3D11Buffer* m_passCb = nullptr;
    ID3D11SamplerState* m_linear = nullptr;
    ID3D11SamplerState* m_point = nullptr;
    ID3D11RasterizerState* m_rs = nullptr;
    ID3D11DepthStencilState* m_dss = nullptr;
    ID3D11BlendState* m_blend = nullptr;

    ID3D11Texture2D* m_copy = nullptr;
    ID3D11ShaderResourceView* m_copySrv = nullptr;
    D3D11_TEXTURE2D_DESC m_copyDesc = {};
    DXGI_FORMAT m_copyViewFormat = DXGI_FORMAT_UNKNOWN;

    ID3D11Texture2D* m_blurTex[2] = {};
    ID3D11RenderTargetView* m_blurRtv[2] = {};
    ID3D11ShaderResourceView* m_blurSrv[2] = {};
    UINT m_blurW = 0, m_blurH2 = 0;

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

    std::string m_lutName;
    std::string m_lutError;
    ID3D11ShaderResourceView* m_lutSrv = nullptr;
    int m_lutSize = 0;
};

} // namespace tmvs_mp
