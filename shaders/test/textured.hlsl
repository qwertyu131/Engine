// builtin:textured-cube shader (plan F2 Task 3): one sRGB texture, no lighting.
// Binding convention (F1): space2 = material, space3 = draw/object; index unique per space.
// Matrices: HLSL default column_major packing + mul(M, v), as the F1 PBR shader (the CPU uploads
// column-major float4x4).

cbuffer Draw : register(b0, space3)
{
    float4x4 mvp; // clip-space Z in [0, 1], +Y up (RHI convention)
};
Texture2D<float4> faceTexture : register(t0, space2);
SamplerState faceSampler : register(s1, space2);

struct VSInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    o.position = mul(mvp, float4(input.position, 1.0));
    o.uv = input.uv;
    return o;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    return faceTexture.Sample(faceSampler, input.uv);
}
