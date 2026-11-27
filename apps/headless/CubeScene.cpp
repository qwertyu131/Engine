// builtin:textured-cube (plan F2 Task 3; scene definition in Scenes.h).
#include "Scenes.h"

#include "Image.h"
#include "rhi/ShaderLibrary.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <numbers>
#include <span>
#include <vector>

namespace eng::scenes {

namespace {

using namespace rhi;

// Column-major 4x4 (element (row r, column c) at m[c * 4 + r]): the layout of an HLSL column_major
// float4x4, used with mul(M, v) in the shaders.
struct Mat4 {
    std::array<float, 16> m{};
    float& at(int r, int c) { return m[static_cast<size_t>(c * 4 + r)]; }
    float at(int r, int c) const { return m[static_cast<size_t>(c * 4 + r)]; }
    static Mat4 identity() {
        Mat4 x;
        for (int i = 0; i < 4; ++i) {
            x.at(i, i) = 1.0f;
        }
        return x;
    }
};

Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 x;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) {
                s += a.at(r, k) * b.at(k, c);
            }
            x.at(r, c) = s;
        }
    }
    return x;
}

Mat4 rotationX(float rad) {
    Mat4 x = Mat4::identity();
    x.at(1, 1) = std::cos(rad);
    x.at(1, 2) = -std::sin(rad);
    x.at(2, 1) = std::sin(rad);
    x.at(2, 2) = std::cos(rad);
    return x;
}

Mat4 rotationY(float rad) {
    Mat4 x = Mat4::identity();
    x.at(0, 0) = std::cos(rad);
    x.at(0, 2) = std::sin(rad);
    x.at(2, 0) = -std::sin(rad);
    x.at(2, 2) = std::cos(rad);
    return x;
}

Mat4 translation(float tx, float ty, float tz) {
    Mat4 x = Mat4::identity();
    x.at(0, 3) = tx;
    x.at(1, 3) = ty;
    x.at(2, 3) = tz;
    return x;
}

// Right-handed perspective, clip Z in [0, 1] (RHI convention), +Y up.
Mat4 perspectiveRhZo(float fovY, float aspect, float zNear, float zFar) {
    const float f = 1.0f / std::tan(fovY * 0.5f);
    Mat4 x;
    x.at(0, 0) = f / aspect;
    x.at(1, 1) = f;
    x.at(2, 2) = zFar / (zNear - zFar);
    x.at(2, 3) = zNear * zFar / (zNear - zFar);
    x.at(3, 2) = -1.0f;
    return x;
}

constexpr float kDeg = std::numbers::pi_v<float> / 180.0f;

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

Mat4 cubeMvp(uint32_t width, uint32_t height) {
    const Mat4 model = rotationX(20.0f * kDeg) * rotationY(30.0f * kDeg);
    const Mat4 view = translation(0.0f, 0.0f, -3.0f); // eye (0, 0, 3) looking down -Z, +Y up
    const Mat4 proj = perspectiveRhZo(45.0f * kDeg, static_cast<float>(width) / static_cast<float>(height), 0.1f, 10.0f);
    return proj * view * model;
}

// --- sRGB + CPU mips -------------------------------------------------------------------------------
float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float linearToSrgb(float c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
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

// Destroys the handles it owns when the scene returns (success or error).
struct Owned {
    IDevice& dev;
    std::vector<Handle> handles;
    template <class H>
    H keep(H h) {
        handles.push_back(h);
        return h;
    }
    void release() {
        for (auto it = handles.rbegin(); it != handles.rend(); ++it) {
            dev.destroy(*it);
        }
        handles.clear();
    }
    ~Owned() { release(); }
};

} // namespace

