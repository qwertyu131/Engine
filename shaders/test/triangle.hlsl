// Trivial test shader for tools/shaderc (plan F1 Task 2): one VS + one FS through all 7 targets.
// Binding convention: register(<type><index>, space<S>), index unique per space across b/t/s/u.
//   space0 = frame, space2 = material.

cbuffer Frame : register(b0, space0)
{
    float4x4 viewProj;
};

cbuffer Material : register(b0, space2)
{
    float4 baseColorFactor;
};
Texture2D<float4> baseColorTexture : register(t1, space2);
SamplerState baseColorSampler : register(s2, space2);

struct VSInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    o.position = mul(viewProj, float4(input.position, 1.0));
    o.uv = input.uv;
    o.color = input.color;
    return o;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    return baseColorTexture.Sample(baseColorSampler, input.uv) * baseColorFactor * input.color;
}
