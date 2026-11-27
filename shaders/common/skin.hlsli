// Skinning (FG_SKIN), as Sample-Viewer getSkinningMatrix / getSkinningNormalMatrix (plan F1 Task 4).
// T0: joint matrices in a cbuffer array (<= 64 joints); T1/T2: StructuredBuffer.
// Weights summing to 0 -> identity (Sample-Viewer: skin == mat4(0) -> mat4(1)).
#ifndef SKIN_HLSLI
#define SKIN_HLSLI

#include "vertex.hlsli"

#if FG_SKIN
float4x4 jointMatrix(uint i)
{
    return jointMatrices[i];
}

// slot 0 = joint matrix, 1 = joint normal matrix.
float4x4 skinMatrix(VSInput input, uint slot)
{
    float4x4 skin = input.weights.x * jointMatrix(input.joints.x * 2u + slot) +
                    input.weights.y * jointMatrix(input.joints.y * 2u + slot) +
                    input.weights.z * jointMatrix(input.joints.z * 2u + slot) +
                    input.weights.w * jointMatrix(input.joints.w * 2u + slot);
    if (dot(input.weights, float4(1.0, 1.0, 1.0, 1.0)) == 0.0)
    {
        skin = float4x4(1.0, 0.0, 0.0, 0.0,
                        0.0, 1.0, 0.0, 0.0,
                        0.0, 0.0, 1.0, 0.0,
                        0.0, 0.0, 0.0, 1.0);
    }
    return skin;
}
#endif

#endif // SKIN_HLSLI
