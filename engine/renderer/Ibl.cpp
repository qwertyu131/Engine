// GPU path of the IBL (plan F3 Task 5, T1 / T2 devices) and the texture upload shared with the CPU path. See Ibl.h.
//
// glTF-Sample-Renderer v1.1.0 (d59d1d6) iblSampler.filterAll (source/ibl_sampler.js), pass by pass:
//   panoramaToCubeMap   shaders/ibl/equirect_to_cube.hlsl, every face and mip of the source cube (the box mip chain of
//                       generateMipmap computed from the panorama, see the shader);
//   cubeMapToLambertian shaders/ibl/prefilter_lambert.hlsl, mip 0, lambertianSampleCount;
//   cubeMapToGGX        shaders/ibl/prefilter_ggx.hlsl, levels 0..mipmapLevels, roughness = level / (mipmapLevels - 1);
//   sampleGGXLut        shaders/ibl/brdf_lut.hlsl, lutResolution^2, lutSampleCount.
// applyFilter's uniforms: u_roughness, u_sampleCount, u_width = textureSize (not the mip's edge), u_lodBias,
// u_distribution, u_currentFace, u_intensityScale = scaleValue = 1 (FLOAT path). Every target is RGBA32F.
#include "Ibl.h"

#include "core/Log.h"
#include "rhi/ShaderLibrary.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace eng::renderer::ibl {

namespace {

using namespace rhi;

struct FilterCB {
    int32_t ints[4];  // face, sampleCount, width, distribution
    float floats[4];  // roughness, lodBias, intensityScale, target size
};
static_assert(sizeof(FilterCB) == 32);

struct Pass {
    PipelineHandle pipeline;
    std::vector<BindGroupLayout> layouts;
    ShaderHandle vs, ps;
};

Result<Pass> makePass(IDevice& dev, const std::filesystem::path& shaderDir, const char* shader, Format format) {
    const Backend b = dev.caps().backend;
    auto vsBlob = ShaderLibrary::load(shaderDir, shader, "VSMain", b, "default");
    if (!vsBlob) {
        return std::unexpected(vsBlob.error());
    }
    auto psBlob = ShaderLibrary::load(shaderDir, shader, "PSMain", b, "default");
    if (!psBlob) {
        return std::unexpected(psBlob.error());
    }
    auto layouts = deriveBindGroupLayouts(vsBlob->reflection, psBlob->reflection);
    if (!layouts) {
        return std::unexpected(layouts.error());
    }
    Pass p;
    auto vs = dev.createShader(*vsBlob);
    if (!vs) {
        return std::unexpected(vs.error());
    }
    p.vs = *vs;
    auto ps = dev.createShader(*psBlob);
    if (!ps) {
        dev.destroy(p.vs);
        return std::unexpected(ps.error());
    }
    p.ps = *ps;
    GraphicsPipelineDesc pd;
    pd.vs = p.vs;
    pd.ps = p.ps;
    pd.colorFormats = {format};
    pd.debugName = std::string("IBL ") + shader;
    auto pipe = dev.createGraphicsPipeline(pd);
    if (!pipe) {
        dev.destroy(p.ps);
        dev.destroy(p.vs);
        return std::unexpected(pipe.error());
    }
    p.pipeline = *pipe;
    p.layouts = std::move(*layouts);
    return p;
}

void destroyPass(IDevice& dev, Pass& p) {
    if (p.pipeline) {
        dev.destroy(p.pipeline);
    }
    if (p.ps) {
        dev.destroy(p.ps);
    }
    if (p.vs) {
        dev.destroy(p.vs);
    }
    p = {};
}

TextureUsage usage(bool renderTarget, bool copySrc) {
    TextureUsage u = TextureUsage::Sampled;
    if (renderTarget) {
        u = u | TextureUsage::RenderTarget;
    }
    if (copySrc) {
        u = u | TextureUsage::CopySrc;
    }
    return u;
}

Result<TextureHandle> cubeTexture(IDevice& dev, uint32_t size, uint32_t mips, bool renderTarget, bool copySrc,
                                  const char* name, std::span<const SubresourceData> init = {}) {
    TextureDesc td;
    td.width = td.height = size;
    td.mipLevels = mips;
    td.arrayLayers = 6;
    td.cube = true;
    td.format = Format::RGBA32_FLOAT;
    td.usage = usage(renderTarget, copySrc);
    td.debugName = name;
    return dev.createTexture(td, init);
}

} // namespace

