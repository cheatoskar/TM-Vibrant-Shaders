#pragma once
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

class Renderer {
public:
    bool init(ID3D11Device* device);
    void release();
    // Size-dependent resources (copies, blur targets, depth view): dropped on ResizeBuffers.
    void releaseTargets();

    // Grades the image of `target` in place. `depth` is the scene's depth buffer or null.
    bool apply(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, ID3D11Resource* depth, const Settings& s);

    // The copy of the image the last apply() graded (for captures).
    ID3D11Texture2D* lastInput() const { return m_copy; }
    bool hasDepthView() const { return m_depthSrv != nullptr; }
    DXGI_FORMAT depthFormat() const { return m_depthFormat; }
    const std::string& lutError() const { return m_lutError; }

private:
    bool ensureCopy(ID3D11Texture2D* source, const D3D11_TEXTURE2D_DESC& desc, DXGI_FORMAT viewFormat);
    bool ensureBlur(UINT width, UINT height);
    ID3D11ShaderResourceView* depthView(ID3D11Resource* depth);
    void updateLut(const std::string& name);
    void pass(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv, UINT width, UINT height,
              ID3D11ShaderResourceView* const* srvs, UINT srvCount, float texelX, float texelY);

    ID3D11Device* m_device = nullptr;
    ID3D11VertexShader* m_vs = nullptr;
    ID3D11PixelShader* m_down = nullptr;
    ID3D11PixelShader* m_blurH = nullptr;
    ID3D11PixelShader* m_blurV = nullptr;
    ID3D11PixelShader* m_grade = nullptr;
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

    ID3D11Resource* m_depthResource = nullptr; // not owned: identity of the cached view
    ID3D11ShaderResourceView* m_depthSrv = nullptr;
    DXGI_FORMAT m_depthFormat = DXGI_FORMAT_UNKNOWN;
    bool m_depthTried = false;

    std::string m_lutName;
    std::string m_lutError;
    ID3D11ShaderResourceView* m_lutSrv = nullptr;
    int m_lutSize = 0;
};

} // namespace tmvs_mp
