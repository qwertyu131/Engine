// builtin:pbr-spheres (plan F2 Task 5): the F1 glTF PBR shader (pbr.hlsl) on every backend configuration,
// Vulkan with spirv T2_SKIN0_MORPH0_UNLIT0, D3D11 with dxbc T1_SKIN0_MORPH0_UNLIT0 (+ the UNLIT1 row), IBL
// generated on the CPU (apps/headless/Ibl.h), environment background (shaders/test/skybox.hlsl).
//
// Probe pixels and colours are NOT taken from a render. They come from scripts/f02_pbr_model.py --probes
// 256 256, an independent Python model of the scene (camera, sphere grid, analytic environment, Khronos
// PBR Neutral, gamma 2.2); the same table is in tests/golden/f02/cases.json (checked against the model by
// tests/unit/scripts/test_run_golden.py):
//   background (6, 6) / (249, 6) / (6, 249) / (249, 249): the environment along the pixel ray, +-3/255
//     (sky top, ground bottom, brighter to the right: a Y flip or an X mirror swaps them);
//   unlit row centres: baseColorFactor through the output transform (identity tone map, gamma 2.2), +-1/255;
//   reflection: mirror metal sphere (col 4, row 0): (212, 14) above its centre B >= R + 20 (sky), (212, 29)
//     below it R >= B + 10 (ground); found by looking at the first render, which drew the spheres inside out
//     (front face mirrored) identically on both backends;
//   sun: the rough dielectric centre (46, 189) is darker (R+G+B lower by >= 10) than the smooth one (46, 25);
//     both reflect into the sun disk.
// The CPU IBL helpers are checked backend-independently in suite rhi.ibl.
#include "Ibl.h"
#include "Scenes.h"

#include "Compare.h"
#include "rhi/Rhi.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <string>

#include "RhiTestEnv.h"

using namespace eng::rhi;
using eng::rhi::test::assetDir;
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
        CAPTURE(expected[c]);
        CHECK(std::abs(got[c] - expected[c]) <= tol);
    }
}

Image renderPbr(const DeviceDesc& desc) {
    auto dev = createDevice(desc);
    REQUIRE_MESSAGE(dev.has_value(), (dev ? std::string() : dev.error().message));
    eng::scenes::SceneDesc sd;
    sd.shaderDir = shaderLibDir();
    sd.assetDir = assetDir();
    sd.colorFormat = Format::RGBA8_UNORM;
    sd.width = 256;
    sd.height = 256;
    auto img = eng::scenes::renderSceneOffscreen(**dev, eng::scenes::kPbrSpheres, sd);
    REQUIRE_MESSAGE(img.has_value(), (img ? std::string() : img.error().message));
    REQUIRE(img->width == 256);
    REQUIRE(img->height == 256);
    CHECK((*dev)->validationErrorCount() == 0);
    return std::move(*img);
}

} // namespace

