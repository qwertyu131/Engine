// Environment background pass (plan F3 Task 5). See Skybox.h and shaders/ibl/background.hlsl.
#include "Skybox.h"

#include "rhi/ShaderLibrary.h"

#include <cstring>
#include <format>

namespace eng::renderer {

using namespace rhi;

namespace {

struct BackgroundCB {
    float viewProj[16];
    float envRotation[16];
    float params[4];   // exposure, intensity, lod
    uint32_t flags[4]; // gamma encode, tonemap operator
};
static_assert(sizeof(BackgroundCB) == 160);

} // namespace

Result<void> BackgroundPass::init(IDevice& dev, const std::filesystem::path& shaderDir, Format colorFormat,
                                  Format depthFormat, bool tier0) {
    const Backend b = dev.caps().backend;
    const char* key = tier0 ? "default-t0" : "default";
    auto vsBlob = ShaderLibrary::load(shaderDir, "background", "VSMain", b, key);
    if (!vsBlob) {
        return std::unexpected(vsBlob.error());
    }
    auto psBlob = ShaderLibrary::load(shaderDir, "background", "PSMain", b, key);
    if (!psBlob) {
        return std::unexpected(psBlob.error());
    }
    auto layouts = deriveBindGroupLayouts(vsBlob->reflection, psBlob->reflection);
    if (!layouts) {
        return std::unexpected(layouts.error());
    }
    auto vs = dev.createShader(*vsBlob);
    if (!vs) {
        return std::unexpected(vs.error());
    }
    vs_ = *vs;
    auto ps = dev.createShader(*psBlob);
    if (!ps) {
        return std::unexpected(ps.error());
    }
    ps_ = *ps;
    GraphicsPipelineDesc pd;
    pd.vs = vs_;
    pd.ps = ps_;
    pd.depth = {false, false, CompareOp::Always};
    pd.colorFormats = {colorFormat};
    pd.depthFormat = depthFormat;
    pd.debugName = "glTF environment background";
    auto pipe = dev.createGraphicsPipeline(pd);
    if (!pipe) {
        return std::unexpected(pipe.error());
    }
    pipeline_ = *pipe;
    layouts_ = std::move(*layouts);
    return {};
}

void BackgroundPass::release(IDevice& dev) {
    for (Handle h : {Handle(pipeline_), Handle(ps_), Handle(vs_)}) {
        if (h.id != 0) {
            dev.destroy(h);
        }
    }
    pipeline_ = {};
    ps_ = {};
    vs_ = {};
}

Result<void> BackgroundPass::record(IDevice& dev, ICommandList& cmd, const BackgroundParams& p, TextureHandle environment,
                                    SamplerHandle sampler) {
    BackgroundCB cb{};
    std::memcpy(cb.viewProj, p.viewProj.m.data(), 64);
    std::memcpy(cb.envRotation, p.envRotation.m.data(), 64);
    cb.params[0] = p.exposure;
    cb.params[1] = p.intensity;
    cb.params[2] = p.lod;
    cb.flags[0] = p.gammaEncode ? 1u : 0u;
    cb.flags[1] = p.tonemap;
    const BufferBinding b = dev.allocTransient(sizeof(cb), 256);
    if (!b.buffer) {
        return std::unexpected(Error{2, "background: allocTransient failed"});
    }
    std::memcpy(b.cpu, &cb, sizeof(cb));
    BindGroupDesc d;
    d.pipeline = pipeline_;
    d.space = 0;
    for (const BindGroupLayout& l : layouts_) {
        if (l.space != 0) {
            continue;
        }
        for (const BindGroupLayoutEntry& e : l.entries) {
            if (e.name == "Background") {
                d.entries.push_back({e.name, b});
            } else if (e.name == "environmentTexture") {
                d.entries.push_back({e.name, environment});
            } else if (e.name == "environmentSampler") {
                d.entries.push_back({e.name, sampler});
            } else {
                return std::unexpected(Error{2, std::format("background: unexpected resource '{}'", e.name)});
            }
        }
    }
    auto g = dev.createBindGroup(d);
    if (!g) {
        return std::unexpected(g.error());
    }
    cmd.setPipeline(pipeline_);
    cmd.setBindGroup(0, *g);
    cmd.draw(36, 1, 0, 0);
    dev.destroy(*g); // deferred until the GPU is done with this frame
    return {};
}

} // namespace eng::renderer
