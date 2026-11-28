#pragma once
// Built-in GPU scenes of `headless`, `viewer` and the RHI tests, drawn through engine/rhi only.
// Plan F2 Task 2: builtin:triangle; Task 3: builtin:textured-cube; Task 5: builtin:pbr-spheres.
//
// A Scene is created once (shaders, pipelines, geometry, textures, IBL) and records itself into any
// colour target of its colour format, every frame (Task 5 / 6: headless --bench and the viewer must not
// rebuild the scene per frame). renderSceneOffscreen() is the one-frame helper of headless and the tests.
#include "rhi/Rhi.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace eng::scenes {

inline constexpr std::string_view kTriangle = "builtin:triangle";
inline constexpr std::string_view kTexturedCube = "builtin:textured-cube";
inline constexpr std::string_view kPbrSpheres = "builtin:pbr-spheres";

// Every built-in scene name, in documentation order.
std::span<const std::string_view> sceneNames();

struct SceneDesc {
    std::filesystem::path shaderDir; // shader library of the `shaders` build step
    std::filesystem::path assetDir;  // tests/assets
    rhi::Format colorFormat = rhi::Format::RGBA8_UNORM; // the format of the targets record() draws into
    uint32_t width = 256;  // initial target size (resize() changes it)
    uint32_t height = 256;
    // builtin:textured-cube only (plan F3 Task 1): face culling; faces are wound clockwise on screen seen from
    // outside (FrontFace::Clockwise). Default None (the depth-test case of plan F2 Task 3).
    rhi::CullMode cubeCull = rhi::CullMode::None;
};

class Scene {
public:
    virtual ~Scene() = default;
    // Size-dependent resources (depth buffer, projection aspect).
    virtual rhi::Result<void> resize(uint32_t width, uint32_t height) = 0;
    // Animation time in seconds (viewer). Offscreen renders use t = 0 (the documented still pose).
    virtual void setTime(float seconds) { (void)seconds; }
    // Records the scene into `color` (state RenderTarget on entry and on exit; size = the last resize):
    // own render pass(es), clear included. Between IDevice::beginFrame() and submit / present.
    virtual rhi::Result<void> record(rhi::ICommandList& cmd, rhi::TextureHandle color) = 0;
};

// Error{2, "unknown scene <name>"} for a name outside sceneNames().
rhi::Result<std::unique_ptr<Scene>> createScene(rhi::IDevice& device, std::string_view name, const SceneDesc& desc);

// Creates the scene, renders one frame into an offscreen target of desc.colorFormat (desc.width x
// desc.height) and reads it back (RGBA8, rows top-down); all resources are destroyed before it returns.
rhi::Result<rhi::Image> renderSceneOffscreen(rhi::IDevice& device, std::string_view name, const SceneDesc& desc);

// Renders an existing scene into a new offscreen target (target destroyed afterwards).
rhi::Result<rhi::Image> renderOffscreen(rhi::IDevice& device, Scene& scene, rhi::Format format, uint32_t width,
                                        uint32_t height);

// One frame of `scene` into an existing offscreen target (usage RenderTarget | CopySrc, the scene's size):
// beginFrame, barrier to RenderTarget (from Undefined when `firstFrame`, else from CopySrc), record, barrier
// to CopySrc, submitAndWait. No readback (headless --bench, plan F2 Task 8: a timed frame is record + submit
// + GPU wait, without target creation or readback). The target is in CopySrc afterwards.
rhi::Result<void> renderFrameInto(rhi::IDevice& device, Scene& scene, rhi::TextureHandle target, bool firstFrame);

// --- builtin:triangle --------------------------------------------------------------------------------
// One triangle with per-vertex colours, cleared to (0.1, 0.1, 0.1, 1). Shader
// shaders/test/color_triangle.hlsl (key "default"; "default-t0" = SM 4.0 on a D3D11 T0 device), no resources.
// Vertices, in pixels of a W x H target (origin top-left), chosen so that the pixel centres of the
// test probes are inside the triangle and less than 0.3 px from a vertex (other colours < 0.6 %):
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

// builtin:triangle on an RGBA8_UNORM target (renderSceneOffscreen wrapper).
rhi::Result<rhi::Image> renderTriangle(rhi::IDevice& device, const std::filesystem::path& shaderDir, uint32_t width,
                                       uint32_t height);

