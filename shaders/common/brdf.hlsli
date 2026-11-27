// glTF 2.0 metal-roughness BRDF (glTF spec Appendix B) as implemented by Khronos glTF-Sample-Viewer
// (@khronosgroup/gltf-viewer 1.1.0, the reference renderer of spec K-B): Lambert diffuse,
// GGX distribution, Smith height-correlated visibility, Schlick Fresnel; IBL with a prefiltered GGX
// cube, a Lambertian irradiance cube and the GGX BRDF LUT (Fdez-Aguera multiple scattering).
// Names follow Sample-Viewer's functions so the two can be diffed side by side.
#ifndef BRDF_HLSLI
#define BRDF_HLSLI

#include "bindings.hlsli"

static const float M_PI = 3.141592653589793;

float clampedDot(float3 x, float3 y)
{
    return clamp(dot(x, y), 0.0, 1.0);
}

float3 F_Schlick(float3 f0, float3 f90, float VdotH)
{
    return f0 + (f90 - f0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
}

// Smith joint GGX, height-correlated (Heitz 2014), including the 1 / (4 NdotL NdotV) term.
float V_GGX(float NdotL, float NdotV, float alphaRoughness)
{
    const float alphaRoughnessSq = alphaRoughness * alphaRoughness;
    const float GGXV = NdotL * sqrt(NdotV * NdotV * (1.0 - alphaRoughnessSq) + alphaRoughnessSq);
    const float GGXL = NdotV * sqrt(NdotL * NdotL * (1.0 - alphaRoughnessSq) + alphaRoughnessSq);
    const float GGX = GGXV + GGXL;
    if (GGX > 0.0)
    {
        return 0.5 / GGX;
    }
    return 0.0;
}

float D_GGX(float NdotH, float alphaRoughness)
{
    const float alphaRoughnessSq = alphaRoughness * alphaRoughness;
    const float f = (NdotH * NdotH) * (alphaRoughnessSq - 1.0) + 1.0;
    return alphaRoughnessSq / (M_PI * f * f);
}

float3 BRDF_lambertian(float3 diffuseColor)
{
    return diffuseColor / M_PI;
}

float3 BRDF_specularGGX(float alphaRoughness, float NdotL, float NdotV, float NdotH)
{
    const float Vis = V_GGX(NdotL, NdotV, alphaRoughness);
    const float D = D_GGX(NdotH, alphaRoughness);
    return float3(Vis * D, Vis * D, Vis * D);
}

// --- IBL ------------------------------------------------------------------------------------
float3 envDirection(float3 d)
{
    return mul((float3x3)envRotation, d); // u_EnvRotation * d
}

float3 getDiffuseLight(float3 n)
{
    // Implicit-LOD sample as Sample-Viewer's texture(): callers stay in uniform control flow (WGSL).
    return irradianceEnvTexture.Sample(envSampler, envDirection(n)).rgb * iblParams.y;
}

float4 getSpecularSample(float3 reflection, float lod)
{
    float4 s = specularEnvTexture.SampleLevel(envSampler, envDirection(reflection), lod);
    s.rgb *= iblParams.y;
    return s;
}

float3 getIBLRadianceGGX(float3 n, float3 v, float roughness)
{
    // u_MipCount comes from the frame cbuffer: GLSL ES has no textureQueryLevels.
    const float lod = roughness * (iblParams.z - 1.0);
    const float3 reflection = normalize(reflect(-v, n));
    return getSpecularSample(reflection, lod).rgb;
}

// Roughness-dependent Fresnel with multiple scattering (Fdez-Aguera), Sample-Viewer getIBLGGXFresnel.
float3 getIBLGGXFresnel(float3 n, float3 v, float roughness, float3 F0, float specularWeight)
{
    const float NdotV = clampedDot(n, v);
    const float2 brdfSamplePoint = clamp(float2(NdotV, roughness), float2(0.0, 0.0), float2(1.0, 1.0));
    const float2 f_ab = brdfLutTexture.Sample(brdfLutSampler, brdfSamplePoint).rg;
    const float oneMinusRoughness = 1.0 - roughness;
    const float3 Fr = max(float3(oneMinusRoughness, oneMinusRoughness, oneMinusRoughness), F0) - F0;
    const float3 k_S = F0 + Fr * pow(1.0 - NdotV, 5.0);
    const float3 FssEss = specularWeight * (k_S * f_ab.x + f_ab.y);
    const float Ems = 1.0 - (f_ab.x + f_ab.y);
    const float3 F_avg = specularWeight * (F0 + (1.0 - F0) / 21.0);
    const float3 FmsEms = Ems * FssEss * F_avg / (1.0 - F_avg * Ems);
    return FssEss + FmsEms;
}

#endif // BRDF_HLSLI
