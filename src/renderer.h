#pragma once
#include <d3d9.h>
#include "config.h"

namespace tmshaders {

class Renderer {
public:
    static Renderer& get();

    void init(IDirect3DDevice9* device);
    void preReset();
    void postReset();
    void release();
    void render(IDirect3DDevice9* device, const ShaderSettings& settings);

private:
    Renderer() = default;
    ~Renderer() { release(); }

    bool compileShader(IDirect3DDevice9* device);
    void updateSurfaces(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format);

    IDirect3DPixelShader9* m_pixelShader = nullptr;
    IDirect3DTexture9* m_sceneTexture = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;
    D3DFORMAT m_format = D3DFMT_UNKNOWN;
    bool m_initialized = false;
};

} // namespace tmshaders
