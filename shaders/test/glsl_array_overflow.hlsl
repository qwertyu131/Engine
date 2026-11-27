// shaderc test fixture (plan F2 Task 1): GLSL ES flattens a binding to space * 16 + index, so a
// resource range must stay inside [0, 16) of its space.
//   PSMain:      t14..t17 (array of 4 at t14) -> "binding range t14..t17 exceeds 16 per space for glsl-es320"
//   PSUnbounded: unbounded array at t0 -> rejected on GLSL targets (no fixed range to flatten)
// Both are valid HLSL for the other targets.

Texture2D<float4> layers[4] : register(t14, space2);
SamplerState layerSampler : register(s0, space2);
Texture2D<float4> unboundedTextures[] : register(t0, space3);
SamplerState unboundedSampler : register(s1, space2);

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    return layers[0].Sample(layerSampler, uv) + layers[3].Sample(layerSampler, uv);
}

float4 PSUnbounded(float4 pos : SV_Position, float2 uv : TEXCOORD0, nointerpolation uint layer : TEXCOORD1) : SV_Target0
{
    return unboundedTextures[NonUniformResourceIndex(layer)].Sample(unboundedSampler, uv);
}
