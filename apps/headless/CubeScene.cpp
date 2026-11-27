// builtin:textured-cube (plan F2 Task 3; scene definition in Scenes.h). Created once, recorded per frame
// (plan F2 Task 5 / 6): the viewer spins it with setTime().
#include "Scenes.h"

#include "Image.h"
#include "SceneUtil.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace eng::scenes {

namespace {

using namespace rhi;
using namespace detail;

// --- atlas / faces (must equal scripts/make_f02_assets.py) ---------------------------------------
constexpr float kAtlasSize = 256.0f;
constexpr float kBorder = 8.0f, kTileW = 80.0f, kTileH = 120.0f, kUvInset = 2.0f;

struct FaceFrame {
    float n[3], right[3], up[3];
};
// Face order = atlas tile order: +X, -X, +Y, -Y, +Z, -Z.
constexpr FaceFrame kFaces[6] = {
    {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
    {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},  {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
};
// Draw order: the faces visible from the camera first (+Z, -X, +Y), the hidden ones last, so without
// the depth test the hidden faces would cover the visible ones (depth probe of the test).
constexpr int kDrawOrder[6] = {4, 1, 2, 0, 3, 5};

struct CubeVertex {
    float position[3];
    float uv[2];
};

void cubeGeometry(std::vector<CubeVertex>& vertices, std::vector<uint16_t>& indices) {
    const float corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}}; // local (u, v), (0, 0) = top-left
    for (const int face : kDrawOrder) {
        const FaceFrame& f = kFaces[face];
        const float x0 = kBorder + kTileW * static_cast<float>(face % 3);
        const float y0 = kBorder + kTileH * static_cast<float>(face / 3);
        const auto base = static_cast<uint16_t>(vertices.size());
        for (const auto& c : corners) {
            CubeVertex v{};
            for (int k = 0; k < 3; ++k) {
                v.position[k] = 0.5f * f.n[k] + (c[0] - 0.5f) * f.right[k] + (0.5f - c[1]) * f.up[k];
            }
            v.uv[0] = (x0 + kUvInset + c[0] * (kTileW - 2.0f * kUvInset)) / kAtlasSize;
            v.uv[1] = (y0 + kUvInset + c[1] * (kTileH - 2.0f * kUvInset)) / kAtlasSize;
            vertices.push_back(v);
        }
        for (const int i : {0, 1, 2, 0, 2, 3}) {
            indices.push_back(static_cast<uint16_t>(base + i));
        }
    }
}

Mat4 cubeMvp(uint32_t width, uint32_t height, float timeSeconds) {
    const Mat4 spin = rotationY(kCubeSpinDegPerSecond * timeSeconds * kDeg);
    const Mat4 model = rotationX(20.0f * kDeg) * rotationY(30.0f * kDeg) * spin;
    const Mat4 view = translation(0.0f, 0.0f, -3.0f); // eye (0, 0, 3) looking down -Z, +Y up
    const Mat4 proj = perspectiveRhZo(45.0f * kDeg, static_cast<float>(width) / static_cast<float>(height), 0.1f, 10.0f);
    return proj * view * model;
}

// Mip chain of an sRGB RGBA8 image (power-of-two not required): 2x2 box filter in linear space (alpha
// linear), down to 1x1. Level 0 is the input.
std::vector<std::vector<uint8_t>> srgbMipChain(const std::vector<uint8_t>& level0, uint32_t w, uint32_t h) {
    std::vector<std::vector<uint8_t>> mips{level0};
    while (w > 1 || h > 1) {
        const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
        const std::vector<uint8_t>& src = mips.back();
        std::vector<uint8_t> dst(size_t(nw) * nh * 4u);
        for (uint32_t y = 0; y < nh; ++y) {
            for (uint32_t x = 0; x < nw; ++x) {
                for (int c = 0; c < 4; ++c) {
                    float sum = 0.0f;
                    for (uint32_t dy = 0; dy < 2; ++dy) {
                        for (uint32_t dx = 0; dx < 2; ++dx) {
                            const uint32_t sx = std::min(w - 1, 2 * x + dx), sy = std::min(h - 1, 2 * y + dy);
                            const float v = src[(size_t(sy) * w + sx) * 4u + size_t(c)] / 255.0f;
                            sum += c == 3 ? v : srgbToLinear(v);
                        }
                    }
                    const float avg = sum * 0.25f;
                    const float out = c == 3 ? avg : linearToSrgb(avg);
                    dst[(size_t(y) * nw + x) * 4u + size_t(c)] =
                        static_cast<uint8_t>(std::lround(std::clamp(out, 0.0f, 1.0f) * 255.0f));
                }
            }
        }
        mips.push_back(std::move(dst));
        w = nw;
        h = nh;
    }
    return mips;
}

class TexturedCubeScene final : public Scene {
public:
    TexturedCubeScene(IDevice& dev, const SceneDesc& d) : dev_(dev), own_(dev), desc_(d) {}
    ~TexturedCubeScene() override {
        if (depth_) {
            dev_.destroy(depth_);
        }
    }

