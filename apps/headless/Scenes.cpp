#include "Scenes.h"

#include "rhi/ShaderLibrary.h"

#include <cstddef>
#include <span>

namespace eng::scenes {

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

rhi::Result<rhi::Image> renderTriangle(rhi::IDevice& dev, const std::filesystem::path& shaderDir, uint32_t width,
                                       uint32_t height) {
    using namespace rhi;
    const Backend backend = dev.caps().backend;
    auto vsBlob = ShaderLibrary::load(shaderDir, "color_triangle", "VSMain", backend, "default");
    if (!vsBlob) {
        return std::unexpected(vsBlob.error());
    }
    auto psBlob = ShaderLibrary::load(shaderDir, "color_triangle", "PSMain", backend, "default");
    if (!psBlob) {
        return std::unexpected(psBlob.error());
    }
    auto vs = dev.createShader(*vsBlob);
    if (!vs) {
        return std::unexpected(vs.error());
    }
    auto ps = dev.createShader(*psBlob);
    if (!ps) {
        return std::unexpected(ps.error());
    }

    TextureDesc td;
    td.width = width;
    td.height = height;
    td.format = Format::RGBA8_UNORM;
    td.usage = TextureUsage::RenderTarget | TextureUsage::CopySrc;
    td.debugName = "builtin:triangle target";
    auto rt = dev.createTexture(td);
    if (!rt) {
        return std::unexpected(rt.error());
    }

    GraphicsPipelineDesc pd;
    pd.vs = *vs;
    pd.ps = *ps;
    pd.vertexLayout.buffers = {{sizeof(TriangleVertex), false}};
    pd.vertexLayout.attributes = {{"POSITION", Format::RG32_FLOAT, 0, offsetof(TriangleVertex, position)},
                                  {"COLOR0", Format::RGBA32_FLOAT, 0, offsetof(TriangleVertex, color)}};
    pd.raster.cull = CullMode::None;
    pd.colorFormats = {Format::RGBA8_UNORM};
    pd.debugName = "builtin:triangle";
    auto pipe = dev.createGraphicsPipeline(pd);
    if (!pipe) {
        return std::unexpected(pipe.error());
    }

    TriangleVertex verts[3];
    triangleVertices(width, height, verts);
    BufferDesc bd;
    bd.size = sizeof(verts);
    bd.usage = BufferUsage::Vertex;
    bd.debugName = "builtin:triangle vertices";
    auto vb = dev.createBuffer(bd, std::as_bytes(std::span(verts)));
    if (!vb) {
        return std::unexpected(vb.error());
    }

    ICommandList& cmd = dev.beginFrame();
    const TextureBarrier toTarget{*rt, ResourceState::Undefined, ResourceState::RenderTarget};
    cmd.barrier({&toTarget, 1});
    RenderPassDesc rp;
    ColorAttachment color;
    color.texture = *rt;
    color.load = LoadOp::Clear;
    color.store = StoreOp::Store;
    for (int i = 0; i < 4; ++i) {
        color.clear[i] = kTriangleClear[i];
    }
    rp.colors.push_back(color);
    cmd.beginRenderPass(rp);
    cmd.setPipeline(*pipe);
    cmd.setVertexBuffer(0, *vb, 0);
    cmd.setViewport({0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f});
    cmd.setScissor({0, 0, width, height});
    cmd.draw(3, 1, 0, 0);
    cmd.endRenderPass();
    const TextureBarrier toCopy{*rt, ResourceState::RenderTarget, ResourceState::CopySrc};
    cmd.barrier({&toCopy, 1});
    if (auto r = dev.submitAndWait(); !r) {
        return std::unexpected(r.error());
    }
    auto img = dev.readback(*rt);
    dev.destroy(*vb);
    dev.destroy(*pipe);
    dev.destroy(*vs);
    dev.destroy(*ps);
    dev.destroy(*rt);
    if (auto r = dev.submitAndWait(); !r && img) {
        return std::unexpected(r.error());
    }
    return img;
}

} // namespace eng::scenes