Result<Image> renderTexturedCube(IDevice& dev, const std::filesystem::path& shaderDir,
                                 const std::filesystem::path& assetDir, const CubeOptions& o) {
    if (o.target != Format::RGBA8_SRGB && o.target != Format::RGBA8_UNORM) {
        return std::unexpected(Error{kExitUsage, "builtin:textured-cube: target must be RGBA8_SRGB or RGBA8_UNORM"});
    }
    const Backend backend = dev.caps().backend;
    Owned own{dev, {}};

    auto vsBlob = ShaderLibrary::load(shaderDir, "textured", "VSMain", backend, "default");
    if (!vsBlob) {
        return std::unexpected(vsBlob.error());
    }
    auto psBlob = ShaderLibrary::load(shaderDir, "textured", "PSMain", backend, "default");
    if (!psBlob) {
        return std::unexpected(psBlob.error());
    }
    auto vs = dev.createShader(*vsBlob);
    if (!vs) {
        return std::unexpected(vs.error());
    }
    own.keep(*vs);
    auto ps = dev.createShader(*psBlob);
    if (!ps) {
        return std::unexpected(ps.error());
    }
    own.keep(*ps);

    // Texture: sRGB atlas + CPU mips.
    const std::filesystem::path png = assetDir / "f02" / "cube_faces.png";
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
    auto tex = dev.createTexture(td, init);
    if (!tex) {
        return std::unexpected(tex.error());
    }
    own.keep(*tex);
    SamplerDesc sd;
    sd.addressU = sd.addressV = sd.addressW = AddressMode::ClampToEdge;
    auto smp = dev.createSampler(sd);
    if (!smp) {
        return std::unexpected(smp.error());
    }
    own.keep(*smp);

    // Targets.
    TextureDesc cd;
    cd.width = o.width;
    cd.height = o.height;
    cd.format = o.target;
    cd.usage = TextureUsage::RenderTarget | TextureUsage::CopySrc;
    cd.debugName = "builtin:textured-cube color";
    auto color = dev.createTexture(cd);
    if (!color) {
        return std::unexpected(color.error());
    }
    own.keep(*color);
    TextureDesc dd;
    dd.width = o.width;
    dd.height = o.height;
    dd.format = Format::D32_FLOAT;
    dd.usage = TextureUsage::DepthStencil;
    dd.debugName = "builtin:textured-cube depth";
    auto depth = dev.createTexture(dd);
    if (!depth) {
        return std::unexpected(depth.error());
    }
    own.keep(*depth);

    GraphicsPipelineDesc pd;
    pd.vs = *vs;
    pd.ps = *ps;
    pd.vertexLayout.buffers = {{sizeof(CubeVertex), false}};
    pd.vertexLayout.attributes = {{"POSITION", Format::RGB32_FLOAT, 0, offsetof(CubeVertex, position)},
                                  {"TEXCOORD0", Format::RG32_FLOAT, 0, offsetof(CubeVertex, uv)}};
    pd.raster.cull = CullMode::None;
    pd.depth = {true, true, CompareOp::Less};
    pd.colorFormats = {o.target};
    pd.depthFormat = Format::D32_FLOAT;
    pd.debugName = "builtin:textured-cube";
    auto pipe = dev.createGraphicsPipeline(pd);
    if (!pipe) {
        return std::unexpected(pipe.error());
    }
    own.keep(*pipe);

    // Device-local geometry (staging upload).
    std::vector<CubeVertex> vertices;
    std::vector<uint16_t> indices;
    cubeGeometry(vertices, indices);
    BufferDesc vbd;
    vbd.size = vertices.size() * sizeof(CubeVertex);
    vbd.usage = BufferUsage::Vertex;
    vbd.debugName = "builtin:textured-cube vertices";
    auto vb = dev.createBuffer(vbd, std::as_bytes(std::span(vertices)));
    if (!vb) {
        return std::unexpected(vb.error());
    }
    own.keep(*vb);
    BufferDesc ibd;
    ibd.size = indices.size() * sizeof(uint16_t);
    ibd.usage = BufferUsage::Index;
    ibd.debugName = "builtin:textured-cube indices";
    auto ib = dev.createBuffer(ibd, std::as_bytes(std::span(indices)));
    if (!ib) {
        return std::unexpected(ib.error());
    }
    own.keep(*ib);

    BindGroupDesc mat;
    mat.pipeline = *pipe;
    mat.space = 2;
    mat.entries = {{"faceTexture", *tex}, {"faceSampler", *smp}};
    auto material = dev.createBindGroup(mat);
    if (!material) {
        return std::unexpected(material.error());
    }
    own.keep(*material);

    ICommandList& cmd = dev.beginFrame();
    const Mat4 mvp = cubeMvp(o.width, o.height);
    const BufferBinding drawConstants = dev.allocTransient(sizeof(mvp.m), 256);
    if (!drawConstants.buffer) {
        (void)dev.submitAndWait();
        return std::unexpected(Error{kExitUsage, "builtin:textured-cube: allocTransient failed"});
    }
    std::memcpy(drawConstants.cpu, mvp.m.data(), sizeof(mvp.m));
    BindGroupDesc drw;
    drw.pipeline = *pipe;
    drw.space = 3;
    drw.entries = {{"Draw", drawConstants}};
    auto draw = dev.createBindGroup(drw);
    if (!draw) {
        (void)dev.submitAndWait();
        return std::unexpected(draw.error());
    }
    own.keep(*draw);

    const TextureBarrier toTargets[] = {{*color, ResourceState::Undefined, ResourceState::RenderTarget},
                                        {*depth, ResourceState::Undefined, ResourceState::DepthWrite}};
    cmd.barrier(toTargets);
    RenderPassDesc rp;
    ColorAttachment ca;
    ca.texture = *color;
    // Background reads back as 51 = 0.2 * 255 on both targets (sRGB target: the linear value that encodes to 51).
    const float clear = o.target == Format::RGBA8_SRGB ? srgbToLinear(51.0f / 255.0f) : 0.2f;
    ca.clear[0] = ca.clear[1] = ca.clear[2] = clear;
    ca.clear[3] = 1.0f;
    rp.colors.push_back(ca);
    rp.depth.texture = *depth;
    rp.depth.load = LoadOp::Clear;
    rp.depth.clearDepth = 1.0f;
    cmd.beginRenderPass(rp);
    cmd.setPipeline(*pipe);
    cmd.setBindGroup(2, *material);
    cmd.setBindGroup(3, *draw);
    cmd.setVertexBuffer(0, *vb, 0);
    cmd.setIndexBuffer(*ib, IndexType::Uint16, 0);
    cmd.setViewport({0.0f, 0.0f, static_cast<float>(o.width), static_cast<float>(o.height), 0.0f, 1.0f});
    cmd.setScissor({0, 0, o.width, o.height});
    cmd.drawIndexed(static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
    cmd.endRenderPass();
    const TextureBarrier toCopy{*color, ResourceState::RenderTarget, ResourceState::CopySrc};
    cmd.barrier({&toCopy, 1});
    if (auto r = dev.submitAndWait(); !r) {
        return std::unexpected(r.error());
    }
    auto img = dev.readback(*color);
    if (!img) {
        return img;
    }
    own.release(); // destroy now, so the submit below runs the deferred destruction
    if (auto r = dev.submitAndWait(); !r) {
        return std::unexpected(r.error());
    }
    return img;
}

} // namespace eng::scenes
