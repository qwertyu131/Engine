// Bind groups (plan F2 Task 3): layouts derived from the shaders' reflection, entries matched by
// reflection name (never by index or backend slot), exact error messages for a missing / unknown entry.
#include "rhi/Rhi.h"
#include "rhi/ShaderLibrary.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>

#include "RhiTestEnv.h"

using namespace eng::rhi;
using eng::rhi::test::currentBackend;
using eng::rhi::test::shaderLibDir;

namespace {

ShaderReflection reflectionOf(Backend b, const char* entry) {
    auto blob = ShaderLibrary::load(shaderLibDir(), "textured", entry, b, "default");
    REQUIRE_MESSAGE(blob.has_value(), (blob ? std::string() : blob.error().message));
    return blob->reflection;
}

const BindGroupLayoutEntry* findEntry(const BindGroupLayout& l, const std::string& name) {
    const auto it = std::find_if(l.entries.begin(), l.entries.end(), [&](const auto& e) { return e.name == name; });
    return it == l.entries.end() ? nullptr : &*it;
}

std::unique_ptr<IDevice> makeDevice() {
    auto dev = createDevice(currentBackend().desc);
    REQUIRE_MESSAGE(dev.has_value(), (dev ? std::string() : dev.error().message));
    return std::move(*dev);
}

PipelineHandle makeTexturedPipeline(IDevice& dev) {
    const Backend b = dev.caps().backend;
    auto vsBlob = ShaderLibrary::load(shaderLibDir(), "textured", "VSMain", b, "default");
    auto psBlob = ShaderLibrary::load(shaderLibDir(), "textured", "PSMain", b, "default");
    REQUIRE(vsBlob.has_value());
    REQUIRE(psBlob.has_value());
    auto vs = dev.createShader(*vsBlob);
    auto ps = dev.createShader(*psBlob);
    REQUIRE(vs.has_value());
    REQUIRE(ps.has_value());
    GraphicsPipelineDesc pd;
    pd.vs = *vs;
    pd.ps = *ps;
    pd.vertexLayout.buffers = {{20, false}};
    pd.vertexLayout.attributes = {{"POSITION", Format::RGB32_FLOAT, 0, 0}, {"TEXCOORD0", Format::RG32_FLOAT, 0, 12}};
    pd.colorFormats = {Format::RGBA8_SRGB};
    pd.depthFormat = Format::D32_FLOAT;
    auto p = dev.createGraphicsPipeline(pd);
    REQUIRE_MESSAGE(p.has_value(), (p ? std::string() : p.error().message));
    return *p;
}

} // namespace

