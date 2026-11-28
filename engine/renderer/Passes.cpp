// Passes of a glTF scene (plan F3 Task 4). See Passes.h.
#include "Passes.h"

#include "builtin/Ibl.h"
#include "core/Log.h"
#include "rhi/ShaderLibrary.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace eng::renderer {

namespace {

using namespace rhi;

constexpr uint32_t kEnvSize = 32, kEnvMips = 6, kIrradianceSize = 32, kLutSize = 128, kLutSamples = 512;

void copyMat(float* dst, const scene::Mat4& m) {
    std::memcpy(dst, m.m.data(), 64);
}

// The built-in IBL is a pure function of the constants above: its CPU data (prefiltered specular, irradiance, BRDF
// LUT) is built once per process and shared by every ModelRenderer (each Renderer / scene used to rebuild it: ~10 s
// per Debug renderer test case). GPU textures stay per device.
struct BuiltinIbl {
    scenes::ibl::Cube specular;
    scenes::ibl::Cube irradiance;
    std::vector<float> lut;
};
const BuiltinIbl& builtinIbl() {
    static const BuiltinIbl data = [] {
        namespace ibl = scenes::ibl;
        const ibl::Cube env = ibl::environmentCube(kEnvSize);
        return BuiltinIbl{ibl::prefilterGgx(env, kEnvMips), ibl::irradiance(env, kIrradianceSize),
                          ibl::brdfLut(kLutSize, kLutSamples)};
    }();
    return data;
}

std::vector<uint16_t> toHalf(std::span<const float> v) {
    std::vector<uint16_t> out(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        out[i] = scenes::ibl::floatToHalf(v[i]);
    }
    return out;
}

} // namespace

const BindGroupLayout* ModelRenderer::Pipeline::layout(uint32_t space) const {
    for (const BindGroupLayout& l : layouts) {
        if (l.space == space) {
            return &l;
        }
    }
    return nullptr;
}

ModelRenderer::~ModelRenderer() {
    releaseIbl();
    background_.release(dev_);
    for (const auto& [k, g] : drawGroups_) {
        dev_.destroy(g);
    }
    for (const auto& [k, g] : materialGroups_) {
        dev_.destroy(g);
    }
    if (depth_) {
        dev_.destroy(depth_);
    }
    for (auto it = owned_.rbegin(); it != owned_.rend(); ++it) {
        dev_.destroy(*it);
    }
    gpu_.reset();
}

Result<std::unique_ptr<ModelRenderer>> ModelRenderer::create(IDevice& device, const std::filesystem::path& shaderDir,
                                                             Format colorFormat, const asset::ModelData& model,
                                                             const scene::SceneInstance& scene,
                                                             const EnvironmentDesc& environment,
                                                             std::vector<asset::Diagnostic>& diags) {
    std::unique_ptr<ModelRenderer> r(new ModelRenderer(device, model, scene));
    r->colorFormat_ = colorFormat;
    r->environment_ = environment;
    if (auto e = r->init(shaderDir, diags); !e) {
        return std::unexpected(e.error());
    }
    return r;
}

Result<void> ModelRenderer::init(const std::filesystem::path& shaderDir, std::vector<asset::Diagnostic>& diags) {
    shaderDir_ = shaderDir;
    const DeviceCaps& caps = dev_.caps();
    tier_ = caps.backend == Backend::Vulkan ? "T2" : (caps.tier == 0 ? "T0" : "T1");
    auto gpu = GpuModel::create(dev_, model_, diags);
    if (!gpu) {
        return std::unexpected(gpu.error());
    }
    gpu_ = std::move(*gpu);
    for (const scene::DrawNode& d : scene_.draws) {
        const asset::Node& n = model_.nodes[d.node];
        if (n.skin >= 0 && tier_ == "T0" && d.joints.size() / 2 > kT0MaxJoints) {
            diags.push_back({asset::Diagnostic::Warning,
                             std::format("node {}: skin with {} joints, T0 renders the first {}", d.node,
                                         d.joints.size() / 2, kT0MaxJoints)});
        }
    }
    if (auto r = background_.init(dev_, shaderDir_, colorFormat_, Format::D32_FLOAT, tier_ == "T0"); !r) {
        return r;
    }
    if (auto r = makeIbl(environment_); !r) {
        return r;
    }
    return makeDrawResources();
}

