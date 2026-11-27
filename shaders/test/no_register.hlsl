// shaderc test fixture (plan F2 Task 1): every resource must declare register(<c><i>, space<S>)
// (binding convention). `Material` has none -> "resource Material has no explicit register()
// (binding convention)". `Frame` is declared correctly.

cbuffer Frame : register(b0, space0)
{
    float4x4 viewProj;
};

cbuffer Material
{
    float4 baseColorFactor;
};

float4 VSMain(float3 position : POSITION) : SV_Position
{
    return mul(viewProj, float4(position, 1.0));
}

float4 PSMain(float4 pos : SV_Position) : SV_Target0
{
    return baseColorFactor;
}
