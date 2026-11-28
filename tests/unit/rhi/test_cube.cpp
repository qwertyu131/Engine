// builtin:textured-cube (plan F2 Task 3, on every backend since Task 4): device-local buffers + texture with CPU mips uploaded through
// staging, sampler, two bind groups (space2 material, space3 draw constants from allocTransient), depth
// D32_FLOAT, sRGB texture.
//
// Probe pixels and colours are NOT taken from a render. They come from scripts/make_f02_assets.py
// --probes 128 128, an independent Python model of the scene (camera, rotation, per-face UVs, ray cast):
//   face-centre -X: pixel (39, 68) -> -X tile (60, 170, 70)     local uv (0.49, 0.50)
//   face-centre +Y: pixel (64, 38) -> +Y tile (50, 90, 200)     local uv (0.50, 0.52)
//   face-centre +Z: pixel (78, 72) -> +Z tile (180, 70, 190)    local uv (0.49, 0.49)
//   depth         : pixel (83, 81) = the hidden far edge +X/-Y, covered by +Z (180, 70, 190), local uv
//                   (0.61, 0.69). The cube is drawn with culling off and the hidden faces last, so
//                   without the depth test this pixel shows the +X or -Y colour.
// Colour expectations (sRGB bytes of tests/assets/f02/cube_faces.png, solid tile interiors):
//   RGBA8_SRGB target: the PNG bytes (sRGB decode on sampling, encode on write), +-3/255;
//   RGBA8_UNORM target: the decoded linear value * 255 (no encode), +-3/255:
//     (60,170,70) -> (11.5, 102.5, 15.6); (50,90,200) -> (8.1, 26.1, 147.3); (180,70,190) -> (116.4, 15.6, 131.3).
//   The UNORM case catches a missing decode that the sRGB -> sRGB round trip would cancel.
// Background: clear chosen per target so both read back (51, 51, 51) (UNORM 0.2; sRGB linear 0.0331).
// Mirrored geometry would put another face's colour under a probe: the visible faces' tiles come from
// both rows and two columns of the atlas.
#include "renderer/builtin/Scenes.h"

#include "rhi/Rhi.h"
#include "rhi/ShaderLibrary.h"

#include <doctest/doctest.h>

#include <array>
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

std::unique_ptr<IDevice> makeDevice() {
    auto dev = createDevice(currentBackend().desc);
    REQUIRE_MESSAGE(dev.has_value(), (dev ? std::string() : dev.error().message));
    return std::move(*dev);
}

Image renderCube(IDevice& dev, Format target, CullMode cull = CullMode::None) {
    eng::scenes::CubeOptions o;
    o.width = 128;
    o.height = 128;
    o.target = target;
    o.cull = cull;
    auto img = eng::scenes::renderTexturedCube(dev, shaderLibDir(), assetDir(), o);
    REQUIRE_MESSAGE(img.has_value(), (img ? std::string() : img.error().message));
    REQUIRE(img->width == 128);
    REQUIRE(img->height == 128);
    return std::move(*img);
}

} // namespace

// Every backend configuration (plan F2 Task 4: parametrised over the backends present).
TEST_SUITE("rhi.device") {

TEST_CASE("builtin:textured-cube 128x128 sRGB target: face centres, depth probe, background, validation clean") {
    auto dev = makeDevice();
    const Image img = renderCube(*dev, Format::RGBA8_SRGB);
    checkProbe(img, 39, 68, {60, 170, 70, 255}, 3);   // -X
    checkProbe(img, 64, 38, {50, 90, 200, 255}, 3);   // +Y
    checkProbe(img, 78, 72, {180, 70, 190, 255}, 3);  // +Z
    checkProbe(img, 83, 81, {180, 70, 190, 255}, 3);  // depth: +Z covers the hidden far edge +X/-Y
    checkProbe(img, 2, 2, {51, 51, 51, 255}, 1);      // background corners
    checkProbe(img, 125, 125, {51, 51, 51, 255}, 1);
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("builtin:textured-cube 128x128 UNORM target: sampling decodes sRGB (linear values written)") {
    auto dev = makeDevice();
    const Image img = renderCube(*dev, Format::RGBA8_UNORM);
    checkProbe(img, 39, 68, {12, 102, 16, 255}, 3);
    checkProbe(img, 64, 38, {8, 26, 147, 255}, 3);
    checkProbe(img, 78, 72, {116, 16, 131, 255}, 3);
    checkProbe(img, 2, 2, {51, 51, 51, 255}, 1);
    CHECK(dev->validationErrorCount() == 0);
}

// Plan F3 Task 1 (F2 carry-over): the same probes with back-face culling. Faces are wound clockwise on
// screen seen from outside (FrontFace::Clockwise); a winding / front-face convention error between the
// backends culls the visible faces instead, and the probes then show the hidden faces' insides.
TEST_CASE("builtin:textured-cube 128x128 sRGB target, CullMode::Back: the same face-centre probes") {
    auto dev = makeDevice();
    const Image img = renderCube(*dev, Format::RGBA8_SRGB, CullMode::Back);
    checkProbe(img, 39, 68, {60, 170, 70, 255}, 3);   // -X
    checkProbe(img, 64, 38, {50, 90, 200, 255}, 3);   // +Y
    checkProbe(img, 78, 72, {180, 70, 190, 255}, 3);  // +Z
    checkProbe(img, 83, 81, {180, 70, 190, 255}, 3);  // +Z over the hidden far edge
    checkProbe(img, 2, 2, {51, 51, 51, 255}, 1);
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("builtin:textured-cube twice on one device: resources are released and re-created cleanly") {
    auto dev = makeDevice();
    const Image a = renderCube(*dev, Format::RGBA8_SRGB);
    const Image b = renderCube(*dev, Format::RGBA8_SRGB);
    CHECK(a.rgba8 == b.rgba8);
    CHECK(dev->validationErrorCount() == 0);
}

} // TEST_SUITE
