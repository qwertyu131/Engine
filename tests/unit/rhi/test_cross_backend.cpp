// Cross-backend image equality (plan F2 Task 4): builtin:textured-cube on D3D11 vs Vulkan, mean FLIP
// <= 0.01. Run by ctest unit.rhi.cross (rhi_unit_tests --rhi-backend=cross), which needs both backends
// (exit 77 = Skipped otherwise, see test_main.cpp). An error made identically on both backends cannot
// be caught here; the absolute probes of test_cube.cpp cover that (Review Focus 1).
#include "Compare.h"
#include "renderer/builtin/Scenes.h"

#include "rhi/Rhi.h"

#include <doctest/doctest.h>

#include <string>

#include "RhiTestEnv.h"

using namespace eng::rhi;
using eng::rhi::test::assetDir;
using eng::rhi::test::shaderLibDir;

namespace {

eng::golden::Image renderCubeOn(const DeviceDesc& desc) {
    auto dev = createDevice(desc);
    REQUIRE_MESSAGE(dev.has_value(), (dev ? std::string() : dev.error().message));
    eng::scenes::CubeOptions o;
    o.width = 128;
    o.height = 128;
    auto img = eng::scenes::renderTexturedCube(**dev, shaderLibDir(), assetDir(), o);
    REQUIRE_MESSAGE(img.has_value(), (img ? std::string() : img.error().message));
    CHECK((*dev)->validationErrorCount() == 0);
    eng::golden::Image out;
    out.width = static_cast<int>(img->width);
    out.height = static_cast<int>(img->height);
    out.channels = 4;
    out.rgba8 = std::move(img->rgba8);
    return out;
}

} // namespace

TEST_SUITE("rhi.cross") {

TEST_CASE("builtin:textured-cube: D3D11 (hardware and WARP) vs Vulkan, mean FLIP <= 0.01") {
    DeviceDesc vk;
    vk.backend = Backend::Vulkan;
    DeviceDesc hw;
    hw.backend = Backend::D3D11;
    DeviceDesc warp = hw;
    warp.warp = true;
    const eng::golden::Image vkImg = renderCubeOn(vk);
    for (const DeviceDesc& d3d : {hw, warp}) {
        CAPTURE(d3d.warp);
        const eng::golden::Image d3dImg = renderCubeOn(d3d);
        auto cmp = eng::golden::compare(d3dImg, vkImg);
        REQUIRE_MESSAGE(cmp.has_value(), (cmp ? std::string() : cmp.error()));
        MESSAGE("cube " << std::string(d3d.warp ? "d3d11-warp" : "d3d11") << " vs vulkan: flipMean=" << cmp->flipMean
                        << " psnr=" << cmp->psnr);
        CHECK(cmp->flipMean <= 0.01);
    }
}

} // TEST_SUITE
