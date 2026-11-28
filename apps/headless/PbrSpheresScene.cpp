// builtin:pbr-spheres (plan F2 Task 5; scene definition in Scenes.h, IBL in Ibl.h).
//
// The F1 glTF PBR shader as-is (shaders/pbr/pbr.hlsl, not modified: the MSL artifacts and the F1 parity
// claims pin it) + shaders/test/skybox.hlsl for the background. Bind-group entries are built from each
// pipeline's reflection-derived layouts (deriveBindGroupLayouts): the UNLIT1 permutation reflects fewer
// resources than UNLIT0, and its D3D11 slots differ, so every pipeline gets its own groups.
#include "Scenes.h"

#include "Ibl.h"
#include "SceneUtil.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <numbers>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace eng::scenes {

namespace {

using namespace rhi;
using namespace detail;

// --- scene constants (must equal scripts/f02_pbr_model.py) -------------------------------------
constexpr float kEyeZ = 12.0f;
constexpr float kFovYDeg = 32.0f;
constexpr float kSpacing = 1.1f;
constexpr float kRadius = 0.45f;
constexpr int kCols = 5, kRows = 5, kUnlitRow = 5;
constexpr uint32_t kSegments = 48, kRings = 24;
constexpr uint32_t kEnvSize = 32, kEnvMips = 6, kIrradianceSize = 32, kLutSize = 128, kLutSamples = 512;
constexpr float kExposure = 1.0f, kEnvIntensity = 1.0f;

// cbuffer layouts of bindings.hlsli / skybox.hlsl (float4 / uint4 / float4x4 members only, so the HLSL,
// SPIR-V and std140 layouts agree; sizes are checked against the reflection).
struct FrameCB {
    float viewProj[16];
    float envRotation[16];
    float cameraPosition[4];
    float iblParams[4];
    uint32_t frameFlags[4];
};
struct MaterialCB {
    float baseColorFactor[4];
    float emissiveFactor[4];
    float pbrParams[4];
    float alphaParams[4];
    uint32_t materialFlags[4];
    uint32_t texCoordSets[4];
    uint32_t texCoordSets2[4];
    float uvTransform[10][4];
};
struct ObjectCB {
    float world[16];
    float normalMatrix[16];
    uint32_t morphInfo[4];
    float morphWeights[2][4];
};
struct SkyCB {
    float cameraToWorld[16];
    float rayParams[4];
    float outputParams[4];
    uint32_t outputFlags[4];
};
static_assert(sizeof(FrameCB) == 176 && sizeof(MaterialCB) == 272 && sizeof(ObjectCB) == 176 && sizeof(SkyCB) == 112);

struct PbrVertex {
    float position[3];
    float normal[3];
    float tangent[4];
    float uv0[2];
    float uv1[2];
    float color[4];
};

void sphereGeometry(std::vector<PbrVertex>& vertices, std::vector<uint16_t>& indices) {
    constexpr float pi = std::numbers::pi_v<float>;
    for (uint32_t i = 0; i <= kRings; ++i) {
        const float theta = pi * static_cast<float>(i) / static_cast<float>(kRings);
        for (uint32_t j = 0; j <= kSegments; ++j) {
            const float phi = 2.0f * pi * static_cast<float>(j) / static_cast<float>(kSegments);
            // phi = 0 faces +Z (the camera); u grows towards +X there, v grows downwards.
            const float n[3] = {std::sin(theta) * std::sin(phi), std::cos(theta), std::sin(theta) * std::cos(phi)};
            PbrVertex v{};
            for (int k = 0; k < 3; ++k) {
                v.position[k] = kRadius * n[k];
                v.normal[k] = n[k];
            }
            v.tangent[0] = std::cos(phi); // d position / d phi, normalized (defined at the poles too)
            v.tangent[1] = 0.0f;
            v.tangent[2] = -std::sin(phi);
            v.tangent[3] = 1.0f;
            v.uv0[0] = static_cast<float>(j) / static_cast<float>(kSegments);
            v.uv0[1] = static_cast<float>(i) / static_cast<float>(kRings);
            v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.0f;
            vertices.push_back(v);
        }
    }
    const uint32_t stride = kSegments + 1;
    for (uint32_t i = 0; i < kRings; ++i) {
        for (uint32_t j = 0; j < kSegments; ++j) {
            const auto a = static_cast<uint16_t>(i * stride + j), b = static_cast<uint16_t>((i + 1) * stride + j);
            const auto c = static_cast<uint16_t>((i + 1) * stride + j + 1), d = static_cast<uint16_t>(i * stride + j + 1);
            // counter-clockwise seen from outside = counter-clockwise on screen (FrontFace::CounterClockwise;
            // a mirrored winding would draw the far hemisphere from inside: caught by the reflection probes)
            for (const uint16_t idx : {a, b, c, a, c, d}) {
                indices.push_back(idx);
            }
        }
    }
}

void sphereCentre(int col, int row, float out[3]) {
    out[0] = static_cast<float>(col - 2) * kSpacing;
    out[1] = (2.5f - static_cast<float>(row)) * kSpacing;
    out[2] = 0.0f;
}

using Resource = std::variant<BufferBinding, TextureHandle, SamplerHandle>;

struct PipelineInfo {
    PipelineHandle pipeline;
    std::vector<BindGroupLayout> layouts;
    const BindGroupLayout* layout(uint32_t space) const {
        for (const BindGroupLayout& l : layouts) {
            if (l.space == space) {
                return &l;
            }
        }
        return nullptr;
    }
};

// The group of `space` for `pi`, every entry its layout names taken from `resources` (by name). A null
// handle if the pipeline does not use the space.
Result<BindGroupHandle> makeGroup(IDevice& dev, const PipelineInfo& pi, uint32_t space,
                                  const std::map<std::string, Resource>& resources) {
    const BindGroupLayout* l = pi.layout(space);
    if (!l) {
        return BindGroupHandle{};
    }
    BindGroupDesc d;
    d.pipeline = pi.pipeline;
    d.space = space;
    for (const BindGroupLayoutEntry& e : l->entries) {
        auto it = resources.find(e.name);
        if (it == resources.end()) {
            return std::unexpected(Error{kExitUsage, "builtin:pbr-spheres: no resource for space " +
                                                         std::to_string(space) + " " + e.name});
        }
        d.entries.push_back({e.name, it->second});
    }
    return dev.createBindGroup(d);
}

Result<void> checkSize(const PipelineInfo& pi, uint32_t space, const std::string& name, size_t bytes) {
    if (const BindGroupLayout* l = pi.layout(space)) {
        for (const BindGroupLayoutEntry& e : l->entries) {
            if (e.name == name && e.sizeBytes != bytes) {
                return std::unexpected(Error{kExitUsage, "builtin:pbr-spheres: cbuffer " + name + " is " +
                                                             std::to_string(e.sizeBytes) + " bytes in the reflection, " +
                                                             std::to_string(bytes) + " on the CPU"});
            }
        }
    }
    return {};
}

std::vector<uint16_t> toHalf(std::span<const float> v) {
    std::vector<uint16_t> out(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        out[i] = ibl::floatToHalf(v[i]);
    }
    return out;
}

class PbrSpheresScene final : public Scene {
public:
    PbrSpheresScene(IDevice& dev, const SceneDesc& d) : dev_(dev), own_(dev), desc_(d) {}
    ~PbrSpheresScene() override {
        if (depth_) {
            dev_.destroy(depth_);
        }
    }

