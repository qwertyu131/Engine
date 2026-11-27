// Binding-convention fixture for tools/shaderc reflection (plan F1 Task 3): every resource kind,
// register(<type><index>, space<S>) with the index unique per space across b/t/s/u.
//   space0 = frame, space1 = view/pass, space2 = material, space3 = draw/object.
// TIER >= 1 adds storage buffers (not available on GLSL ES 3.00 / T0).
#ifndef TIER
#define TIER 0
#endif

cbuffer Frame : register(b0, space0)
{
    float4x4 viewProj;
};

cbuffer Pass : register(b0, space1)
{
    float4 passParams;
};

cbuffer Material : register(b0, space2)
{
    float4 baseColorFactor;
};
Texture2D<float4> baseColorTexture : register(t1, space2);
SamplerState baseColorSampler : register(s2, space2);
TextureCube<float4> envTexture : register(t3, space2);
SamplerState envSampler : register(s4, space2);

cbuffer Object : register(b0, space3)
{
    float4x4 world;
};
#if TIER >= 1
StructuredBuffer<float4> offsets : register(t1, space3);
RWStructuredBuffer<uint> fragmentCounter : register(u1, space1);
#endif

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    uint vertexId : SV_VertexID;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(VSInput input)
{
    float3 p = input.position;
#if TIER >= 1
    p += offsets[input.vertexId].xyz;
#endif
    VSOutput o;
    o.position = mul(viewProj, mul(world, float4(p, 1.0)));
    o.normal = mul((float3x3)world, input.normal);
    o.uv = input.uv;
    return o;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    float4 c = baseColorTexture.Sample(baseColorSampler, input.uv) * baseColorFactor;
    c.rgb += envTexture.Sample(envSampler, normalize(input.normal)).rgb * passParams.x;
#if TIER >= 1
    InterlockedAdd(fragmentCounter[0], 1u);
#endif
    return c;
}