void ModelRenderer::releaseIbl() {
    for (auto it = iblOwned_.rbegin(); it != iblOwned_.rend(); ++it) {
        dev_.destroy(*it);
    }
    iblOwned_.clear();
    frameResources_.clear();
    specularMips_ = 0;
}

Result<void> ModelRenderer::setEnvironment(const EnvironmentDesc& environment) {
    if (environment == environment_ && specularMips_ != 0) {
        return {};
    }
    (void)dev_.submitAndWait(); // the material / frame groups of earlier frames may still reference the textures
    releaseIbl();
    environment_ = environment;
    return makeIbl(environment_);
}

Result<void> ModelRenderer::makeIbl(const EnvironmentDesc& environment) {
    namespace bibl = scenes::ibl;
    TextureHandle specTex, irrTex, lutTex;
    if (environment.source == "builtin") {
        const BuiltinIbl& data = builtinIbl();
        specularMips_ = kEnvMips;
        const auto cube = [&](const bibl::Cube& c, const char* name) -> Result<TextureHandle> {
            const auto mips = static_cast<uint32_t>(c.levels.size());
            std::vector<std::vector<uint16_t>> halves;
            for (const auto& level : c.levels) {
                halves.push_back(toHalf(level));
            }
            std::vector<SubresourceData> init;
            for (uint32_t face = 0; face < 6; ++face) {
                for (uint32_t m = 0; m < mips; ++m) {
                    const uint32_t sz = std::max(1u, c.size >> m);
                    const size_t faceElems = size_t(sz) * sz * 4u;
                    init.push_back({std::as_bytes(std::span(halves[m]).subspan(face * faceElems, faceElems)), 0});
                }
            }
            TextureDesc td;
            td.width = td.height = c.size;
            td.mipLevels = mips;
            td.arrayLayers = 6;
            td.cube = true;
            td.format = Format::RGBA16_FLOAT;
            td.usage = TextureUsage::Sampled;
            td.debugName = name;
            return dev_.createTexture(td, init);
        };
        auto spec = cube(data.specular, "IBL specular (builtin)");
        if (!spec) {
            return std::unexpected(spec.error());
        }
        specTex = *spec;
        iblOwned_.push_back(specTex);
        auto irr = cube(data.irradiance, "IBL irradiance (builtin)");
        if (!irr) {
            return std::unexpected(irr.error());
        }
        irrTex = *irr;
        iblOwned_.push_back(irrTex);
        const std::vector<uint16_t> lutHalf = toHalf(data.lut);
        const SubresourceData lutInit{std::as_bytes(std::span(lutHalf)), 0};
        TextureDesc ld;
        ld.width = ld.height = kLutSize;
        ld.format = Format::RGBA16_FLOAT;
        ld.usage = TextureUsage::Sampled;
        ld.debugName = "IBL BRDF LUT (builtin)";
        auto lut = dev_.createTexture(ld, {&lutInit, 1});
        if (!lut) {
            return std::unexpected(lut.error());
        }
        lutTex = *lut;
        iblOwned_.push_back(lutTex);
    } else {
        // An .hdr environment (Ibl.h): the GPU passes on T1 / T2, the CPU twin at T0.
        Result<ibl::GpuIbl> built = std::unexpected(Error{2, "IBL: not built"});
        if (tier_ == "T0") {
            built = ibl::buildCpu(dev_, environment.source, environment.params);
        } else {
            auto pano = ibl::loadHdr(environment.source);
            if (!pano) {
                return std::unexpected(pano.error());
            }
            built = ibl::buildGpu(dev_, shaderDir_, *pano, environment.params);
        }
        if (!built) {
            return std::unexpected(built.error());
        }
        specTex = built->specular;
        irrTex = built->irradiance;
        lutTex = built->lut;
        iblOwned_.push_back(specTex);
        iblOwned_.push_back(irrTex);
        iblOwned_.push_back(lutTex);
        specularMips_ = built->mipCount;
    }
    // Sample-Renderer samplers (_loadEnvironmentFromPanorama): specular LINEAR_MIPMAP_LINEAR, diffuse and LUT LINEAR,
    // all CLAMP_TO_EDGE.
    SamplerDesc envSd;
    envSd.addressU = envSd.addressV = envSd.addressW = AddressMode::ClampToEdge;
    auto envSmp = dev_.createSampler(envSd);
    if (!envSmp) {
        return std::unexpected(envSmp.error());
    }
    iblOwned_.push_back(*envSmp);
    SamplerDesc lutSd = envSd;
    lutSd.mipFilter = Filter::Nearest;
    auto lutSmp = dev_.createSampler(lutSd);
    if (!lutSmp) {
        return std::unexpected(lutSmp.error());
    }
    iblOwned_.push_back(*lutSmp);
    frameResources_ = {{"specularEnvTexture", specTex}, {"irradianceEnvTexture", irrTex}, {"brdfLutTexture", lutTex},
                       {"envSampler", *envSmp},          {"brdfLutSampler", *lutSmp}};
    return {};
}

