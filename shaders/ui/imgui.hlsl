// Dear ImGui through the RHI (plan F2 Task 6; engine/ui/ImGuiRenderer.cpp). Vertex = ImDrawVert
// {float2 pos, float2 uv, RGBA8 colour}; one texture per draw.
// Binding convention (F1): space2 = material (the ImGui texture), space3 = draw; index unique per space.
// ImGui colours are sRGB-encoded: on an sRGB target (flags.x = 1) they are decoded to linear first, so the
// target's encode gives back the authored colour.
cbuffer UiDraw : register(b0, space3)
{
    float4 scaleTranslate; // pixel -> NDC: xy = scale, zw = translate (NDC +Y up)
    uint4 flags;           // x = 1: sRGB target
};
Texture2D<float4> uiTexture : register(t0, space2);
SamplerState uiSampler : register(s1, space2);

struct VSInput
{
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

float3 srgbToLinear(float3 c)
{
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    o.position = float4(input.position * scaleTranslate.xy + scaleTranslate.zw, 0.0, 1.0);
    o.uv = input.uv;
    o.color = input.color;
    if (flags.x != 0u)
    {
        o.color.rgb = srgbToLinear(input.color.rgb);
    }
    return o;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    return input.color * uiTexture.Sample(uiSampler, input.uv);
}
