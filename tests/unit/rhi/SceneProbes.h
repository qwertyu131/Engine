#pragma once
// Absolute pixel probes of the built-in scenes, shared by the per-backend suites and the T0 (D3D11
// FL10_0) suite (plan F2 Task 7). The expected values are hand-computed / modelled, never copied from a
// render; their derivation is documented where they were introduced:
//   builtin:triangle     tests/unit/rhi/test_triangle.cpp (plan F2 Task 2);
//   builtin:pbr-spheres  tests/unit/rhi/test_pbr_spheres.cpp, scripts/f02_pbr_model.py (plan F2 Task 5).
#include "rhi/Types.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstdlib>

namespace eng::rhi::test {

using Rgba = std::array<int, 4>;

inline Rgba pixel(const Image& img, uint32_t x, uint32_t y) {
    const size_t i = (static_cast<size_t>(y) * img.width + x) * 4u;
    return {img.rgba8[i], img.rgba8[i + 1], img.rgba8[i + 2], img.rgba8[i + 3]};
}

inline void checkProbe(const Image& img, uint32_t x, uint32_t y, Rgba expected, int tol) {
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

// builtin:triangle, 64 x 64 RGBA8_UNORM.
inline void checkTriangleProbes(const Image& img) {
    REQUIRE(img.width == 64);
    REQUIRE(img.height == 64);
    REQUIRE(img.rgba8.size() == 64u * 64u * 4u);
    checkProbe(img, 8, 8, {255, 0, 0, 255}, 2);   // red, top-left vertex (8.4, 8.4)
    checkProbe(img, 55, 8, {0, 255, 0, 255}, 2);  // green, top-right vertex (55.6, 8.4)
    checkProbe(img, 32, 56, {0, 0, 255, 255}, 2); // blue, bottom vertex (32.5, 56.75)
    // Background: clear colour 0.1 (UNORM 26), corners outside the triangle.
    checkProbe(img, 2, 61, {26, 26, 26, 255}, 1);
    checkProbe(img, 61, 61, {26, 26, 26, 255}, 1);
    checkProbe(img, 32, 2, {26, 26, 26, 255}, 1);
    // Centroid (32.17, 24.52): all three colours mixed, none dominant.
    const Rgba mid = pixel(img, 32, 24);
    CHECK(mid[0] > 40);
    CHECK(mid[1] > 40);
    CHECK(mid[2] > 40);
}

// builtin:pbr-spheres, 256 x 256 RGBA8_UNORM.
inline void checkPbrSpheresProbes(const Image& img) {
    REQUIRE(img.width == 256);
    REQUIRE(img.height == 256);
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

} // namespace eng::rhi::test
