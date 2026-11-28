// Scene factory, offscreen helper and builtin:triangle (scene definitions in Scenes.h).
#include "Scenes.h"

#include "SceneUtil.h"

#include <cstddef>
#include <span>
#include <string>

namespace eng::scenes {

// Defined in CubeScene.cpp / PbrSpheresScene.cpp.
rhi::Result<std::unique_ptr<Scene>> createTexturedCubeScene(rhi::IDevice& device, const SceneDesc& desc);
rhi::Result<std::unique_ptr<Scene>> createPbrSpheresScene(rhi::IDevice& device, const SceneDesc& desc);

namespace {

using namespace rhi;
using detail::Owned;

class TriangleScene final : public Scene {
public:
    TriangleScene(IDevice& dev, uint32_t w, uint32_t h) : dev_(dev), own_(dev), width_(w), height_(h) {}

    Result<void> init(const SceneDesc& d) {
        auto sp = detail::loadShaderPair(dev_, own_, d.shaderDir, "color_triangle", detail::tierKey(dev_, "default"));
        if (!sp) {
            return std::unexpected(sp.error());
        }
        GraphicsPipelineDesc pd;
        pd.vs = sp->vs;
        pd.ps = sp->ps;
        pd.vertexLayout.buffers = {{sizeof(TriangleVertex), false}};
        pd.vertexLayout.attributes = {{"POSITION", Format::RG32_FLOAT, 0, offsetof(TriangleVertex, position)},
                                      {"COLOR0", Format::RGBA32_FLOAT, 0, offsetof(TriangleVertex, color)}};
        pd.raster.cull = CullMode::None;
        pd.colorFormats = {d.colorFormat};
        pd.debugName = "builtin:triangle";
        auto pipe = dev_.createGraphicsPipeline(pd);
        if (!pipe) {
            return std::unexpected(pipe.error());
        }
        pipeline_ = own_.keep(*pipe);
        return rebuildVertices();
    }

    Result<void> resize(uint32_t w, uint32_t h) override {
        width_ = w;
        height_ = h;
        return rebuildVertices();
    }

