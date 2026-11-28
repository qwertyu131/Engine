// D3D11-only RHI rules (plan F2 Task 4):
//   - Review Focus 2: a texture bound as SRV while it is the bound render target -> debug-layer message
//     -> exit 4 (the validation hook is really wired for resource hazards);
//   - F1 Risk 1: a VS / PS pair from different DXBC paths is rejected at pipeline creation;
//     a dxbc blob whose reflection does not record its path is rejected too (the check must not be
//     silently skipped).
// Plan F3 Task 1 (F2 carry-overs):
//   - setBindGroup checks the group's layout against the bound pipeline's layout for that space (names,
//     kinds, per-stage register class / slot): a pbr UNLIT1 material group bound with the UNLIT0 pipeline
//     (baseColorTexture t0 vs t3) is a validation error (exit 4), a group without a bound pipeline too; a
//     space whose layout is identical in both pipelines (space3 Object, b1) still binds cleanly;
//   - a uniform buffer smaller than 256 bytes bound whole (size 0) or with its exact size, then drawn,
//     gives no debug-layer message (count clamped to the buffer, buffers padded to 256 bytes).
#include "rhi/Rhi.h"
#include "rhi/ShaderLibrary.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "RhiTestEnv.h"

using namespace eng::rhi;
using eng::rhi::test::currentBackend;
using eng::rhi::test::shaderLibDir;

namespace {

std::unique_ptr<IDevice> makeDevice() {
    auto dev = createDevice(currentBackend().desc);
    REQUIRE_MESSAGE(dev.has_value(), (dev ? std::string() : dev.error().message));
    return std::move(*dev);
}

ShaderHandle loadShader(IDevice& dev, const char* entry, const char* key, bool dropPath = false) {
    auto blob = ShaderLibrary::load(shaderLibDir(), "textured", entry, Backend::D3D11, key);
    REQUIRE_MESSAGE(blob.has_value(), (blob ? std::string() : blob.error().message));
    if (dropPath) {
        blob->reflection.dxbcPath.clear();
    }
    auto s = dev.createShader(*blob);
    REQUIRE_MESSAGE(s.has_value(), (s ? std::string() : s.error().message));
    return *s;
}

GraphicsPipelineDesc texturedDesc(ShaderHandle vs, ShaderHandle ps) {
    GraphicsPipelineDesc pd;
    pd.vs = vs;
    pd.ps = ps;
    pd.vertexLayout.buffers = {{20, false}};
    pd.vertexLayout.attributes = {{"POSITION", Format::RGB32_FLOAT, 0, 0}, {"TEXCOORD0", Format::RG32_FLOAT, 0, 12}};
    pd.colorFormats = {Format::RGBA8_UNORM};
    return pd;
}

PipelineHandle pbrPipeline(IDevice& dev, const char* key) {
    ShaderHandle st[2];
    const char* entries[2] = {"VSMain", "PSMain"};
    for (int i = 0; i < 2; ++i) {
        auto blob = ShaderLibrary::load(shaderLibDir(), "pbr", entries[i], Backend::D3D11, key);
        REQUIRE_MESSAGE(blob.has_value(), (blob ? std::string() : blob.error().message));
        auto s = dev.createShader(*blob);
        REQUIRE_MESSAGE(s.has_value(), (s ? std::string() : s.error().message));
        st[i] = *s;
    }
    GraphicsPipelineDesc pd;
    pd.vs = st[0];
    pd.ps = st[1];
    // PbrVertex of apps/headless/PbrSpheresScene.cpp: position, normal, tangent, uv0, uv1, color.
    pd.vertexLayout.buffers = {{72, false}};
    pd.vertexLayout.attributes = {{"POSITION", Format::RGB32_FLOAT, 0, 0},  {"NORMAL", Format::RGB32_FLOAT, 0, 12},
                                  {"TANGENT", Format::RGBA32_FLOAT, 0, 24}, {"TEXCOORD0", Format::RG32_FLOAT, 0, 40},
                                  {"TEXCOORD1", Format::RG32_FLOAT, 0, 48}, {"COLOR0", Format::RGBA32_FLOAT, 0, 56}};
    pd.colorFormats = {Format::RGBA8_UNORM};
    auto p = dev.createGraphicsPipeline(pd);
    REQUIRE_MESSAGE(p.has_value(), (p ? std::string() : p.error().message));
    return *p;
}

BufferHandle uniformBuffer(IDevice& dev, uint64_t size) {
    BufferDesc bd;
    bd.size = size;
    bd.usage = BufferUsage::Uniform;
    const std::vector<std::byte> zeros(size, std::byte{0});
    auto b = dev.createBuffer(bd, zeros);
    REQUIRE_MESSAGE(b.has_value(), (b ? std::string() : b.error().message));
    return *b;
}

TextureHandle whiteTexel(IDevice& dev) {
    TextureDesc td;
    td.width = td.height = 1;
    td.format = Format::RGBA8_UNORM;
    td.usage = TextureUsage::Sampled;
    static const std::array<uint8_t, 4> white{255, 255, 255, 255};
    const SubresourceData texel{std::as_bytes(std::span(white)), 0};
    auto tex = dev.createTexture(td, {&texel, 1});
    REQUIRE_MESSAGE(tex.has_value(), (tex ? std::string() : tex.error().message));
    return *tex;
}

TextureHandle colorTarget(IDevice& dev) {
    TextureDesc td;
    td.width = td.height = 16;
    td.format = Format::RGBA8_UNORM;
    td.usage = TextureUsage::RenderTarget | TextureUsage::CopySrc;
    auto rt = dev.createTexture(td);
    REQUIRE(rt.has_value());
    return *rt;
}

void beginPass(ICommandList& cmd, TextureHandle rt) {
    RenderPassDesc rp;
    rp.colors.push_back({rt, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 1}});
    cmd.beginRenderPass(rp);
}

} // namespace