    Result<void> init() {
        // Vulkan T2 (spirv); D3D11 the device's tier: T1 (dxbc SM 5.0, FL11_x) or T0 (dxbc SM 4.0, FL10_x;
        // plan F2 Task 7).
        const bool vulkan = dev_.caps().backend == Backend::Vulkan;
        const std::string tier = vulkan ? "T2" : (dev_.caps().tier == 0 ? "T0" : "T1");
        if (auto r = makePipeline(lit_, "pbr", tier + "_SKIN0_MORPH0_UNLIT0", true); !r) {
            return r;
        }
        if (auto r = makePipeline(unlit_, "pbr", tier + "_SKIN0_MORPH0_UNLIT1", true); !r) {
            return r;
        }
        if (auto r = makePipeline(sky_, "skybox", tierKey(dev_, "default"), false); !r) {
            return r;
        }
        for (const PipelineInfo* pi : {&lit_, &unlit_}) {
            if (auto r = checkSize(*pi, 0, "Frame", sizeof(FrameCB)); !r) {
                return r;
            }
            if (auto r = checkSize(*pi, 2, "Material", sizeof(MaterialCB)); !r) {
                return r;
            }
            if (auto r = checkSize(*pi, 3, "Object", sizeof(ObjectCB)); !r) {
                return r;
            }
        }
        if (auto r = checkSize(sky_, 0, "Sky", sizeof(SkyCB)); !r) {
            return r;
        }
        if (auto r = makeIbl(); !r) {
            return r;
        }
        if (auto r = makeGeometry(); !r) {
            return r;
        }
        if (auto r = makeSpheres(); !r) {
            return r;
        }
        return resize(desc_.width, desc_.height);
    }

