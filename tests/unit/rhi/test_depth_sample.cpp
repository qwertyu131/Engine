// Sampled depth textures (plan F3 Task 7: the shadow atlas). Suite rhi.device (every backend configuration):
//   a D32_FLOAT texture with DepthStencil | Sampled is cleared / drawn as a depth-only attachment (no colour target,
//   void pixel stage), then read by a colour pass (Texture2D<float>.Load), 3 times in one frame (one command list):
//   the depth attachment of pass n + 1 is the texture pass n left bound as a shader resource (D3D11: a resource cannot
//   be a depth target while it is still bound as an SRV; the debug layer reports that, any message is exit 4).
//   Pass 0 clears to 0.25; pass 1 draws z = 0.75 over a 1.0 clear except where the pixel stage discards (x < 2);
//   pass 2 clears to 0.5. Each pass writes its own RGBA32_FLOAT target, red = the depth value (read back raw).
#include "RhiTestEnv.h"

#include <doctest/doctest.h>

#include <cstring>
#include <vector>

using namespace eng;
using namespace eng::rhi;
using namespace eng::rhi::test;

TEST_SUITE("rhi.device") {

TEST_CASE("D32 depth texture: depth-only pass, then sampled by the next pass, three times in one frame") {
    auto dev = createDevice(currentBackend().desc);
    REQUIRE(dev.has_value());
    IDevice& d = **dev;
    const Backend b = d.caps().backend;
    const char* key = d.caps().tier == 0 ? "default-t0" : "default";
    const auto shader = [&](const char* entry) {
        auto blob = ShaderLibrary::load(shaderLibDir(), "depth_sample", entry, b, key);
        REQUIRE_MESSAGE(blob.has_value(), (blob ? std::string() : blob.error().message));
        return *blob;
    };
    const ShaderBlob vsDepth = shader("VSDepth"), psDepth = shader("PSDepth"), vsMain = shader("VSMain"),
                     psMain = shader("PSMain");
    auto vd = d.createShader(vsDepth);
    auto pdS = d.createShader(psDepth);
    auto vm = d.createShader(vsMain);
    auto pm = d.createShader(psMain);
    REQUIRE((vd && pdS && vm && pm));

    constexpr uint32_t kW = 8, kH = 4;
    TextureDesc dd;
    dd.width = kW;
    dd.height = kH;
    dd.format = Format::D32_FLOAT;
    dd.usage = TextureUsage::DepthStencil | TextureUsage::Sampled;
    dd.debugName = "sampled depth";
    auto depth = d.createTexture(dd);
    REQUIRE_MESSAGE(depth.has_value(), (depth ? std::string() : depth.error().message));
    TextureDesc cd;
    cd.width = kW;
    cd.height = kH;
    cd.format = Format::RGBA32_FLOAT;
    cd.usage = TextureUsage::RenderTarget | TextureUsage::CopySrc;
    cd.debugName = "depth readout";
    TextureHandle colors[3];
    for (TextureHandle& c : colors) {
        auto t = d.createTexture(cd);
        REQUIRE(t.has_value());
        c = *t;
    }

    GraphicsPipelineDesc dp;
    dp.vs = *vd;
    dp.ps = *pdS;
    dp.depth = {true, true, CompareOp::LessEqual};
    dp.depthFormat = Format::D32_FLOAT; // no colour target
    dp.debugName = "depth-only";
    auto depthPipe = d.createGraphicsPipeline(dp);
    REQUIRE_MESSAGE(depthPipe.has_value(), (depthPipe ? std::string() : depthPipe.error().message));
    GraphicsPipelineDesc cp;
    cp.vs = *vm;
    cp.ps = *pm;
    cp.colorFormats = {Format::RGBA32_FLOAT};
    cp.debugName = "depth readout";
    auto colorPipe = d.createGraphicsPipeline(cp);
    REQUIRE_MESSAGE(colorPipe.has_value(), (colorPipe ? std::string() : colorPipe.error().message));

    BufferDesc bd;
    bd.size = 16;
    bd.usage = BufferUsage::Uniform;
    const float params[4] = {0.75f, 1.0f, 0.0f, 0.0f};
    auto cb = d.createBuffer(bd, std::as_bytes(std::span(params)));
    REQUIRE(cb.has_value());
    BindGroupDesc g3;
    g3.pipeline = *depthPipe;
    g3.space = 3;
    g3.entries = {{"DepthParams", BufferBinding{*cb, 0, 16}}};
    auto depthGroup = d.createBindGroup(g3);
    REQUIRE_MESSAGE(depthGroup.has_value(), (depthGroup ? std::string() : depthGroup.error().message));
    BindGroupDesc g2;
    g2.pipeline = *colorPipe;
    g2.space = 2;
    g2.entries = {{"depthTexture", *depth}};
    auto readGroup = d.createBindGroup(g2);
    REQUIRE_MESSAGE(readGroup.has_value(), (readGroup ? std::string() : readGroup.error().message));

    struct Pass {
        float clear;
        bool draw;
    };
    const Pass passes[3] = {{0.25f, false}, {1.0f, true}, {0.5f, false}};
    ResourceState depthState = ResourceState::Undefined;
    ICommandList& cmd = d.beginFrame();
    for (int f = 0; f < 3; ++f) {
        const TextureBarrier toDepth{*depth, depthState, ResourceState::DepthWrite};
        cmd.barrier({&toDepth, 1});
        RenderPassDesc rp;
        rp.depth.texture = *depth;
        rp.depth.load = LoadOp::Clear;
        rp.depth.store = StoreOp::Store;
        rp.depth.clearDepth = passes[f].clear;
        cmd.beginRenderPass(rp);
        if (passes[f].draw) {
            cmd.setViewport({0.0f, 0.0f, float(kW), float(kH), 0.0f, 1.0f});
            cmd.setScissor({0, 0, kW, kH});
            cmd.setPipeline(*depthPipe);
            cmd.setBindGroup(3, *depthGroup);
            cmd.draw(3, 1, 0, 0);
        }
        cmd.endRenderPass();
        const TextureBarrier toRead[2] = {{*depth, ResourceState::DepthWrite, ResourceState::ShaderRead},
                                          {colors[f], ResourceState::Undefined, ResourceState::RenderTarget}};
        cmd.barrier(toRead);
        depthState = ResourceState::ShaderRead;
        RenderPassDesc cpd;
        ColorAttachment ca;
        ca.texture = colors[f];
        ca.load = LoadOp::Clear;
        cpd.colors.push_back(ca);
        cmd.beginRenderPass(cpd);
        cmd.setViewport({0.0f, 0.0f, float(kW), float(kH), 0.0f, 1.0f});
        cmd.setScissor({0, 0, kW, kH});
        cmd.setPipeline(*colorPipe);
        cmd.setBindGroup(2, *readGroup);
        cmd.draw(3, 1, 0, 0);
        cmd.endRenderPass();
        const TextureBarrier toCopy{colors[f], ResourceState::RenderTarget, ResourceState::CopySrc};
        cmd.barrier({&toCopy, 1});
    }
    REQUIRE(d.submitAndWait().has_value());
    for (int f = 0; f < 3; ++f) {
        auto bytes = d.readbackSubresource(colors[f], 0, 0);
        REQUIRE_MESSAGE(bytes.has_value(), (bytes ? std::string() : bytes.error().message));
        std::vector<float> px(bytes->size() / 4);
        std::memcpy(px.data(), bytes->data(), bytes->size());
        int wrong = 0;
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                const float want = passes[f].draw ? (x < 2 ? 1.0f : 0.75f) : passes[f].clear;
                const float got = px[(size_t(y) * kW + x) * 4];
                if (std::abs(got - want) > 1e-6f) {
                    ++wrong;
                    if (wrong <= 4) {
                        MESSAGE("pass ", f, " (", x, ", ", y, "): ", got, " want ", want);
                    }
                }
            }
        }
        CAPTURE(f);
        CHECK(wrong == 0);
    }
    for (Handle h : {Handle(*readGroup), Handle(*depthGroup), Handle(*cb), Handle(*colorPipe), Handle(*depthPipe),
                     Handle(colors[0]), Handle(colors[1]), Handle(colors[2]), Handle(*depth), Handle(*pm), Handle(*vm), Handle(*pdS), Handle(*vd)}) {
        d.destroy(h);
    }
    REQUIRE(d.submitAndWait().has_value());
    CHECK(d.validationErrorCount() == 0);
}

} // TEST_SUITE