TEST_SUITE("rhi.layout") {

TEST_CASE("bind group layouts: one per used space, merged across stages, per-stage target bindings") {
    for (const Backend b : {Backend::Vulkan, Backend::D3D11}) {
        CAPTURE(toString(b));
        const ShaderReflection vs = reflectionOf(b, "VSMain");
        const ShaderReflection ps = reflectionOf(b, "PSMain");
        auto layouts = deriveBindGroupLayouts(vs, ps);
        REQUIRE_MESSAGE(layouts.has_value(), (layouts ? std::string() : layouts.error().message));
        REQUIRE(layouts->size() == 2);
        const BindGroupLayout& material = (*layouts)[0];
        const BindGroupLayout& draw = (*layouts)[1];
        CHECK(material.space == 2);
        CHECK(draw.space == 3);

        const BindGroupLayoutEntry* tex = findEntry(material, "faceTexture");
        const BindGroupLayoutEntry* smp = findEntry(material, "faceSampler");
        const BindGroupLayoutEntry* ubo = findEntry(draw, "Draw");
        REQUIRE(tex);
        REQUIRE(smp);
        REQUIRE(ubo);
        CHECK(tex->kind == BindingKind::Texture);
        CHECK(smp->kind == BindingKind::Sampler);
        CHECK(ubo->kind == BindingKind::UniformBuffer);
        CHECK(ubo->sizeBytes == 64);
        // Visibility: the texture / sampler only in the PS, the draw constants only in the VS.
        CHECK_FALSE(tex->stages[kStageVertex].used);
        CHECK(tex->stages[kStageFragment].used);
        CHECK(ubo->stages[kStageVertex].used);
        CHECK_FALSE(ubo->stages[kStageFragment].used);
        if (b == Backend::Vulkan) {
            CHECK(tex->stages[kStageFragment].set == 2);
            CHECK(tex->stages[kStageFragment].binding == 0);
            CHECK(smp->stages[kStageFragment].binding == 1);
            CHECK(ubo->stages[kStageVertex].set == 3);
        } else {
            // D3D11: per-stage flattened registers from each stage's reflection (F1).
            CHECK(tex->stages[kStageFragment].registerClass == 't');
            CHECK(tex->stages[kStageFragment].registerSlot == 0);
            CHECK(smp->stages[kStageFragment].registerClass == 's');
            CHECK(smp->stages[kStageFragment].registerSlot == 0);
            CHECK(ubo->stages[kStageVertex].registerClass == 'b');
            CHECK(ubo->stages[kStageVertex].registerSlot == 0);
        }
    }
}

TEST_CASE("bind group entries: missing / unknown name / wrong kind messages (no device)") {
    const ShaderReflection vs = reflectionOf(Backend::Vulkan, "VSMain");
    const ShaderReflection ps = reflectionOf(Backend::Vulkan, "PSMain");
    auto layouts = deriveBindGroupLayouts(vs, ps);
    REQUIRE(layouts.has_value());
    const BindGroupLayout& material = (*layouts)[0];

    BindGroupDesc missing;
    missing.space = 2;
    missing.entries = {{"faceTexture", TextureHandle{1}}};
    auto m = matchBindGroupEntries(material, missing);
    REQUIRE_FALSE(m.has_value());
    CHECK(m.error().exitCode == 2);
    CHECK(m.error().message == "bind group space 2 missing faceSampler");

    BindGroupDesc unknown;
    unknown.space = 2;
    unknown.entries = {{"faceTexture", TextureHandle{1}}, {"faceSampler", SamplerHandle{1}}, {"albedo", TextureHandle{2}}};
    auto u = matchBindGroupEntries(material, unknown);
    REQUIRE_FALSE(u.has_value());
    CHECK(u.error().exitCode == 2);
    CHECK(u.error().message == "bind group space 2 has no resource albedo");

    BindGroupDesc wrongKind;
    wrongKind.space = 2;
    wrongKind.entries = {{"faceTexture", SamplerHandle{1}}, {"faceSampler", SamplerHandle{1}}};
    auto k = matchBindGroupEntries(material, wrongKind);
    REQUIRE_FALSE(k.has_value());
    CHECK(k.error().message == "bind group space 2 entry faceTexture must be a texture");

    BindGroupDesc ok;
    ok.space = 2;
    ok.entries = {{"faceSampler", SamplerHandle{1}}, {"faceTexture", TextureHandle{1}}}; // any order
    auto good = matchBindGroupEntries(material, ok);
    REQUIRE(good.has_value());
    REQUIRE(good->size() == 2);
    CHECK(material.entries[0].name == ok.entries[(*good)[0]].name);
    CHECK(material.entries[1].name == ok.entries[(*good)[1]].name);
}

} // TEST_SUITE

TEST_SUITE("rhi.vulkan") {

TEST_CASE("createBindGroup: missing / unknown entry -> Error{2, exact message}") {
    auto dev = makeDevice();
    const PipelineHandle pipe = makeTexturedPipeline(*dev);

    BindGroupDesc missing;
    missing.pipeline = pipe;
    missing.space = 3;
    auto m = dev->createBindGroup(missing);
    REQUIRE_FALSE(m.has_value());
    CHECK(m.error().exitCode == 2);
    CHECK(m.error().message == "bind group space 3 missing Draw");

    BufferDesc bd;
    bd.size = 256;
    bd.usage = BufferUsage::Uniform;
    auto ubo = dev->createBuffer(bd);
    REQUIRE(ubo.has_value());
    BindGroupDesc unknown;
    unknown.pipeline = pipe;
    unknown.space = 3;
    unknown.entries = {{"Draw", BufferBinding{*ubo}}, {"Frame", BufferBinding{*ubo}}};
    auto u = dev->createBindGroup(unknown);
    REQUIRE_FALSE(u.has_value());
    CHECK(u.error().exitCode == 2);
    CHECK(u.error().message == "bind group space 3 has no resource Frame");
    CHECK(dev->validationErrorCount() == 0);
}

} // TEST_SUITE
