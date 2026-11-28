// DeviceCaps measurement + the T0 tier (plan F2 Task 7; spec §6.1 / §6.2; Review Focus 4).
//
// Suites:
//   rhi.caps    backend-independent: the field list behind --dump-caps and the viewer panel, the JSON shape,
//               the Vulkan required-feature rule (a device without it exits 3 with the feature named);
//   rhi.device  every backend configuration: the measured caps are consistent with the backend / tier;
//   rhi.t0      D3D11 capped at FL10_0 (ctest unit.rhi.d3d11-fl10_0 on the GPU and unit.rhi.d3d11-warp-fl10_0
//               on WARP; DeviceDesc::d3d11FeatureLevelCap = 0xa000):
//                 - tier 0, FL10_0 limits from the SDK constants, computeShaders = the device's own
//                   D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS answer (queried here independently);
//                 - a T1 (SM 5.0) PBR pipeline is rejected at creation with "pipeline needs TIER 1, device is
//                   TIER 0" (exit 3) and no debug-layer message (the SM 5.0 blob never reaches the driver);
//                 - builtin:triangle (color_triangle "default-t0", SM 4.0) and builtin:pbr-spheres (dxbc
//                   T0_SKIN0_MORPH0_UNLIT0 + UNLIT1, skybox "default-t0") render with the absolute probes of
//                   the FL11 suites and validation 0;
//                 - pbr-spheres T0 vs the same adapter at FL11 (T1 shaders): mean FLIP <= 0.05. This is a K-C
//                   regression gate between T0 and T1 shader bodies (SM 4.0 vs SM 5.0 compiles of the same
//                   HLSL), not a reference gate.
// The FL10_0 numbers are API limits of the RTX 3070 Ti driver and of WARP at a capped feature level, not
// measurements of real FL10 hardware.
#include "Compare.h"
#include "Scenes.h"

#include "rhi/Rhi.h"
#include "rhi/ShaderLibrary.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <set>
#include <string>
#include <vector>

#include "RhiTestEnv.h"
#include "SceneProbes.h"

#if defined(ENG_RHI_HAS_VULKAN)
#include "rhi/vulkan/VkFeatureCheck.h"
#endif
#if defined(ENG_RHI_HAS_D3D11)
#include <d3d11.h>
#endif

using namespace eng::rhi;
using eng::rhi::test::assetDir;
using eng::rhi::test::currentBackend;
using eng::rhi::test::shaderLibDir;

namespace {

std::unique_ptr<IDevice> makeDevice(const DeviceDesc& desc) {
    auto dev = createDevice(desc);
    REQUIRE_MESSAGE(dev.has_value(), (dev ? std::string() : dev.error().message));
    return std::move(*dev);
}

std::vector<std::string> fieldNames(const DeviceCaps& caps) {
    std::vector<std::string> names;
    forEachCapsField(caps, [&](std::string_view n, const CapsValue&) { names.emplace_back(n); });
    return names;
}

eng::golden::Image toGolden(Image&& img) {
    eng::golden::Image g;
    g.width = static_cast<int>(img.width);
    g.height = static_cast<int>(img.height);
    g.channels = 4;
    g.rgba8 = std::move(img.rgba8);
    return g;
}

Image renderPbr(IDevice& dev) {
    eng::scenes::SceneDesc sd;
    sd.shaderDir = shaderLibDir();
    sd.assetDir = assetDir();
    sd.colorFormat = Format::RGBA8_UNORM;
    sd.width = 256;
    sd.height = 256;
    auto img = eng::scenes::renderSceneOffscreen(dev, eng::scenes::kPbrSpheres, sd);
    REQUIRE_MESSAGE(img.has_value(), (img ? std::string() : img.error().message));
    return std::move(*img);
}

ShaderHandle loadPbr(IDevice& dev, const char* entry, const char* key) {
    auto blob = ShaderLibrary::load(shaderLibDir(), "pbr", entry, Backend::D3D11, key);
    REQUIRE_MESSAGE(blob.has_value(), (blob ? std::string() : blob.error().message));
    auto s = dev.createShader(*blob);
    REQUIRE_MESSAGE(s.has_value(), (s ? std::string() : s.error().message));
    return *s;
}

GraphicsPipelineDesc pbrPipelineDesc(ShaderHandle vs, ShaderHandle ps) {
    GraphicsPipelineDesc pd;
    pd.vs = vs;
    pd.ps = ps;
    // Every VS input of pbr.hlsl (SKIN0 MORPH0): POSITION NORMAL TANGENT TEXCOORD0/1 COLOR0, one buffer.
    pd.vertexLayout.buffers = {{72, false}}; // apps/headless/PbrSpheresScene.cpp PbrVertex
    pd.vertexLayout.attributes = {{"POSITION", Format::RGB32_FLOAT, 0, 0},   {"NORMAL", Format::RGB32_FLOAT, 0, 12},
                                  {"TANGENT", Format::RGBA32_FLOAT, 0, 24},  {"TEXCOORD0", Format::RG32_FLOAT, 0, 40},
                                  {"TEXCOORD1", Format::RG32_FLOAT, 0, 48}, {"COLOR0", Format::RGBA32_FLOAT, 0, 56}};
    pd.colorFormats = {Format::RGBA8_UNORM};
    pd.depthFormat = Format::D32_FLOAT;
    return pd;
}

} // namespace

