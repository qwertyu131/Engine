#pragma once
// Built-in GPU scenes of `headless` (and the RHI tests), drawn through engine/rhi only.
// Plan F2 Task 2: builtin:triangle. Task 5 adds builtin:textured-cube and builtin:pbr-spheres.
#include "rhi/Rhi.h"

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace eng::scenes {

inline constexpr std::string_view kTriangle = "builtin:triangle";

// builtin:triangle: one triangle with per-vertex colours on an RGBA8_UNORM target cleared to
// (0.1, 0.1, 0.1, 1). Shader shaders/test/color_triangle.hlsl (key "default"), no resources.
// Vertices, in pixels of a W x H target (origin top-left), chosen so that the pixel centres
// kTriangleProbes are inside the triangle and less than 0.3 px from a vertex (other colours < 0.6 %):
//   red   (W * 8.4 / 64,  H * 8.4 / 64)     top-left
//   green (W * 55.6 / 64, H * 8.4 / 64)     top-right
//   blue  (W * 32.5 / 64, H * 56.75 / 64)   bottom
// Winding top-left -> top-right -> bottom = clockwise on screen.
inline constexpr float kTriangleClear[4] = {0.1f, 0.1f, 0.1f, 1.0f};

struct TriangleVertex {
    float position[2]; // NDC, +Y up
    float color[4];
};
// The three vertices for a target of `width` x `height` pixels.
void triangleVertices(uint32_t width, uint32_t height, TriangleVertex out[3]);

// Renders builtin:triangle offscreen and reads it back (RGBA8, rows top-down).
rhi::Result<rhi::Image> renderTriangle(rhi::IDevice& device, const std::filesystem::path& shaderDir, uint32_t width,
                                       uint32_t height);

} // namespace eng::scenes
