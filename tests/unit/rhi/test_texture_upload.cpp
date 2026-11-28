// glTF image upload (plan F3 Task 3, Review Focus 3): engine/renderer/TextureUpload on the RHI backends.
//
// Suites:
//   rhi.device  (vulkan, d3d11, d3d11-warp)
//     - TextureLinearInterpolationTest: its 2 x 1 emissive texture (sRGB by use) sampled midway, bilinear, gives 0.5
//       linear (interpolation after the sRGB decode, the asset's purpose);
//     - the crafted colour-space probe (tests/assets/f03/colorspace, scripts/make_f03_assets.py): the same design with a
//       second texel of 188, used as emissive (sRGB) and as occlusion (linear); each passes with its view, and the
//       swapped view (the mutation, committed here as a negative check) misses the probe by > 20 / 255;
//     - KTX2 (KHR_texture_basisu): FlightHelmet's base colour (sRGB DFD) is uploaded as BC7_SRGB, its normal map as
//       BC7_UNORM; a mip level read back through point sampling equals the CPU (bcdec) decode of the same transcode;
//     - formats the device lacks (ETC2 / ASTC on a desktop GPU, no DXGI format on D3D11) are refused (exit 2).
//   rhi.t0      (d3d11-fl10_0, d3d11-warp-fl10_0): no BC7 at FL10_0 -> the KTX2 images go to BC1 / BC3 (*_SRGB for the
//               sRGB DFD) and upload with validation 0.
// Expected values come from the assets' design (texel values, the sRGB curve), never from a render.
#include "RhiTestEnv.h"

#include "asset/GltfLoader.h"
#include "asset/Ktx2.h"
#include "renderer/TextureUpload.h"
#include "rhi/ShaderLibrary.h"

#include <doctest/doctest.h>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include <bcdec.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

using namespace eng;
using namespace eng::rhi;
using namespace eng::rhi::test;