TEST_SUITE("rhi.caps") {

TEST_CASE("forEachCapsField lists every DeviceCaps field of plan F2 Task 7, once, in order") {
    DeviceCaps caps;
    caps.formatSupport = {{Format::RGBA8_UNORM, {}}, {Format::D32_FLOAT, {}}};
    const std::vector<std::string> names = fieldNames(caps);
    const std::vector<std::string> expected = {
        "backend", "tier", "apiVersion", "adapterName", "vendorId", "software", "maxTexture2D", "maxCubeSize",
        "maxColorAttachments", "maxVertexAttributes", "maxUniformBufferBytes", "maxBindingsPerStage.cbuffer",
        "maxBindingsPerStage.texture", "maxBindingsPerStage.sampler", "maxBindingsPerStage.storage", "computeShaders",
        "storageBuffers", "vertexStorageBuffers", "formatSupport.RGBA8_UNORM", "formatSupport.D32_FLOAT", "bc", "bc6h7",
        "etc2", "astc", "msaaSampleCounts", "float16", "waveOps", "rayTracing", "bindless", "timestampQueries"};
    CHECK(names == expected);
    CHECK(std::set<std::string>(names.begin(), names.end()).size() == names.size());
}

TEST_CASE("capsToJson: nested objects, typed values, note") {
    DeviceCaps caps;
    caps.backend = Backend::D3D11;
    caps.tier = 0;
    caps.apiVersion = "10_0";
    caps.adapterName = "Gerät ç";
    caps.maxTexture2D = 8192;
    caps.maxUniformBufferBytes = 65536;
    caps.maxBindingsPerStage = {14, 128, 16, 0};
    caps.computeShaders = true;
    FormatCaps rgba;
    rgba.sample = rgba.filter = rgba.render = rgba.blend = true;
    caps.formatSupport = {{Format::RGBA8_UNORM, rgba}};
    caps.msaaSampleCounts = 0b1101;
    const nlohmann::json j = nlohmann::json::parse(capsToJson(caps, "API limits, not real FL10 hardware"));
    CHECK(j.at("backend") == "d3d11");
    CHECK(j.at("tier") == 0);
    CHECK(j.at("apiVersion") == "10_0");
    CHECK(j.at("adapterName") == "Gerät ç");
    CHECK(j.at("maxTexture2D") == 8192);
    CHECK(j.at("maxUniformBufferBytes") == 65536);
    CHECK(j.at("maxBindingsPerStage") == nlohmann::json({{"cbuffer", 14}, {"texture", 128}, {"sampler", 16}, {"storage", 0}}));
    CHECK(j.at("computeShaders") == true);
    CHECK(j.at("formatSupport").at("RGBA8_UNORM") ==
          nlohmann::json({{"sample", true}, {"filter", true}, {"render", true}, {"blend", true}, {"depth", false},
                          {"vertex", false}}));
    CHECK(j.at("msaaSampleCounts") == 13);
    CHECK(j.at("note") == "API limits, not real FL10 hardware");
    CHECK_FALSE(nlohmann::json::parse(capsToJson(caps, "")).contains("note"));
    CHECK(toString(rgba) == "sample filter render blend");
    CHECK(toString(FormatCaps{}) == "-");
}

#if defined(ENG_RHI_HAS_VULKAN)
TEST_CASE("Vulkan: a device without a required feature is unusable, exit 3 with the feature named") {
    using eng::rhi::vk::PhysicalDeviceSupport;
    PhysicalDeviceSupport ok13{true, 1, 3, 0, true, false};
    CHECK(eng::rhi::vk::missingRequiredFeature(ok13).empty());
    PhysicalDeviceSupport ok12{true, 1, 2, 199, false, true};
    CHECK(eng::rhi::vk::missingRequiredFeature(ok12).empty());

    PhysicalDeviceSupport noDr13 = ok13;
    noDr13.dynamicRenderingFeature = false;
    CHECK(eng::rhi::vk::missingRequiredFeature(noDr13) == "dynamicRendering");
    PhysicalDeviceSupport noDr12 = ok12;
    noDr12.khrDynamicRenderingExtension = false;
    CHECK(eng::rhi::vk::missingRequiredFeature(noDr12) == "dynamicRendering (Vulkan 1.3 or VK_KHR_dynamic_rendering)");
    PhysicalDeviceSupport v11{true, 1, 1, 128, true, true};
    CHECK(eng::rhi::vk::missingRequiredFeature(v11) == "Vulkan 1.2 (device has 1.1.128)");
    PhysicalDeviceSupport noQueue = ok13;
    noQueue.graphicsQueue = false;
    CHECK(eng::rhi::vk::missingRequiredFeature(noQueue) == "graphics queue");

    const Error e = eng::rhi::vk::missingFeatureError("Test GPU", "dynamicRendering");
    CHECK(e.exitCode == 3);
    CHECK(e.message == "vulkan: device 'Test GPU' lacks required feature: dynamicRendering");
}
#endif

} // TEST_SUITE