    Result<void> init() {
        auto sp = loadShaderPair(dev_, own_, desc_.shaderDir, "textured", "default");
        if (!sp) {
            return std::unexpected(sp.error());
        }
        // Texture: sRGB atlas + CPU mips.
        const std::filesystem::path png = desc_.assetDir / "f02" / "cube_faces.png";
        auto atlas = golden::loadPng(png);
        if (!atlas) {
            return std::unexpected(Error{kExitUsage, "builtin:textured-cube: " + atlas.error()});
        }
        const auto aw = static_cast<uint32_t>(atlas->width), ah = static_cast<uint32_t>(atlas->height);
        const std::vector<std::vector<uint8_t>> mips = srgbMipChain(atlas->rgba8, aw, ah);
        std::vector<SubresourceData> init;
        for (const auto& m : mips) {
            init.push_back({std::as_bytes(std::span(m)), 0});
        }
        TextureDesc td;
        td.width = aw;
        td.height = ah;
        td.mipLevels = static_cast<uint32_t>(mips.size());
        td.format = Format::RGBA8_SRGB;
        td.usage = TextureUsage::Sampled;
        td.debugName = "cube_faces.png";
        auto tex = dev_.createTexture(td, init);
        if (!tex) {
            return std::unexpected(tex.error());
        }
        own_.keep(*tex);
        SamplerDesc sd;
        sd.addressU = sd.addressV = sd.addressW = AddressMode::ClampToEdge;
        auto smp = dev_.createSampler(sd);
        if (!smp) {
            return std::unexpected(smp.error());
        }
        own_.keep(*smp);

        GraphicsPipelineDesc pd;
        pd.vs = sp->vs;
        pd.ps = sp->ps;
        pd.vertexLayout.buffers = {{sizeof(CubeVertex), false}};
        pd.vertexLayout.attributes = {{"POSITION", Format::RGB32_FLOAT, 0, offsetof(CubeVertex, position)},
                                      {"TEXCOORD0", Format::RG32_FLOAT, 0, offsetof(CubeVertex, uv)}};
        pd.raster.cull = CullMode::None;
        pd.depth = {true, true, CompareOp::Less};
        pd.colorFormats = {desc_.colorFormat};
        pd.depthFormat = Format::D32_FLOAT;
        pd.debugName = "builtin:textured-cube";
        auto pipe = dev_.createGraphicsPipeline(pd);
        if (!pipe) {
            return std::unexpected(pipe.error());
        }
        pipeline_ = own_.keep(*pipe);

        // Device-local geometry (staging upload).
        std::vector<CubeVertex> vertices;
        std::vector<uint16_t> indices;
        cubeGeometry(vertices, indices);
        indexCount_ = static_cast<uint32_t>(indices.size());
        BufferDesc vbd;
        vbd.size = vertices.size() * sizeof(CubeVertex);
        vbd.usage = BufferUsage::Vertex;
        vbd.debugName = "builtin:textured-cube vertices";
        auto vb = dev_.createBuffer(vbd, std::as_bytes(std::span(vertices)));
        if (!vb) {
            return std::unexpected(vb.error());
        }
        vb_ = own_.keep(*vb);
        BufferDesc ibd;
        ibd.size = indices.size() * sizeof(uint16_t);
        ibd.usage = BufferUsage::Index;
        ibd.debugName = "builtin:textured-cube indices";
        auto ib = dev_.createBuffer(ibd, std::as_bytes(std::span(indices)));
        if (!ib) {
            return std::unexpected(ib.error());
        }
        ib_ = own_.keep(*ib);

        BindGroupDesc mat;
        mat.pipeline = pipeline_;
        mat.space = 2;
        mat.entries = {{"faceTexture", *tex}, {"faceSampler", *smp}};
        auto material = dev_.createBindGroup(mat);
        if (!material) {
            return std::unexpected(material.error());
        }
        material_ = own_.keep(*material);
        return resize(desc_.width, desc_.height);
    }

