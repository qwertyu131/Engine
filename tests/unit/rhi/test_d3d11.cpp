// D3D11-only RHI rules (plan F2 Task 4):
//   - Review Focus 2: a texture bound as SRV while it is the bound render target -> debug-layer message
//     -> exit 4 (the validation hook is really wired for resource hazards);
//   - F1 Risk 1: a VS / PS pair from different DXBC paths is rejected at pipeline creation;
//     a dxbc blob whose reflection does not record its path is rejected too (the check must not be
//     silently skipped).
#include "rhi/Rhi.h"
#include "rhi/ShaderLibrary.h"

#include <doctest/doctest.h>

#include <string>

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

} // namespace

TEST_SUITE("rhi.d3d11") {

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
