#pragma once
// Passes of a glTF scene (plan F3 Task 4 / Task 5): one render pass into the colour target with a D32 depth buffer, in
// glTF-Sample-Renderer v1.1.0 drawScene order (renderer.js):
//   1. clear (r.Background=Color: the background colour; Environment: black, the background covers it);
//   2. environment background (r.Background=Environment): Skybox.h, shaders/ibl/background.hlsl, depth test off;
//   3. opaque + mask front-to-back (MASK discards below alphaCutoff in pbr.hlsl);
//   4. blend back-to-front (standard alpha blending, depth test, no depth write).
// The tonemap runs in the fragment shaders (shaders/post/tonemap.hlsl).
// Punctual lights and shadows (plan F3 Task 7, Lights.h / Shadows.h): before the render pass, a depth-only pass renders
// the casters (OPAQUE + MASK triangles; BLEND casts none) of every shadow tile into the D32 shadow atlas
// (shaders/shadow/shadow.hlsl, cull none, frustum-culled per tile); the lit pass binds space1 = the object's light list
// (cbuffer Lights, one per distinct per-object selection), the tiles (cbuffer Shadows) and the atlas.
// IBL (Ibl.h): the environment of EnvironmentDesc: an .hdr filtered as Sample-Renderer's iblSampler (GPU passes on
// T1 / T2, the CPU twin at T0), or "builtin", the deterministic CPU gradient of builtin:pbr-spheres (builtin/Ibl.h:
// 32 x 32 sky / ground cube, GGX prefilter, Lambertian irradiance, GGX BRDF LUT).
//
// Pipelines (shaders/pbr/pbr.hlsl): permutation <TIER>_SKIN<s>_MORPH<m>_UNLIT0 (Vulkan T2, D3D11 T1 / T0 by the
// device's tier), keyed by skin, morph, cull (doubleSided=false: back), front face (glTF counter-clockwise; a negative
// determinant flips it to clockwise, glTF 2.0 §3.7.4), blend and topology. Bind groups: space0 frame (per frame),
// space2 material (per material and permutation), space3 draw (per node-primitive and permutation: world, normal
// matrix, joint matrices, morph weights / deltas).
#include "DrawList.h"
#include "GpuModel.h"
#include "Ibl.h"
#include "Lights.h"
#include "RenderSettings.h"
#include "Shadows.h"
#include "Skybox.h"
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
    float exposure = 1.0f;         // r.Exposure
    float intensity = 1.0f;        // r.IBL.Intensity
    float rotationDeg = 0.0f;      // r.IBL.Rotation (about +Y, gl-matrix rotateY)
    float backgroundBlur = 0.0f;   // r.Background.Blur (u_EnvBlurNormalized)
    uint32_t tonemap = 0;          // TonemapOp
    float znear = 0.01f, zfar = 100.0f; // the camera's planes (cascade splits)
    LightingSettings lighting;     // effectiveLighting() of this frame (punctual lights / shadows)
};

// The IBL environment of a scene: "builtin" or an .hdr file, filtered with `params`.
struct EnvironmentDesc {
    std::string source = "builtin";
    ibl::Params params;
    bool operator==(const EnvironmentDesc&) const = default;
};

class ModelRenderer {
public:
    static Result<std::unique_ptr<ModelRenderer>> create(rhi::IDevice& device, const std::filesystem::path& shaderDir,
                                                         rhi::Format colorFormat, const asset::ModelData& model,
                                                         const scene::SceneInstance& scene,
                                                         const EnvironmentDesc& environment,
                                                         std::vector<asset::Diagnostic>& diags);
    ~ModelRenderer();
    ModelRenderer(const ModelRenderer&) = delete;
    ModelRenderer& operator=(const ModelRenderer&) = delete;