namespace {

std::filesystem::path f03Assets() {
    return assetDir() / "f03";
}
std::filesystem::path sampleModel(const char* model, const char* variant) {
    const std::filesystem::path dir = assetDir() / "glTF-Sample-Assets" / "Models" / model / variant;
    return dir / (std::string(model) + ".gltf");
}

asset::ModelData loadModel(const std::filesystem::path& p) {
    auto m = asset::loadGltf(p, asset::LoadOptions{}, nullptr);
    REQUIRE_MESSAGE(m.has_value(), (m ? std::string() : m.error().message));
    return std::move(*m);
}

double srgbToLinear(double v) {
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

// Draws a full-target quad sampling `tex` with the shaders/test/textured.hlsl pipeline into a `w` x `h` target of
// `target` format and reads it back. UV (u0, v0) at the top-left corner, (u1, v1) at the bottom-right; u0 == u1 and
// v0 == v1 = every pixel samples the same point.
struct Probe {
    IDevice& dev;
    ShaderHandle vs, ps;
    std::vector<Handle> own;
    explicit Probe(IDevice& d) : dev(d) {
        auto vb = ShaderLibrary::load(shaderLibDir(), "textured", "VSMain", d.caps().backend, "default");
        auto pb = ShaderLibrary::load(shaderLibDir(), "textured", "PSMain", d.caps().backend, "default");
        REQUIRE(vb.has_value());
        REQUIRE(pb.has_value());
        auto v = d.createShader(*vb);
        auto p = d.createShader(*pb);
        REQUIRE(v.has_value());
        REQUIRE(p.has_value());
        vs = *v;
        ps = *p;
        own = {vs, ps};
    }
    ~Probe() {
        for (auto it = own.rbegin(); it != own.rend(); ++it) {
            dev.destroy(*it);
        }
    }
    Image render(TextureHandle tex, const SamplerDesc& sd, Format target, uint32_t w, uint32_t h, float u0, float v0,
                 float u1, float v1) {
        struct V {
            float pos[3];
            float uv[2];
        };
        // Two clockwise triangles (framebuffer space), culling off.
        const V quad[6] = {{{-1, 1, 0.5f}, {u0, v0}}, {{1, 1, 0.5f}, {u1, v0}}, {{1, -1, 0.5f}, {u1, v1}},
                           {{-1, 1, 0.5f}, {u0, v0}}, {{1, -1, 0.5f}, {u1, v1}}, {{-1, -1, 0.5f}, {u0, v1}}};
        BufferDesc bd;
        bd.size = sizeof(quad);
        bd.usage = BufferUsage::Vertex;
        auto vb = dev.createBuffer(bd, std::as_bytes(std::span(quad)));
        REQUIRE(vb.has_value());
        auto smp = dev.createSampler(sd);
        REQUIRE(smp.has_value());
        GraphicsPipelineDesc pd;
        pd.vs = vs;
        pd.ps = ps;
        pd.vertexLayout.buffers = {{sizeof(V), false}};
        pd.vertexLayout.attributes = {{"POSITION", Format::RGB32_FLOAT, 0, offsetof(V, pos)},
                                      {"TEXCOORD0", Format::RG32_FLOAT, 0, offsetof(V, uv)}};
        pd.colorFormats = {target};
        auto pipe = dev.createGraphicsPipeline(pd);
        REQUIRE_MESSAGE(pipe.has_value(), (pipe ? std::string() : pipe.error().message));
        BindGroupDesc mat;
        mat.pipeline = *pipe;
        mat.space = 2;
        mat.entries = {{"faceTexture", tex}, {"faceSampler", *smp}};
        auto group = dev.createBindGroup(mat);
        REQUIRE(group.has_value());
        TextureDesc td;
        td.width = w;
        td.height = h;
        td.format = target;
        td.usage = TextureUsage::RenderTarget | TextureUsage::CopySrc;
        auto rt = dev.createTexture(td);
        REQUIRE(rt.has_value());

        ICommandList& cmd = dev.beginFrame();
        const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const BufferBinding cb = dev.allocTransient(sizeof(identity));
        REQUIRE(cb.buffer);
        std::memcpy(cb.cpu, identity, sizeof(identity));
        BindGroupDesc drw;
        drw.pipeline = *pipe;
        drw.space = 3;
        drw.entries = {{"Draw", cb}};
        auto draw = dev.createBindGroup(drw);
        REQUIRE(draw.has_value());
        const TextureBarrier toRt{*rt, ResourceState::Undefined, ResourceState::RenderTarget};
        cmd.barrier({&toRt, 1});
        RenderPassDesc rp;
        ColorAttachment ca;
        ca.texture = *rt;
        rp.colors.push_back(ca);
        cmd.beginRenderPass(rp);
        cmd.setPipeline(*pipe);
        cmd.setBindGroup(2, *group);
        cmd.setBindGroup(3, *draw);
        cmd.setVertexBuffer(0, *vb, 0);
        cmd.setViewport({0, 0, float(w), float(h), 0, 1});
        cmd.setScissor({0, 0, w, h});
        cmd.draw(6, 1, 0, 0);
        cmd.endRenderPass();
        const TextureBarrier toCopy{*rt, ResourceState::RenderTarget, ResourceState::CopySrc};
        cmd.barrier({&toCopy, 1});
        auto sub = dev.submitAndWait();
        REQUIRE_MESSAGE(sub.has_value(), (sub ? std::string() : sub.error().message));
        auto img = dev.readback(*rt);
        REQUIRE(img.has_value());
        for (Handle hnd : {Handle(*draw), Handle(*group), Handle(*rt), Handle(*pipe), Handle(*smp), Handle(*vb)}) {
            dev.destroy(hnd);
        }
        return std::move(*img);
    }
};

// Samples the 2 x 1 texture of `image` midway (u = 0.5: between the two texel centres), bilinear, mip 0, into an
// RGBA8_UNORM target (no output encode: the bytes are the linear value the shader got). Returns the green byte.
int midpointGreen(IDevice& dev, Probe& probe, const asset::Image& image, bool srgbView, bool swapView) {
    auto data = renderer::prepareTexture(image, dev.caps(), srgbView);
    REQUIRE(data.has_value());
    REQUIRE((data->desc.format == (srgbView ? Format::RGBA8_SRGB : Format::RGBA8_UNORM)));
    if (swapView) { // the Review Focus 3 mutation: the other colour space for the same bytes
        data->desc.format = srgbView ? Format::RGBA8_UNORM : Format::RGBA8_SRGB;
    }
    auto tex = renderer::createTexture(dev, *data);
    REQUIRE(tex.has_value());
    SamplerDesc sd;
    sd.addressU = sd.addressV = AddressMode::ClampToEdge;
    sd.mipFilter = Filter::Nearest;
    const Image out = probe.render(*tex, sd, Format::RGBA8_UNORM, 4, 4, 0.5f, 0.5f, 0.5f, 0.5f);
    dev.destroy(*tex);
    REQUIRE(out.rgba8.size() == 64);
    return out.rgba8[(1 * 4 + 1) * 4 + 1];
}

std::unique_ptr<IDevice> device() {
    auto dev = createDevice(currentBackend().desc);
    REQUIRE(dev.has_value());
    return std::move(*dev);
}

// CPU decode (bcdec) of one level -> RGBA8 rows.
std::vector<uint8_t> cpuDecode(const std::vector<uint8_t>& blocks, Format f, uint32_t w, uint32_t h) {
    std::vector<uint8_t> out(size_t(w) * h * 4);
    const uint32_t bx = (w + 3) / 4, by = (h + 3) / 4, bytes = formatBytes(f);
    REQUIRE(blocks.size() == size_t(bx) * by * bytes);
    uint8_t block[64];
    for (uint32_t y = 0; y < by; ++y) {
        for (uint32_t x = 0; x < bx; ++x) {
            const uint8_t* src = blocks.data() + (size_t(y) * bx + x) * bytes;
            if (f == Format::BC7_SRGB || f == Format::BC7_UNORM) {
                bcdec_bc7(src, block, 16);
            } else if (f == Format::BC3_SRGB || f == Format::BC3_UNORM) {
                bcdec_bc3(src, block, 16);
            } else if (f == Format::BC1_SRGB || f == Format::BC1_UNORM) {
                bcdec_bc1(src, block, 16);
            } else {
                REQUIRE_MESSAGE(false, "no CPU decoder for " << toString(f));
            }
            for (uint32_t r = 0; r < 4 && y * 4 + r < h; ++r) {
                for (uint32_t c = 0; c < 4 && x * 4 + c < w; ++c) {
                    std::memcpy(&out[((size_t(y) * 4 + r) * w + x * 4 + c) * 4], &block[(r * 4 + c) * 4], 4);
                }
            }
        }
    }
    return out;
}

} // namespace

TEST_SUITE("rhi.device") {

TEST_CASE("Review Focus 3: TextureLinearInterpolationTest emissive midpoint = 0.5 linear (sRGB decode before filtering)") {
    auto dev = device();
    Probe probe(*dev);
    const asset::ModelData m = loadModel(sampleModel("TextureLinearInterpolationTest", "glTF"));
    REQUIRE(m.materials.size() >= 2);
    const asset::TextureRef& em = m.materials[1].emissive; // material 1: emissiveTexture 0 (0_0_0-0_255_0.png)
    REQUIRE(em.valid());
    const asset::Image& img = m.images[em.image];
    CHECK(img.colorSpace == asset::ColorSpace::Srgb); // emissive = sRGB by use
    REQUIRE(img.width == 2);
    REQUIRE(img.height == 1);
    const int g = midpointGreen(*dev, probe, img, true, false);
    CHECK(std::abs(g - 128) <= 1); // 0.5 linear = the JSON-only sphere's emissive 0.5
    // Measured, recorded: texels 0 and 255 are fixed points of the sRGB curve, so the swapped (UNORM) view also gives
    // 0.5 here; this asset cannot see a swap (the crafted probe below can).
    const int swapped = midpointGreen(*dev, probe, img, true, true);
    MESSAGE("TextureLinearInterpolationTest midpoint: sRGB view " << g << ", swapped (UNORM) view " << swapped);
    CHECK(std::abs(swapped - 128) <= 1);
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("Review Focus 3: crafted colour-space probe passes with the use's view, the swapped view fails it") {
    auto dev = device();
    Probe probe(*dev);
    const asset::ModelData m = loadModel(f03Assets() / "colorspace" / "linear_interpolation_crafted.gltf");
    REQUIRE(m.materials.size() == 2);
    const asset::Image& emissive = m.images[m.materials[0].emissive.image];
    const asset::Image& occlusion = m.images[m.materials[1].occlusion.image];
    CHECK(emissive.colorSpace == asset::ColorSpace::Srgb);
    CHECK(occlusion.colorSpace == asset::ColorSpace::Linear);
    // Texels (0, 0, 0) and (0, 188, 0) (scripts/make_f03_assets.py), sampled midway.
    const int srgbExpected = static_cast<int>(std::lround(0.5 * srgbToLinear(188.0 / 255.0) * 255.0)); // 64
    const int linearExpected = 94;                                                                     // 188 / 2
    const int e = midpointGreen(*dev, probe, emissive, true, false);
    const int o = midpointGreen(*dev, probe, occlusion, false, false);
    CHECK(std::abs(e - srgbExpected) <= 2);
    CHECK(std::abs(o - linearExpected) <= 2);
    // The mutation: the swapped view must fail the same probe (by far more than its tolerance).
    const int eSwapped = midpointGreen(*dev, probe, emissive, true, true);
    const int oSwapped = midpointGreen(*dev, probe, occlusion, false, true);
    MESSAGE("crafted probe: emissive " << e << " (swapped " << eSwapped << ", expected " << srgbExpected << "), occlusion "
                                       << o << " (swapped " << oSwapped << ", expected " << linearExpected << ")");
    CHECK(std::abs(eSwapped - srgbExpected) > 20);
    CHECK(std::abs(oSwapped - linearExpected) > 20);
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("KTX2: sRGB DFD -> BC7_SRGB, linear -> BC7_UNORM; a mip level reads back as the CPU decode") {
    auto dev = device();
    REQUIRE(dev->caps().bc6h7); // RTX 3070 Ti / WARP at FL11: BC7 sampled
    Probe probe(*dev);
    const asset::ModelData m = loadModel(sampleModel("FlightHelmet", "glTF-KTX-BasisU"));
    for (const asset::Image& img : m.images) {
        const bool base = img.uri.find("LeatherPartsMat_BaseColor") != std::string::npos;
        const bool normal = img.uri.find("LeatherPartsMat_Normal") != std::string::npos;
        if (!base && !normal) {
            continue;
        }
        CAPTURE(img.uri);
        auto data = renderer::prepareTexture(img, dev->caps(), /*ignored for KTX2*/ !base);
        REQUIRE(data.has_value());
        CHECK((data->desc.format == (base ? Format::BC7_SRGB : Format::BC7_UNORM)));
        REQUIRE(data->desc.mipLevels == 12);
        auto tex = renderer::createTexture(*dev, *data);
        REQUIRE_MESSAGE(tex.has_value(), (tex ? std::string() : tex.error().message));
        // Level 5 (64 x 64) of the 2048 x 2048 texture: a 64 x 64 target with UV 0..1 samples it texel for texel
        // (LOD log2(2048 / 64) = 5, point filtering, nearest mip). An sRGB texture goes to an sRGB target, so the
        // read-back bytes are the stored sRGB bytes again (decode + encode), like the CPU decode.
        const uint32_t level = 5, w = 64, h = 64;
        SamplerDesc sd;
        sd.minFilter = sd.magFilter = sd.mipFilter = Filter::Nearest;
        sd.addressU = sd.addressV = AddressMode::ClampToEdge;
        const Image out =
            probe.render(*tex, sd, base ? Format::RGBA8_SRGB : Format::RGBA8_UNORM, w, h, 0.0f, 0.0f, 1.0f, 1.0f);
        const std::vector<uint8_t> cpu = cpuDecode(data->levels[level], data->desc.format, w, h);
        int worst = 0;
        for (size_t i = 0; i < cpu.size(); i += 4) {
            for (int c = 0; c < 3; ++c) {
                worst = std::max(worst, std::abs(int(cpu[i + c]) - int(out.rgba8[i + c])));
            }
        }
        MESSAGE(img.uri << ": level 5 GPU vs CPU decode, worst channel difference " << worst);
        CHECK(worst <= 1);
        dev->destroy(*tex);
    }
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("formats the device lacks are refused, not created") {
    auto dev = device();
    const DeviceCaps& c = dev->caps();
    for (const Format f : {Format::ETC2_RGBA8_UNORM, Format::ASTC_4x4_SRGB}) {
        if (c.format(f).sample) {
            continue; // supported here: nothing to refuse
        }
        CAPTURE(toString(f));
        TextureDesc td;
        td.width = td.height = 4;
        td.format = f;
        std::vector<uint8_t> block(16, 0);
        const SubresourceData sub{std::as_bytes(std::span(block)), 0};
        auto t = dev->createTexture(td, {&sub, 1});
        REQUIRE_FALSE(t.has_value());
        CHECK(t.error().exitCode == 2);
    }
    CHECK(dev->validationErrorCount() == 0);
}

} // TEST_SUITE

TEST_SUITE("rhi.t0") {

TEST_CASE("KTX2 at FL10_0: no BC7 -> BC1 / BC3, *_SRGB for the sRGB DFD; uploads with validation 0") {
    auto dev = device();
    REQUIRE(dev->caps().tier == 0);
    REQUIRE_FALSE(dev->caps().bc6h7);
    const asset::ModelData m = loadModel(sampleModel("FlightHelmet", "glTF-KTX-BasisU"));
    size_t n = 0;
    for (const asset::Image& img : m.images) {
        if (img.uri.find("LeatherPartsMat") == std::string::npos) {
            continue;
        }
        CAPTURE(img.uri);
        auto data = renderer::prepareTexture(img, dev->caps(), true);
        REQUIRE(data.has_value());
        const Format f = data->desc.format;
        if (img.ktx2Srgb) {
            CHECK((f == Format::BC1_SRGB || f == Format::BC3_SRGB));
        } else {
            CHECK((f == Format::BC1_UNORM || f == Format::BC3_UNORM));
        }
        auto tex = renderer::createTexture(*dev, *data);
        REQUIRE_MESSAGE(tex.has_value(), (tex ? std::string() : tex.error().message));
        dev->destroy(*tex);
        ++n;
    }
    CHECK(n == 3);
    CHECK(dev->validationErrorCount() == 0);
}

} // TEST_SUITE
