// TM Clips: the picture to NV12 on the graphics card (src/clips/grabber.cpp). The frame (already at
// the video's size) goes into two targets the CPU reads back: luma, a quarter of the video's width
// with four Y values in each texel, and chroma, a quarter of the width and half the height with two
// Cb Cr pairs in each. A8R8G8B8 lies in memory as B G R A, hence the swizzles. BT.709, studio range,
// from the picture's (gamma-encoded) values, as players expect from an MP4.

sampler2D Frame : register(s0);
float4 Size : register(c0); // 1 / video width, 1 / video height

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

float3 rgbAt(float2 pixel) {
    return tex2Dlod(Frame, float4(pixel * Size.xy, 0.0, 0.0)).rgb;
}

float4 PS_ClipLuma(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {
    float2 cell = floor(vpos);
    float4 y;
    [unroll] for (int k = 0; k < 4; k++) {
        float2 p = float2(cell.x * 4.0 + k + 0.5, cell.y + 0.5);
        y[k] = (16.0 + 219.0 * dot(saturate(rgbAt(p)), kLuma)) / 255.0;
    }
    return y.zyxw;
}

float4 PS_ClipChroma(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {
    float2 cell = floor(vpos);
    float4 c;
    [unroll] for (int k = 0; k < 2; k++) {
        // The middle of the chroma sample's 2 x 2 pixels: the bilinear tap there is their average.
        float2 p = float2(cell.x * 4.0 + k * 2.0 + 1.0, cell.y * 2.0 + 1.0);
        float3 rgb = saturate(rgbAt(p));
        float l = dot(rgb, kLuma);
        c[k * 2] = (128.0 + 224.0 * (rgb.b - l) / 1.8556) / 255.0;
        c[k * 2 + 1] = (128.0 + 224.0 * (rgb.r - l) / 1.5748) / 255.0;
    }
    return c.zyxw;
}
