// glTF 2.0 core metal-roughness PBR (plan F1 Task 4, spec 5.4 / K-A / K-B).
// One source for all 7 shaderc targets. Matches Khronos glTF-Sample-Viewer (@khronosgroup/gltf-viewer
// 1.1.0, primitive.vert + pbr.frag) with USE_IBL, TONEMAP_KHR_PBR_NEUTRAL and no punctual lights
// (the K-B reference images are rendered with punctual lights off; punctual lights come in F3).
//
// Permutations (shaders/pbr/permutations.json): TIER=0|1|2 x FG_SKIN x FG_MORPH x FG_UNLIT.
// FG_CLEARCOAT_SHEEN_SPEC and FG_TRANSMISSION_VOLUME are reserved (F6) and compile as no-ops.
// alphaMode, double-sided, texture-coordinate sets and KHR_texture_transform are uniforms
// (Material cbuffer), not defines, to stay within K-A's <= 64 variants per backend.
#include "../common/bindings.hlsli"
#include "../common/vertex.hlsli"
#include "../common/skin.hlsli"
#include "../common/morph.hlsli"
#include "../common/brdf.hlsli"
#include "../common/tonemap.hlsli"

// ----------------------------------------------------------------------------------------------
// Vertex stage (Sample-Viewer primitive.vert)
// ----------------------------------------------------------------------------------------------
VSOutput VSMain(VSInput input)
{
    const MorphDelta morph = morphDelta(input);
    float4 position = float4(input.position + morph.position, 1.0);
    float3 normal = input.normal + morph.normal;
    float3 tangent = input.tangent.xyz + morph.tangent;
#if FG_SKIN
    const float4x4 skin = skinMatrix(input, 0u);
    const float4x4 skinNormal = skinMatrix(input, 1u);
    position = mul(skin, position);
    normal = mul((float3x3)skinNormal, normal);
    tangent = mul((float3x3)skin, tangent);
#endif
    normal = normalize(normal);
    tangent = normalize(tangent);

    const float4 worldPos = mul(world, position);
    VSOutput o;
    o.position = mul(viewProj, worldPos);
    o.worldPosition = worldPos.xyz / worldPos.w;
    o.normalW = normalize(mul(normalMatrix, float4(normal, 0.0)).xyz);
    o.tangentW = normalize(mul(world, float4(tangent, 0.0)).xyz);
    o.bitangentW = cross(o.normalW, o.tangentW) * input.tangent.w;
    o.uv0 = input.uv0;
    o.uv1 = input.uv1;
    o.color = input.color;
    return o;
}

// ----------------------------------------------------------------------------------------------
// Fragment stage (Sample-Viewer pbr.frag)
// ----------------------------------------------------------------------------------------------

// TEXCOORD_n selection + KHR_texture_transform for material texture `slot`.
float2 textureUV(VSOutput input, uint slot, uint texCoordSet)
{
    float2 uv = input.uv0;
    if (texCoordSet == 1u)
    {
        uv = input.uv1;
    }
    const float4 r0 = uvTransform[2u * slot];
    const float4 r1 = uvTransform[2u * slot + 1u];
    return float2(dot(r0.xy, uv) + r0.z, dot(r1.xy, uv) + r1.z);
}

float4 PSMain(VSOutput input, bool frontFacing : SV_IsFrontFace) : SV_Target0
{
    // getBaseColor(): factor * texture * COLOR_0.
    float4 baseColor = baseColorFactor *
                       baseColorTexture.Sample(baseColorSampler, textureUV(input, TEX_BASE_COLOR, texCoordSets.x)) *
                       input.color;
    const uint alphaMode = materialFlags.x;
    if (alphaMode == 0u)
    {
        baseColor.a = 1.0; // OPAQUE
    }

    float3 color;
#if FG_UNLIT
    // KHR_materials_unlit: base color only (still tonemapped, as Sample-Viewer).
    color = baseColor.rgb;
#else
    const float3 v = normalize(cameraPosition.xyz - input.worldPosition);

    // getNormalInfo() with vertex TBN (MikkTSpace tangents, tangent.w = bitangent sign).
    float3 t = normalize(input.tangentW);
    float3 b = normalize(input.bitangentW);
    float3 ng = normalize(input.normalW);
    if (!frontFacing)
    {
        // Double-sided back faces see the flipped frame; single-sided back faces are culled.
        t = -t;
        b = -b;
        ng = -ng;
    }
    const float3 normalSample =
        normalTexture.Sample(normalSampler, textureUV(input, TEX_NORMAL, texCoordSets.z)).rgb;
    float3 n = ng;
    if (materialFlags.z != 0u)
    {
        const float normalScale = pbrParams.z;
        const float3 ntex = normalize((normalSample * 2.0 - 1.0) * float3(normalScale, normalScale, 1.0));
        n = normalize(ntex.x * t + ntex.y * b + ntex.z * ng); // mat3(t, b, ng) * ntex
    }

    // getMetallicRoughnessInfo(): B = metal, G = rough.
    const float4 mrSample = metallicRoughnessTexture.Sample(metallicRoughnessSampler,
                                                            textureUV(input, TEX_METALLIC_ROUGHNESS, texCoordSets.y));
    const float metallic = clamp(pbrParams.x * mrSample.b, 0.0, 1.0);
    const float perceptualRoughness = clamp(pbrParams.y * mrSample.g, 0.0, 1.0);
    const float3 f0Dielectric = float3(0.04, 0.04, 0.04); // ior 1.5
    const float specularWeight = 1.0;

#if FG_CLEARCOAT_SHEEN_SPEC
    // Reserved feature group (F6: KHR_materials_clearcoat / sheen / specular): no-op in F1.
#endif
#if FG_TRANSMISSION_VOLUME
    // Reserved feature group (F6: KHR_materials_transmission / volume): no-op in F1.
#endif

    // IBL (USE_IBL).
    const float3 f_diffuse = getDiffuseLight(n) * baseColor.rgb;
    const float3 f_specular = getIBLRadianceGGX(n, v, perceptualRoughness);
    const float3 f_metal_fresnel_ibl = getIBLGGXFresnel(n, v, perceptualRoughness, baseColor.rgb, 1.0);
    const float3 f_metal_brdf_ibl = f_metal_fresnel_ibl * f_specular;
    const float3 f_dielectric_fresnel_ibl = getIBLGGXFresnel(n, v, perceptualRoughness, f0Dielectric, specularWeight);
    const float3 f_dielectric_brdf_ibl = lerp(f_diffuse, f_specular, f_dielectric_fresnel_ibl);
    color = lerp(f_dielectric_brdf_ibl, f_metal_brdf_ibl, metallic);

    // Occlusion applies to the IBL term only.
    const float ao = occlusionTexture.Sample(occlusionSampler, textureUV(input, TEX_OCCLUSION, texCoordSets.w)).r;
    color = color * (1.0 + pbrParams.w * (ao - 1.0));

    const float3 emissiveSample =
        emissiveTexture.Sample(emissiveSampler, textureUV(input, TEX_EMISSIVE, texCoordSets2.x)).rgb;
    const float3 f_emissive = emissiveFactor.rgb * emissiveSample;
    color = f_emissive + color;
#endif

    if (alphaMode == 1u)
    {
        // MASK
        if (baseColor.a < alphaParams.x)
        {
            discard;
        }
        baseColor.a = 1.0;
    }
    return float4(toneMap(color, iblParams.x, frameFlags.x), baseColor.a);
}
