#pragma once
// CPU-generated IBL inputs of builtin:pbr-spheres (plan F2 Task 5). Deterministic (no threads, no random
// numbers), so the scene renders the same image on every run. Not reference mode (F3).
//
// Cube faces in the D3D / Vulkan order and orientation (+X, -X, +Y, -Y, +Z, -Z; texel (0, 0) = first row
// in memory), face coordinates s, t in [-1, 1] at texel centres:
//   +X ( 1, -t, -s)   -X (-1, -t,  s)   +Y ( s,  1,  t)   -Y ( s, -1, -t)   +Z ( s, -t,  1)   -Z (-s, -t, -1)
// Cube data layout: per mip, the 6 faces one after the other, each size x size RGBA float texels.
#include <cstdint>
#include <vector>

namespace eng::scenes::ibl {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};
Vec3 normalize(Vec3 v);

// The analytic environment (must equal scripts/f02_pbr_model.py): linear HDR radiance in world direction
// d (normalized). Sun disk of radius kSunRadiusDeg around normalize(kSunDirection) with kSunRadiance;
// elsewhere a vertical gradient, sky (d.y >= 0) horizon -> zenith, ground (d.y < 0) near -> far, with
// t = min(1, |d.y| * 2.5), multiplied by max(0, 1 + 0.5 * d.x).
inline constexpr Vec3 kSunDirection{0.18f, -0.045f, 1.0f}; // unnormalized; off every axis
inline constexpr float kSunRadiusDeg = 18.0f;
inline constexpr Vec3 kSunRadiance{6.0f, 5.6f, 5.0f};
Vec3 environmentRadiance(Vec3 d);

// World direction (normalized) of the centre of texel (x, y) of `face` in a size x size cube face.
Vec3 cubeTexelDirection(uint32_t face, uint32_t x, uint32_t y, uint32_t size);

struct Cube {
    uint32_t size = 0;                      // mip 0 edge
    std::vector<std::vector<float>> levels; // [mip] = 6 * s * s * 4 floats, s = max(1, size >> mip)
};

// Mip 0 of the environment: radiance at every texel centre direction (alpha 1).
Cube environmentCube(uint32_t size);

// GGX-prefiltered radiance, mipCount levels; level m has roughness m / (mipCount - 1) (Sample-Viewer
// u_MipCount convention: lod = roughness * (mipCount - 1)). Level 0 = env level 0 unchanged. Level m > 0:
// brute-force convolution over every texel of env level 0 with weight D_GGX(N.H, roughness^2) * N.L *
// solid angle, N = V = R (split-sum assumption; the limit of Sample-Viewer's importance-sampled filterColor).
Cube prefilterGgx(const Cube& env, uint32_t mipCount);

// Lambertian irradiance (cosine-weighted mean radiance, the shader multiplies it by the base colour
// directly, as Sample-Viewer's u_LambertianEnvSampler), one level of `size`, from env level 0.
Cube irradiance(const Cube& env, uint32_t size);

// GGX BRDF LUT (Sample-Viewer ibl_filtering LUT(): Hammersley samples, GGX importance sampling,
// V_SmithGGXCorrelated): size x size RGBA, x = N.V = (i + 0.5) / size, y = roughness = (j + 0.5) / size
// (row 0 = roughness ~0, first row in memory); rgba = (scale, bias, 0, 1).
std::vector<float> brdfLut(uint32_t size, uint32_t samples);

// IEEE half (round to nearest even), for RGBA16_FLOAT uploads.
uint16_t floatToHalf(float f);

} // namespace eng::scenes::ibl
