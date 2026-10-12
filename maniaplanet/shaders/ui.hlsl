// The F8 menu on ManiaPlanet's sRGB back buffer (shader model 4).
// ImGui's colours, pictures and blending are display values. Drawn straight into an sRGB view they
// were encoded a second time: the whole menu came out pale and too bright. So ImGui draws into its
// own UNORM layer (its blending onto transparent black leaves it premultiplied), and this pass lays
// that layer over a copy of the frame in display values, as a UNORM back buffer would have it.
// entries: PS_UiComposite:ps_4_0

Texture2D g_frame : register(t0); // the frame, through an sRGB view (reads linear)
Texture2D g_ui : register(t1);    // the menu layer: display values, premultiplied alpha

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float3 toGamma(float3 c)
{
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float3 toLinear(float3 c)
{
    c = saturate(c);
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float4 PS_UiComposite(VSOut i) : SV_Target
{
    int3 p = int3(i.pos.xy, 0);
    float4 frame = g_frame.Load(p);
    float4 ui = g_ui.Load(p);
    float3 c = ui.rgb + toGamma(frame.rgb) * (1.0 - ui.a);
    // The target is an sRGB view: it encodes again what we hand it.
    return float4(toLinear(c), frame.a);
}