// --- builtin:textured-cube (plan F2 Task 3) ------------------------------------------------------------
// A unit cube (+-0.5) textured with tests/assets/f02/cube_faces.png (RGBA8_SRGB, CPU-generated mips in
// linear space, linear/linear/linear clamp sampler), no lighting.
//   camera  eye (0, 0, 3) looking at the origin, +Y up, right-handed, vertical FOV 45 deg, near 0.1,
//           far 10, clip Z in [0, 1];
//   model   Rx(20 deg) * Ry(30 deg) at t = 0: visible faces +Z, -X, +Y; the viewer adds Ry(45 deg/s * t);
//   faces   face i samples atlas tile i (+X, -X, +Y, -Y, +Z, -Z; 80 x 120 px tiles in a 3 x 2 grid with an
//           8 px border), inset by 2 px; local (0, 0) = the face's top-left seen from outside (frames in
//           scripts/make_f02_assets.py, which models the same scene independently for the test probes);
//   depth   D32_FLOAT, test Less + write; culling off (default) and the hidden faces drawn last, so the image
//           is only right with a working depth test; SceneDesc::cubeCull = Back culls the hidden faces instead
//           (front face = clockwise on screen, plan F3 Task 1);
//   binding space2 (material) = {faceTexture, faceSampler}, persistent; space3 (draw) = {Draw}, from
//           IDevice::allocTransient each frame.
// Clear: (51, 51, 51) in the read-back bytes for UNORM and sRGB targets (UNORM 0.2; sRGB: linear 0.0331).
inline constexpr float kCubeSpinDegPerSecond = 45.0f;

struct CubeOptions {
    uint32_t width = 128;
    uint32_t height = 128;
    rhi::Format target = rhi::Format::RGBA8_SRGB; // RGBA8_SRGB or RGBA8_UNORM
    rhi::CullMode cull = rhi::CullMode::None;     // SceneDesc::cubeCull
};

// assetDir = tests/assets (cube_faces.png is under f02/). renderSceneOffscreen wrapper.
rhi::Result<rhi::Image> renderTexturedCube(rhi::IDevice& device, const std::filesystem::path& shaderDir,
                                           const std::filesystem::path& assetDir, const CubeOptions& options);

// --- builtin:pbr-spheres (plan F2 Task 5) --------------------------------------------------------------
// The F1 glTF PBR shader shaders/pbr/pbr.hlsl with its IBL path, permutation per backend:
//   Vulkan: spirv T2_SKIN0_MORPH0_UNLIT0 (+ T2_SKIN0_MORPH0_UNLIT1 for the unlit row)
//   D3D11:  dxbc  T1_SKIN0_MORPH0_UNLIT0 (+ T1_SKIN0_MORPH0_UNLIT1); bodies identical in F1;
//   D3D11 at FL10_x (T0, plan F2 Task 7): dxbc T0_SKIN0_MORPH0_UNLIT0 (+ UNLIT1), SM 4.0; skybox "default-t0".
// Target RGBA8_UNORM with the shader's gamma 2.2 encode (Frame.frameFlags.x = 1, as Sample-Viewer).
//   camera   eye (0, 0, 12) looking down -Z, +Y up, right-handed, vertical FOV 32 deg, near 0.1, far 100;
//   spheres  UV spheres (48 segments x 24 rings), radius 0.45, centre ((col - 2) * 1.1, (2.5 - row) * 1.1, 0);
//            rows 0..4, cols 0..4: metallic = col / 4, roughness = row / 4 (row 0 at the top),
//            baseColorFactor (0.95, 0.64, 0.54, 1);
//            row 5 (unlit, FG_UNLIT=1): baseColorFactor kPbrUnlitColors[col];
//            cull back faces, front = counter-clockwise on screen (seen from outside);
//   IBL      generated on the CPU at scene creation, deterministic (Ibl.h): 32 x 32 RGBA16_FLOAT environment
//            cube (vertical sky / ground gradient, X tint, sun disk off every axis), its GGX-prefiltered
//            mips (6 levels, roughness = mip / 5), a 32 x 32 Lambertian irradiance cube, a 128 x 128 GGX
//            BRDF LUT (Sample-Viewer formula, 512 Hammersley samples); not reference mode (that is F3);
//   frame    exposure 1, environment intensity 1, envRotation identity;
//   background shaders/test/skybox.hlsl: the environment cube along the pixel's camera ray (mip 0),
//            through the same exposure / Khronos PBR Neutral / gamma 2.2 output transform.
// scripts/f02_pbr_model.py models the scene independently (probe table of the tests and cases.json).
inline constexpr float kPbrBaseColor[4] = {0.95f, 0.64f, 0.54f, 1.0f};
inline constexpr float kPbrUnlitColors[5][3] = {
    {0.70f, 0.20f, 0.00f}, {0.00f, 0.60f, 0.30f}, {0.25f, 0.00f, 0.70f}, {0.55f, 0.50f, 0.00f}, {0.00f, 0.35f, 0.65f}};

} // namespace eng::scenes
