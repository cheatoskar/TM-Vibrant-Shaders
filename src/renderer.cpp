#include "renderer.h"
#include <d3dcompiler.h>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

namespace tmshaders {
namespace {

const char* g_hlslShaderSource = R"(
sampler2D sceneSampler : register(s0);

// c0: (exposure, contrast, colorTemp, vibrance)
float4 u_Param0 : register(c0);
// c1: (skyVibrance, foliageBoost, tonemapMode, unused)
float4 u_Param1 : register(c1);
// c2: (sunTint.r, sunTint.g, sunTint.b, rayExposure)
float4 u_Param2 : register(c2);
// c3: (bloomIntensity, bloomThreshold, anamorphicIntensity, rayDecay)
float4 u_Param3 : register(c3);
// c4: (flareTint.r, flareTint.g, flareTint.b, rayDensity)
float4 u_Param4 : register(c4);
// c5: (sunPos.x, sunPos.y, rcpW, rcpH)
float4 u_Param5 : register(c5);

float3 ACESFilm(float3 x)
{
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 RGBtoHSV(float3 c)
{
    float4 K = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    float4 p = lerp(float4(c.bg, K.wz), float4(c.gb, K.xy), step(c.b, c.g));
    float4 q = lerp(float4(p.xyw, c.r), float4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return float3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

float3 HSVtoRGB(float3 c)
{
    float4 K = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * lerp(K.xxx, saturate(p - K.xxx), c.y);
}

float4 main(float2 texcoord : TEXCOORD0) : COLOR0
{
    float3 baseColor = tex2D(sceneSampler, texcoord).rgb;

    // 1. Exposure adjustment
    float3 color = baseColor * u_Param0.x;

    // 2. Color Temperature (Warm golden sunlight like Sildur's)
    float temp = u_Param0.z;
    float3 tempShift = float3(1.0 + temp * 0.45, 1.0 + temp * 0.12, 1.0 - temp * 0.45);
    color *= tempShift;

    // 3. Highlight / Shadow Split Toning
    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    float3 splitTone = lerp(float3(0.95, 0.97, 1.03), u_Param2.xyz, smoothstep(0.2, 0.8, luma));
    color *= splitTone;

    // 4. Selective Foliage & Sky Vibrance (BSL & Sildurs look)
    float3 hsv = RGBtoHSV(color);
    float skyMask = smoothstep(0.48, 0.58, hsv.x) * (1.0 - smoothstep(0.68, 0.76, hsv.x));
    hsv.y += skyMask * u_Param1.x;

    float foliageMask = smoothstep(0.18, 0.26, hsv.x) * (1.0 - smoothstep(0.42, 0.50, hsv.x));
    hsv.y += foliageMask * u_Param1.y;

    // Smart Vibrance
    float maxCol = max(color.r, max(color.g, color.b));
    float minCol = min(color.r, min(color.g, color.b));
    float sat = maxCol - minCol;
    hsv.y += (1.0 - sat) * u_Param0.w * 0.5;

    color = HSVtoRGB(saturate(hsv));

    // 5. ACES Filmic Tonemapping
    color = ACESFilm(color);

    // 6. Contrast S-Curve
    float contrast = u_Param0.y;
    color = saturate(color);
    color = pow(color, contrast.xxx);
    color = color * color * (3.0 - 2.0 * color);

    // 7. Volumetric Sun Rays (Screen-Space Crepuscular Rays)
    float rayExp = u_Param2.w;
    if (rayExp > 0.01)
    {
        float2 sunPos = u_Param5.xy;
        float2 delta = (texcoord - sunPos) * (1.0 / 16.0) * u_Param4.w;
        float2 rayUV = texcoord;
        float3 rayAccum = 0.0;
        float decay = 1.0;
        float rayDecayVal = u_Param3.w;

        for (int i = 0; i < 16; i++)
        {
            rayUV -= delta;
            float3 sampleCol = tex2D(sceneSampler, rayUV).rgb;
            float sampleLuma = dot(sampleCol, float3(0.299, 0.587, 0.114));
            float bright = saturate(sampleLuma - 0.70) * 3.33;
            rayAccum += bright * u_Param2.xyz * decay;
            decay *= rayDecayVal;
        }
        color += rayAccum * (rayExp * 0.05);
    }

    // 8. Cinematic Bloom & Anamorphic Lens Flare Streaks (IterationT)
    float bloomInt = u_Param3.x;
    float anamorphicInt = u_Param3.z;
    float bloomThresh = u_Param3.y;

    if (bloomInt > 0.01 || anamorphicInt > 0.01)
    {
        float2 rcpSize = u_Param5.zw;
        float3 bloom = 0.0;
        float3 flare = 0.0;

        float2 offsets[4] = {
            float2( 2.5,  2.5) * rcpSize,
            float2(-2.5,  2.5) * rcpSize,
            float2( 2.5, -2.5) * rcpSize,
            float2(-2.5, -2.5) * rcpSize
        };
        for (int b = 0; b < 4; b++)
        {
            float3 sampleCol = tex2D(sceneSampler, texcoord + offsets[b]).rgb;
            float sampleLuma = dot(sampleCol, float3(0.2126, 0.7152, 0.0722));
            float bright = max(0.0, sampleLuma - bloomThresh);
            bloom += sampleCol * bright;
        }
        bloom *= 0.25 * bloomInt;

        if (anamorphicInt > 0.01)
        {
            for (int f = -6; f <= 6; f++)
            {
                float2 flareUV = texcoord + float2(float(f) * 12.0 * rcpSize.x, 0.0);
                float3 sCol = tex2D(sceneSampler, flareUV).rgb;
                float sLuma = dot(sCol, float3(0.2126, 0.7152, 0.0722));
                float bVal = max(0.0, sLuma - (bloomThresh - 0.05));
                float weight = 1.0 - abs(float(f)) / 7.0;
                flare += sCol * bVal * weight;
            }
            flare = (flare / 7.0) * anamorphicInt * u_Param4.xyz;
        }

        color += bloom + flare;
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
    // Recreated on next frame
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
    float param0[4] = { settings.exposure, settings.contrast, settings.colorTemp, settings.vibrance };
    float param1[4] = { settings.skyVibrance, settings.foliageBoost, static_cast<float>(settings.tonemapMode), 1.0f };
    float param2[4] = { settings.sunTint[0], settings.sunTint[1], settings.sunTint[2], settings.enableSunRays ? settings.rayExposure : 0.0f };
    float param3[4] = { settings.enableBloom ? settings.bloomIntensity : 0.0f, settings.bloomThreshold, settings.enableBloom ? settings.anamorphicIntensity : 0.0f, settings.rayDecay };
    float param4[4] = { settings.flareTint[0], settings.flareTint[1], settings.flareTint[2], settings.rayDensity };
    float param5[4] = { settings.sunPos[0], settings.sunPos[1], 1.0f / desc.Width, 1.0f / desc.Height };

    device->SetPixelShaderConstantF(0, param0, 1);
    device->SetPixelShaderConstantF(1, param1, 1);
    device->SetPixelShaderConstantF(2, param2, 1);
    device->SetPixelShaderConstantF(3, param3, 1);
    device->SetPixelShaderConstantF(4, param4, 1);
    device->SetPixelShaderConstantF(5, param5, 1);

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