Result<void> ModelRenderer::makeDrawResources() {
    const bool t0 = tier_ == "T0";
    for (uint32_t d = 0; d < scene_.draws.size(); ++d) {
        const scene::DrawNode& dn = scene_.draws[d];
        const auto& prims = gpu_->meshes()[dn.mesh];
        for (uint32_t p = 0; p < prims.size(); ++p) {
            const GpuPrimitive& g = prims[p];
            if (g.indexCount == 0) {
                continue;
            }
            DrawResources res;
            ObjectCB ob{};
            copyMat(ob.world, dn.world);
            copyMat(ob.normalMatrix, dn.normalMatrix);
            ob.morphInfo[0] = std::min<uint32_t>(g.targetCount, static_cast<uint32_t>(dn.morphWeights.size()));
            ob.morphInfo[1] = g.vertexCount;
            ob.morphInfo[2] = g.tangentDeltas ? 1u : 0u;
            for (uint32_t t = 0; t < 8 && t < dn.morphWeights.size(); ++t) {
                ob.morphWeights[t / 4][t % 4] = dn.morphWeights[t];
            }
            BufferDesc bd;
            bd.size = sizeof(ob);
            bd.usage = BufferUsage::Uniform;
            bd.debugName = std::format("draw {} primitive {} object", d, p);
            auto ob_ = dev_.createBuffer(bd, std::as_bytes(std::span(&ob, 1)));
            if (!ob_) {
                return std::unexpected(ob_.error());
            }
            res.object = keep(*ob_);
            if (!dn.joints.empty() && g.skinned) {
                std::vector<float> j(dn.joints.size() * 16, 0.0f);
                size_t count = dn.joints.size();
                if (t0) {
                    count = std::min<size_t>(count, 2 * kT0MaxJoints);
                    j.assign(2 * kT0MaxJoints * 16, 0.0f);
                }
                for (size_t i = 0; i < count; ++i) {
                    std::memcpy(&j[i * 16], dn.joints[i].m.data(), 64);
                }
                BufferDesc jd;
                jd.size = j.size() * sizeof(float);
                jd.usage = t0 ? BufferUsage::Uniform : BufferUsage::Storage;
                jd.debugName = std::format("draw {} joints", d);
                auto jb = dev_.createBuffer(jd, std::as_bytes(std::span(j)));
                if (!jb) {
                    return std::unexpected(jb.error());
                }
                res.joints = keep(*jb);
            }
            if (g.targetCount > 0 && !t0) {
                std::vector<float> w(std::max<size_t>(g.targetCount, 4), 0.0f);
                for (uint32_t t = 0; t < g.targetCount && t < dn.morphWeights.size(); ++t) {
                    w[t] = dn.morphWeights[t];
                }
                BufferDesc wd;
                wd.size = w.size() * sizeof(float);
                wd.usage = BufferUsage::Storage;
                wd.debugName = std::format("draw {} morph weights", d);
                auto wb = dev_.createBuffer(wd, std::as_bytes(std::span(w)));
                if (!wb) {
                    return std::unexpected(wb.error());
                }
                res.morphWeights = keep(*wb);
            }
            drawRes_[{d, p}] = res;
        }
    }
    return {};
}