    Result<void> resize(uint32_t w, uint32_t h) override {
        if (depth_) {
            dev_.destroy(depth_);
            depth_ = {};
        }
        desc_.width = w;
        desc_.height = h;
        TextureDesc dd;
        dd.width = w;
        dd.height = h;
        dd.format = Format::D32_FLOAT;
        dd.usage = TextureUsage::DepthStencil;
        dd.debugName = "builtin:textured-cube depth";
        auto depth = dev_.createTexture(dd);
        if (!depth) {
            return std::unexpected(depth.error());
        }
        depth_ = *depth;
        depthState_ = ResourceState::Undefined;
        return {};
    }

    void setTime(float seconds) override { time_ = seconds; }

    Result<void> record(ICommandList& cmd, TextureHandle color) override {
        const Mat4 mvp = cubeMvp(desc_.width, desc_.height, time_);
        const BufferBinding drawConstants = dev_.allocTransient(sizeof(mvp.m), 256);
        if (!drawConstants.buffer) {
            return std::unexpected(Error{kExitUsage, "builtin:textured-cube: allocTransient failed"});
        }
        std::memcpy(drawConstants.cpu, mvp.m.data(), sizeof(mvp.m));
        BindGroupDesc drw;
        drw.pipeline = pipeline_;
        drw.space = 3;
        drw.entries = {{"Draw", drawConstants}};
        auto draw = dev_.createBindGroup(drw);
        if (!draw) {
            return std::unexpected(draw.error());
        }

        // DepthWrite -> DepthWrite after the first frame: orders this frame's depth clear after the
        // previous frame's depth writes (frames in flight share the buffer).
        const TextureBarrier toDepth{depth_, depthState_, ResourceState::DepthWrite};
        cmd.barrier({&toDepth, 1});
        depthState_ = ResourceState::DepthWrite;
        RenderPassDesc rp;
        ColorAttachment ca;
        ca.texture = color;
        // Background reads back as 51 = 0.2 * 255 on both targets (sRGB target: the linear value that encodes to 51).
        const float clear = isSrgb(desc_.colorFormat) ? srgbToLinear(51.0f / 255.0f) : 0.2f;
        ca.clear[0] = ca.clear[1] = ca.clear[2] = clear;
        ca.clear[3] = 1.0f;
        rp.colors.push_back(ca);
        rp.depth.texture = depth_;
        rp.depth.load = LoadOp::Clear;
        rp.depth.clearDepth = 1.0f;
        cmd.beginRenderPass(rp);
        cmd.setPipeline(pipeline_);
        cmd.setBindGroup(2, material_);
        cmd.setBindGroup(3, *draw);
        cmd.setVertexBuffer(0, vb_, 0);
        cmd.setIndexBuffer(ib_, IndexType::Uint16, 0);
        cmd.setViewport({0.0f, 0.0f, static_cast<float>(desc_.width), static_cast<float>(desc_.height), 0.0f, 1.0f});
        cmd.setScissor({0, 0, desc_.width, desc_.height});
        cmd.drawIndexed(indexCount_, 1, 0, 0, 0);
        cmd.endRenderPass();
        dev_.destroy(*draw); // deferred until the GPU is done with this frame
        return {};
    }

private:
    IDevice& dev_;
    Owned own_;
    SceneDesc desc_;
    PipelineHandle pipeline_;
    BufferHandle vb_, ib_;
    BindGroupHandle material_;
    TextureHandle depth_;
    ResourceState depthState_ = ResourceState::Undefined;
    uint32_t indexCount_ = 0;
    float time_ = 0.0f;
};

} // namespace

Result<std::unique_ptr<Scene>> createTexturedCubeScene(IDevice& dev, const SceneDesc& desc) {
    auto s = std::make_unique<TexturedCubeScene>(dev, desc);
    if (auto r = s->init(); !r) {
        return std::unexpected(r.error());
    }
    return std::unique_ptr<Scene>(std::move(s));
}

} // namespace eng::scenes
