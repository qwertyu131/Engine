#include <doctest/doctest.h>

#include "TestPattern.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace {

using Rgb = std::array<uint8_t, 3>;

Rgb pixel(const eng::Image8& img, int x, int y) {
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(img.width) + static_cast<size_t>(x)) * 4u;
    return {img.rgba8[i], img.rgba8[i + 1], img.rgba8[i + 2]};
}

uint8_t alpha(const eng::Image8& img, int x, int y) {
    return img.rgba8[(static_cast<size_t>(y) * static_cast<size_t>(img.width) + static_cast<size_t>(x)) * 4u + 3u];
}

constexpr Rgb kWhite{255, 255, 255};

} // namespace

TEST_SUITE("headless") {

TEST_CASE("test pattern: size, channel count and opaque alpha") {
    const eng::Image8 img = eng::makeTestPattern(256, 256);
    CHECK(img.width == 256);
    CHECK(img.height == 256);
    CHECK(img.channels == 3);
    REQUIRE(img.rgba8.size() == 256u * 256u * 4u);
    for (int y = 0; y < 256; y += 17) {
        for (int x = 0; x < 256; x += 13) {
            CHECK(alpha(img, x, y) == 255);
        }
    }
}

TEST_CASE("test pattern: 1-px white border, first checker colour at (16,16)") {
    const eng::Image8 img = eng::makeTestPattern(256, 256);
    CHECK(pixel(img, 0, 0) == kWhite);
    CHECK(pixel(img, 255, 0) == kWhite);
    CHECK(pixel(img, 0, 255) == kWhite);
    CHECK(pixel(img, 255, 255) == kWhite);
    CHECK(pixel(img, 128, 0) == kWhite);
    CHECK(pixel(img, 0, 128) == kWhite);

    // Cell (column 0, row 0) is sRGB red; just inside the border too.
    CHECK(pixel(img, 16, 16) == Rgb{255, 0, 0});
    CHECK(pixel(img, 1, 1) == Rgb{255, 0, 0});
    // Row 0 walks the colour palette: red, green, blue, yellow, cyan, magenta, black, mid gray.
    CHECK(pixel(img, 48, 16) == Rgb{0, 255, 0});
    CHECK(pixel(img, 80, 16) == Rgb{0, 0, 255});
    CHECK(pixel(img, 240, 16) == Rgb{128, 128, 128});
    // Row 1 is shifted by one palette entry.
    CHECK(pixel(img, 16, 48) == Rgb{0, 255, 0});
    // Rows 4..7: 32-step gray ramp 0..255 (index = (row - 4) * 8 + column, value = index * 255 / 31).
    CHECK(pixel(img, 16, 144) == Rgb{0, 0, 0});
    CHECK(pixel(img, 48, 144) == Rgb{8, 8, 8});
    CHECK(pixel(img, 16, 176) == Rgb{65, 65, 65});
    CHECK(pixel(img, 240, 240) == Rgb{255, 255, 255});
    // Cell boundary: column 0 ends at x = 31, column 1 starts at x = 32.
    CHECK(pixel(img, 31, 16) == Rgb{255, 0, 0});
    CHECK(pixel(img, 32, 16) == Rgb{0, 255, 0});
}

TEST_CASE("test pattern: deterministic buffer hash (FNV-1a 64)") {
    const eng::Image8 a = eng::makeTestPattern(256, 256);
    const eng::Image8 b = eng::makeTestPattern(256, 256);
    CHECK(a.rgba8 == b.rgba8);
    // Recorded from the implementation and cross-checked by an independent Python re-implementation
    // of the documented mapping; any change to the pattern must update this constant
    // and tests/golden/ref/f00/test_pattern_256.png together.
    CHECK(eng::fnv1a64(a.rgba8) == 0xd93623dccfab4b98ull);
}

TEST_CASE("test pattern: FNV-1a 64 reference vectors") {
    CHECK(eng::fnv1a64(std::span<const uint8_t>()) == 0xcbf29ce484222325ull);
    const std::string s = "a";
    CHECK(eng::fnv1a64(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()), s.size())) ==
          0xaf63dc4c8601ec8cull);
}

TEST_CASE("test pattern: non-multiple sizes stay in bounds, invalid sizes give an empty image") {
    const eng::Image8 odd = eng::makeTestPattern(13, 7);
    CHECK(odd.width == 13);
    CHECK(odd.height == 7);
    CHECK(odd.rgba8.size() == 13u * 7u * 4u);
    CHECK(pixel(odd, 0, 0) == kWhite);
    CHECK(pixel(odd, 12, 6) == kWhite);

    const eng::Image8 one = eng::makeTestPattern(1, 1);
    CHECK(one.rgba8.size() == 4u);
    CHECK(pixel(one, 0, 0) == kWhite);

    CHECK(eng::makeTestPattern(0, 16).rgba8.empty());
    CHECK(eng::makeTestPattern(16, -1).rgba8.empty());
    CHECK(eng::makeTestPattern(eng::kMaxTestPatternSize + 1, 16).rgba8.empty());
}

} // TEST_SUITE
