// GPU upload of a ModelData (plan F3 Task 4). See GpuModel.h.
#include "GpuModel.h"

#include "asset/Geometry.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <map>

namespace eng::renderer {

namespace {

using namespace rhi;

constexpr int kTexBase = 0, kTexMr = 1, kTexNormal = 2, kTexOcclusion = 3, kTexEmissive = 4;

SamplerDesc samplerOf(const asset::Sampler& s, bool& usesMips) {
    SamplerDesc d;
    d.magFilter = s.magFilter == asset::Filter::Nearest ? Filter::Nearest : Filter::Linear;
    d.minFilter = s.minFilter == asset::Filter::Nearest ? Filter::Nearest : Filter::Linear;
    usesMips = s.mipFilter != asset::MipFilter::None;
    // No mipmapping in glTF (minFilter NEAREST / LINEAR): the texture is then made with one level (below), so the
    // RHI's nearest mip filter samples level 0 only.
    d.mipFilter = s.mipFilter == asset::MipFilter::Linear ? Filter::Linear : Filter::Nearest;
    const auto wrap = [](asset::Wrap w) {
        return w == asset::Wrap::ClampToEdge      ? AddressMode::ClampToEdge
               : w == asset::Wrap::MirroredRepeat ? AddressMode::MirroredRepeat
                                                  : AddressMode::Repeat;
    };
    d.addressU = wrap(s.wrapS);
    d.addressV = wrap(s.wrapT);
    d.addressW = AddressMode::Repeat;
    return d;
}

void identityUv(MaterialCB& cb) {
    for (int slot = 0; slot < 5; ++slot) {
        float* r0 = cb.uvTransform[2 * slot];
        float* r1 = cb.uvTransform[2 * slot + 1];
        r0[0] = 1.0f, r0[1] = 0.0f, r0[2] = 0.0f, r0[3] = 0.0f;
        r1[0] = 0.0f, r1[1] = 1.0f, r1[2] = 0.0f, r1[3] = 0.0f;
    }
}

} // namespace

GpuModel::~GpuModel() {
    for (auto it = owned_.rbegin(); it != owned_.rend(); ++it) {
        dev_.destroy(*it);
    }
}

const GpuMaterial& GpuModel::material(int index) const {
    return index >= 0 && static_cast<size_t>(index) < materials_.size() ? materials_[index] : defaultMaterial_;
}

Result<rhi::BufferHandle> GpuModel::buffer(std::span<const std::byte> data, rhi::BufferUsage usage,
                                           const std::string& name) {
    BufferDesc bd;
    bd.size = std::max<uint64_t>(data.size(), 16);
    bd.usage = usage;
    bd.debugName = name;
    std::vector<std::byte> padded;
    if (data.size() < bd.size) {
        padded.assign(bd.size, std::byte{0});
        std::copy(data.begin(), data.end(), padded.begin());
        data = padded;
    }
    auto b = dev_.createBuffer(bd, data);
    if (!b) {
        return std::unexpected(b.error());
    }
    return keep(*b);
}

Result<std::unique_ptr<GpuModel>> GpuModel::create(rhi::IDevice& device, const asset::ModelData& model,
                                                   std::vector<asset::Diagnostic>& diags) {
    std::unique_ptr<GpuModel> m(new GpuModel(device));
    if (auto r = m->init(model, diags); !r) {
        return std::unexpected(r.error());
    }
    return m;
}

Result<void> GpuModel::init(const asset::ModelData& model, std::vector<asset::Diagnostic>& diags) {
    const auto warn = [&](std::string s) { diags.push_back({asset::Diagnostic::Warning, std::move(s)}); };
    const DeviceCaps& caps = dev_.caps();
    const bool t0 = caps.backend == Backend::D3D11 && caps.tier == 0;

    // Samplers: one per glTF sampler, + the default (repeat, linear, mipmapped).
    std::vector<SamplerHandle> samplers;
    std::vector<bool> samplerMips;
    for (const asset::Sampler& s : model.samplers) {
        bool mips = true;
        auto h = dev_.createSampler(samplerOf(s, mips));
        if (!h) {
            return std::unexpected(h.error());
        }
        samplers.push_back(keep(*h));
        samplerMips.push_back(mips);
    }
    auto defSampler = dev_.createSampler(SamplerDesc{});
    if (!defSampler) {
        return std::unexpected(defSampler.error());
    }
    const SamplerHandle defaultSampler = keep(*defSampler);
    const auto samplerFor = [&](int s) { return s >= 0 && size_t(s) < samplers.size() ? samplers[s] : defaultSampler; };

    // Default 1 x 1 textures (bindings.hlsli: missing textures are white; the normal map is only read with
    // materialFlags.z = 1).
    const uint8_t white[4] = {255, 255, 255, 255};
    const SubresourceData whiteInit{std::as_bytes(std::span(white)), 0};
    TextureDesc wd;
    wd.format = Format::RGBA8_UNORM;
    wd.usage = TextureUsage::Sampled;
    wd.debugName = "default white";
    auto whiteTex = dev_.createTexture(wd, {&whiteInit, 1});
    if (!whiteTex) {
        return std::unexpected(whiteTex.error());
    }
    const TextureHandle whiteTexture = keep(*whiteTex);

    // Which images need mips (any sampler using them mipmaps): a texture used only with non-mipmapping samplers
    // gets one level, so the glTF "no mipmapping" filter is exact.
    std::vector<bool> imageMips(model.images.size(), false);
    for (const asset::Material& mat : model.materials) {
        for (const asset::TextureRef* t : {&mat.baseColor, &mat.metallicRoughness, &mat.normal, &mat.occlusion, &mat.emissive}) {
            if (t->valid() && size_t(t->image) < imageMips.size()) {
                imageMips[t->image] = imageMips[t->image] || t->sampler < 0 || size_t(t->sampler) >= samplerMips.size() ||
                                      samplerMips[t->sampler];
            }
        }
    }
    // Textures: one per (image, view), made on first use. An image without pixels (over the decode budget) or a KTX2
    // that does not transcode gets the default texture and a warning.
    std::map<std::pair<int, bool>, TextureHandle> textures;
    const auto texture = [&](int image, bool srgb) -> Result<TextureHandle> {
        const auto key = std::make_pair(image, srgb);
        if (auto it = textures.find(key); it != textures.end()) {
            return it->second;
        }
        const asset::Image& img = model.images[image];
        auto data = prepareTexture(img, caps, srgb);
        if (!data) {
            warn(std::format("image {}: {}: the default texture is used", image, data.error().message));
            textures[key] = whiteTexture;
            return whiteTexture;
        }
        if (!img.ktx2 && !imageMips[image]) {
            data->levels.resize(1);
            data->desc.mipLevels = 1;
        }
        auto t = createTexture(dev_, *data);
        if (!t) {
            return std::unexpected(t.error());
        }
        textures[key] = keep(*t);
        return *t;
    };

    // Materials.
    const auto makeMaterial = [&](const asset::Material& in, int index, GpuMaterial& out) -> Result<void> {
        MaterialCB& cb = out.constants;
        std::memset(&cb, 0, sizeof(cb));
        std::copy(in.baseColorFactor.begin(), in.baseColorFactor.end(), cb.baseColorFactor);
        cb.emissiveFactor[0] = in.emissiveFactor[0];
        cb.emissiveFactor[1] = in.emissiveFactor[1];
        cb.emissiveFactor[2] = in.emissiveFactor[2];
        cb.pbrParams[0] = in.metallicFactor;
        cb.pbrParams[1] = in.roughnessFactor;
        cb.pbrParams[2] = in.normal.valid() ? in.normal.scale : 1.0f;
        cb.pbrParams[3] = in.occlusion.valid() ? in.occlusion.scale : 1.0f;
        cb.alphaParams[0] = in.alphaCutoff;
        cb.materialFlags[0] = in.alphaMode == asset::AlphaMode::Mask ? 1u : in.alphaMode == asset::AlphaMode::Blend ? 2u : 0u;
        cb.materialFlags[1] = in.doubleSided ? 1u : 0u;
        cb.materialFlags[2] = in.normal.valid() ? 1u : 0u;
        identityUv(cb);
        out.alphaMode = in.alphaMode;
        out.doubleSided = in.doubleSided;
        const asset::TextureRef* refs[5] = {&in.baseColor, &in.metallicRoughness, &in.normal, &in.occlusion, &in.emissive};
        const bool srgbSlot[5] = {true, false, false, false, true};
        const char* slotName[5] = {"baseColor", "metallicRoughness", "normal", "occlusion", "emissive"};
        uint32_t sets[5] = {0, 0, 0, 0, 0};
        for (int s = 0; s < 5; ++s) {
            out.textures[s] = whiteTexture;
            out.samplers[s] = defaultSampler;
            const asset::TextureRef& t = *refs[s];
            if (!t.valid() || size_t(t.image) >= model.images.size()) {
                continue;
            }
            auto tex = texture(t.image, srgbSlot[s]);
            if (!tex) {
                return std::unexpected(tex.error());
            }
            out.textures[s] = *tex;
            out.samplers[s] = samplerFor(t.sampler);
            sets[s] = t.texCoord;
            if (t.texCoord > 1) {
                warn(std::format("material {} {}Texture uses TEXCOORD_{}: the F1 PBR shader has TEXCOORD_0 / 1 only, "
                                 "sampled with TEXCOORD_0",
                                 index, slotName[s], t.texCoord));
                sets[s] = 0;
            }
        }
        cb.texCoordSets[0] = sets[kTexBase];
        cb.texCoordSets[1] = sets[kTexMr];
        cb.texCoordSets[2] = sets[kTexNormal];
        cb.texCoordSets[3] = sets[kTexOcclusion];
        cb.texCoordSets2[0] = sets[kTexEmissive];
        auto b = buffer(std::as_bytes(std::span(&cb, 1)), BufferUsage::Uniform,
                        std::format("material {} constants", index));
        if (!b) {
            return std::unexpected(b.error());
        }
        out.cbuffer = *b;
        return {};
    };
    materials_.resize(model.materials.size());
    for (size_t i = 0; i < model.materials.size(); ++i) {
        if (auto r = makeMaterial(model.materials[i], static_cast<int>(i), materials_[i]); !r) {
            return r;
        }
    }
    if (auto r = makeMaterial(asset::Material{}, -1, defaultMaterial_); !r) { // glTF default material
        return r;
    }

    // Meshes.
    meshes_.resize(model.meshes.size());
    for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
        for (size_t pi = 0; pi < model.meshes[mi].primitives.size(); ++pi) {
            asset::Primitive p = model.meshes[mi].primitives[pi]; // copy: strips become lists below
            const std::string where = std::format("mesh {} primitive {}", mi, pi);
            asset::stripToList(p); // the RHI draws lists (TRIANGLE_STRIP / LINE_STRIP -> list, winding kept)
            GpuPrimitive g;
            g.vertexCount = p.vertexCount;
            g.material = p.material;
            g.bounds = p.bounds;
            g.triangles = p.topology == asset::Topology::TriangleList;
            g.topology = p.topology == asset::Topology::PointList  ? PrimitiveTopology::PointList
                         : p.topology == asset::Topology::LineList ? PrimitiveTopology::LineList
                                                                   : PrimitiveTopology::TriangleList;
            if (p.vertexCount == 0 || p.indices.empty()) {
                meshes_[mi].push_back(g); // nothing to draw
                continue;
            }
            std::vector<PbrVertex> v(p.vertexCount);
            for (uint32_t i = 0; i < p.vertexCount; ++i) {
                PbrVertex& o = v[i];
                std::copy(p.position[i].begin(), p.position[i].end(), o.position);
                if (!p.normal.empty()) {
                    std::copy(p.normal[i].begin(), p.normal[i].end(), o.normal);
                } else {
                    o.normal[2] = 1.0f; // lines / points without NORMAL
                }
                if (!p.tangent.empty()) {
                    std::copy(p.tangent[i].begin(), p.tangent[i].end(), o.tangent);
                } else {
                    o.tangent[0] = 1.0f, o.tangent[3] = 1.0f;
                }
                if (!p.texCoord[0].empty()) {
                    o.uv0[0] = p.texCoord[0][i][0], o.uv0[1] = p.texCoord[0][i][1];
                }
                if (!p.texCoord[1].empty()) {
                    o.uv1[0] = p.texCoord[1][i][0], o.uv1[1] = p.texCoord[1][i][1];
                }
                if (!p.color0.empty()) {
                    std::copy(p.color0[i].begin(), p.color0[i].end(), o.color);
                } else {
                    o.color[0] = o.color[1] = o.color[2] = o.color[3] = 1.0f;
                }
            }
            auto vb = buffer(std::as_bytes(std::span(v)), BufferUsage::Vertex, where + " vertices");
            if (!vb) {
                return std::unexpected(vb.error());
            }
            g.vertices = *vb;
            auto ib = buffer(std::as_bytes(std::span(p.indices)), BufferUsage::Index, where + " indices");
            if (!ib) {
                return std::unexpected(ib.error());
            }
            g.indices = *ib;
            g.indexCount = static_cast<uint32_t>(p.indices.size());
            if (!p.joints[0].empty() && !p.weights[0].empty()) {
                if (!p.joints[1].empty()) {
                    warn(where + ": JOINTS_1 / WEIGHTS_1 (more than 4 influences) are not in the F1 shader: the first 4 "
                                 "are used");
                }
                std::vector<SkinVertex> s(p.vertexCount);
                for (uint32_t i = 0; i < p.vertexCount; ++i) {
                    std::copy(p.joints[0][i].begin(), p.joints[0][i].end(), s[i].joints);
                    std::copy(p.weights[0][i].begin(), p.weights[0][i].end(), s[i].weights);
                }
                auto sb = buffer(std::as_bytes(std::span(s)), BufferUsage::Vertex, where + " skin");
                if (!sb) {
                    return std::unexpected(sb.error());
                }
                g.skin = *sb;
                g.skinned = true;
            }
            if (!p.targets.empty()) {
                g.targetCount = static_cast<uint32_t>(p.targets.size());
                if (t0) {
                    // T0: position deltas of the first 8 targets as MORPH_POSITION0..7.
                    if (g.targetCount > kT0MaxMorphTargets) {
                        warn(std::format("{}: {} morph targets, T0 renders the first {}", where, g.targetCount,
                                         kT0MaxMorphTargets));
                        g.targetCount = kT0MaxMorphTargets;
                    }
                    std::vector<float> a(size_t(p.vertexCount) * kT0MaxMorphTargets * 3, 0.0f);
                    for (uint32_t t = 0; t < g.targetCount; ++t) {
                        if (p.targets[t].position.empty()) {
                            continue;
                        }
                        for (uint32_t i = 0; i < p.vertexCount; ++i) {
                            for (int k = 0; k < 3; ++k) {
                                a[(size_t(i) * kT0MaxMorphTargets + t) * 3 + k] = p.targets[t].position[i][k];
                            }
                        }
                    }
                    auto mb = buffer(std::as_bytes(std::span(a)), BufferUsage::Vertex, where + " morph positions");
                    if (!mb) {
                        return std::unexpected(mb.error());
                    }
                    g.morphAttributes = *mb;
                } else {
                    std::vector<float> d(size_t(g.targetCount) * p.vertexCount * 3 * 4, 0.0f);
                    for (uint32_t t = 0; t < g.targetCount; ++t) {
                        const asset::MorphTarget& mt = p.targets[t];
                        g.tangentDeltas = g.tangentDeltas || !mt.tangent.empty();
                        for (uint32_t i = 0; i < p.vertexCount; ++i) {
                            const size_t base = (size_t(t) * p.vertexCount + i) * 3;
                            const std::vector<asset::Vec3>* streams[3] = {&mt.position, &mt.normal, &mt.tangent};
                            for (int k = 0; k < 3; ++k) {
                                if (!streams[k]->empty()) {
                                    std::copy((*streams[k])[i].begin(), (*streams[k])[i].end(), &d[(base + k) * 4]);
                                }
                            }
                        }
                    }
                    auto db = buffer(std::as_bytes(std::span(d)), BufferUsage::Storage, where + " morph deltas");
                    if (!db) {
                        return std::unexpected(db.error());
                    }
                    g.morphDeltas = *db;
                }
            }
            meshes_[mi].push_back(g);
        }
    }
    return {};
}

} // namespace eng::renderer
