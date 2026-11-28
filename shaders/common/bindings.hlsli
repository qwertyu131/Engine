// glTF PBR resource bindings (plan F1 Task 4), following the binding convention:
//   register(<type><index>, space<S>), index unique per space across b/t/s/u.
//   space0 = frame, space1 = view/pass (punctual lights + shadows, plan F3 Task 7), space2 = material,
//   space3 = draw/object.
// Every cbuffer is made of float4 / uint4 / float4x4 only, so the HLSL cbuffer packing, the DXC
// SPIR-V layout and GLSL std140 agree byte for byte (no float3 members, no scalar arrays).
//
// Feature-group defines (spec K-A), each 0|1, and TIER=0|1|2 (spec 6.1 / K-E).
#ifndef FEATURE_DEFINES_HLSLI
#define FEATURE_DEFINES_HLSLI
#ifndef TIER
#define TIER 2
#endif
#ifndef FG_SKIN
#define FG_SKIN 0
#endif
#ifndef FG_MORPH
#define FG_MORPH 0
#endif
#ifndef FG_CLEARCOAT_SHEEN_SPEC
#define FG_CLEARCOAT_SHEEN_SPEC 0 // reserved (F6): compiles as a no-op in F1
#endif
#ifndef FG_TRANSMISSION_VOLUME
#define FG_TRANSMISSION_VOLUME 0 // reserved (F6): compiles as a no-op in F1
#endif
#ifndef FG_UNLIT
#define FG_UNLIT 0
#endif
#if TIER < 0 || TIER > 2
#error "TIER must be 0, 1 or 2"
#endif
#endif

#ifndef BINDINGS_HLSLI
#define BINDINGS_HLSLI

// T0 limits (spec K-E): joints in a uniform array, morph targets as vertex attributes.
#define T0_MAX_JOINTS 64
#define T0_MAX_MORPH_TARGETS 8

// Material texture slots, also the index into Material.uvTransform / texCoordSets.
#define TEX_BASE_COLOR 0
#define TEX_METALLIC_ROUGHNESS 1
#define TEX_NORMAL 2
#define TEX_OCCLUSION 3
#define TEX_EMISSIVE 4

// --- space0: frame --------------------------------------------------------------------------
cbuffer Frame : register(b0, space0)
{
    float4x4 viewProj;
    float4x4 envRotation;  // IBL rotation (Sample-Viewer u_EnvRotation); upper 3x3 used
    float4 cameraPosition; // xyz = world-space camera position
    float4 iblParams;      // x = exposure, y = environment intensity, z = specular mip count, w unused
    uint4 frameFlags;      // x = 1: apply the gamma 2.2 output encode (UNORM target, as Sample-Viewer), y = tonemap
                           // operator (shaders/post/tonemap.hlsl: 0 PbrNeutral, 1 Aces, 2 None)
};
TextureCube<float4> specularEnvTexture : register(t1, space0);   // GGX-prefiltered radiance (u_GGXEnvSampler)
TextureCube<float4> irradianceEnvTexture : register(t2, space0); // Lambertian irradiance (u_LambertianEnvSampler)
Texture2D<float4> brdfLutTexture : register(t3, space0);         // GGX BRDF LUT (u_GGXLUT)
SamplerState envSampler : register(s4, space0);
SamplerState brdfLutSampler : register(s5, space0);

// --- space1: view / pass: punctual lights and shadows (plan F3 Task 7, shaders/pbr/lights.hlsli) -----------
// Lights: the (<= MAX_LIGHTS) lights of the objects drawn with this group (the renderer picks the strongest
// r.Lighting.MaxLights per object, glTF-Sample-Renderer order otherwise). Shadows: the tiles of the shadow atlas, a
// D32 depth texture read with SampleLevel through a nearest-filter sampler at texel centres (no comparison sampler and
// no texelFetch: every shader target, the GLSL targets have no sampler-less fetch; explicit LOD: no derivatives in the
// light loop).
#if TIER == 0
#define MAX_LIGHTS 8 // spec K-E: T0 forward, <= 8 lights per object
#else
#define MAX_LIGHTS 64 // T1 / T2 in F3 (clustered lighting is F8)
#endif
#define MAX_SHADOW_TILES 64
#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT 1
#define LIGHT_SPOT 2