    Result<void> resize(uint32_t w, uint32_t h) override {
        if (depth_) {
            dev_.destroy(depth_);
            depth_ = {};
        }
        desc_.width = w;
        desc_.height = h;
        TextureDesc dd;
        dd.width = w;
        dd.height = h;
        dd.format = Format::D32_FLOAT;
        dd.usage = TextureUsage::DepthStencil;
        dd.debugName = "builtin:pbr-spheres depth";
        auto depth = dev_.createTexture(dd);
        if (!depth) {
            return std::unexpected(depth.error());
        }
        depth_ = *depth;
        depthState_ = ResourceState::Undefined;
        return {};
    }

    Result<void> record(ICommandList& cmd, TextureHandle color) override {
        const float aspect = static_cast<float>(desc_.width) / static_cast<float>(desc_.height);
        const Mat4 proj = perspectiveRhZo(kFovYDeg * kDeg, aspect, 0.1f, 100.0f);
        const Mat4 viewProj = proj * translation(0.0f, 0.0f, -kEyeZ);
        const Mat4 identity = Mat4::identity();
        FrameCB frame{};
        std::memcpy(frame.viewProj, viewProj.m.data(), sizeof(frame.viewProj));
        std::memcpy(frame.envRotation, identity.m.data(), sizeof(frame.envRotation));
        frame.cameraPosition[2] = kEyeZ;
        frame.cameraPosition[3] = 1.0f;
        frame.iblParams[0] = kExposure;
        frame.iblParams[1] = kEnvIntensity;
        frame.iblParams[2] = static_cast<float>(kEnvMips);
        frame.frameFlags[0] = isSrgb(desc_.colorFormat) ? 0u : 1u; // gamma 2.2 encode on UNORM targets
        SkyCB sky{};
        std::memcpy(sky.cameraToWorld, identity.m.data(), sizeof(sky.cameraToWorld));
        const float tanHalf = std::tan(kFovYDeg * kDeg * 0.5f);
        sky.rayParams[0] = tanHalf * aspect;
        sky.rayParams[1] = tanHalf;
        sky.outputParams[0] = kExposure;
        sky.outputParams[1] = kEnvIntensity;
        sky.outputFlags[0] = frame.frameFlags[0];

        const BufferBinding frameCb = dev_.allocTransient(sizeof(frame), 256);
        const BufferBinding skyCb = dev_.allocTransient(sizeof(sky), 256);
        if (!frameCb.buffer || !skyCb.buffer) {
            return std::unexpected(Error{kExitUsage, "builtin:pbr-spheres: allocTransient failed"});
        }
        std::memcpy(frameCb.cpu, &frame, sizeof(frame));
        std::memcpy(skyCb.cpu, &sky, sizeof(sky));
        std::map<std::string, Resource> frameRes = frameResources_;
        frameRes["Frame"] = frameCb;
        frameRes["Sky"] = skyCb;
        std::vector<BindGroupHandle> transientGroups;
        Error failure;
        bool failed = false;
        auto group = [&](const PipelineInfo& pi) -> BindGroupHandle {
            auto g = makeGroup(dev_, pi, 0, frameRes);
            if (!g) {
                failure = g.error();
                failed = true;
                return {};
            }
            if (*g) {
                transientGroups.push_back(*g);
            }
            return *g;
        };
        const BindGroupHandle litFrame = group(lit_);
        const BindGroupHandle unlitFrame = group(unlit_);
        const BindGroupHandle skyFrame = group(sky_);
        if (failed) {
            for (BindGroupHandle h : transientGroups) {
                dev_.destroy(h);
            }
            return std::unexpected(failure);
        }

        // DepthWrite -> DepthWrite after the first frame: orders this frame's clear after the previous
        // frame's depth writes (frames in flight share the buffer).
        const TextureBarrier toDepth{depth_, depthState_, ResourceState::DepthWrite};
        cmd.barrier({&toDepth, 1});
        depthState_ = ResourceState::DepthWrite;
        RenderPassDesc rp;
        ColorAttachment ca;
        ca.texture = color;
        ca.load = LoadOp::Clear; // the background covers every pixel anyway
        rp.colors.push_back(ca);
        rp.depth.texture = depth_;
        rp.depth.load = LoadOp::Clear;
        rp.depth.clearDepth = 1.0f;
        cmd.beginRenderPass(rp);
        cmd.setViewport({0.0f, 0.0f, static_cast<float>(desc_.width), static_cast<float>(desc_.height), 0.0f, 1.0f});
        cmd.setScissor({0, 0, desc_.width, desc_.height});

        cmd.setPipeline(sky_.pipeline);
        if (skyFrame) {
            cmd.setBindGroup(0, skyFrame);
        }
        cmd.draw(3, 1, 0, 0);

        for (const bool unlit : {false, true}) {
            const PipelineInfo& pi = unlit ? unlit_ : lit_;
            cmd.setPipeline(pi.pipeline);
            const BindGroupHandle frameGroup = unlit ? unlitFrame : litFrame;
            if (frameGroup) {
                cmd.setBindGroup(0, frameGroup);
            }
            cmd.setVertexBuffer(0, vb_, 0);
            cmd.setIndexBuffer(ib_, IndexType::Uint16, 0);
            for (const Sphere& s : spheres_) {
                if (s.unlit != unlit) {
                    continue;
                }
                cmd.setBindGroup(2, s.material);
                cmd.setBindGroup(3, s.object);
                cmd.drawIndexed(indexCount_, 1, 0, 0, 0);
            }
        }
        cmd.endRenderPass();
        for (BindGroupHandle h : transientGroups) {
            dev_.destroy(h); // deferred until the GPU is done with this frame
        }
        return {};
    }

private:
    struct Sphere {
        bool unlit = false;
        BindGroupHandle material, object;
    };