TEST_SUITE("rhi.device") {

TEST_CASE("DeviceCaps: measured values are consistent with the backend and tier") {
    auto dev = makeDevice(currentBackend().desc);
    const DeviceCaps& c = dev->caps();
    MESSAGE(capsToJson(c, ""));
    REQUIRE(c.formatSupport.size() == allFormats().size());
    for (size_t i = 0; i < allFormats().size(); ++i) {
        CHECK((c.formatSupport[i].first == allFormats()[i]));
    }
    const FormatCaps& rgba = c.format(Format::RGBA8_UNORM);
    CHECK(rgba.sample);
    CHECK(rgba.filter);
    CHECK(rgba.render);
    CHECK(rgba.blend);
    CHECK_FALSE(rgba.depth);
    CHECK(c.format(Format::RGBA8_SRGB).render);
    CHECK(c.format(Format::RGBA16_FLOAT).filter);
    CHECK(c.format(Format::D32_FLOAT).depth);
    CHECK(c.format(Format::RGB32_FLOAT).vertex);
    CHECK(c.format(Format::RG32_FLOAT).vertex);
    CHECK((c.msaaSampleCounts & 1u) == 1u);
    CHECK(c.maxColorAttachments >= 4);
    CHECK(c.maxBindingsPerStage.sampler >= 16);
    CHECK(c.maxUniformBufferBytes >= 16384);
    CHECK(c.timestampQueries);
    if (c.backend == Backend::Vulkan) {
        CHECK(c.tier == 2);
        // Vulkan 1.2 required minimums.
        CHECK(c.maxTexture2D >= 4096);
        CHECK(c.maxCubeSize >= 4096);
        CHECK(c.maxVertexAttributes >= 16);
        CHECK(c.computeShaders);
        CHECK(c.storageBuffers);
        CHECK(c.vertexStorageBuffers);
        CHECK(c.maxBindingsPerStage.storage >= 4);
    } else {
        // D3D11 at FL11_x (the FL10_0 configurations run suite rhi.t0): D3D11_* SDK limits.
        CHECK(c.tier == 1);
        CHECK(c.apiVersion.rfind("11_", 0) == 0);
        CHECK(c.maxTexture2D == 16384);
        CHECK(c.maxCubeSize == 16384);
        CHECK(c.maxColorAttachments == 8);
        CHECK(c.maxVertexAttributes == 32);
        CHECK(c.maxUniformBufferBytes == 4096u * 16u);
        CHECK(c.maxBindingsPerStage.cbuffer == 14);
        CHECK(c.maxBindingsPerStage.texture == 128);
        CHECK(c.maxBindingsPerStage.storage == (c.apiVersion == "11_1" ? 64u : 8u));
        CHECK(c.computeShaders);
        CHECK(c.storageBuffers);
        CHECK(c.vertexStorageBuffers);
        CHECK(c.bc);
        CHECK(c.bc6h7);
        CHECK_FALSE(c.etc2);
        CHECK_FALSE(c.astc);
        CHECK_FALSE(c.waveOps);
        CHECK_FALSE(c.rayTracing);
        CHECK_FALSE(c.bindless);
    }
    CHECK(dev->validationErrorCount() == 0);
}

} // TEST_SUITE