TEST_SUITE("rhi.device") {

TEST_CASE("builtin:pbr-spheres 256x256: background, unlit row, sun probe, validation clean") {
    const Image img = renderPbr(currentBackend().desc);
    // background = environment lookup (scripts/f02_pbr_model.py)
    checkProbe(img, 6, 6, {157, 175, 210, 255}, 3);
    checkProbe(img, 249, 6, {176, 196, 234, 255}, 3);
    checkProbe(img, 6, 249, {98, 88, 75, 255}, 3);
    checkProbe(img, 249, 249, {114, 102, 89, 255}, 3);
    // unlit row = baseColorFactor (identity PBR Neutral, gamma 2.2)
    checkProbe(img, 46, 230, {217, 123, 0, 255}, 1);
    checkProbe(img, 87, 230, {0, 202, 148, 255}, 1);
    checkProbe(img, 128, 230, {136, 0, 217, 255}, 1);
    checkProbe(img, 168, 230, {194, 186, 0, 255}, 1);
    checkProbe(img, 209, 230, {0, 158, 210, 255}, 1);
    // reflection: the mirror metal sphere (metallic 1, roughness 0) reflects the sky above its centre and
    // the ground below it; an inside-out sphere (wrong front face) swaps them on every backend alike
    const Rgba above = pixel(img, 212, 14), below = pixel(img, 212, 29);
    CAPTURE(above[0]);
    CAPTURE(above[2]);
    CAPTURE(below[0]);
    CAPTURE(below[2]);
    CHECK(above[2] >= above[0] + 20);
    CHECK(below[0] >= below[2] + 10);
    // sun: rough dielectric centre darker than the smooth dielectric centre
    const Rgba rough = pixel(img, 46, 189), smooth = pixel(img, 46, 25);
    CAPTURE(rough[0]);
    CAPTURE(rough[1]);
    CAPTURE(rough[2]);
    CAPTURE(smooth[0]);
    CAPTURE(smooth[1]);
    CAPTURE(smooth[2]);
    CHECK(rough[0] + rough[1] + rough[2] + 10 <= smooth[0] + smooth[1] + smooth[2]);
}

} // TEST_SUITE

TEST_SUITE("rhi.cross") {

TEST_CASE("builtin:pbr-spheres: D3D11 (dxbc T1) vs Vulkan (spirv T2), mean FLIP <= 0.03") {
    DeviceDesc vk;
    vk.backend = Backend::Vulkan;
    DeviceDesc hw;
    hw.backend = Backend::D3D11;
    auto toGolden = [](Image&& img) {
        eng::golden::Image g;
        g.width = static_cast<int>(img.width);
        g.height = static_cast<int>(img.height);
        g.channels = 4;
        g.rgba8 = std::move(img.rgba8);
        return g;
    };
    const eng::golden::Image a = toGolden(renderPbr(hw));
    const eng::golden::Image b = toGolden(renderPbr(vk));
    auto cmp = eng::golden::compare(a, b);
    REQUIRE_MESSAGE(cmp.has_value(), (cmp ? std::string() : cmp.error()));
    MESSAGE("pbr-spheres d3d11 vs vulkan: flipMean=" << cmp->flipMean << " psnr=" << cmp->psnr);
    CHECK(cmp->flipMean <= 0.03);
}

} // TEST_SUITE