TEST_SUITE("rhi.d3d11") {

TEST_CASE("setBindGroup: a group whose layout differs from the bound pipeline's -> exit 4, bind skipped") {
    auto dev = makeDevice();
    const PipelineHandle lit = pbrPipeline(*dev, "T1_SKIN0_MORPH0_UNLIT0");
    const PipelineHandle unlit = pbrPipeline(*dev, "T1_SKIN0_MORPH0_UNLIT1");
    const TextureHandle tex = whiteTexel(*dev);
    auto smp = dev->createSampler({});
    REQUIRE(smp.has_value());
    // Space 2 of the UNLIT1 pipeline: Material b1, baseColorTexture t0, baseColorSampler s0 (UNLIT0: t3 / s2).
    BindGroupDesc md;
    md.pipeline = unlit;
    md.space = 2;
    md.entries = {{"Material", BufferBinding{uniformBuffer(*dev, 272), 0, 0}},
                  {"baseColorTexture", tex},
                  {"baseColorSampler", *smp}};
    auto material = dev->createBindGroup(md);
    REQUIRE_MESSAGE(material.has_value(), (material ? std::string() : material.error().message));
    // Space 3 is the same in both pipelines (Object, VS b1): bindable with either.
    BindGroupDesc od;
    od.pipeline = lit;
    od.space = 3;
    od.entries = {{"Object", BufferBinding{uniformBuffer(*dev, 176), 0, 0}}};
    auto object = dev->createBindGroup(od);
    REQUIRE_MESSAGE(object.has_value(), (object ? std::string() : object.error().message));
    const TextureHandle rt = colorTarget(*dev);
    REQUIRE(dev->validationErrorCount() == 0);

    SUBCASE("same layout in both pipelines: binds cleanly") {
        ICommandList& cmd = dev->beginFrame();
        beginPass(cmd, rt);
        cmd.setPipeline(unlit);
        cmd.setBindGroup(3, *object);
        cmd.setBindGroup(2, *material);
        cmd.setPipeline(lit);
        cmd.setBindGroup(3, *object);
        cmd.endRenderPass();
        auto r = dev->submitAndWait();
        CHECK_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
        CHECK(dev->validationErrorCount() == 0);
    }
    SUBCASE("UNLIT1 material group with the UNLIT0 pipeline") {
        ICommandList& cmd = dev->beginFrame();
        beginPass(cmd, rt);
        cmd.setPipeline(lit);
        cmd.setBindGroup(2, *material);
        cmd.endRenderPass();
        auto r = dev->submitAndWait();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().exitCode == 4);
        CAPTURE(r.error().message);
        CHECK(r.error().message.find("bind group space 2 layout does not match pipeline") != std::string::npos);
        CHECK(dev->validationErrorCount() == 1);
    }
    SUBCASE("no pipeline bound") {
        ICommandList& cmd = dev->beginFrame();
        beginPass(cmd, rt);
        cmd.setBindGroup(3, *object);
        cmd.endRenderPass();
        auto r = dev->submitAndWait();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().exitCode == 4);
        CAPTURE(r.error().message);
        CHECK(r.error().message.find("bind group space 3 bound without a pipeline") != std::string::npos);
    }
}

