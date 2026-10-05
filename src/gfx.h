#pragma once
#include <d3d9.h>
#include <string>

// Small Direct3D 9 helpers shared by the in-game plugin and the offline previewer.
namespace tmshaders {
namespace gfx {

template <typename T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// Compiles an HLSL source string. Errors go to the log; returns nullptr on failure.
IDirect3DPixelShader9* compilePixelShader(IDirect3DDevice9* device, const std::string& source, const char* entry,
                                          const char* name, std::string* errors = nullptr);
IDirect3DVertexShader9* compileVertexShader(IDirect3DDevice9* device, const std::string& source, const char* entry,
                                            const char* name, std::string* errors = nullptr);

// A 2D render-target texture with cached level-0 surface.
struct Target {
    IDirect3DTexture9* texture = nullptr;
    IDirect3DSurface9* surface = nullptr;
    UINT width = 0;
    UINT height = 0;
    D3DFORMAT format = D3DFMT_UNKNOWN;

    bool create(IDirect3DDevice9* device, UINT w, UINT h, D3DFORMAT fmt);
    void destroy();
    bool valid() const { return surface != nullptr; }
};

// Full-screen pass helper: vs_3_0 quad (ps_3_0 requires a vs_3_0 vertex shader).
class Quad {
public:
    bool init(IDirect3DDevice9* device);
    void destroy();
    // Draws to the currently bound render target, covering it entirely.
    void draw(IDirect3DDevice9* device, UINT targetWidth, UINT targetHeight);

private:
    IDirect3DVertexShader9* m_vs = nullptr;
    IDirect3DVertexDeclaration9* m_decl = nullptr;
};

} // namespace gfx
} // namespace tmshaders
