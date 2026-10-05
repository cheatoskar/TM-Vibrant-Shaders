#include "renderer.h"
#include <d3dcompiler.h>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

namespace tmshaders {
namespace {

const char* g_hlslShaderSource = R"(
sampler2D sceneSampler : register(s0);

// c0: (exposure, contrast, saturation, sharpness)
float4 u_ColorSettings : register(c0);
// c1: (bloomIntensity, bloomThreshold, flareIntensity, vignette)
float4 u_EffectSettings : register(c1);
// c2: (sunRayIntensity, sunRayDecay, warmth, clarity)
float4 u_LightSettings : register(c2);
// c3: (skyBoost, foliageBoost, roadSheen, unused)
float4 u_GradingSettings : register(c3);
// c4: (sunPos.x, sunPos.y, rcpW, rcpH)
float4 u_ScreenSettings : register(c4);

// Linear-space Filmic Tone Mapping (prevents clipping, rich rolloff)
float3 FilmicToneMap(float3 col)
{
    float3 x = max(0.0, col - 0.004);
    return (x * (6.2 * x + 0.5)) / (x * (6.2 * x + 1.7) + 0.06);
}

// RGB to HSV
float3 RGBtoHSV(float3 c)
{
    float4 K = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    float4 p = lerp(float4(c.bg, K.wz), float4(c.gb, K.xy), step(c.b, c.g));
    float4 q = lerp(float4(p.xyw, c.r), float4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return float3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

// HSV to RGB
float3 HSVtoRGB(float3 c)
{
    float4 K = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * lerp(K.xxx, saturate(p - K.xxx), c.y);
}

float4 main(float2 texcoord : TEXCOORD0) : COLOR0
{
    float2 rcpSize = u_ScreenSettings.zw;

    // 1. Sample Center & Neighbors for FidelityFX Contrast-Adaptive Sharpening (CAS)
    float3 colC = tex2D(sceneSampler, texcoord).rgb;
    float3 colT = tex2D(sceneSampler, texcoord + float2(0.0, -rcpSize.y)).rgb;
    float3 colB = tex2D(sceneSampler, texcoord + float2(0.0,  rcpSize.y)).rgb;
    float3 colL = tex2D(sceneSampler, texcoord + float2(-rcpSize.x, 0.0)).rgb;
    float3 colR = tex2D(sceneSampler, texcoord + float2( rcpSize.x, 0.0)).rgb;

    // Convert to linear space for physically accurate lighting
    float3 linC = pow(saturate(colC), 2.2);
    float3 linT = pow(saturate(colT), 2.2);
    float3 linB = pow(saturate(colB), 2.2);
    float3 linL = pow(saturate(colL), 2.2);
    float3 linR = pow(saturate(colR), 2.2);

    // FidelityFX CAS: compute local contrast and apply high-frequency detail boost
    float sharpAmount = u_ColorSettings.w;
    float3 minNeighbor = min(linC, min(min(linT, linB), min(linL, linR)));
    float3 maxNeighbor = max(linC, max(max(linT, linB), max(linL, linR)));
    float3 amp = saturate(min(minNeighbor, 2.0 - maxNeighbor) / max(maxNeighbor, 0.001));
    float3 w = -sqrt(amp) * (sharpAmount * 0.18);
    float3 color = (linC + (linT + linB + linL + linR) * w) / (1.0 + 4.0 * w);
    color = max(0.0, color);

    // 2. Contact Shading & Micro-AO (deepens crevices under tires, barriers, curb edges)
    float clarity = u_LightSettings.w;
    if (clarity > 0.01)
    {
        float lumaC = dot(linC, float3(0.2126, 0.7152, 0.0722));
        float lumaAvg = (dot(linT, float3(0.2126, 0.7152, 0.0722)) +
                         dot(linB, float3(0.2126, 0.7152, 0.0722)) +
                         dot(linL, float3(0.2126, 0.7152, 0.0722)) +
                         dot(linR, float3(0.2126, 0.7152, 0.0722))) * 0.25;
        float crevice = lumaAvg - lumaC;
        if (crevice > 0.002)
        {
            color *= (1.0 - saturate(crevice * 8.0) * clarity * 0.35);
        }
    }

    // 3. Road Sheen (TM2020 Tarmac Specular Fresnel in lower screen half)
    float roadSheen = u_GradingSettings.z;
    if (roadSheen > 0.01 && texcoord.y > 0.40)
    {
        float grazing = pow(texcoord.y, 2.2) * (1.0 - abs(texcoord.x - 0.5) * 1.4);
        float roadMask = smoothstep(0.40, 0.95, texcoord.y);
        color += float3(0.03, 0.04, 0.06) * saturate(grazing) * roadMask * roadSheen;
    }

    // 4. White Balance / Warmth (Subtle Kelvin adjustment, NOT bleaching yellow)
    float warmth = u_LightSettings.z;
    float3 wb = float3(1.0 + warmth * 0.08, 1.0 + warmth * 0.02, 1.0 - warmth * 0.08);
    color *= wb;

    // 5. Exposure
    color *= u_ColorSettings.x;

    // 6. Selective Foliage & Sky Vibrance (preserves road concrete neutral)
    float3 hsv = RGBtoHSV(pow(saturate(color), 1.0 / 2.2));
    float skyMask = smoothstep(0.50, 0.58, hsv.x) * (1.0 - smoothstep(0.68, 0.76, hsv.x));
    hsv.y += skyMask * u_GradingSettings.x; // Sky blue boost

    float foliageMask = smoothstep(0.18, 0.26, hsv.x) * (1.0 - smoothstep(0.42, 0.50, hsv.x));
    hsv.y += foliageMask * u_GradingSettings.y; // Grass boost

    // Overall Saturation
    float satMul = u_ColorSettings.z;
    hsv.y = saturate(hsv.y * satMul);
    color = pow(HSVtoRGB(hsv), 2.2);

    // 7. Volumetric Sun Rays (Strictly bounded to sky; never bleaches road!)
    float rayIntensity = u_LightSettings.x;
    if (rayIntensity > 0.01)
    {
        // Only cast in upper portion of screen
        float skyBound = saturate(1.0 - texcoord.y * 2.2);
        if (skyBound > 0.01)
        {
            float2 sunPos = u_ScreenSettings.xy;
            float2 delta = (texcoord - sunPos) * (1.0 / 16.0);
            float2 rayUV = texcoord;
            float3 rayAccum = 0.0;
            float decay = 1.0;
            float rayDecayVal = u_LightSettings.y;

            for (int i = 0; i < 16; i++)
            {
                rayUV -= delta;
                float3 sCol = pow(saturate(tex2D(sceneSampler, rayUV).rgb), 2.2);
                float sLuma = dot(sCol, float3(0.2126, 0.7152, 0.0722));
                float bright = max(0.0, sLuma - 0.75) * 2.5;
                rayAccum += bright * float3(1.0, 0.94, 0.82) * decay;
                decay *= rayDecayVal;
            }
            color += rayAccum * (rayIntensity * 0.06) * skyBound;
        }
    }

    // 8. Emissive Bloom & Anamorphic Flares (Strict threshold on real lights only)
    float bloomInt = u_EffectSettings.x;
    float bloomThresh = u_EffectSettings.y;
    float flareInt = u_EffectSettings.z;

    if (bloomInt > 0.01 || flareInt > 0.01)
    {
        float3 bloom = 0.0;
        float3 flare = 0.0;

        // Multi-tap soft bloom on emissive highlights
        if (bloomInt > 0.01)
        {
            float2 bOffsets[4] = {
                float2( 3.0,  3.0) * rcpSize,
                float2(-3.0,  3.0) * rcpSize,
                float2( 3.0, -3.0) * rcpSize,
                float2(-3.0, -3.0) * rcpSize
            };
            for (int b = 0; b < 4; b++)
            {
                float3 bCol = pow(saturate(tex2D(sceneSampler, texcoord + bOffsets[b]).rgb), 2.2);
                float bLuma = dot(bCol, float3(0.2126, 0.7152, 0.0722));
                float bright = max(0.0, bLuma - bloomThresh);
                bloom += bCol * bright;
            }
            bloom *= 0.25 * bloomInt;
        }

        // Horizontal Anamorphic Flare Streaks
        if (flareInt > 0.01)
        {
            for (int f = -6; f <= 6; f++)
            {
                float2 fUV = texcoord + float2(float(f) * 14.0 * rcpSize.x, 0.0);
                float3 fCol = pow(saturate(tex2D(sceneSampler, fUV).rgb), 2.2);
                float fLuma = dot(fCol, float3(0.2126, 0.7152, 0.0722));
                float fBright = max(0.0, fLuma - (bloomThresh - 0.04));
                float fWeight = 1.0 - abs(float(f)) / 7.0;
                flare += fCol * fBright * fWeight;
            }
            flare = (flare / 7.0) * flareInt * float3(0.5, 0.8, 1.0); // Cyan/blue anamorphic streak
        }

        color += bloom + flare;
    }

    // 9. Filmic Tone Mapping (Converts linear HDR back to compressed display range)
    color = FilmicToneMap(color);

    // 10. Contrast S-Curve in display space
    float contrast = u_ColorSettings.y;
    color = saturate(color);
    color = pow(color, contrast.xxx);

    // 11. Subtle Cinematic Vignette
    float vigAmount = u_EffectSettings.w;
    if (vigAmount > 0.01)
    {
        float2 vigUV = texcoord - 0.5;
        float vigDist = dot(vigUV, vigUV);
        float vig = saturate(1.0 - vigDist * vigAmount * 1.8);
        color *= vig;
    }

    return float4(saturate(color), 1.0);
}
)";

struct ScreenVertex {
    float x, y, z, rhw;
    float u, v;
};

#define D3DFVF_SCREENVERTEX (D3DFVF_XYZRHW | D3DFVF_TEX1)

} // namespace

Renderer& Renderer::get() {
    static Renderer instance;
    return instance;
}

void Renderer::init(IDirect3DDevice9* device) {
    if (m_initialized) return;
    if (compileShader(device)) {
        m_initialized = true;
    }
}

bool Renderer::compileShader(IDirect3DDevice9* device) {
    if (m_pixelShader) {
        m_pixelShader->Release();
        m_pixelShader = nullptr;
    }

    ID3DBlob* shaderBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    HRESULT hr = D3DCompile(
        g_hlslShaderSource,
        strlen(g_hlslShaderSource),
        "TMVibrantShader",
        nullptr,
        nullptr,
        "main",
        "ps_3_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &shaderBlob,
        &errorBlob
    );

    if (FAILED(hr) || !shaderBlob) {
        if (errorBlob) {
            OutputDebugStringA(static_cast<const char*>(errorBlob->GetBufferPointer()));
            errorBlob->Release();
        }
        return false;
    }

    hr = device->CreatePixelShader(
        reinterpret_cast<const DWORD*>(shaderBlob->GetBufferPointer()),
        &m_pixelShader
    );
    shaderBlob->Release();
    if (errorBlob) errorBlob->Release();

    return SUCCEEDED(hr) && (m_pixelShader != nullptr);
}

void Renderer::updateSurfaces(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format) {
    if (m_sceneTexture && (m_width != width || m_height != height || m_format != format)) {
        m_sceneTexture->Release();
        m_sceneTexture = nullptr;
    }

    if (!m_sceneTexture) {
        m_width = width;
        m_height = height;
        m_format = format;
        device->CreateTexture(m_width, m_height, 1, D3DUSAGE_RENDERTARGET, m_format, D3DPOOL_DEFAULT, &m_sceneTexture, nullptr);
    }
}

void Renderer::preReset() {
    if (m_sceneTexture) {
        m_sceneTexture->Release();
        m_sceneTexture = nullptr;
    }
    if (m_pixelShader) {
        m_pixelShader->Release();
        m_pixelShader = nullptr;
    }
    m_initialized = false;
}

void Renderer::postReset() {
}

void Renderer::release() {
    preReset();
}

void Renderer::render(IDirect3DDevice9* device, const ShaderSettings& settings) {
    if (!settings.enabled) return;

    if (!m_initialized || !m_pixelShader) {
        init(device);
        if (!m_initialized || !m_pixelShader) return;
    }

    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(device->GetRenderTarget(0, &backBuffer)) || !backBuffer) return;

