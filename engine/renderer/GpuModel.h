#pragma once
// GPU upload of a ModelData (plan F3 Task 4): vertex / index / morph buffers per primitive, textures (TextureUpload:
// one texture per image and colour-space view), samplers, and the material constants of shaders/pbr/pbr.hlsl
// (bindings.hlsli Material cbuffer). Static after creation (rest pose); bind groups are made per pipeline by the
// passes (Passes.cpp), since a group belongs to a pipeline's layout.
//
// Vertex streams (pbr.hlsl VSInput; missing glTF attributes get the shader's documented defaults):
//   stream 0  PbrVertex: POSITION, NORMAL (lines / points without NORMAL: +Z), TANGENT (none: (1, 0, 0, 1); a normal map
//             gets MikkTSpace tangents from the loader), TEXCOORD0 / TEXCOORD1 (none: 0), COLOR0 (none: white);
//   stream 1  skin (FG_SKIN): BLENDINDICES0 = JOINTS_0 (uint16 x 4), BLENDWEIGHT0 = WEIGHTS_0. JOINTS_1 / WEIGHTS_1
//             (more than 4 influences) are not in the F1 shader: warned, the first four are used;
//   stream 2  T0 morph (FG_MORPH, TIER 0): MORPH_POSITION0..7 = position deltas of the first 8 targets (more: warned);
//   storage   T1/T2 morph: morphDeltas = [(t * vertexCount + v) * 3 + {0 position, 1 normal, 2 tangent}] (float4).
// TEXCOORD_2 / TEXCOORD_3 sampled by a material: the F1 shader has two UV sets, so the texture is sampled with
// TEXCOORD_0 and a warning names the material and slot (never silently).
#include "TextureUpload.h"
#include "asset/GltfLoader.h"
#include "asset/ModelData.h"
#include "rhi/Rhi.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace eng::renderer {

// bindings.hlsli cbuffer layouts (float4 / uint4 / float4x4 members only: HLSL, SPIR-V and std140 agree; sizes are
// checked against the shader reflection when pipelines are made).
struct MaterialCB {
    float baseColorFactor[4];
    float emissiveFactor[4];
    float pbrParams[4];   // metallic, roughness, normal scale, occlusion strength
    float alphaParams[4]; // alphaCutoff
    uint32_t materialFlags[4]; // alphaMode (0 OPAQUE, 1 MASK, 2 BLEND), doubleSided, has normal texture
    uint32_t texCoordSets[4];  // base colour, metallic-roughness, normal, occlusion
    uint32_t texCoordSets2[4]; // emissive
    float uvTransform[10][4];
};
struct ObjectCB {
    float world[16];
    float normalMatrix[16];
    uint32_t morphInfo[4]; // active targets, vertex count, has tangent deltas
    float morphWeights[2][4];
};
struct FrameCB {
    float viewProj[16];
    float envRotation[16];
    float cameraPosition[4];
    float iblParams[4]; // exposure, environment intensity, specular mip count
    uint32_t frameFlags[4]; // x = 1: gamma 2.2 output encode (UNORM target)
};
struct SkyCB {
    float cameraToWorld[16];
    float rayParams[4];
    float outputParams[4];
    uint32_t outputFlags[4];
};
static_assert(sizeof(MaterialCB) == 272 && sizeof(ObjectCB) == 176 && sizeof(FrameCB) == 176 && sizeof(SkyCB) == 112);

struct PbrVertex {
    float position[3];
    float normal[3];
    float tangent[4];
    float uv0[2];
    float uv1[2];
    float color[4];
};
struct SkinVertex {
    uint16_t joints[4];
    float weights[4];
};
inline constexpr uint32_t kT0MaxMorphTargets = 8; // bindings.hlsli T0_MAX_MORPH_TARGETS
inline constexpr uint32_t kT0MaxJoints = 64;      // bindings.hlsli T0_MAX_JOINTS

struct GpuPrimitive {
    rhi::BufferHandle vertices, skin, morphAttributes, morphDeltas, indices;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    rhi::PrimitiveTopology topology = rhi::PrimitiveTopology::TriangleList;
    bool triangles = true;
    bool skinned = false;          // JOINTS_0 + WEIGHTS_0 present
    uint32_t targetCount = 0;      // morph targets (T0: <= 8 uploaded)
    bool tangentDeltas = false;
    int material = -1;             // -1 = the glTF default material
    asset::Bounds bounds;
};

struct GpuMaterial {
    MaterialCB constants{};
    rhi::BufferHandle cbuffer;
    // baseColor, metallicRoughness, normal, occlusion, emissive (TEX_* order of bindings.hlsli)
    std::array<rhi::TextureHandle, 5> textures{};
    std::array<rhi::SamplerHandle, 5> samplers{};
    asset::AlphaMode alphaMode = asset::AlphaMode::Opaque;
    bool doubleSided = false;
};

class GpuModel {
public:
    // `tier` = the device's (Vulkan T2 storage-buffer morph, D3D11 T1, T0 vertex-attribute morph).
    static Result<std::unique_ptr<GpuModel>> create(rhi::IDevice& device, const asset::ModelData& model,
                                                    std::vector<asset::Diagnostic>& diags);
    ~GpuModel();
    GpuModel(const GpuModel&) = delete;
    GpuModel& operator=(const GpuModel&) = delete;

    const std::vector<std::vector<GpuPrimitive>>& meshes() const { return meshes_; } // [mesh][primitive]
    const GpuMaterial& material(int index) const;                                    // -1 = default material

private:
    explicit GpuModel(rhi::IDevice& d) : dev_(d) {}
    Result<void> init(const asset::ModelData& model, std::vector<asset::Diagnostic>& diags);
    template <class H>
    H keep(H h) {
        owned_.push_back(h);
        return h;
    }
    Result<rhi::BufferHandle> buffer(std::span<const std::byte> data, rhi::BufferUsage usage, const std::string& name);

    rhi::IDevice& dev_;
    std::vector<rhi::Handle> owned_;
    std::vector<std::vector<GpuPrimitive>> meshes_;
    std::vector<GpuMaterial> materials_;
    GpuMaterial defaultMaterial_;
};

} // namespace eng::renderer