Result<const ModelRenderer::Permutation*> ModelRenderer::permutation(bool skin, bool morph) {
    const auto key = std::make_pair(skin, morph);
    if (auto it = permutations_.find(key); it != permutations_.end()) {
        return &it->second;
    }
    const std::string name = std::format("{}_SKIN{}_MORPH{}_UNLIT0", tier_, skin ? 1 : 0, morph ? 1 : 0);
    const Backend b = dev_.caps().backend;
    auto vsBlob = ShaderLibrary::load(shaderDir_, "pbr", "VSMain", b, name);
    if (!vsBlob) {
        return std::unexpected(vsBlob.error());
    }
    auto psBlob = ShaderLibrary::load(shaderDir_, "pbr", "PSMain", b, name);
    if (!psBlob) {
        return std::unexpected(psBlob.error());
    }
    // cbuffer sizes of the reflection = the CPU structs.
    for (const ShaderReflection* r : {&vsBlob->reflection, &psBlob->reflection}) {
        for (const ReflectedResource& res : r->resources) {
            const size_t want = res.name == "Frame" ? sizeof(FrameCB) : res.name == "Material" ? sizeof(MaterialCB)
                                : res.name == "Object" ? sizeof(ObjectCB) : 0;
            if (want && res.sizeBytes != want) {
                return std::unexpected(Error{2, std::format("pbr {}: cbuffer {} is {} bytes in the reflection, {} on the CPU",
                                                            name, res.name, res.sizeBytes, want)});
            }
        }
    }
    auto layouts = deriveBindGroupLayouts(vsBlob->reflection, psBlob->reflection);
    if (!layouts) {
        return std::unexpected(layouts.error());
    }
    auto vs = dev_.createShader(*vsBlob);
    if (!vs) {
        return std::unexpected(vs.error());
    }
    keep(*vs);
    auto ps = dev_.createShader(*psBlob);
    if (!ps) {
        return std::unexpected(ps.error());
    }
    keep(*ps);
    return &(permutations_[key] = Permutation{*vs, *ps, std::move(*layouts)});
}

Result<const ModelRenderer::Pipeline*> ModelRenderer::pipeline(const DrawItem& item) {
    const GpuMaterial& m = *item.material;
    const bool blend = m.alphaMode == asset::AlphaMode::Blend;
    const CullMode cull = m.doubleSided || !item.prim->triangles ? CullMode::None : CullMode::Back;
    // glTF front faces are counter-clockwise as seen on screen; a negative determinant flips them.
    const FrontFace front = item.flipWinding ? FrontFace::Clockwise : FrontFace::CounterClockwise;
    const PipelineKey key{item.skinned, item.morph, cull, front, blend, item.prim->topology};
    if (auto it = pipelines_.find(key); it != pipelines_.end()) {
        return &it->second;
    }
    auto perm = permutation(item.skinned, item.morph);
    if (!perm) {
        return std::unexpected(perm.error());
    }
    const std::string name = std::format("{}_SKIN{}_MORPH{}_UNLIT0", tier_, item.skinned ? 1 : 0, item.morph ? 1 : 0);
    GraphicsPipelineDesc pd;
    pd.vs = (*perm)->vs;
    pd.ps = (*perm)->ps;
    pd.vertexLayout.buffers = {{sizeof(PbrVertex), false}};
    pd.vertexLayout.attributes = {{"POSITION", Format::RGB32_FLOAT, 0, offsetof(PbrVertex, position)},
                                  {"NORMAL", Format::RGB32_FLOAT, 0, offsetof(PbrVertex, normal)},
                                  {"TANGENT", Format::RGBA32_FLOAT, 0, offsetof(PbrVertex, tangent)},
                                  {"TEXCOORD0", Format::RG32_FLOAT, 0, offsetof(PbrVertex, uv0)},
                                  {"TEXCOORD1", Format::RG32_FLOAT, 0, offsetof(PbrVertex, uv1)},
                                  {"COLOR0", Format::RGBA32_FLOAT, 0, offsetof(PbrVertex, color)}};
    uint32_t slot = 1;
    if (item.skinned) {
        pd.vertexLayout.buffers.push_back({sizeof(SkinVertex), false});
        pd.vertexLayout.attributes.push_back({"BLENDINDICES0", Format::RGBA16_UINT, slot, offsetof(SkinVertex, joints)});
        pd.vertexLayout.attributes.push_back({"BLENDWEIGHT0", Format::RGBA32_FLOAT, slot, offsetof(SkinVertex, weights)});
        ++slot;
    }
    if (item.morph && tier_ == "T0") {
        pd.vertexLayout.buffers.push_back({kT0MaxMorphTargets * 12, false});
        for (uint32_t t = 0; t < kT0MaxMorphTargets; ++t) {
            pd.vertexLayout.attributes.push_back({std::format("MORPH_POSITION{}", t), Format::RGB32_FLOAT, slot, t * 12});
        }
        ++slot;
    }
    pd.topology = item.prim->topology;
    pd.raster.cull = cull;
    pd.raster.frontFace = front;
    pd.depth = {true, !blend, CompareOp::Less};
    pd.colorFormats = {colorFormat_};
    pd.blend = {BlendState{blend}};
    pd.depthFormat = Format::D32_FLOAT;
    pd.debugName = "glTF pbr " + name;
    auto pipe = dev_.createGraphicsPipeline(pd);
    if (!pipe) {
        return std::unexpected(pipe.error());
    }
    Pipeline p;
    p.handle = keep(*pipe);
    p.layouts = (*perm)->layouts;
    return &(pipelines_[key] = std::move(p));
}