void destroy(IDevice& dev, GpuIbl& ibl) {
    for (TextureHandle t : {ibl.specular, ibl.irradiance, ibl.lut}) {
        if (t) {
            dev.destroy(t);
        }
    }
    ibl = {};
}

Result<GpuIbl> upload(IDevice& dev, const Cube& specular, const Cube& irradiance, const std::vector<float>& lut,
                      uint32_t lutResolution, uint32_t mipCount, bool copySrc) {
    GpuIbl out;
    out.mipCount = mipCount;
    const auto cube = [&](const Cube& c, const char* name) -> Result<TextureHandle> {
        const auto mips = static_cast<uint32_t>(c.levels.size());
        std::vector<SubresourceData> init;
        for (uint32_t face = 0; face < 6; ++face) {
            for (uint32_t m = 0; m < mips; ++m) {
                const uint32_t s = std::max(1u, c.size >> m);
                const size_t faceElems = size_t(s) * s * 4u;
                init.push_back({std::as_bytes(std::span(c.levels[m]).subspan(face * faceElems, faceElems)), 0});
            }
        }
        return cubeTexture(dev, c.size, mips, false, copySrc, name, init);
    };
    auto spec = cube(specular, "IBL specular (GGX)");
    if (!spec) {
        return std::unexpected(spec.error());
    }
    out.specular = *spec;
    auto irr = cube(irradiance, "IBL irradiance (Lambertian)");
    if (!irr) {
        destroy(dev, out);
        return std::unexpected(irr.error());
    }
    out.irradiance = *irr;
    TextureDesc ld;
    ld.width = ld.height = lutResolution;
    ld.format = Format::RGBA32_FLOAT;
    ld.usage = usage(false, copySrc);
    ld.debugName = "IBL GGX LUT";
    const SubresourceData lutInit{std::as_bytes(std::span(lut)), 0};
    auto l = dev.createTexture(ld, {&lutInit, 1});
    if (!l) {
        destroy(dev, out);
        return std::unexpected(l.error());
    }
    out.lut = *l;
    return out;
}

