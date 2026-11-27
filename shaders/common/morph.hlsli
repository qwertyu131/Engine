// Morph targets (FG_MORPH), as Sample-Viewer getTargetPosition / Normal / Tangent (plan F1 Task 4).
// T0: <= 8 active targets, position deltas only, as vertex attributes; weights in Object.morphWeights.
// T1/T2: any number of targets (Object.morphInfo.x), position/normal/tangent deltas in a
// StructuredBuffer, weights in a StructuredBuffer.
// TEXCOORD_n / COLOR_0 deltas are not part of F1.
#ifndef MORPH_HLSLI
#define MORPH_HLSLI

#include "vertex.hlsli"

struct MorphDelta
{
    float3 position;
    float3 normal;
    float3 tangent;
};

MorphDelta morphDelta(VSInput input)
{
    MorphDelta d;
    d.position = float3(0.0, 0.0, 0.0);
    d.normal = float3(0.0, 0.0, 0.0);
    d.tangent = float3(0.0, 0.0, 0.0);
#if FG_MORPH && TIER == 0
    d.position = morphWeights[0].x * input.morphPosition0 + morphWeights[0].y * input.morphPosition1 +
                 morphWeights[0].z * input.morphPosition2 + morphWeights[0].w * input.morphPosition3 +
                 morphWeights[1].x * input.morphPosition4 + morphWeights[1].y * input.morphPosition5 +
                 morphWeights[1].z * input.morphPosition6 + morphWeights[1].w * input.morphPosition7;
#elif FG_MORPH
    for (uint t = 0u; t < morphInfo.x; ++t)
    {
        const uint base = (t * morphInfo.y + input.vertexId) * 3u;
        const float w = morphTargetWeights[t];
        d.position += w * morphDeltas[base].xyz;
        d.normal += w * morphDeltas[base + 1u].xyz;
        d.tangent += w * morphDeltas[base + 2u].xyz;
    }
#endif
    return d;
}

#endif // MORPH_HLSLI