    Result<void> makePipeline(PipelineInfo& out, std::string_view shader, const std::string& key, bool pbr) {
        auto sp = loadShaderPair(dev_, own_, desc_.shaderDir, shader, key);
        if (!sp) {
            return std::unexpected(sp.error());
        }
        auto layouts = deriveBindGroupLayouts(sp->vsReflection, sp->psReflection);
        if (!layouts) {
            return std::unexpected(layouts.error());
        }
        GraphicsPipelineDesc pd;
        pd.vs = sp->vs;
        pd.ps = sp->ps;
        if (pbr) {
            pd.vertexLayout.buffers = {{sizeof(PbrVertex), false}};
            pd.vertexLayout.attributes = {{"POSITION", Format::RGB32_FLOAT, 0, offsetof(PbrVertex, position)},
                                          {"NORMAL", Format::RGB32_FLOAT, 0, offsetof(PbrVertex, normal)},
                                          {"TANGENT", Format::RGBA32_FLOAT, 0, offsetof(PbrVertex, tangent)},
                                          {"TEXCOORD0", Format::RG32_FLOAT, 0, offsetof(PbrVertex, uv0)},
                                          {"TEXCOORD1", Format::RG32_FLOAT, 0, offsetof(PbrVertex, uv1)},
                                          {"COLOR0", Format::RGBA32_FLOAT, 0, offsetof(PbrVertex, color)}};
            pd.raster.cull = CullMode::Back;
            pd.raster.frontFace = FrontFace::CounterClockwise;
            pd.depth = {true, true, CompareOp::Less};
        } else {
            pd.raster.cull = CullMode::None;
            pd.depth = {false, false, CompareOp::Always};
        }
        pd.colorFormats = {desc_.colorFormat};
        pd.depthFormat = Format::D32_FLOAT;
        pd.debugName = "builtin:pbr-spheres " + std::string(shader) + " " + key;
        auto pipe = dev_.createGraphicsPipeline(pd);
        if (!pipe) {
            return std::unexpected(pipe.error());
        }
        out.pipeline = own_.keep(*pipe);
        out.layouts = std::move(*layouts);
        return {};
    }

    Result<TextureHandle> makeTexture(const TextureDesc& td, std::span<const SubresourceData> init) {
        auto t = dev_.createTexture(td, init);
        if (!t) {
            return std::unexpected(t.error());
        }
        return own_.keep(*t);
    }

