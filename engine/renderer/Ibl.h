#pragma once
// Image-based lighting from an equirectangular HDR (plan F3 Task 5, spec K-B reference mode / K-F).
//
// A by-value port of glTF-Sample-Renderer v1.1.0 (d59d1d6) iblSampler (source/ibl_sampler.js) with its shaders
// (panorama_to_cubemap.frag, ibl_filtering.frag, fullscreen.vert), on the FLOAT path (RGBA32F everywhere: the
// reference browser, Firefox 160 / ANGLE D3D11 on this machine, has EXT_color_buffer_float and
// OES_texture_float_linear, so prepareTextureData keeps the HDR in float and scaleValue = 1):
//   1. the HDR (loadHDR / _rgbeToFloat; here stb_image .hdr, the same RGBE decode: byte * 2^(e - 136)) is a LINEAR,
//      MIRRORED_REPEAT, single-level texture;
//   2. panoramaToCubeMap: a textureSize cube, then generateMipmap (2 x 2 box filter down to 1 x 1);
//   3. cubeMapToLambertian: mip 0 of a textureSize cube, lambertianSampleCount samples;
//   4. cubeMapToGGX: levels 0..mipmapLevels (inclusive) of a textureSize cube, roughness = level / (mipmapLevels - 1),
//      ggxSampleCount samples, u_width = textureSize at every level, lodBias;
//   5. sampleGGXLut: lutResolution^2, 512 samples (hard-coded in sampleLut).
// mipmapLevels = floor(log2(textureSize)) + 1 - lowestMipLevel = u_MipCount of the PBR and background shaders
// (lod = roughness * (u_MipCount - 1)). Sheen (Charlie cube / LUT) is F6 and not built.
//
// Two implementations with the same Params and the same functions (names as ibl_filtering.frag):
//   GPU (Ibl.cpp, T1 / T2): full-screen passes shaders/ibl/{equirect_to_cube, prefilter_ggx, prefilter_lambert,
//       brdf_lut}.hlsl into each face / mip (ColorAttachment::{arrayLayer, mipLevel});
//   CPU (IblCpu.cpp, T0): the same math in C++, multi-threaded, with seamless trilinear cube sampling.
// Both give the same cubes and LUT within 2 / 255 (tests/unit/renderer/test_reference_mode.cpp).
//
// Cube data layout (CPU): per mip, the 6 faces one after the other (+X, -X, +Y, -Y, +Z, -Z), each s x s RGBA float
// texels, row 0 first in memory (the GL / D3D / Vulkan face texel order: the same direction -> (face, s, t) table).
#include "core/Result.h"
#include "rhi/Rhi.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace eng::renderer::ibl {

// iblSampler constructor values (ibl_sampler.js); sampleLut's u_sampleCount 512.
struct Params {
    uint32_t textureSize = 256;           // r.IBL.Resolution
    uint32_t ggxSampleCount = 1024;       // r.IBL.SpecularSamples
    uint32_t lambertianSampleCount = 2048; // r.IBL.DiffuseSamples
    float lodBias = 0.0f;
    uint32_t lowestMipLevel = 4;
    uint32_t lutResolution = 1024;
    uint32_t lutSampleCount = 512;
    // floor(log2(textureSize)) + 1 - lowestMipLevel (at least 2, so roughness = level / (mipmapLevels - 1) is finite).
    uint32_t mipmapLevels() const;
    // Levels written by cubeMapToGGX: 0..mipmapLevels.
    uint32_t ggxLevels() const { return mipmapLevels() + 1; }
    bool operator==(const Params&) const = default;
};

struct Panorama {
    uint32_t width = 0, height = 0;
    std::vector<float> rgb; // linear, rows top-down (first HDR scanline first)
};
// stb_image .hdr (Radiance RGBE). Error{2} for a missing or undecodable file.
Result<Panorama> loadHdr(const std::filesystem::path& path);

struct Cube {
    uint32_t size = 0;                      // mip 0 edge
    std::vector<std::vector<float>> levels; // [mip] = 6 * s * s * 4 floats, s = max(1, size >> mip)
};

// CPU path (IblCpu.cpp).
Cube panoramaToCube(const Panorama& pano, uint32_t size);              // mip 0 + the full box mip chain
Cube cubeMapToLambertian(const Cube& source, const Params& p);         // 1 level, p.textureSize
Cube cubeMapToGGX(const Cube& source, const Params& p);                // p.ggxLevels() levels
std::vector<float> sampleGGXLut(const Params& p);                      // lutResolution^2 RGBA, row 0 = roughness ~0
// Seamless trilinear cube sample as the GPU's SampleLevel (exposed for the tests).
void sampleCubeLod(const Cube& c, const float dir[3], float lod, float out[3]);

// GPU textures of one environment (RGBA32F, sampled).
struct GpuIbl {
    rhi::TextureHandle specular;   // ggxLevels() mips (u_GGXEnvSampler)
    rhi::TextureHandle irradiance; // 1 mip (u_LambertianEnvSampler)
    rhi::TextureHandle lut;        // lutResolution^2 (u_GGXLUT)
    uint32_t mipCount = 0;         // u_MipCount = mipmapLevels()
};

// Uploads CPU results (T0, and the built-in environment).
Result<GpuIbl> upload(rhi::IDevice& dev, const Cube& specular, const Cube& irradiance, const std::vector<float>& lut,
                      uint32_t lutResolution, uint32_t mipCount, bool copySrc = false);
// GPU path: runs the passes on `dev` (submits and waits). copySrc adds TextureUsage::CopySrc (tests read back).
Result<GpuIbl> buildGpu(rhi::IDevice& dev, const std::filesystem::path& shaderDir, const Panorama& pano,
                        const Params& p, bool copySrc = false);
// CPU path, cached per process by (file, Params): the T0 renderer reuses it across scenes.
Result<GpuIbl> buildCpu(rhi::IDevice& dev, const std::filesystem::path& hdr, const Params& p, bool copySrc = false);

void destroy(rhi::IDevice& dev, GpuIbl& ibl);

} // namespace eng::renderer::ibl
