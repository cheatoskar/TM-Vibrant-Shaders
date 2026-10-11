#pragma once
#include "shaderdb.h"
#include <d3d11.h>
#include <string>

namespace tmvs_mp {

// One-frame capture for working offline: <data>\captures\capture_NNN\ with
//   trace.txt        every draw of the frame: shader, role, render target, depth, inputs
//   hdr_tN.dds       the tone map's inputs (the engine's HDR image and what else it reads)
//   input.dds/.bmp   the image the mod graded (before), output.dds/.bmp the frame at Present
//   depth.dds        the scene depth the mod found
namespace capture {

void request();
bool active();            // this frame is being captured
const std::wstring& lastFolder();

void traceDraw(ID3D11DeviceContext* ctx, const char* shader, Role role, unsigned index);
void onToneMap(ID3D11Device* device, ID3D11DeviceContext* ctx);
void onGraded(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* input, ID3D11Resource* depth);
void onPresent(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Resource* backBuffer);
void endFrame();

} // namespace capture
} // namespace tmvs_mp
