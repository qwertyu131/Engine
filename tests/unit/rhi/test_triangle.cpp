// builtin:triangle on every backend configuration (plan F2 Task 2) + the negative validation tests
// (Review Focus 2: the validation hook must really be wired).
//
// Probe values are computed by hand from the scene definition (apps/headless/Scenes.h), never copied
// from a render:
//   target RGBA8_UNORM, clear 0.1 -> round(0.1 * 255) = 26 (25.5 + float error rounds up; +-1 allowed for
//   the UNORM rounding rule);
//   probe pixel centres lie < 0.3 px inside the triangle from their vertex, so the other two colours
//   weigh < 0.6 % = 1.5/255: pure R / G / B within +-2/255.
// An error made identically on both backends (Y flip, winding) cannot pass: red must be top-left,
// green top-right, blue bottom.
#include "Scenes.h"

#include "rhi/Rhi.h"
#include "rhi/ShaderLibrary.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdlib>
#include <string>

#include "RhiTestEnv.h"

using namespace eng::rhi;
using eng::rhi::test::currentBackend;
using eng::rhi::test::shaderLibDir;

namespace {

using Rgba = std::array<int, 4>;

Rgba pixel(const Image& img, uint32_t x, uint32_t y) {
    const size_t i = (static_cast<size_t>(y) * img.width + x) * 4u;
    return {img.rgba8[i], img.rgba8[i + 1], img.rgba8[i + 2], img.rgba8[i + 3]};
}

void checkProbe(const Image& img, uint32_t x, uint32_t y, Rgba expected, int tol) {
    const Rgba got = pixel(img, x, y);
    CAPTURE(x);
    CAPTURE(y);
    for (int c = 0; c < 4; ++c) {
        CAPTURE(c);
        CAPTURE(got[c]);
        CHECK(std::abs(got[c] - expected[c]) <= tol);
    }
}

std::unique_ptr<IDevice> makeDevice() {
    auto dev = createDevice(currentBackend().desc);
    REQUIRE_MESSAGE(dev.has_value(), (dev ? std::string() : dev.error().message));
    return std::move(*dev);
}

TextureHandle makeTarget(IDevice& dev, uint32_t w, uint32_t h) {
    TextureDesc td;
    td.width = w;
    td.height = h;
    td.format = Format::RGBA8_UNORM;
    td.usage = TextureUsage::RenderTarget | TextureUsage::CopySrc;
    auto t = dev.createTexture(td);
    REQUIRE_MESSAGE(t.has_value(), (t ? std::string() : t.error().message));
    return *t;
}

PipelineHandle makeColorTrianglePipeline(IDevice& dev, Format colorFormat) {
    const Backend b = currentBackend().desc.backend;
    auto vsBlob = ShaderLibrary::load(shaderLibDir(), "color_triangle", "VSMain", b, "default");
    auto psBlob = ShaderLibrary::load(shaderLibDir(), "color_triangle", "PSMain", b, "default");
    REQUIRE(vsBlob.has_value());
    REQUIRE(psBlob.has_value());
    auto vs = dev.createShader(*vsBlob);
    auto ps = dev.createShader(*psBlob);
    REQUIRE(vs.has_value());
    REQUIRE(ps.has_value());
    GraphicsPipelineDesc pd;
    pd.vs = *vs;
    pd.ps = *ps;
    pd.vertexLayout.buffers = {{sizeof(eng::scenes::TriangleVertex), false}};
    pd.vertexLayout.attributes = {{"POSITION", Format::RG32_FLOAT, 0, 0}, {"COLOR0", colorFormat, 0, 8}};
    pd.colorFormats = {Format::RGBA8_UNORM};
    auto p = dev.createGraphicsPipeline(pd);
    REQUIRE_MESSAGE(p.has_value(), (p ? std::string() : p.error().message));
    return *p;
}

} // namespace