Result<BindGroupHandle> ModelRenderer::group(const Pipeline& p, uint32_t space,
                                             const std::map<std::string, Resource>& res) {
    const BindGroupLayout* l = p.layout(space);
    if (!l) {
        return BindGroupHandle{};
    }
    BindGroupDesc d;
    d.pipeline = p.handle;
    d.space = space;
    for (const BindGroupLayoutEntry& e : l->entries) {
        auto it = res.find(e.name);
        if (it == res.end()) {
            return std::unexpected(Error{2, std::format("glTF passes: no resource for space {} {}", space, e.name)});
        }
        d.entries.push_back({e.name, it->second});
    }
    return dev_.createBindGroup(d);
}

Result<BindGroupHandle> ModelRenderer::materialGroup(const Pipeline& p, const DrawItem& item, bool skin, bool morph) {
    const auto key = std::make_tuple(item.prim->material, skin, morph);
    if (auto it = materialGroups_.find(key); it != materialGroups_.end()) {
        return it->second;
    }
    const GpuMaterial& m = *item.material;
    BufferBinding cb;
    cb.buffer = m.cbuffer;
    cb.size = sizeof(MaterialCB);
    std::map<std::string, Resource> res{{"Material", cb}};
    const char* names[5] = {"baseColor", "metallicRoughness", "normal", "occlusion", "emissive"};
    for (int s = 0; s < 5; ++s) {
        res[std::string(names[s]) + "Texture"] = m.textures[s];
        res[std::string(names[s]) + "Sampler"] = m.samplers[s];
    }
    auto g = group(p, 2, res);
    if (!g) {
        return std::unexpected(g.error());
    }
    materialGroups_[key] = *g;
    return *g;
}

Result<BindGroupHandle> ModelRenderer::drawGroup(const Pipeline& p, const DrawItem& item, bool skin, bool morph) {
    const auto key = std::make_tuple(item.draw, item.primitive, skin, morph);
    if (auto it = drawGroups_.find(key); it != drawGroups_.end()) {
        return it->second;
    }
    const DrawResources& r = drawRes_.at({item.draw, item.primitive});
    std::map<std::string, Resource> res;
    BufferBinding ob;
    ob.buffer = r.object;
    ob.size = sizeof(ObjectCB);
    res["Object"] = ob;
    if (r.joints) {
        BufferBinding jb;
        jb.buffer = r.joints;
        res["jointMatrices"] = jb; // T1/T2 storage
        res["Skin"] = jb;          // T0 cbuffer
    }
    if (item.prim->morphDeltas) {
        BufferBinding db;
        db.buffer = item.prim->morphDeltas;
        res["morphDeltas"] = db;
    }
    if (r.morphWeights) {
        BufferBinding wb;
        wb.buffer = r.morphWeights;
        res["morphTargetWeights"] = wb;
    }
    auto g = group(p, 3, res);
    if (!g) {
        return std::unexpected(g.error());
    }
    drawGroups_[key] = *g;
    return *g;
}