    Result<void> record(ICommandList& cmd, TextureHandle color) override {
        RenderPassDesc rp;
        ColorAttachment ca;
        ca.texture = color;
        ca.load = LoadOp::Clear;
        ca.store = StoreOp::Store;
        for (int i = 0; i < 4; ++i) {
            ca.clear[i] = kTriangleClear[i];
        }
        rp.colors.push_back(ca);
        cmd.beginRenderPass(rp);
        cmd.setPipeline(pipeline_);
        cmd.setVertexBuffer(0, vb_, 0);
        cmd.setViewport({0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f});
        cmd.setScissor({0, 0, width_, height_});
        cmd.draw(3, 1, 0, 0);
        cmd.endRenderPass();
        return {};
    }

private:
    Result<void> rebuildVertices() {
        if (vb_) {
            dev_.destroy(vb_);
            vb_ = {};
        }
        TriangleVertex verts[3];
        triangleVertices(width_, height_, verts);
        BufferDesc bd;
        bd.size = sizeof(verts);
        bd.usage = BufferUsage::Vertex;
        bd.debugName = "builtin:triangle vertices";
        auto vb = dev_.createBuffer(bd, std::as_bytes(std::span(verts)));
        if (!vb) {
            return std::unexpected(vb.error());
        }
        vb_ = *vb;
        return {};
    }

public:
    ~TriangleScene() override {
        if (vb_) {
            dev_.destroy(vb_);
        }
    }

private:
    IDevice& dev_;
    Owned own_;
    PipelineHandle pipeline_;
    BufferHandle vb_;
    uint32_t width_, height_;
};

constexpr std::string_view kNames[] = {kTriangle, kTexturedCube, kPbrSpheres};

} // namespace

std::span<const std::string_view> sceneNames() {
    return kNames;
}

void triangleVertices(uint32_t width, uint32_t height, TriangleVertex out[3]) {
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    // Pixel position (fraction of a 64-px reference) -> NDC: x = px / (W / 2) - 1, y = 1 - py / (H / 2).
    auto ndc = [&](float fx, float fy, float* p) {
        const float px = fx / 64.0f * w, py = fy / 64.0f * h;
        p[0] = px / (w * 0.5f) - 1.0f;
        p[1] = 1.0f - py / (h * 0.5f);
    };
    ndc(8.4f, 8.4f, out[0].position);
    ndc(55.6f, 8.4f, out[1].position);
    ndc(32.5f, 56.75f, out[2].position);
    const float colors[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
    for (int i = 0; i < 3; ++i) {
        for (int c = 0; c < 4; ++c) {
            out[i].color[c] = colors[i][c];
        }
    }
}

Result<std::unique_ptr<Scene>> createScene(IDevice& dev, std::string_view name, const SceneDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        return std::unexpected(Error{kExitUsage, "scene " + std::string(name) + ": zero target size"});
    }
    if (name == kTriangle) {
        auto s = std::make_unique<TriangleScene>(dev, desc.width, desc.height);
        if (auto r = s->init(desc); !r) {
            return std::unexpected(r.error());
        }
        return std::unique_ptr<Scene>(std::move(s));
    }
    if (name == kTexturedCube) {
        return createTexturedCubeScene(dev, desc);
    }
    if (name == kPbrSpheres) {
        return createPbrSpheresScene(dev, desc);
    }
    return std::unexpected(Error{kExitUsage, "unknown scene " + std::string(name)});
}

Result<Image> renderOffscreen(IDevice& dev, Scene& scene, Format format, uint32_t width, uint32_t height) {
    TextureDesc td;
    td.width = width;
    td.height = height;
    td.format = format;
    td.usage = TextureUsage::RenderTarget | TextureUsage::CopySrc;
    td.debugName = "offscreen target";
    auto rt = dev.createTexture(td);
    if (!rt) {
        return std::unexpected(rt.error());
    }
    Owned own(dev);
    own.keep(*rt);
    ICommandList& cmd = dev.beginFrame();
    const TextureBarrier toTarget{*rt, ResourceState::Undefined, ResourceState::RenderTarget};
    cmd.barrier({&toTarget, 1});
    const Result<void> rec = scene.record(cmd, *rt);
    const TextureBarrier toCopy{*rt, ResourceState::RenderTarget, ResourceState::CopySrc};
    cmd.barrier({&toCopy, 1});
    const Result<void> sub = dev.submitAndWait();
    if (!rec) {
        return std::unexpected(rec.error());
    }
    if (!sub) {
        return std::unexpected(sub.error());
    }
    return dev.readback(*rt);
}

Result<void> renderFrameInto(IDevice& dev, Scene& scene, TextureHandle target, bool firstFrame) {
    ICommandList& cmd = dev.beginFrame();
    const TextureBarrier toTarget{target, firstFrame ? ResourceState::Undefined : ResourceState::CopySrc,
                                  ResourceState::RenderTarget};
    cmd.barrier({&toTarget, 1});
    const Result<void> rec = scene.record(cmd, target);
    const TextureBarrier toCopy{target, ResourceState::RenderTarget, ResourceState::CopySrc};
    cmd.barrier({&toCopy, 1});
    const Result<void> sub = dev.submitAndWait();
    if (!rec) {
        return std::unexpected(rec.error());
    }
    return sub;
}

Result<Image> renderSceneOffscreen(IDevice& dev, std::string_view name, const SceneDesc& desc) {
    Result<Image> img;
    {
        auto scene = createScene(dev, name, desc);
        if (!scene) {
            return std::unexpected(scene.error());
        }
        img = renderOffscreen(dev, **scene, desc.colorFormat, desc.width, desc.height);
    }
    // The scene's resources are destroyed now; this submit runs the deferred destruction.
    if (auto r = dev.submitAndWait(); !r && img) {
        return std::unexpected(r.error());
    }
    return img;
}

Result<Image> renderTriangle(IDevice& dev, const std::filesystem::path& shaderDir, uint32_t width, uint32_t height) {
    SceneDesc d;
    d.shaderDir = shaderDir;
    d.colorFormat = Format::RGBA8_UNORM;
    d.width = width;
    d.height = height;
    return renderSceneOffscreen(dev, kTriangle, d);
}

Result<Image> renderTexturedCube(IDevice& dev, const std::filesystem::path& shaderDir,
                                 const std::filesystem::path& assetDir, const CubeOptions& o) {
    if (o.target != Format::RGBA8_SRGB && o.target != Format::RGBA8_UNORM) {
        return std::unexpected(Error{kExitUsage, "builtin:textured-cube: target must be RGBA8_SRGB or RGBA8_UNORM"});
    }
    SceneDesc d;
    d.shaderDir = shaderDir;
    d.assetDir = assetDir;
    d.colorFormat = o.target;
    d.width = o.width;
    d.height = o.height;
    d.cubeCull = o.cull;
    return renderSceneOffscreen(dev, kTexturedCube, d);
}

} // namespace eng::scenes