// --- CPU IBL (backend-independent) -----------------------------------------------------------------
TEST_SUITE("rhi.ibl") {

namespace ibl = eng::scenes::ibl;

TEST_CASE("cube texel directions follow the D3D / Vulkan face table (+X, -X, +Y, -Y, +Z, -Z)") {
    const uint32_t n = 32;
    auto near = [](ibl::Vec3 a, ibl::Vec3 b) {
        return std::abs(a.x - b.x) < 0.06f && std::abs(a.y - b.y) < 0.06f && std::abs(a.z - b.z) < 0.06f;
    };
    const float k = 1.0f / std::sqrt(3.0f);
    // face centres
    CHECK(near(ibl::cubeTexelDirection(0, n / 2, n / 2, n), {1, 0, 0}));
    CHECK(near(ibl::cubeTexelDirection(1, n / 2, n / 2, n), {-1, 0, 0}));
    CHECK(near(ibl::cubeTexelDirection(2, n / 2, n / 2, n), {0, 1, 0}));
    CHECK(near(ibl::cubeTexelDirection(3, n / 2, n / 2, n), {0, -1, 0}));
    CHECK(near(ibl::cubeTexelDirection(4, n / 2, n / 2, n), {0, 0, 1}));
    CHECK(near(ibl::cubeTexelDirection(5, n / 2, n / 2, n), {0, 0, -1}));
    // first texel (top-left in memory) of each face
    CHECK(near(ibl::cubeTexelDirection(0, 0, 0, n), {k, k, k}));    // +X: s -> -z, t -> -y
    CHECK(near(ibl::cubeTexelDirection(1, 0, 0, n), {-k, k, -k}));  // -X: s -> +z, t -> -y
    CHECK(near(ibl::cubeTexelDirection(2, 0, 0, n), {-k, k, -k}));  // +Y: s -> +x, t -> +z
    CHECK(near(ibl::cubeTexelDirection(3, 0, 0, n), {-k, -k, k}));  // -Y: s -> +x, t -> -z
    CHECK(near(ibl::cubeTexelDirection(4, 0, 0, n), {-k, k, k}));   // +Z: s -> +x, t -> -y
    CHECK(near(ibl::cubeTexelDirection(5, 0, 0, n), {k, k, -k}));   // -Z: s -> -x, t -> -y
}

TEST_CASE("environment: sun disk, sky above, ground below, brighter to +X") {
    const ibl::Vec3 sun = ibl::normalize(ibl::kSunDirection);
    const ibl::Vec3 s = ibl::environmentRadiance(sun);
    CHECK(s.x == doctest::Approx(6.0f));
    CHECK(s.y == doctest::Approx(5.6f));
    CHECK(s.z == doctest::Approx(5.0f));
    const ibl::Vec3 up = ibl::environmentRadiance(ibl::normalize({0, 1, -0.2f}));
    const ibl::Vec3 down = ibl::environmentRadiance(ibl::normalize({0, -1, -0.2f}));
    CHECK(up.z > up.x);     // zenith blue
    CHECK(down.x > down.z); // ground brown
    const ibl::Vec3 l = ibl::environmentRadiance(ibl::normalize({-0.3f, 0.1f, -1}));
    const ibl::Vec3 r = ibl::environmentRadiance(ibl::normalize({0.3f, 0.1f, -1}));
    CHECK(r.x > l.x);
}

TEST_CASE("IBL inputs: prefiltered mip 0 = environment, constant environment stays constant, LUT edges") {
    const ibl::Cube env = ibl::environmentCube(8);
    const ibl::Cube pre = ibl::prefilterGgx(env, 4);
    REQUIRE(pre.levels.size() == 4);
    CHECK(pre.levels[0] == env.levels[0]);
    CHECK(pre.levels[3].size() == 6u * 1u * 1u * 4u);

    ibl::Cube flat = env;
    for (size_t i = 0; i < flat.levels[0].size(); ++i) {
        flat.levels[0][i] = (i % 4 == 3) ? 1.0f : 0.5f;
    }
    const ibl::Cube irr = ibl::irradiance(flat, 4);
    for (size_t i = 0; i < irr.levels[0].size(); ++i) {
        CHECK(irr.levels[0][i] == doctest::Approx(i % 4 == 3 ? 1.0f : 0.5f).epsilon(1e-4));
    }
    const ibl::Cube preFlat = ibl::prefilterGgx(flat, 4);
    for (const auto& level : preFlat.levels) {
        for (size_t i = 0; i < level.size(); ++i) {
            CHECK(level[i] == doctest::Approx(i % 4 == 3 ? 1.0f : 0.5f).epsilon(1e-4));
        }
    }

    const uint32_t n = 32;
    const std::vector<float> lut = ibl::brdfLut(n, 256);
    REQUIRE(lut.size() == size_t(n) * n * 4u);
    auto at = [&](uint32_t x, uint32_t y, int c) { return lut[(size_t(y) * n + x) * 4u + size_t(c)]; };
    // NdotV ~ 1, roughness ~ 0 (row 0 = roughness 0): scale ~ 1, bias ~ 0
    CHECK(at(n - 1, 0, 0) == doctest::Approx(1.0f).epsilon(0.03));
    CHECK(at(n - 1, 0, 1) == doctest::Approx(0.0f).epsilon(0.03));
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            CHECK(at(x, y, 0) + at(x, y, 1) <= 1.001f);
            CHECK(at(x, y, 0) >= 0.0f);
            CHECK(at(x, y, 1) >= 0.0f);
        }
    }
}

} // TEST_SUITE
