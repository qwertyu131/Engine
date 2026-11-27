// Environment background of builtin:pbr-spheres (plan F2 Task 5): a full-screen triangle; each pixel
// shows the environment cube (mip 0) along its camera ray, through the PBR shader's output transform
// (exposure, Khronos PBR Neutral, optional gamma 2.2: shaders/common/tonemap.hlsli), so the background
// matches the "environment lookup" probe of the golden case.
// Binding convention (F1): space0 = frame; index unique per space.
#include "../common/tonemap.hlsli"

cbuffer Sky : register(b0, space0)
{
    float4x4 cameraToWorld; // upper 3x3: camera axes in world space (columns right, up, back)
    float4 rayParams;       // x = tan(fovY / 2) * aspect, y = tan(fovY / 2)
    float4 outputParams;    // x = exposure, y = environment intensity
    uint4 outputFlags;      // x = 1: gamma 2.2 encode (UNORM target)
};
TextureCube<float4> environmentTexture : register(t1, space0);
SamplerState environmentSampler : register(s2, space0);

struct VSOutput
{
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    // (-1, -1), (3, -1), (-1, 3): covers the viewport; NDC +Y up (RHI convention).
    const float2 p = float2(vertexId == 1u ? 3.0 : -1.0, vertexId == 2u ? 3.0 : -1.0);
    VSOutput o;
    o.position = float4(p, 1.0, 1.0);
    o.ndc = p;
    return o;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float3 rayCamera = float3(input.ndc.x * rayParams.x, input.ndc.y * rayParams.y, -1.0);
    const float3 dir = normalize(mul((float3x3)cameraToWorld, rayCamera));
    const float3 radiance = environmentTexture.SampleLevel(environmentSampler, dir, 0.0).rgb * outputParams.y;
    return float4(toneMap(radiance, outputParams.x, outputFlags.x), 1.0);
}