    Result<void> resize(uint32_t width, uint32_t height);
    // Rebuilds the IBL when `environment` differs from the current one (outside a frame).
    Result<void> setEnvironment(const EnvironmentDesc& environment);
    const EnvironmentDesc& environment() const { return environment_; }
    // Records the scene into `color` (state RenderTarget on entry and exit; size = the last resize).
    Result<void> record(rhi::ICommandList& cmd, rhi::TextureHandle color, const FrameParams& frame);
    // The scene's punctual lights (Lights.h): the asset's, then added ones; the facade toggles / appends them.
    std::vector<SceneLight>& lights() { return lights_; }
    // Shadow tiles drawn by the last record() (0 without shadows).
    uint32_t lastShadowTiles() const { return lastShadowTiles_; }

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
    Result<void> makeIbl(const EnvironmentDesc& environment);
    void releaseIbl();
    Result<void> makeDrawResources();
    Result<const Permutation*> permutation(bool skin, bool morph);
    Result<const Pipeline*> pipeline(const DrawItem& item);
    Result<rhi::BindGroupHandle> group(const Pipeline& p, uint32_t space, const std::map<std::string, Resource>& res);
    using MaterialGroups = std::map<std::tuple<int, bool, bool>, rhi::BindGroupHandle>;
    using DrawGroups = std::map<std::tuple<uint32_t, uint32_t, bool, bool>, rhi::BindGroupHandle>;
    Result<rhi::BindGroupHandle> materialGroup(const Pipeline& p, const DrawItem& item, MaterialGroups& cache);
    Result<rhi::BindGroupHandle> drawGroup(const Pipeline& p, const DrawItem& item, DrawGroups& cache);
    rhi::VertexLayout vertexLayout(const DrawItem& item) const;
    Result<const Permutation*> shadowPermutation(bool skin, bool morph);
    Result<const Pipeline*> shadowPipeline(const DrawItem& item);
    // Depth-only pass of `plan` into the atlas (outside the lit render pass); the atlas ends in ShaderRead.
    Result<void> recordShadows(rhi::ICommandList& cmd, const ShadowPlan& plan, const DrawLists& lists,
                               std::vector<rhi::BindGroupHandle>& transient);
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
    BackgroundPass background_;
    EnvironmentDesc environment_;
    std::vector<rhi::Handle> iblOwned_;              // IBL textures / samplers (released on setEnvironment)
    std::map<std::string, Resource> frameResources_; // IBL textures / samplers
    uint32_t specularMips_ = 0;                      // u_MipCount
    // Per node-primitive resources: Object cbuffer, joints (storage or T0 cbuffer), morph weights (storage).
    struct DrawResources {
        rhi::BufferHandle object, joints, morphWeights;
    };
    std::map<std::pair<uint32_t, uint32_t>, DrawResources> drawRes_;
    MaterialGroups materialGroups_;
    DrawGroups drawGroups_;
    // Punctual lights and shadows (plan F3 Task 7).
    std::vector<SceneLight> lights_;
    uint32_t tierMaxLights_ = kMaxLightsT12;
    std::map<std::pair<uint32_t, uint32_t>, std::pair<scene::Vec3, float>> drawSpheres_; // world bounding spheres
    ShadowSceneBounds sceneBounds_;
    std::map<std::pair<bool, bool>, Permutation> shadowPermutations_;
    using ShadowPipelineKey = std::tuple<bool, bool, rhi::PrimitiveTopology>;
    std::map<ShadowPipelineKey, Pipeline> shadowPipelines_;
    MaterialGroups shadowMaterialGroups_;
    DrawGroups shadowDrawGroups_;
    rhi::TextureHandle atlas_;
    uint32_t atlasSize_ = 0;
    rhi::ResourceState atlasState_ = rhi::ResourceState::Undefined;
    rhi::TextureHandle noShadowTexture_; // 1 x 1 white, bound when no tile is drawn
    rhi::SamplerHandle shadowSampler_;   // nearest, clamp
    uint32_t lastShadowTiles_ = 0;
    bool truncatedWarned_ = false;
    bool maxLightsWarned_ = false;
    rhi::TextureHandle depth_;
    rhi::ResourceState depthState_ = rhi::ResourceState::Undefined;
    uint32_t width_ = 0, height_ = 0;
    bool pointsWarned_ = false;
};

} // namespace eng::renderer