cbuffer Lights : register(b0, space1)
{
    uint4 lightInfo;      // x = light count (<= MAX_LIGHTS), y = 1: punctual specular (r.Lighting.Specular), z = 1: PCF
                          // 3 x 3 (0: one tap), w unused
    float4 shadowInfo;    // x = normal offset in texels, y = depth bias in texels, zw = 1 / atlas width, height
    float4 viewForward;   // xyz = camera forward (world): directional cascades are picked by view depth
    // Light i (Sample-Renderer punctual.glsl struct Light, glTF-Sample-Renderer v1.1.0):
    //   lights[4i]     = direction.xyz, range (<= 0: no range cut-off)
    //   lights[4i + 1] = color.rgb, intensity
    //   lights[4i + 2] = position.xyz, innerConeCos
    //   lights[4i + 3] = outerConeCos, type (LIGHT_*), first shadow tile (< 0: none), shadow tile count
    float4 lights[MAX_LIGHTS * 4];
};
cbuffer Shadows : register(b1, space1)
{
    float4x4 shadowMatrices[MAX_SHADOW_TILES]; // world -> tile clip space
    float4 shadowRects[MAX_SHADOW_TILES];      // xy = tile origin in the atlas (texels), z = tile edge (texels),
                                               // w = cascade far view depth (directional), else unused
    float4 shadowParams[MAX_SHADOW_TILES];     // x = world size of a texel (perspective: per unit distance), y = 1:
                                               // perspective, z = near, w = far (the tile projection's planes)
};
Texture2D<float> shadowAtlas : register(t2, space1);
SamplerState shadowSampler : register(s3, space1); // nearest, clamp to edge

// --- space2: material -----------------------------------------------------------------------
cbuffer Material : register(b0, space2)
{
    float4 baseColorFactor;
    float4 emissiveFactor; // rgb
    float4 pbrParams;      // x = metallicFactor, y = roughnessFactor, z = normal scale, w = occlusion strength
    float4 alphaParams;    // x = alphaCutoff
    uint4 materialFlags;   // x = alphaMode (0 OPAQUE, 1 MASK, 2 BLEND), y = doubleSided (pipeline cull mode; the
                           // shader flips back faces always, as Sample-Viewer), z = has normal texture
    uint4 texCoordSets;    // TEXCOORD_n per texture: x base color, y metallic-roughness, z normal, w occlusion
    uint4 texCoordSets2;   // x = emissive
    // KHR_texture_transform per texture (identity in F1): uv' = (dot(r0.xy, uv) + r0.z, dot(r1.xy, uv) + r1.z),
    // rows uvTransform[2 * slot] and uvTransform[2 * slot + 1].
    float4 uvTransform[10];
};
// Missing glTF textures are bound to 1x1 defaults by the renderer (white; the normal texture is only
// read when materialFlags.z = 1), so the shader samples unconditionally.
Texture2D<float4> baseColorTexture : register(t1, space2);
Texture2D<float4> metallicRoughnessTexture : register(t2, space2);
Texture2D<float4> normalTexture : register(t3, space2);
Texture2D<float4> occlusionTexture : register(t4, space2);
Texture2D<float4> emissiveTexture : register(t5, space2);
SamplerState baseColorSampler : register(s6, space2);
SamplerState metallicRoughnessSampler : register(s7, space2);
SamplerState normalSampler : register(s8, space2);
SamplerState occlusionSampler : register(s9, space2);
SamplerState emissiveSampler : register(s10, space2);

// --- space3: draw / object ------------------------------------------------------------------
cbuffer Object : register(b0, space3)
{
    float4x4 world;
    float4x4 normalMatrix; // inverse-transpose of world
    uint4 morphInfo;       // x = active morph target count, y = vertex count (T1/T2 delta indexing), z = has tangent deltas,
                           // w = bit 0: points / lines without NORMAL, unlit; bit 1: points / lines without TANGENT, no
                           // normal texture (Sample-Renderer NOT_TRIANGLE)
    float4 morphWeights[2]; // T0: weights of the <= 8 active targets
};

#if FG_SKIN
#if TIER == 0
cbuffer Skin : register(b1, space3)
{
    // [2 * j] = joint matrix, [2 * j + 1] = joint normal matrix (Sample-Viewer u_jointsSampler layout).
    float4x4 jointMatrices[2 * T0_MAX_JOINTS];
};
#else
StructuredBuffer<float4x4> jointMatrices : register(t2, space3); // same [2 * j], [2 * j + 1] layout
#endif
#endif

#if FG_MORPH && TIER >= 1
// Deltas of target t for vertex v: [(t * vertexCount + v) * 3 + {0 position, 1 normal, 2 tangent}].
StructuredBuffer<float4> morphDeltas : register(t3, space3);
StructuredBuffer<float> morphTargetWeights : register(t4, space3); // weight of target t in [t]
#endif

#endif // BINDINGS_HLSLI
