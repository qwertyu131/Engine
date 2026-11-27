#include "TestPattern.h"

#include <array>

namespace eng {

namespace {

using Rgb = std::array<uint8_t, 3>;

constexpr std::array<Rgb, 8> kPalette{{
    {255, 0, 0},     // red
    {0, 255, 0},     // green
    {0, 0, 255},     // blue
    {255, 255, 0},   // yellow
    {0, 255, 255},   // cyan
    {255, 0, 255},   // magenta
    {0, 0, 0},       // black
    {128, 128, 128}, // mid gray
}};

Rgb cellColor(int cx, int cy) {
    if (cy < 4) {
        return kPalette[static_cast<size_t>((cx + cy) % 8)];
    }
    const int index = (cy - 4) * 8 + cx; // 0..31
    const auto v = static_cast<uint8_t>(index * 255 / 31);
    return {v, v, v};
}

} // namespace

Image8 makeTestPattern(int width, int height) {
    Image8 img;
    if (width < 1 || height < 1 || width > kMaxTestPatternSize || height > kMaxTestPatternSize) {
        return img;
    }
    img.width = width;
    img.height = height;
    img.channels = 3;
    img.rgba8.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);

    for (int y = 0; y < height; ++y) {
        const int cy = static_cast<int>(static_cast<int64_t>(y) * 8 / height);
        for (int x = 0; x < width; ++x) {
            const int cx = static_cast<int>(static_cast<int64_t>(x) * 8 / width);
            const bool border = x == 0 || y == 0 || x == width - 1 || y == height - 1;
            const Rgb c = border ? Rgb{255, 255, 255} : cellColor(cx, cy);
            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
            img.rgba8[i + 0] = c[0];
            img.rgba8[i + 1] = c[1];
            img.rgba8[i + 2] = c[2];
            img.rgba8[i + 3] = 255;
        }
    }
    return img;
}

uint64_t fnv1a64(std::span<const uint8_t> bytes) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const uint8_t b : bytes) {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    return h;
}

} // namespace eng
