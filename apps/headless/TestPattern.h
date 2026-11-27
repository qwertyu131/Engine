#pragma once
// Deterministic CPU test pattern used by `headless --backend cpu --scene builtin:test-pattern`
// (plan F0 Task 5). Integer-only, so every platform/compiler produces the same bytes.

#include "Image.h" // tools/golden: eng::golden::Image (RGBA8 storage + source channel count)

#include <cstdint>
#include <span>

namespace eng {

// 8-bit image as written to PNG by golden_core's savePng.
using Image8 = golden::Image;

inline constexpr int kMaxTestPatternSize = 16384;

// 8x8 colour checker with a 1-px white border. channels = 3 (RGB PNG), alpha stored as 255.
//   cell column cx = x * 8 / width, cell row cy = y * 8 / height (integer division, full image).
//   rows 0..3: palette[(cx + cy) % 8], palette = red, green, blue, yellow, cyan, magenta,
//              black, mid gray (128) -- sRGB primaries / secondaries.
//   rows 4..7: gray ramp, index = (cy - 4) * 8 + cx (0..31), value = index * 255 / 31.
//   border (x == 0, y == 0, x == width-1, y == height-1): white.
// width / height outside [1, kMaxTestPatternSize] -> empty image (width = height = 0).
Image8 makeTestPattern(int width, int height);

// FNV-1a 64-bit over the bytes (offset basis 0xcbf29ce484222325, prime 0x100000001b3).
uint64_t fnv1a64(std::span<const uint8_t> bytes);

} // namespace eng
