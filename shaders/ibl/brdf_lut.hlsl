// GGX BRDF LUT (plan F3 Task 5): glTF-Sample-Renderer v1.1.0 ibl_filtering.frag LUT() with u_isGeneratingLUT = 1,
// u_distribution = cGGX, as iblSampler.sampleGGXLut / sampleLut (ibl_sampler.js) run it: lutResolution x
// lutResolution, u_sampleCount = 512. x = N.V = texCoord.x, y = roughness = texCoord.y (row 0 = first in memory, the
// row GL writes at window y = 0). rgba = (scale, bias, 0, 1) as the FLOAT path stores it.
// CPU twin: engine/renderer/IblCpu.cpp.
#include "ibl_filtering.hlsli"

float3 LUT(float NdotV, float roughness)
{
    const int sampleCount = filterInts.y;
    // Compute spherical view vector: (sin(phi), 0, cos(phi)).
    const float3 V = float3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
    // The macro surface normal just points up.
    const float3 N = float3(0.0, 0.0, 1.0);
    float A = 0.0;
    float B = 0.0;
    for (int i = 0; i < sampleCount; ++i)
    {
        const float4 importanceSample = getImportanceSample(i, N, roughness, sampleCount, cGGX);
        const float3 H = importanceSample.xyz;
        const float3 L = normalize(reflect(-V, H));
        const float NdotL = saturate1(L.z);
        const float NdotH = saturate1(H.z);
        const float VdotH = saturate1(dot(V, H));
        if (NdotL > 0.0)
        {
            const float V_pdf = V_SmithGGXCorrelated(NdotV, NdotL, roughness) * VdotH * NdotL / NdotH;
            const float Fc = pow(1.0 - VdotH, 5.0);
            A += (1.0 - Fc) * V_pdf;
            B += Fc * V_pdf;
        }
    }
    return float3(4.0 * A, 4.0 * B, 0.0) / float(sampleCount);
}

float4 PSMain(FullscreenOut input) : SV_Target0
{
    const float2 texCoord = input.position.xy / filterFloats.w;
    return float4(LUT(texCoord.x, texCoord.y), 1.0);
}