#if defined(ENG_RHI_HAS_D3D11)
TEST_SUITE("rhi.t0") {

TEST_CASE("FL10_0: tier 0, FL10_0 limits, computeShaders = D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS") {
    REQUIRE(currentBackend().desc.d3d11FeatureLevelCap == 0xa000);
    auto dev = makeDevice(currentBackend().desc);
    const DeviceCaps& c = dev->caps();
    MESSAGE(capsToJson(c, ""));
    CHECK((c.backend == Backend::D3D11));
    CHECK(c.tier == 0);
    CHECK(c.apiVersion == "10_0");
    CHECK(c.software == currentBackend().desc.warp);
    auto* d3d = static_cast<ID3D11Device*>(dev->nativeHandles().d3d11Device);
    REQUIRE(d3d != nullptr);
    REQUIRE(d3d->GetFeatureLevel() == D3D_FEATURE_LEVEL_10_0);
    D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS hw{};
    REQUIRE(SUCCEEDED(d3d->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &hw, sizeof(hw))));
    CAPTURE(hw.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x);
    CHECK(c.computeShaders == (hw.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x != FALSE));
    // D3D10_* constants of the Windows SDK d3d10.h (FL10_0).
    CHECK(c.maxTexture2D == 8192);
    CHECK(c.maxCubeSize == 8192);
    CHECK(c.maxColorAttachments == 8);
    CHECK(c.maxVertexAttributes == 16);
    CHECK(c.maxUniformBufferBytes == 4096u * 16u);
    CHECK(c.maxBindingsPerStage.cbuffer == 14);
    CHECK(c.maxBindingsPerStage.texture == 128);
    CHECK(c.maxBindingsPerStage.sampler == 16);
    CHECK(c.maxBindingsPerStage.storage == 0);
    CHECK_FALSE(c.storageBuffers);
    CHECK_FALSE(c.vertexStorageBuffers);
    CHECK(c.bc);
    CHECK_FALSE(c.bc6h7);
    CHECK(c.format(Format::RGBA8_UNORM).render);
    CHECK(c.format(Format::RGBA16_FLOAT).filter);
    CHECK(c.format(Format::D32_FLOAT).depth);
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("FL10_0: a T1 (SM 5.0) PBR pipeline is rejected at creation (Review Focus 4)") {
    auto dev = makeDevice(currentBackend().desc);
    // createShader keeps an SM 5.0 blob without handing it to the driver (it would fail there with a
    // debug-layer error); the pipeline is the one place that reports the tier.
    const ShaderHandle vs1 = loadPbr(*dev, "VSMain", "T1_SKIN0_MORPH0_UNLIT0");
    const ShaderHandle ps1 = loadPbr(*dev, "PSMain", "T1_SKIN0_MORPH0_UNLIT0");
    auto t1 = dev->createGraphicsPipeline(pbrPipelineDesc(vs1, ps1));
    REQUIRE_FALSE(t1.has_value());
    CHECK(t1.error().exitCode == 3);
    CHECK(t1.error().message == "pipeline needs TIER 1, device is TIER 0");
    // One SM 5.0 stage is enough.
    const ShaderHandle vs0 = loadPbr(*dev, "VSMain", "T0_SKIN0_MORPH0_UNLIT0");
    const ShaderHandle ps0 = loadPbr(*dev, "PSMain", "T0_SKIN0_MORPH0_UNLIT0");
    auto mixed = dev->createGraphicsPipeline(pbrPipelineDesc(vs0, ps1));
    REQUIRE_FALSE(mixed.has_value());
    CHECK(mixed.error().message == "pipeline needs TIER 1, device is TIER 0");
    // The T0 pair (SM 4.0) is accepted.
    auto t0 = dev->createGraphicsPipeline(pbrPipelineDesc(vs0, ps0));
    CHECK_MESSAGE(t0.has_value(), (t0 ? std::string() : t0.error().message));
    REQUIRE(dev->submitAndWait().has_value());
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("FL10_0: builtin:triangle with the SM 4.0 shader, probes, validation clean") {
    auto dev = makeDevice(currentBackend().desc);
    auto img = eng::scenes::renderTriangle(*dev, shaderLibDir(), 64, 64);
    REQUIRE_MESSAGE(img.has_value(), (img ? std::string() : img.error().message));
    eng::rhi::test::checkTriangleProbes(*img);
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("FL10_0: builtin:pbr-spheres with dxbc T0, probes, validation clean; vs FL11 (T1) mean FLIP <= 0.05") {
    auto t0 = makeDevice(currentBackend().desc);
    Image img0 = renderPbr(*t0);
    eng::rhi::test::checkPbrSpheresProbes(img0);
    CHECK(t0->validationErrorCount() == 0);

    DeviceDesc fl11 = currentBackend().desc;
    fl11.d3d11FeatureLevelCap = 0;
    auto t1 = makeDevice(fl11);
    REQUIRE(t1->caps().tier == 1);
    Image img1 = renderPbr(*t1);
    CHECK(t1->validationErrorCount() == 0);
    auto cmp = eng::golden::compare(toGolden(std::move(img0)), toGolden(std::move(img1)));
    REQUIRE_MESSAGE(cmp.has_value(), (cmp ? std::string() : cmp.error()));
    MESSAGE("pbr-spheres " << currentBackend().name << " (T0) vs FL11 (T1): flipMean=" << cmp->flipMean
                           << " psnr=" << cmp->psnr);
    CHECK(cmp->flipMean <= 0.05);
}

} // TEST_SUITE
#endif
