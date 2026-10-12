#pragma once
#include "renderer.h"
#include "shaderdb.h"
#include <d3d11.h>
#include <string>

namespace tmvs_mp {

// One-frame capture for working offline: <data>\captures\capture_NNN\ with
//   trace.txt        every draw of the frame: shader, role, render target, depth, inputs
//   hdr_tN.dds       the tone map's inputs (the engine's HDR image and what else it reads)
//   input.dds/.bmp   the image the mod graded (before), output.dds/.bmp the frame at Present
//   engine_*.dds     what the mod read from the engine: hdr, exposure, depth, normal, diffuse and
//                    specular light, light mask, motion
//   camera.txt       the engine camera (decoded), scenev.bin its raw SceneV constants
//   view.txt         the format the game's image was drawn with (for tools/preview.cpp)
namespace capture {

void request();
bool active();            // this frame is being captured
const std::wstring& lastFolder();

void traceDraw(ID3D11DeviceContext* ctx, const char* shader, Role role, unsigned index);
void onToneMap(ID3D11Device* device, ID3D11DeviceContext* ctx);
void onGraded(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* input, DXGI_FORMAT viewFormat,
              const EngineView& engine);
void onPresent(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Resource* backBuffer);
void endFrame();

} // namespace capture
} // namespace tmvs_mp