TEST_CASE("a uniform buffer smaller than 256 bytes, bound whole or with its size and drawn: no debug-layer message") {
    uint64_t size = 0;
    SUBCASE("bound whole (size 0)") { size = 0; }
    SUBCASE("bound with its size (64)") { size = 64; }
    CAPTURE(size);
    auto dev = makeDevice();
    const ShaderHandle vs = loadShader(*dev, "VSMain", "default");
    const ShaderHandle ps = loadShader(*dev, "PSMain", "default");
    auto pipe = dev->createGraphicsPipeline(texturedDesc(vs, ps));
    REQUIRE_MESSAGE(pipe.has_value(), (pipe ? std::string() : pipe.error().message));
    auto smp = dev->createSampler({});
    REQUIRE(smp.has_value());
    BindGroupDesc mat;
    mat.pipeline = *pipe;
    mat.space = 2;
    mat.entries = {{"faceTexture", whiteTexel(*dev)}, {"faceSampler", *smp}};
    auto material = dev->createBindGroup(mat);
    REQUIRE(material.has_value());
    BufferDesc vbd;
    vbd.size = 3 * 20;
    vbd.usage = BufferUsage::Vertex;
    const std::vector<std::byte> zeros(vbd.size, std::byte{0});
    auto vb = dev->createBuffer(vbd, zeros);
    REQUIRE(vb.has_value());
    // cbuffer Draw = one float4x4 (64 bytes), the buffer exactly that size.
    BindGroupDesc dd;
    dd.pipeline = *pipe;
    dd.space = 3;
    dd.entries = {{"Draw", BufferBinding{uniformBuffer(*dev, 64), 0, size}}};
    auto drawGroup = dev->createBindGroup(dd);
    REQUIRE_MESSAGE(drawGroup.has_value(), (drawGroup ? std::string() : drawGroup.error().message));
    const TextureHandle rt = colorTarget(*dev);
    REQUIRE(dev->validationErrorCount() == 0);

    ICommandList& cmd = dev->beginFrame();
    beginPass(cmd, rt);
    cmd.setPipeline(*pipe);
    cmd.setBindGroup(2, *material);
    cmd.setBindGroup(3, *drawGroup);
    cmd.setVertexBuffer(0, *vb, 0);
    cmd.setViewport({0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f});
    cmd.setScissor({0, 0, 16, 16});
    cmd.draw(3, 1, 0, 0);
    cmd.endRenderPass();
    auto r = dev->submitAndWait();
    CHECK_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
    CHECK(dev->validationErrorCount() == 0);
}


TEST_CASE("negative: texture bound as SRV while it is the render target -> exit 4") {
    auto dev = makeDevice();
    const ShaderHandle vs = loadShader(*dev, "VSMain", "default");
    const ShaderHandle ps = loadShader(*dev, "PSMain", "default");
    auto pipe = dev->createGraphicsPipeline(texturedDesc(vs, ps));
    REQUIRE_MESSAGE(pipe.has_value(), (pipe ? std::string() : pipe.error().message));

    TextureDesc td;
    td.width = td.height = 16;
    td.format = Format::RGBA8_UNORM;
    td.usage = TextureUsage::RenderTarget | TextureUsage::Sampled;
    auto rt = dev->createTexture(td);
    REQUIRE(rt.has_value());
    auto smp = dev->createSampler({});
    REQUIRE(smp.has_value());
    BindGroupDesc mat;
    mat.pipeline = *pipe;
    mat.space = 2;
    mat.entries = {{"faceTexture", *rt}, {"faceSampler", *smp}}; // the render target itself
    auto material = dev->createBindGroup(mat);
    REQUIRE_MESSAGE(material.has_value(), (material ? std::string() : material.error().message));
    REQUIRE(dev->validationErrorCount() == 0);

    ICommandList& cmd = dev->beginFrame();
    RenderPassDesc rp;
    rp.colors.push_back({*rt, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 1}});
    cmd.beginRenderPass(rp);
    cmd.setPipeline(*pipe);
    cmd.setBindGroup(2, *material); // SRV of the bound RTV
    cmd.endRenderPass();
    auto r = dev->submitAndWait();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().exitCode == 4);
    CAPTURE(r.error().message);
    // "WARNING id=... ID3D11DeviceContext::PSSetShaderResources: Resource being set to PS shader resource
    // slot 0 is still bound on output! Forcing to NULL."
    CHECK(r.error().message.find("PSSetShaderResources") != std::string::npos);
    CHECK(dev->validationErrorCount() > 0);
}

TEST_CASE("mixed DXBC paths: a cross VS with a direct PS is rejected at pipeline creation") {
    auto dev = makeDevice();
    const ShaderHandle vsCross = loadShader(*dev, "VSMain", "default");
    const ShaderHandle psDirect = loadShader(*dev, "PSMain", "default-direct");
    auto mixed = dev->createGraphicsPipeline(texturedDesc(vsCross, psDirect));
    REQUIRE_FALSE(mixed.has_value());
    CHECK(mixed.error().exitCode == 2);
    CHECK(mixed.error().message == "VS and PS from different dxbc paths");

    // Same path on both stages: accepted (cross / cross and direct / direct).
    const ShaderHandle psCross = loadShader(*dev, "PSMain", "default");
    const ShaderHandle vsDirect = loadShader(*dev, "VSMain", "default-direct");
    CHECK(dev->createGraphicsPipeline(texturedDesc(vsCross, psCross)).has_value());
    CHECK(dev->createGraphicsPipeline(texturedDesc(vsDirect, psDirect)).has_value());
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("a dxbc shader whose reflection does not record dxbcPath is rejected") {
    auto dev = makeDevice();
    const ShaderHandle vs = loadShader(*dev, "VSMain", "default", /*dropPath=*/true);
    const ShaderHandle ps = loadShader(*dev, "PSMain", "default");
    auto p = dev->createGraphicsPipeline(texturedDesc(vs, ps));
    REQUIRE_FALSE(p.has_value());
    CHECK(p.error().exitCode == 2);
    CHECK(p.error().message == "d3d11: VSMain: reflection has no dxbcPath (rebuild the shaders with this shaderc)");
}

} // TEST_SUITE
