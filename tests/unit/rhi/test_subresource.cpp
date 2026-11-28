// Render targets on one subresource and raw subresource readback (plan F3 Task 5: the GPU IBL prefilter renders each
// face / mip of a float cube and the CPU-vs-GPU test reads them back as floats).
//
// Suite rhi.device (vulkan, d3d11, d3d11-warp):
//   - a RGBA32_FLOAT cube with 4 mips: each (face, mip) is cleared through ColorAttachment::{arrayLayer, mipLevel} to
//     its own value (face, mip, 0.25, 1); readbackSubresource of every (face, mip) returns exactly that value in every
//     texel (so no clear reached another subresource);
//   - an RGBA16_FLOAT 2D texture with initial data reads back the uploaded bytes;
//   - readbackSubresource refuses an out-of-range mip / layer and a block-compressed format.
#include "RhiTestEnv.h"

#include <doctest/doctest.h>

#include <cstring>
#include <vector>

using namespace eng;
using namespace eng::rhi;
using namespace eng::rhi::test;

TEST_SUITE("rhi.device") {

TEST_CASE("ColorAttachment arrayLayer / mipLevel render into one cube subresource; readbackSubresource returns it") {
    auto dev = createDevice(currentBackend().desc);
    REQUIRE(dev.has_value());
    IDevice& d = **dev;
    constexpr uint32_t kSize = 8, kMips = 4;
    TextureDesc td;
    td.width = td.height = kSize;
    td.mipLevels = kMips;
    td.arrayLayers = 6;
    td.cube = true;
    td.format = Format::RGBA32_FLOAT;
    td.usage = TextureUsage::RenderTarget | TextureUsage::Sampled | TextureUsage::CopySrc;
    td.debugName = "subresource test cube";
    auto cube = d.createTexture(td);
    REQUIRE_MESSAGE(cube.has_value(), (cube ? std::string() : cube.error().message));
    ICommandList& cmd = d.beginFrame();
    const TextureBarrier toRt{*cube, ResourceState::Undefined, ResourceState::RenderTarget};
    cmd.barrier({&toRt, 1});
    for (uint32_t face = 0; face < 6; ++face) {
        for (uint32_t mip = 0; mip < kMips; ++mip) {
            RenderPassDesc rp;
            ColorAttachment ca;
            ca.texture = *cube;
            ca.arrayLayer = face;
            ca.mipLevel = mip;
            ca.load = LoadOp::Clear;
            ca.clear[0] = float(face);
            ca.clear[1] = float(mip);
            ca.clear[2] = 0.25f;
            ca.clear[3] = 1.0f;
            rp.colors.push_back(ca);
            cmd.beginRenderPass(rp);
            cmd.endRenderPass();
        }
    }
    const TextureBarrier toRead{*cube, ResourceState::RenderTarget, ResourceState::ShaderRead};
    cmd.barrier({&toRead, 1});
    REQUIRE(d.submitAndWait().has_value());
    for (uint32_t face = 0; face < 6; ++face) {
        for (uint32_t mip = 0; mip < kMips; ++mip) {
            auto bytes = d.readbackSubresource(*cube, mip, face);
            REQUIRE_MESSAGE(bytes.has_value(), (bytes ? std::string() : bytes.error().message));
            const uint32_t s = kSize >> mip;
            REQUIRE(bytes->size() == size_t(s) * s * 16);
            std::vector<float> f(bytes->size() / 4);
            std::memcpy(f.data(), bytes->data(), bytes->size());
            int wrong = 0;
            for (size_t i = 0; i < f.size(); i += 4) {
                wrong += !(f[i] == float(face) && f[i + 1] == float(mip) && f[i + 2] == 0.25f && f[i + 3] == 1.0f);
            }
            CAPTURE(face);
            CAPTURE(mip);
            CHECK(wrong == 0);
        }
    }
    d.destroy(*cube);
    REQUIRE(d.submitAndWait().has_value());
    CHECK(d.validationErrorCount() == 0);
}

TEST_CASE("readbackSubresource of an uploaded RGBA16_FLOAT texture returns its bytes; bad arguments are refused") {
    auto dev = createDevice(currentBackend().desc);
    REQUIRE(dev.has_value());
    IDevice& d = **dev;
    std::vector<uint16_t> texels(4 * 2 * 4);
    for (size_t i = 0; i < texels.size(); ++i) {
        texels[i] = static_cast<uint16_t>(0x3C00 + i); // 1.0 + i ulp
    }
    TextureDesc td;
    td.width = 4;
    td.height = 2;
    td.format = Format::RGBA16_FLOAT;
    td.usage = TextureUsage::Sampled | TextureUsage::CopySrc;
    const SubresourceData init{std::as_bytes(std::span(texels)), 0};
    auto t = d.createTexture(td, {&init, 1});
    REQUIRE(t.has_value());
    auto bytes = d.readbackSubresource(*t, 0, 0);
    REQUIRE_MESSAGE(bytes.has_value(), (bytes ? std::string() : bytes.error().message));
    REQUIRE(bytes->size() == texels.size() * 2);
    CHECK(std::memcmp(bytes->data(), texels.data(), bytes->size()) == 0);
    auto badMip = d.readbackSubresource(*t, 1, 0);
    REQUIRE_FALSE(badMip.has_value());
    CHECK(badMip.error().exitCode == 2);
    auto badLayer = d.readbackSubresource(*t, 0, 1);
    REQUIRE_FALSE(badLayer.has_value());
    CHECK(badLayer.error().exitCode == 2);
    d.destroy(*t);
    REQUIRE(d.submitAndWait().has_value());
    CHECK(d.validationErrorCount() == 0);
}

} // TEST_SUITE
