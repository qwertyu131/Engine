// Binding-convention violation fixture (plan F1 Task 3, Review Focus 1): b0 and t0 in the same space.
// Vulkan/WGSL would map both to (set 2, binding 0). The cbuffer is used only by VSMain and the texture
// only by PSMain, so each stage's SPIR-V alone shows no collision: shaderc must check the declarations.
cbuffer Material : register(b0, space2)
{
    float4 scaleOffset;
};
Texture2D<float4> baseColorTexture : register(t0, space2);
SamplerState baseColorSampler : register(s1, space2);

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(float3 position : POSITION, float2 uv : TEXCOORD0)
{
    VSOutput o;
    o.position = float4(position * scaleOffset.x + scaleOffset.y, 1.0);
    o.uv = uv;
    return o;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    return baseColorTexture.Sample(baseColorSampler, input.uv);
}