Result<GpuIbl> buildGpu(IDevice& dev, const std::filesystem::path& shaderDir, const Panorama& pano, const Params& p,
                        bool copySrc) {
    if (!dev.caps().format(Format::RGBA32_FLOAT).filter) {
        return std::unexpected(Error{2, "IBL: the device cannot filter RGBA32_FLOAT textures"});
    }
    const uint32_t size = p.textureSize;
    const uint32_t sourceMips = static_cast<uint32_t>(std::floor(std::log2(double(size)))) + 1u;
    const uint32_t mipmapLevels = p.mipmapLevels();
    GpuIbl out;
    out.mipCount = mipmapLevels;
    std::vector<Handle> temp; // destroyed at the end (deferred until the GPU is done)
    Pass passes[4];
    const auto cleanup = [&] {
        for (Pass& ps : passes) {
            destroyPass(dev, ps);
        }
        for (auto it = temp.rbegin(); it != temp.rend(); ++it) {
            dev.destroy(*it);
        }
        temp.clear();
    };
    const auto fail = [&](const Error& e) -> Result<GpuIbl> {
        cleanup();
        destroy(dev, out);
        return std::unexpected(e);
    };
    const char* names[4] = {"equirect_to_cube", "prefilter_lambert", "prefilter_ggx", "brdf_lut"};
    for (int i = 0; i < 4; ++i) {
        auto ps = makePass(dev, shaderDir, names[i], Format::RGBA32_FLOAT);
        if (!ps) {
            return fail(ps.error());
        }
        passes[i] = std::move(*ps);
    }

    // Panorama: RGB32F padded to RGBA32F (loadTextureHDR's FLOAT branch keeps RGB32F; alpha is never read).
    std::vector<float> rgba(size_t(pano.width) * pano.height * 4);
    for (size_t i = 0, n = size_t(pano.width) * pano.height; i < n; ++i) {
        rgba[i * 4 + 0] = pano.rgb[i * 3 + 0];
        rgba[i * 4 + 1] = pano.rgb[i * 3 + 1];
        rgba[i * 4 + 2] = pano.rgb[i * 3 + 2];
        rgba[i * 4 + 3] = 1.0f;
    }
    TextureDesc pd;
    pd.width = pano.width;
    pd.height = pano.height;
    pd.format = Format::RGBA32_FLOAT;
    pd.usage = TextureUsage::Sampled;
    pd.debugName = "IBL panorama";
    const SubresourceData panoInit{std::as_bytes(std::span(rgba)), 0};
    auto panoTex = dev.createTexture(pd, {&panoInit, 1});
    if (!panoTex) {
        return fail(panoTex.error());
    }
    temp.push_back(*panoTex);
    SamplerDesc panoSd; // LINEAR, MIRRORED_REPEAT, no mips (loadTextureHDR)
    panoSd.mipFilter = Filter::Nearest;
    panoSd.addressU = panoSd.addressV = panoSd.addressW = AddressMode::MirroredRepeat;
    auto panoSmp = dev.createSampler(panoSd);
    if (!panoSmp) {
        return fail(panoSmp.error());
    }
    temp.push_back(*panoSmp);
    SamplerDesc cubeSd; // createCubemapTexture(true): LINEAR_MIPMAP_LINEAR, LINEAR, CLAMP_TO_EDGE
    cubeSd.addressU = cubeSd.addressV = cubeSd.addressW = AddressMode::ClampToEdge;
    auto cubeSmp = dev.createSampler(cubeSd);
    if (!cubeSmp) {
        return fail(cubeSmp.error());
    }
    temp.push_back(*cubeSmp);

    auto source = cubeTexture(dev, size, sourceMips, true, copySrc, "IBL source cube");
    if (!source) {
        return fail(source.error());
    }
    temp.push_back(*source);
    auto irr = cubeTexture(dev, size, 1, true, copySrc, "IBL irradiance (Lambertian)");
    if (!irr) {
        return fail(irr.error());
    }
    out.irradiance = *irr;
    auto spec = cubeTexture(dev, size, p.ggxLevels(), true, copySrc, "IBL specular (GGX)");
    if (!spec) {
        return fail(spec.error());
    }
    out.specular = *spec;
    TextureDesc ld;
    ld.width = ld.height = p.lutResolution;
    ld.format = Format::RGBA32_FLOAT;
    ld.usage = usage(true, copySrc);
    ld.debugName = "IBL GGX LUT";
    auto lut = dev.createTexture(ld);
    if (!lut) {
        return fail(lut.error());
    }
    out.lut = *lut;

    ICommandList& cmd = dev.beginFrame();
    const auto group = [&](const Pass& pass, const FilterCB& cb, TextureHandle tex, SamplerHandle smp,
                           const char* texName, const char* smpName) -> Result<BindGroupHandle> {
        const BufferBinding b = dev.allocTransient(sizeof(cb), 256);
        if (!b.buffer) {
            return std::unexpected(Error{2, "IBL: allocTransient failed"});
        }
        std::memcpy(b.cpu, &cb, sizeof(cb));
        BindGroupDesc d;
        d.pipeline = pass.pipeline;
        d.space = 0;
        for (const BindGroupLayout& l : pass.layouts) {
            if (l.space != 0) {
                continue;
            }
            for (const BindGroupLayoutEntry& e : l.entries) {
                if (e.name == "Filter") {
                    d.entries.push_back({e.name, b});
                } else if (e.name == texName) {
                    d.entries.push_back({e.name, tex});
                } else if (e.name == smpName) {
                    d.entries.push_back({e.name, smp});
                } else {
                    return std::unexpected(Error{2, std::format("IBL: unexpected resource '{}'", e.name)});
                }
            }
        }
        auto g = dev.createBindGroup(d);
        if (g) {
            temp.push_back(*g);
        }
        return g;
    };
    const auto draw = [&](const Pass& pass, BindGroupHandle g, TextureHandle target, uint32_t mip, uint32_t layer,
                          uint32_t edge) {
        RenderPassDesc rp;
        ColorAttachment ca;
        ca.texture = target;
        ca.mipLevel = mip;
        ca.arrayLayer = layer;
        ca.load = LoadOp::DontCare;
        rp.colors.push_back(ca);
        cmd.beginRenderPass(rp);
        cmd.setViewport({0.0f, 0.0f, float(edge), float(edge), 0.0f, 1.0f});
        cmd.setScissor({0, 0, edge, edge});
        cmd.setPipeline(pass.pipeline);
        cmd.setBindGroup(0, g);
        cmd.draw(3, 1, 0, 0);
        cmd.endRenderPass();
    };
    const TextureBarrier toRt[4] = {{*source, ResourceState::Undefined, ResourceState::RenderTarget},
                                    {out.irradiance, ResourceState::Undefined, ResourceState::RenderTarget},
                                    {out.specular, ResourceState::Undefined, ResourceState::RenderTarget},
                                    {out.lut, ResourceState::Undefined, ResourceState::RenderTarget}};
    cmd.barrier(toRt);
    Result<void> rec;
    // 1. panoramaToCubeMap + generateMipmap.
    for (uint32_t mip = 0; mip < sourceMips && rec; ++mip) {
        for (int32_t face = 0; face < 6 && rec; ++face) {
            const FilterCB cb{{face, int32_t(1u << mip), int32_t(size), 0}, {0, 0, 1, float(std::max(1u, size >> mip))}};
            auto g = group(passes[0], cb, *panoTex, *panoSmp, "u_panorama", "u_panoramaSampler");
            if (!g) {
                rec = std::unexpected(g.error());
                break;
            }
            draw(passes[0], *g, *source, mip, uint32_t(face), std::max(1u, size >> mip));
        }
    }
    const TextureBarrier srcRead{*source, ResourceState::RenderTarget, ResourceState::ShaderRead};
    cmd.barrier({&srcRead, 1});
    // 2. cubeMapToLambertian.
    for (int32_t face = 0; face < 6 && rec; ++face) {
        const FilterCB cb{{face, int32_t(p.lambertianSampleCount), int32_t(size), 0},
                          {0.0f, p.lodBias, 1.0f, float(size)}};
        auto g = group(passes[1], cb, *source, *cubeSmp, "uCubeMap", "uCubeMapSampler");
        if (!g) {
            rec = std::unexpected(g.error());
            break;
        }
        draw(passes[1], *g, out.irradiance, 0, uint32_t(face), size);
    }
    // 3. cubeMapToGGX.
    for (uint32_t level = 0; level <= mipmapLevels && rec; ++level) {
        const float roughness = float(level) / float(mipmapLevels - 1);
        const uint32_t edge = std::max(1u, size >> level);
        for (int32_t face = 0; face < 6 && rec; ++face) {
            const FilterCB cb{{face, int32_t(p.ggxSampleCount), int32_t(size), 1}, {roughness, p.lodBias, 1.0f, float(edge)}};
            auto g = group(passes[2], cb, *source, *cubeSmp, "uCubeMap", "uCubeMapSampler");
            if (!g) {
                rec = std::unexpected(g.error());
                break;
            }
            draw(passes[2], *g, out.specular, level, uint32_t(face), edge);
        }
    }
    // 4. sampleGGXLut.
    if (rec) {
        const FilterCB cb{{0, int32_t(p.lutSampleCount), 0, 1}, {0.0f, 0.0f, 1.0f, float(p.lutResolution)}};
        auto g = group(passes[3], cb, {}, {}, "", "");
        if (!g) {
            rec = std::unexpected(g.error());
        } else {
            draw(passes[3], *g, out.lut, 0, 0, p.lutResolution);
        }
    }
    const TextureBarrier toRead[3] = {{out.irradiance, ResourceState::RenderTarget, ResourceState::ShaderRead},
                                      {out.specular, ResourceState::RenderTarget, ResourceState::ShaderRead},
                                      {out.lut, ResourceState::RenderTarget, ResourceState::ShaderRead}};
    cmd.barrier(toRead);
    Result<void> sub = dev.submitAndWait();
    cleanup();
    if (!rec) {
        destroy(dev, out);
        return std::unexpected(rec.error());
    }
    if (!sub) {
        destroy(dev, out);
        return std::unexpected(sub.error());
    }
    return out;
}

} // namespace eng::renderer::ibl