    Result<TextureHandle> cubeTexture(const ibl::Cube& c, const char* name) {
        const auto mips = static_cast<uint32_t>(c.levels.size());
        std::vector<std::vector<uint16_t>> halves;
        for (const auto& level : c.levels) {
            halves.push_back(toHalf(level));
        }
        std::vector<SubresourceData> init; // layer * mipLevels + mip
        for (uint32_t face = 0; face < 6; ++face) {
            for (uint32_t m = 0; m < mips; ++m) {
                const uint32_t s = std::max(1u, c.size >> m);
                const size_t faceElems = size_t(s) * s * 4u;
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
        return makeTexture(td, init); // copies the data before `halves` goes away
    }

    Result<void> makeIbl() {
        // Deterministic CPU IBL (Ibl.h).
        const ibl::Cube env = ibl::environmentCube(kEnvSize);
        const ibl::Cube specular = ibl::prefilterGgx(env, kEnvMips);
        const ibl::Cube irr = ibl::irradiance(env, kIrradianceSize);
        const std::vector<float> lut = ibl::brdfLut(kLutSize, kLutSamples);

        auto specTex = cubeTexture(specular, "pbr-spheres specular env");
        if (!specTex) {
            return std::unexpected(specTex.error());
        }
        auto irrTex = cubeTexture(irr, "pbr-spheres irradiance env");
        if (!irrTex) {
            return std::unexpected(irrTex.error());
        }
        const std::vector<uint16_t> lutHalf = toHalf(lut);
        const SubresourceData lutInit{std::as_bytes(std::span(lutHalf)), 0};
        TextureDesc ld;
        ld.width = ld.height = kLutSize;
        ld.format = Format::RGBA16_FLOAT;
        ld.usage = TextureUsage::Sampled;
        ld.debugName = "pbr-spheres BRDF LUT";
        auto lutTex = makeTexture(ld, {&lutInit, 1});
        if (!lutTex) {
            return std::unexpected(lutTex.error());
        }

        SamplerDesc envSd; // trilinear, clamp (cube sampling is seamless on both backends)
        envSd.addressU = envSd.addressV = envSd.addressW = AddressMode::ClampToEdge;
        auto envSmp = dev_.createSampler(envSd);
        if (!envSmp) {
            return std::unexpected(envSmp.error());
        }
        own_.keep(*envSmp);
        SamplerDesc lutSd = envSd;
        lutSd.mipFilter = Filter::Nearest;
        auto lutSmp = dev_.createSampler(lutSd);
        if (!lutSmp) {
            return std::unexpected(lutSmp.error());
        }
        own_.keep(*lutSmp);
        frameResources_ = {{"specularEnvTexture", *specTex}, {"irradianceEnvTexture", *irrTex},
                           {"brdfLutTexture", *lutTex},      {"envSampler", *envSmp},
                           {"brdfLutSampler", *lutSmp},      {"environmentTexture", *specTex},
                           {"environmentSampler", *envSmp}};
        return {};
    }

    Result<void> makeGeometry() {
        std::vector<PbrVertex> vertices;
        std::vector<uint16_t> indices;
        sphereGeometry(vertices, indices);
        indexCount_ = static_cast<uint32_t>(indices.size());
        BufferDesc vbd;
        vbd.size = vertices.size() * sizeof(PbrVertex);
        vbd.usage = BufferUsage::Vertex;
        vbd.debugName = "pbr-spheres sphere vertices";
        auto vb = dev_.createBuffer(vbd, std::as_bytes(std::span(vertices)));
        if (!vb) {
            return std::unexpected(vb.error());
        }
        vb_ = own_.keep(*vb);
        BufferDesc ibd;
        ibd.size = indices.size() * sizeof(uint16_t);
        ibd.usage = BufferUsage::Index;
        ibd.debugName = "pbr-spheres sphere indices";
        auto ib = dev_.createBuffer(ibd, std::as_bytes(std::span(indices)));
        if (!ib) {
            return std::unexpected(ib.error());
        }
        ib_ = own_.keep(*ib);
        return {};
    }

    Result<BufferBinding> makeUniform(const void* data, size_t bytes, const char* name) {
        BufferDesc bd;
        bd.size = bytes;
        bd.usage = BufferUsage::Uniform;
        bd.debugName = name;
        auto b = dev_.createBuffer(bd, std::span(static_cast<const std::byte*>(data), bytes));
        if (!b) {
            return std::unexpected(b.error());
        }
        own_.keep(*b);
        BufferBinding bb;
        bb.buffer = *b;
        bb.size = bytes;
        return bb;
    }

    Result<void> makeSpheres() {
        // Missing glTF textures = 1x1 white defaults (bindings.hlsli); the normal map is off (materialFlags.z = 0).
        const uint8_t white[4] = {255, 255, 255, 255};
        const SubresourceData whiteInit{std::as_bytes(std::span(white)), 0};
        TextureDesc wd;
        wd.format = Format::RGBA8_UNORM;
        wd.usage = TextureUsage::Sampled;
        wd.debugName = "pbr-spheres white 1x1";
        auto whiteTex = makeTexture(wd, {&whiteInit, 1});
        if (!whiteTex) {
            return std::unexpected(whiteTex.error());
        }
        auto matSmp = dev_.createSampler(SamplerDesc{});
        if (!matSmp) {
            return std::unexpected(matSmp.error());
        }
        own_.keep(*matSmp);
        std::map<std::string, Resource> base;
        for (const char* t : {"baseColor", "metallicRoughness", "normal", "occlusion", "emissive"}) {
            base[std::string(t) + "Texture"] = *whiteTex;
            base[std::string(t) + "Sampler"] = *matSmp;
        }

        for (int row = 0; row <= kUnlitRow; ++row) {
            for (int col = 0; col < kCols; ++col) {
                const bool unlit = row == kUnlitRow;
                MaterialCB m{};
                if (unlit) {
                    for (int c = 0; c < 3; ++c) {
                        m.baseColorFactor[c] = kPbrUnlitColors[col][c];
                    }
                    m.baseColorFactor[3] = 1.0f;
                } else {
                    std::memcpy(m.baseColorFactor, kPbrBaseColor, sizeof(m.baseColorFactor));
                }
                m.pbrParams[0] = unlit ? 0.0f : static_cast<float>(col) / static_cast<float>(kCols - 1); // metallic
                m.pbrParams[1] = unlit ? 1.0f : static_cast<float>(row) / static_cast<float>(kRows - 1); // roughness
                m.pbrParams[2] = 1.0f; // normal scale
                m.pbrParams[3] = 1.0f; // occlusion strength
                m.alphaParams[0] = 0.5f;
                for (int slot = 0; slot < 5; ++slot) { // identity KHR_texture_transform
                    m.uvTransform[2 * slot][0] = 1.0f;
                    m.uvTransform[2 * slot + 1][1] = 1.0f;
                }
                ObjectCB o{};
                float c[3];
                sphereCentre(col, row, c);
                const Mat4 world = translation(c[0], c[1], c[2]);
                std::memcpy(o.world, world.m.data(), sizeof(o.world));
                const Mat4 identity = Mat4::identity(); // inverse-transpose of a translation (used with w = 0)
                std::memcpy(o.normalMatrix, identity.m.data(), sizeof(o.normalMatrix));

                auto mb = makeUniform(&m, sizeof(m), "pbr-spheres material");
                if (!mb) {
                    return std::unexpected(mb.error());
                }
                auto ob = makeUniform(&o, sizeof(o), "pbr-spheres object");
                if (!ob) {
                    return std::unexpected(ob.error());
                }
                const PipelineInfo& pi = unlit ? unlit_ : lit_;
                std::map<std::string, Resource> matRes = base;
                matRes["Material"] = *mb;
                auto mg = makeGroup(dev_, pi, 2, matRes);
                if (!mg) {
                    return std::unexpected(mg.error());
                }
                auto og = makeGroup(dev_, pi, 3, {{"Object", *ob}});
                if (!og) {
                    return std::unexpected(og.error());
                }
                if (!*mg || !*og) {
                    return std::unexpected(Error{kExitUsage, "builtin:pbr-spheres: pbr pipeline has no space2 / space3"});
                }
                own_.keep(*mg);
                own_.keep(*og);
                spheres_.push_back({unlit, *mg, *og});
            }
        }
        return {};
    }

    IDevice& dev_;
    Owned own_;
    SceneDesc desc_;
    PipelineInfo lit_, unlit_, sky_;
    std::map<std::string, Resource> frameResources_;
    BufferHandle vb_, ib_;
    uint32_t indexCount_ = 0;
    std::vector<Sphere> spheres_;
    TextureHandle depth_;
    ResourceState depthState_ = ResourceState::Undefined;
};

} // namespace

Result<std::unique_ptr<Scene>> createPbrSpheresScene(IDevice& dev, const SceneDesc& desc) {
    if (desc.colorFormat != Format::RGBA8_UNORM && desc.colorFormat != Format::BGRA8_UNORM &&
        desc.colorFormat != Format::RGBA8_SRGB && desc.colorFormat != Format::BGRA8_SRGB) {
        return std::unexpected(Error{kExitUsage, "builtin:pbr-spheres: target must be an RGBA8 / BGRA8 format"});
    }
    auto s = std::make_unique<PbrSpheresScene>(dev, desc);
    if (auto r = s->init(); !r) {
        return std::unexpected(r.error());
    }
    return std::unique_ptr<Scene>(std::move(s));
}

} // namespace eng::scenes
