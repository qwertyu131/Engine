#pragma once
// Passes of a glTF scene (plan F3 Task 4, normal mode): one render pass into the colour target with a D32 depth buffer:
//   1. clear (r.Background=Color: the background colour; Environment: black, the skybox covers it);
//   2. opaque + mask front-to-back (MASK discards below alphaCutoff in pbr.hlsl);
//   3. skybox (r.Background=Environment): shaders/test/skybox.hlsl, depth test LessEqual at the far plane, no write;
//   4. blend back-to-front (standard alpha blending, depth test, no depth write).
// Shadows (normal mode, Task 7), reference-mode IBL / tonemap / background blur (Task 5) come later. Until Task 5 the
// IBL is the deterministic CPU one of builtin:pbr-spheres (builtin/Ibl.h: 32 x 32 sky / ground gradient cube, GGX
// prefilter, Lambertian irradiance, GGX BRDF LUT).
//
// Pipelines (shaders/pbr/pbr.hlsl): permutation <TIER>_SKIN<s>_MORPH<m>_UNLIT0 (Vulkan T2, D3D11 T1 / T0 by the
// device's tier), keyed by skin, morph, cull (doubleSided=false: back), front face (glTF counter-clockwise; a negative
// determinant flips it to clockwise, glTF 2.0 §3.7.4), blend and topology. Bind groups: space0 frame (per frame),
// space2 material (per material and permutation), space3 draw (per node-primitive and permutation: world, normal
// matrix, joint matrices, morph weights / deltas).
#include "DrawList.h"
#include "GpuModel.h"
#include "scene/Scene.h"

#include <filesystem>
#include <map>
#include <memory>
#include <tuple>
#include <variant>

namespace eng::renderer {

struct FrameParams {
    scene::Mat4 view;
    scene::Mat4 proj;
    scene::Vec3 cameraPosition{0, 0, 0};
    float yfov = 0.785398163f;
    float aspect = 1.0f;
    bool gammaEncode = true;     // UNORM target: the shader's gamma 2.2 output encode (as Sample-Viewer)
    bool environmentBackground = true;
    float clear[4] = {0, 0, 0, 1}; // target-space clear value (linear for sRGB targets)
};

class ModelRenderer {
public:
    static Result<std::unique_ptr<ModelRenderer>> create(rhi::IDevice& device, const std::filesystem::path& shaderDir,
                                                         rhi::Format colorFormat, const asset::ModelData& model,
                                                         const scene::SceneInstance& scene,
                                                         std::vector<asset::Diagnostic>& diags);
    ~ModelRenderer();
    ModelRenderer(const ModelRenderer&) = delete;
    ModelRenderer& operator=(const ModelRenderer&) = delete;

    Result<void> resize(uint32_t width, uint32_t height);
    // Records the scene into `color` (state RenderTarget on entry and exit; size = the last resize).
    Result<void> record(rhi::ICommandList& cmd, rhi::TextureHandle color, const FrameParams& frame);

private:
    using Resource = std::variant<rhi::BufferBinding, rhi::TextureHandle, rhi::SamplerHandle>;
    struct Pipeline {
        rhi::PipelineHandle handle;
        std::vector<rhi::BindGroupLayout> layouts;
        const rhi::BindGroupLayout* layout(uint32_t space) const;
    };
    struct Permutation { // shader pair of one TIER / SKIN / MORPH combination
        rhi::ShaderHandle vs, ps;
        std::vector<rhi::BindGroupLayout> layouts;
    };
    using PipelineKey = std::tuple<bool, bool, rhi::CullMode, rhi::FrontFace, bool, rhi::PrimitiveTopology>;

    ModelRenderer(rhi::IDevice& d, const asset::ModelData& m, const scene::SceneInstance& s)
        : dev_(d), model_(m), scene_(s) {}
    Result<void> init(const std::filesystem::path& shaderDir, std::vector<asset::Diagnostic>& diags);
    Result<void> makeIbl();
    Result<void> makeDrawResources();
    Result<const Permutation*> permutation(bool skin, bool morph);
    Result<const Pipeline*> pipeline(const DrawItem& item);
    Result<rhi::BindGroupHandle> group(const Pipeline& p, uint32_t space, const std::map<std::string, Resource>& res);
    Result<rhi::BindGroupHandle> materialGroup(const Pipeline& p, const DrawItem& item, bool skin, bool morph);
    Result<rhi::BindGroupHandle> drawGroup(const Pipeline& p, const DrawItem& item, bool skin, bool morph);
    template <class H>
    H keep(H h) {
        owned_.push_back(h);
        return h;
    }

    rhi::IDevice& dev_;
    const asset::ModelData& model_;
    const scene::SceneInstance& scene_;
    std::filesystem::path shaderDir_;
    rhi::Format colorFormat_ = rhi::Format::RGBA8_UNORM;
    std::string tier_;
    std::unique_ptr<GpuModel> gpu_;
    std::vector<rhi::Handle> owned_;
    std::map<std::pair<bool, bool>, Permutation> permutations_;
    std::map<PipelineKey, Pipeline> pipelines_;
    Pipeline sky_;
    std::map<std::string, Resource> frameResources_; // IBL textures / samplers
    uint32_t specularMips_ = 0;
    // Per node-primitive resources: Object cbuffer, joints (storage or T0 cbuffer), morph weights (storage).
    struct DrawResources {
        rhi::BufferHandle object, joints, morphWeights;
    };
    std::map<std::pair<uint32_t, uint32_t>, DrawResources> drawRes_;
    std::map<std::tuple<int, bool, bool>, rhi::BindGroupHandle> materialGroups_;
    std::map<std::tuple<uint32_t, uint32_t, bool, bool>, rhi::BindGroupHandle> drawGroups_;
    rhi::TextureHandle depth_;
    rhi::ResourceState depthState_ = rhi::ResourceState::Undefined;
    uint32_t width_ = 0, height_ = 0;
    bool pointsWarned_ = false;
};

} // namespace eng::renderer
