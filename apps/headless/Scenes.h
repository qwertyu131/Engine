#pragma once
// Built-in GPU scenes of `headless` (and the RHI tests), drawn through engine/rhi only.
// Plan F2 Task 2: builtin:triangle; Task 3: builtin:textured-cube. Task 5 adds builtin:pbr-spheres.
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

// builtin:textured-cube (plan F2 Task 3): a unit cube (+-0.5) textured with tests/assets/f02/cube_faces.png
// (RGBA8_SRGB, CPU-generated mips in linear space, linear/linear/linear clamp sampler), no lighting.
//   camera  eye (0, 0, 3) looking at the origin, +Y up, right-handed, vertical FOV 45 deg, near 0.1,
//           far 10, clip Z in [0, 1];
//   model   Rx(20 deg) * Ry(30 deg): visible faces +Z, -X, +Y;
//   faces   face i samples atlas tile i (+X, -X, +Y, -Y, +Z, -Z; 80 x 120 px tiles in a 3 x 2 grid with an
//           8 px border), inset by 2 px; local (0, 0) = the face's top-left seen from outside (frames in
//           scripts/make_f02_assets.py, which models the same scene independently for the test probes);
//   depth   D32_FLOAT, test Less + write; culling off and the hidden faces drawn last, so the image is
//           only right with a working depth test;
//   binding space2 (material) = {faceTexture, faceSampler}, persistent; space3 (draw) = {Draw}, from
//           IDevice::allocTransient each frame.
// Clear: (51, 51, 51) in the read-back bytes for both targets (UNORM 0.2; sRGB: linear 0.0331).
inline constexpr std::string_view kTexturedCube = "builtin:textured-cube";

struct CubeOptions {
    uint32_t width = 128;
    uint32_t height = 128;
    rhi::Format target = rhi::Format::RGBA8_SRGB; // RGBA8_SRGB or RGBA8_UNORM
};

// assetDir = tests/assets (cube_faces.png is under f02/).
rhi::Result<rhi::Image> renderTexturedCube(rhi::IDevice& device, const std::filesystem::path& shaderDir,
                                           const std::filesystem::path& assetDir, const CubeOptions& options);

} // namespace eng::scenes
