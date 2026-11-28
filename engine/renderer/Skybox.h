#pragma once
// Environment background pass of glTF scenes (plan F3 Task 5, r.Background=Environment): shaders/ibl/background.hlsl,
// glTF-Sample-Renderer v1.1.0 EnvironmentRenderer.drawEnvironmentMap (environment_renderer.js): drawn first, depth
// test off, culling off (from the cube's centre every visible triangle faces inwards), no blending.
#include "rhi/Rhi.h"
#include "scene/Math.h"

#include <filesystem>
#include <vector>

namespace eng::renderer {

struct BackgroundParams {
    scene::Mat4 viewProj;
    scene::Mat4 envRotation;
    float exposure = 1.0f;
    float intensity = 1.0f;
    float lod = 0.0f; // u_EnvBlurNormalized * (u_MipCount - 1)
    bool gammaEncode = true;
    uint32_t tonemap = 0;
};

class BackgroundPass {
public:
    // tier0: the SM 4.0 variant (D3D11 FL10_x).
    Result<void> init(rhi::IDevice& dev, const std::filesystem::path& shaderDir, rhi::Format colorFormat,
                      rhi::Format depthFormat, bool tier0);
    void release(rhi::IDevice& dev);
    // Inside a render pass. The group is created per frame (destroyed deferred).
    Result<void> record(rhi::IDevice& dev, rhi::ICommandList& cmd, const BackgroundParams& p,
                        rhi::TextureHandle environment, rhi::SamplerHandle sampler);

private:
    rhi::ShaderHandle vs_, ps_;
    rhi::PipelineHandle pipeline_;
    std::vector<rhi::BindGroupLayout> layouts_;
};

} // namespace eng::renderer