    D3DSURFACE_DESC desc;
    backBuffer->GetDesc(&desc);

    updateSurfaces(device, desc.Width, desc.Height, desc.Format);
    if (!m_sceneTexture) {
        backBuffer->Release();
        return;
    }

    IDirect3DSurface9* sceneSurface = nullptr;
    if (FAILED(m_sceneTexture->GetSurfaceLevel(0, &sceneSurface)) || !sceneSurface) {
        backBuffer->Release();
        return;
    }

    // Copy game image to sceneTexture
    device->StretchRect(backBuffer, nullptr, sceneSurface, nullptr, D3DTEXF_NONE);
    sceneSurface->Release();

    // Preserve original render state
    IDirect3DStateBlock9* stateBlock = nullptr;
    if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) && stateBlock) {
        stateBlock->Capture();
    }

    // Set Shader Constants
    // c0: (exposure, contrast, saturation, sharpness)
    float param0[4] = { settings.exposure, settings.contrast, settings.saturation, settings.sharpness };
    // c1: (bloomIntensity, bloomThreshold, flareIntensity, vignette)
    float param1[4] = { settings.enableBloom ? settings.bloomIntensity : 0.0f, settings.bloomThreshold, settings.enableFlares ? settings.flareIntensity : 0.0f, settings.vignette };
    // c2: (sunRayIntensity, sunRayDecay, warmth, clarity)
    float param2[4] = { settings.enableSunRays ? settings.sunRayIntensity : 0.0f, settings.sunRayDecay, settings.warmth, settings.clarity };
    // c3: (skyBoost, foliageBoost, roadSheen, unused)
    float param3[4] = { settings.skyBoost, settings.foliageBoost, settings.roadSheen, 0.0f };
    // c4: (sunPos.x, sunPos.y, rcpW, rcpH)
    float param4[4] = { settings.sunPos[0], settings.sunPos[1], 1.0f / desc.Width, 1.0f / desc.Height };

    device->SetPixelShaderConstantF(0, param0, 1);
    device->SetPixelShaderConstantF(1, param1, 1);
    device->SetPixelShaderConstantF(2, param2, 1);
    device->SetPixelShaderConstantF(3, param3, 1);
    device->SetPixelShaderConstantF(4, param4, 1);

    // Setup rendering states
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);

    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    device->SetTexture(0, m_sceneTexture);
    device->SetPixelShader(m_pixelShader);
    device->SetRenderTarget(0, backBuffer);

    // Half-pixel offset for D3D9 texel alignment
    float w = static_cast<float>(desc.Width);
    float h = static_cast<float>(desc.Height);
    ScreenVertex verts[4] = {
        { -0.5f,     -0.5f,     0.0f, 1.0f, 0.0f, 0.0f },
        { w - 0.5f, -0.5f,     0.0f, 1.0f, 1.0f, 0.0f },
        { -0.5f,     h - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f },
        { w - 0.5f, h - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f },
    };

    device->SetFVF(D3DFVF_SCREENVERTEX);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(ScreenVertex));

    // Restore render states
    if (stateBlock) {
        stateBlock->Apply();
        stateBlock->Release();
    }

    backBuffer->Release();
}

} // namespace tmshaders
