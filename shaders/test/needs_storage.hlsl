// Storage-buffer fixture (plan F1 Task 3, Review Focus 2): T1/T2 skinning reads joint matrices from a
// StructuredBuffer. GLSL ES 3.00 (T0) has no storage buffers -> shaderc must reject it by name.
// VSWrite writes a storage buffer from the vertex stage, which WebGPU does not allow.
StructuredBuffer<float4x4> jointMatrices : register(t0, space3);
RWStructuredBuffer<float4> vertexOut : register(u1, space3);

float4 VSMain(float3 position : POSITION, uint4 joints : BLENDINDICES0, float4 weights : BLENDWEIGHT0) : SV_Position
{
    float4x4 skin = weights.x * jointMatrices[joints.x] + weights.y * jointMatrices[joints.y] +
                    weights.z * jointMatrices[joints.z] + weights.w * jointMatrices[joints.w];
    return mul(skin, float4(position, 1.0));
}

float4 VSWrite(float3 position : POSITION, uint vertexId : SV_VertexID) : SV_Position
{
    vertexOut[vertexId] = float4(position, 1.0);
    return float4(position, 1.0);
}