Result<void> ModelRenderer::resize(uint32_t width, uint32_t height) {
    if (depth_) {
        dev_.destroy(depth_);
        depth_ = {};
    }
    width_ = std::max(1u, width);
    height_ = std::max(1u, height);
    TextureDesc dd;
    dd.width = width_;
    dd.height = height_;
    dd.format = Format::D32_FLOAT;
    dd.usage = TextureUsage::DepthStencil;
    dd.debugName = "glTF depth";
    auto depth = dev_.createTexture(dd);
    if (!depth) {
        return std::unexpected(depth.error());
    }
    depth_ = *depth;
    depthState_ = ResourceState::Undefined;
    return {};
}

Result<void> ModelRenderer::record(ICommandList& cmd, TextureHandle color, const FrameParams& frame) {
    if (!depth_) {
        return std::unexpected(Error{2, "glTF passes: record before resize"});
    }
    const DrawLists lists = buildDrawLists(scene_, model_, *gpu_, frame.cameraPosition);

    FrameCB fcb{};
    copyMat(fcb.viewProj, frame.proj * frame.view);
    // u_EnvRotation: gl-matrix rotateY(identity, rotation) (renderer.js applyEnvironmentMap), upper 3x3 used.
    scene::Mat4 envRotation = scene::Mat4::identity();
    {
        const float rad = frame.rotationDeg / 180.0f * 3.14159265358979323846f;
        const float c = std::cos(rad), sn = std::sin(rad);
        envRotation.at(0, 0) = c;
        envRotation.at(2, 0) = -sn;
        envRotation.at(0, 2) = sn;
        envRotation.at(2, 2) = c;
    }
    copyMat(fcb.envRotation, envRotation);
    fcb.cameraPosition[0] = frame.cameraPosition[0];
    fcb.cameraPosition[1] = frame.cameraPosition[1];
    fcb.cameraPosition[2] = frame.cameraPosition[2];
    fcb.cameraPosition[3] = 1.0f;
    fcb.iblParams[0] = frame.exposure;
    fcb.iblParams[1] = frame.intensity;
    fcb.iblParams[2] = static_cast<float>(specularMips_);
    fcb.frameFlags[0] = frame.gammaEncode ? 1u : 0u;
    fcb.frameFlags[1] = frame.tonemap;
    const BufferBinding frameCb = dev_.allocTransient(sizeof(fcb), 256);
    if (!frameCb.buffer) {
        return std::unexpected(Error{2, "glTF passes: allocTransient failed"});
    }
    std::memcpy(frameCb.cpu, &fcb, sizeof(fcb));
    std::map<std::string, Resource> frameRes = frameResources_;
    frameRes["Frame"] = frameCb;

    // Pipelines and groups first (no command recorded yet if one fails).
    struct Prepared {
        const Pipeline* pipeline;
        BindGroupHandle material, draw;
        const DrawItem* item;
    };
    std::vector<Prepared> opaque, blend;
    std::map<const Pipeline*, BindGroupHandle> frameGroups;
    std::vector<BindGroupHandle> transient;
    const auto release = [&] {
        for (BindGroupHandle h : transient) {
            dev_.destroy(h);
        }
    };
    const auto prepare = [&](const std::vector<DrawItem>& in, std::vector<Prepared>& out) -> Result<void> {
        for (const DrawItem& it : in) {
            auto p = pipeline(it);
            if (!p && it.prim->topology == PrimitiveTopology::PointList) {
                // A device that cannot draw points with these shaders (Vulkan without VK_KHR_maintenance5): the point
                // primitives are skipped, logged once (never a silent wrong render).
                if (!pointsWarned_) {
                    ENG_LOG_WARN("glTF passes: POINTS primitives are not drawn: {}", p.error().message);
                    pointsWarned_ = true;
                }
                continue;
            }
            if (!p) {
                return std::unexpected(p.error());
            }
            if (!frameGroups.count(*p)) {
                auto fg = group(**p, 0, frameRes);
                if (!fg) {
                    return std::unexpected(fg.error());
                }
                frameGroups[*p] = *fg;
                if (*fg) {
                    transient.push_back(*fg);
                }
            }
            auto mg = materialGroup(**p, it, it.skinned, it.morph);
            if (!mg) {
                return std::unexpected(mg.error());
            }
            auto dg = drawGroup(**p, it, it.skinned, it.morph);
            if (!dg) {
                return std::unexpected(dg.error());
            }
            out.push_back({*p, *mg, *dg, &it});
        }
        return {};
    };
    if (auto r = prepare(lists.opaque, opaque); !r) {
        release();
        return r;
    }
    if (auto r = prepare(lists.blend, blend); !r) {
        release();
        return r;
    }
    const TextureBarrier toDepth{depth_, depthState_, ResourceState::DepthWrite};
    cmd.barrier({&toDepth, 1});
    depthState_ = ResourceState::DepthWrite;
    RenderPassDesc rp;
    ColorAttachment ca;
    ca.texture = color;
    ca.load = LoadOp::Clear;
    std::copy(frame.clear, frame.clear + 4, ca.clear);
    rp.colors.push_back(ca);
    rp.depth.texture = depth_;
    rp.depth.load = LoadOp::Clear;
    rp.depth.clearDepth = 1.0f;
    cmd.beginRenderPass(rp);
    cmd.setViewport({0.0f, 0.0f, float(width_), float(height_), 0.0f, 1.0f});
    cmd.setScissor({0, 0, width_, height_});
    const auto draw = [&](const Prepared& p) {
        cmd.setPipeline(p.pipeline->handle);
        if (BindGroupHandle fg = frameGroups[p.pipeline]) {
            cmd.setBindGroup(0, fg);
        }
        if (p.material) {
            cmd.setBindGroup(2, p.material);
        }
        if (p.draw) {
            cmd.setBindGroup(3, p.draw);
        }
        const GpuPrimitive& g = *p.item->prim;
        uint32_t slot = 0;
        cmd.setVertexBuffer(slot++, g.vertices, 0);
        if (p.item->skinned) {
            cmd.setVertexBuffer(slot++, g.skin, 0);
        }
        if (p.item->morph && tier_ == "T0") {
            cmd.setVertexBuffer(slot++, g.morphAttributes, 0);
        }
        cmd.setIndexBuffer(g.indices, IndexType::Uint32, 0);
        cmd.drawIndexed(g.indexCount, 1, 0, 0, 0);
    };
    if (frame.environmentBackground) {
        // drawEnvironmentMap: u_GGXEnvSampler at LOD u_EnvBlurNormalized * (u_MipCount - 1).
        BackgroundParams bp;
        bp.viewProj = frame.proj * frame.view;
        bp.envRotation = envRotation;
        bp.exposure = frame.exposure;
        bp.intensity = frame.intensity;
        bp.lod = frame.backgroundBlur * float(specularMips_ - 1);
        bp.gammaEncode = frame.gammaEncode;
        bp.tonemap = frame.tonemap;
        auto it = frameResources_.find("specularEnvTexture");
        auto smp = frameResources_.find("envSampler");
        if (auto r = background_.record(dev_, cmd, bp, std::get<TextureHandle>(it->second),
                                        std::get<SamplerHandle>(smp->second));
            !r) {
            cmd.endRenderPass();
            release();
            return r;
        }
    }
    for (const Prepared& p : opaque) {
        draw(p);
    }
    for (const Prepared& p : blend) {
        draw(p);
    }
    cmd.endRenderPass();
    release(); // deferred until the GPU is done with this frame
    return {};
}

} // namespace eng::renderer
