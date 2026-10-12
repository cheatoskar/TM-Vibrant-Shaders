#include "engine_frame.h"
#include <cmath>
#include <cstring>

namespace tmvs_mp {
namespace {

bool invert(const float m[16], float out[16]) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] -
             m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] +
             m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] -
             m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] +
              m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] +
             m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] -
             m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] +
             m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] -
              m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] -
             m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] +
             m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] -
              m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] +
              m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] +
             m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] -
             m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] +
              m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] -
              m[8] * m[2] * m[5];
    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!std::isfinite(det) || std::fabs(det) < 1e-20f) return false;
    for (int i = 0; i < 16; i++) out[i] = inv[i] / det;
    return true;
}

float rowDot(const float m[16], int row, const float v[4]) {
    return m[row * 4] * v[0] + m[row * 4 + 1] * v[1] + m[row * 4 + 2] * v[2] + m[row * 4 + 3] * v[3];
}

} // namespace

const char* engineTextureName(int index) {
    static const char* const kNames[kTexCount] = {"hdr", "exposure", "depth", "normal", "diffuse", "specular", "lightmask",
                                                   "motion"};
    return index >= 0 && index < kTexCount ? kNames[index] : "?";
}

DXGI_FORMAT readableFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_UNKNOWN; // typed depth: no view possible
    default: return f;
    }
}

bool decodeCamera(Camera& c) {
    float vp[16];
    memcpy(vp, c.raw + kSceneVWorldPrCamera, sizeof(vp));
    memcpy(c.worldToCamera, c.raw + kSceneVWorldToCamera, sizeof(c.worldToCamera));
    float eye4[4];
    memcpy(eye4, c.raw + kSceneVEyeInWorld, sizeof(eye4));
    memcpy(c.eye, eye4, sizeof(c.eye));
    memcpy(c.sunColour, c.raw + kSceneVLightDirRgb0, sizeof(c.sunColour));
    memcpy(c.minMaxZ, c.raw + kSceneVCameraMinMaxZ, sizeof(c.minMaxZ));

    // The eye projects to w = 0 with the right reading of the matrix (rows or columns).
    const float e[4] = {c.eye[0], c.eye[1], c.eye[2], 1.0f};
    float vpT[16];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) vpT[r * 4 + k] = vp[k * 4 + r];
    const float wRows = std::fabs(rowDot(vp, 3, e)), wCols = std::fabs(rowDot(vpT, 3, e));
    c.transposed = wCols < wRows;
    memcpy(c.viewProj, c.transposed ? vpT : vp, sizeof(c.viewProj));
    const float scale = std::fabs(c.eye[0]) + std::fabs(c.eye[1]) + std::fabs(c.eye[2]) + 1.0f;
    c.valid = std::fmin(wRows, wCols) < 1e-3f * scale && invert(c.viewProj, c.invViewProj);

    // Camera projection, stored like WorldPrCamera.
    float p[16];
    memcpy(p, c.raw + kSceneVCameraProjection, sizeof(p));
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) c.projection[r * 4 + k] = c.transposed ? p[k * 4 + r] : p[r * 4 + k];
    memcpy(c.ambient, c.raw + kSceneVLightAmbientLinear, sizeof(c.ambient));

    // The sun from the shadow map's matrix (row 2 runs along the light): the lighting constants
    // (SceneP) replace it when they were seen. The sign: the sun is the end the light comes from;
    // a capture at sunset (sun colour orange) had row 2 pointing 13 degrees up.
    if (!c.sunFromSceneP) {
        float s[16];
        memcpy(s, c.raw + kSceneVShadowLDir, sizeof(s));
        float d[3] = {s[8], s[9], s[10]};
        if (c.transposed) {
            d[0] = s[2];
            d[1] = s[6];
            d[2] = s[10];
        }
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        c.sunDirectionKnown = std::isfinite(len) && len > 1e-12f;
        if (c.sunDirectionKnown)
            for (int i = 0; i < 3; i++) c.sunDirection[i] = d[i] / len;
    }
    return c.valid;
}

bool decodeSceneP(Camera& c) {
    if (!c.hasSceneP) return false;
    float l[4];
    memcpy(l, c.rawP + kScenePLightDirInWorld0, sizeof(l));
    const float len = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    if (!std::isfinite(len) || len < 1e-6f) return false;
    // The shader lights with N.(-L): L runs from the sun into the scene.
    for (int i = 0; i < 3; i++) c.sunDirection[i] = -l[i] / len;
    c.sunDirectionKnown = true;
    c.sunFromSceneP = true;
    memcpy(c.ambient, c.rawP + kScenePLightAmbientLinear, sizeof(c.ambient));
    memcpy(c.sunColour, c.rawP + kScenePLightDirRgbLinear0, sizeof(c.sunColour));
    memcpy(c.fog, c.rawP + kScenePFog, sizeof(c.fog));
    return true;
}

} // namespace tmvs_mp