TEST_SUITE("rhi.device") {

TEST_CASE("builtin:triangle 64x64: corner colours, clear colour, validation clean") {
    auto dev = makeDevice();
    auto img = eng::scenes::renderTriangle(*dev, shaderLibDir(), 64, 64);
    REQUIRE_MESSAGE(img.has_value(), (img ? std::string() : img.error().message));
    REQUIRE(img->width == 64);
    REQUIRE(img->height == 64);
    REQUIRE(img->rgba8.size() == 64u * 64u * 4u);
    checkProbe(*img, 8, 8, {255, 0, 0, 255}, 2);  // red, top-left vertex (8.4, 8.4)
    checkProbe(*img, 55, 8, {0, 255, 0, 255}, 2); // green, top-right vertex (55.6, 8.4)
    checkProbe(*img, 32, 56, {0, 0, 255, 255}, 2); // blue, bottom vertex (32.5, 56.75)
    // Background: clear colour 0.1 (UNORM 26), corners outside the triangle.
    checkProbe(*img, 2, 61, {26, 26, 26, 255}, 1);
    checkProbe(*img, 61, 61, {26, 26, 26, 255}, 1);
    checkProbe(*img, 32, 2, {26, 26, 26, 255}, 1);
    // Centroid (32.17, 24.52): all three colours mixed, none dominant.
    const Rgba mid = pixel(*img, 32, 24);
    CHECK(mid[0] > 40);
    CHECK(mid[1] > 40);
    CHECK(mid[2] > 40);
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("stale handle bookkeeping: destroy then create reuses the slot with a new generation") {
    auto dev = makeDevice();
    const TextureHandle a = makeTarget(*dev, 4, 4);
    dev->destroy(a);
    REQUIRE(dev->submitAndWait().has_value()); // deferred destruction runs after the GPU is done
    const TextureHandle b = makeTarget(*dev, 4, 4);
    CHECK(a.id != b.id);
    CHECK((a.id & 0xFFFFFu) == (b.id & 0xFFFFFu)); // same slot, new generation
    CHECK(dev->validationErrorCount() == 0);
}

} // TEST_SUITE

TEST_SUITE("rhi.vulkan") {

TEST_CASE("negative: draw without setPipeline -> exit 4 with a VUID message") {
    auto dev = makeDevice();
    const TextureHandle rt = makeTarget(*dev, 16, 16);
    ICommandList& cmd = dev->beginFrame();
    const TextureBarrier toRt{rt, ResourceState::Undefined, ResourceState::RenderTarget};
    cmd.barrier({&toRt, 1});
    RenderPassDesc rp;
    rp.colors.push_back({rt, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 1}});
    cmd.beginRenderPass(rp);
    cmd.setViewport({0, 0, 16, 16, 0, 1});
    cmd.setScissor({0, 0, 16, 16});
    cmd.draw(3, 1, 0, 0); // no pipeline bound: VUID-vkCmdDraw-None-08606 (or similar)
    cmd.endRenderPass();
    auto r = dev->submitAndWait();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().exitCode == 4);
    CAPTURE(r.error().message);
    CHECK(r.error().message.find("VUID") != std::string::npos);
    CHECK(dev->validationErrorCount() > 0);
}

} // TEST_SUITE

TEST_SUITE("rhi.d3d11") {

TEST_CASE("negative: draw with a mismatched input layout -> exit 4") {
    auto dev = makeDevice();
    const TextureHandle rt = makeTarget(*dev, 16, 16);
    // COLOR0 is float4 in the VS; the layout feeds it as RGBA32_UINT.
    const PipelineHandle pipe = makeColorTrianglePipeline(*dev, Format::RGBA32_UINT);
    eng::scenes::TriangleVertex v[3];
    eng::scenes::triangleVertices(16, 16, v);
    BufferDesc bd;
    bd.size = sizeof(v);
    bd.usage = BufferUsage::Vertex;
    auto vb = dev->createBuffer(bd, std::as_bytes(std::span(v)));
    REQUIRE(vb.has_value());
    ICommandList& cmd = dev->beginFrame();
    RenderPassDesc rp;
    rp.colors.push_back({rt, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 1}});
    cmd.beginRenderPass(rp);
    cmd.setPipeline(pipe);
    cmd.setVertexBuffer(0, *vb, 0);
    cmd.setViewport({0, 0, 16, 16, 0, 1});
    cmd.setScissor({0, 0, 16, 16});
    cmd.draw(3, 1, 0, 0);
    cmd.endRenderPass();
    auto r = dev->submitAndWait();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().exitCode == 4);
    CAPTURE(r.error().message);
    // Debug layer: "WARNING id=391 ID3D11Device::CreateInputLayout: ... mismatched format".
    CHECK(r.error().message.find("CreateInputLayout") != std::string::npos);
    CHECK(dev->validationErrorCount() > 0);
}

} // TEST_SUITE
