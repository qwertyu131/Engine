// Reference mode (plan F3 Task 5, spec K-B): the IBL of glTF-Sample-Renderer v1.1.0 on both paths, the reference-mode
// cvars, and the fit camera on degenerate scenes (Review Focus 5).
//
//   renderer_reference_tests --backend=vulkan|d3d11|d3d11-fl10_0 [doctest options]
//
// Cases:
//   - ibl::Params defaults are iblSampler's constructor values (ibl_sampler.js) and the mip count rule;
//   - the reference-mode cvar table reproduces tests/golden/ref/sample-viewer/settings.json by value;
//   - reference mode forces the table at CVarSource::Code over a CommandLine value and reports it; a Console value
//     outranks it and is reported, not overwritten;
//   - CPU and GPU prefilter (source cube, Lambertian, GGX levels, LUT) agree within 2 / 255 (relative above 1) on every
//     texel, at a reduced size (64, 128 / 256 samples, LUT 64) so the Debug CPU path stays short (T1 / T2 devices: the
//     GPU path exists there);
//   - Review Focus 5: a zero-size box (one triangle at the origin, no area) and an empty scene give a finite fit camera
//     and render the background only (the two frames are identical: the same camera, nothing drawn); the Cameras asset
//     renders with a finite camera. Validation 0 everywhere.
#include <engine/Renderer.h>

#include "renderer/Ibl.h"
#include "renderer/RenderSettings.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

eng::RendererDesc g_desc;
std::string g_backendName;

std::filesystem::path u8(std::string_view s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}
std::filesystem::path assets() {
    return u8(ENG_TEST_ASSET_DIR);
}
std::filesystem::path cannon() {
    return assets() / "environments" / "Cannon_Exterior.hdr";
}

bool finite(const eng::Camera& c) {
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(c.position[i]) || !std::isfinite(c.target[i]) || !std::isfinite(c.up[i])) {
            return false;
        }
    }
    return std::isfinite(c.yfov) && std::isfinite(c.znear) && std::isfinite(c.zfar) && c.znear > 0.0f &&
           c.zfar > c.znear;
}

struct Frame {
    eng::Camera camera;
    eng::Image image;
};

Frame renderReference(const std::filesystem::path& file) {
    eng::RendererDesc d = g_desc;
    d.width = d.height = 128;
    d.shaderDir = u8(ENG_TEST_SHADER_LIB_DIR);
    d.assetDir = assets();
    auto r = eng::Renderer::create(d);
    REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
    // Console: the cvar test above leaves r.ReferenceMode set by Console (it outranks Code).
    REQUIRE((*r)->settings().set("r.ReferenceMode", "1", eng::CVarSource::Console));
    auto a = (*r)->loadAsset(file);
    REQUIRE_MESSAGE(a.has_value(), (a ? std::string() : a.error().message));
    auto s = (*r)->instantiate(*a);
    REQUIRE_MESSAGE(s.has_value(), (s ? std::string() : s.error().message));
    Frame f;
    f.camera = (*r)->fitCamera(*s, 1.0f);
    (*r)->setCamera(f.camera);
    auto rf = (*r)->renderFrame();
    REQUIRE_MESSAGE(rf.has_value(), (rf ? std::string() : rf.error().message));
    auto img = (*r)->readback();
    REQUIRE(img.has_value());
    f.image = std::move(*img);
    REQUIRE((*r)->finish().has_value());
    CHECK((*r)->validationErrorCount() == 0);
    REQUIRE((*r)->settings().set("r.ReferenceMode", "0", eng::CVarSource::Console));
    return f;
}

double stdDev(const eng::Image& img) {
    double sum = 0.0, sq = 0.0;
    const size_t n = img.rgba8.size() / 4;
    for (size_t i = 0; i < n; ++i) {
        const double v = img.rgba8[i * 4] + img.rgba8[i * 4 + 1] + img.rgba8[i * 4 + 2];
        sum += v;
        sq += v * v;
    }
    const double mean = sum / double(n);
    return std::sqrt(std::max(0.0, sq / double(n) - mean * mean));
}

std::vector<float> floats(const std::vector<std::byte>& bytes) {
    std::vector<float> f(bytes.size() / 4);
    std::memcpy(f.data(), bytes.data(), bytes.size());
    return f;
}

// Largest |gpu - cpu| / max(1, |cpu|) over RGB of every texel (alpha is 1 on both).
struct Diff {
    double worst = 0.0;
    size_t over = 0, texels = 0;
};
void accumulate(Diff& d, const std::vector<float>& gpu, const std::vector<float>& cpu, double tol) {
    REQUIRE(gpu.size() == cpu.size());
    for (size_t i = 0; i < cpu.size(); i += 4) {
        double t = 0.0;
        for (size_t k = 0; k < 3; ++k) {
            t = std::max(t, std::abs(double(gpu[i + k]) - double(cpu[i + k])) / std::max(1.0, std::abs(double(cpu[i + k]))));
        }
        d.worst = std::max(d.worst, t);
        d.over += t > tol;
        ++d.texels;
    }
}

} // namespace

TEST_CASE("ibl::Params defaults are glTF-Sample-Renderer v1.1.0 iblSampler's") {
    const eng::renderer::ibl::Params p;
    CHECK(p.textureSize == 256);
    CHECK(p.ggxSampleCount == 1024);
    CHECK(p.lambertianSampleCount == 2048);
    CHECK(p.lodBias == 0.0f);
    CHECK(p.lowestMipLevel == 4);
    CHECK(p.lutResolution == 1024);
    CHECK(p.lutSampleCount == 512);
    CHECK(p.mipmapLevels() == 5); // floor(log2(256)) + 1 - 4 = u_MipCount
    CHECK(p.ggxLevels() == 6);    // cubeMapToGGX writes levels 0..mipmapLevels
}

TEST_CASE("the reference-mode cvar table reproduces tests/golden/ref/sample-viewer/settings.json") {
    std::ifstream f(assets().parent_path() / "golden" / "ref" / "sample-viewer" / "settings.json");
    REQUIRE(f);
    const nlohmann::json j = nlohmann::json::parse(f);
    const nlohmann::json& s = j.at("settings");
    std::map<std::string, std::string> table;
    for (const auto& v : eng::renderer::referenceModeValues()) {
        table[std::string(v.cvar)] = std::string(v.value);
    }
    CHECK(table.at("r.IBL.Environment") == s.at("environment").get<std::string>());
    CHECK(std::stod(table.at("r.IBL.Rotation")) == s.at("environmentRotationDeg").get<double>());
    CHECK(std::stod(table.at("r.IBL.Intensity")) == s.at("iblIntensity").get<double>());
    CHECK(std::stod(table.at("r.Exposure")) == s.at("exposure").get<double>());
    CHECK(s.at("toneMap").get<std::string>() == "Khronos PBR Neutral");
    CHECK(table.at("r.Tonemap") == "PbrNeutral");
    CHECK(s.at("renderEnvironmentMap").get<bool>());
    CHECK(table.at("r.Background") == "Environment");
    CHECK(s.at("blurEnvironmentMap").get<bool>());
    CHECK(table.at("r.Background.Blur") == "0.6"); // cubemap.frag u_EnvBlurNormalized when blurEnvironmentMap
    CHECK_FALSE(s.at("usePunctual").get<bool>());   // punctual lights: Task 7 adds the cvar; none in F3 Task 5
    CHECK(s.at("width").get<int>() == 512);
    CHECK(s.at("height").get<int>() == 512);
    const eng::renderer::ibl::Params p;
    CHECK(std::stoi(table.at("r.IBL.Resolution")) == int(p.textureSize));
    CHECK(std::stoi(table.at("r.IBL.SpecularSamples")) == int(p.ggxSampleCount));
    CHECK(std::stoi(table.at("r.IBL.DiffuseSamples")) == int(p.lambertianSampleCount));
    // The environment file of the table is the one settings.json pins.
    CHECK(j.at("pinnedFiles").contains(table.at("r.IBL.Environment") + ".hdr"));
}

TEST_CASE("reference mode forces its values at Code over CommandLine, and reports what it cannot override") {
    eng::CVarRegistry& reg = eng::CVarRegistry::get();
    (void)eng::renderer::renderCVars();
    REQUIRE(reg.set("r.Exposure", "2", eng::CVarSource::CommandLine));
    REQUIRE(reg.set("r.ReferenceMode", "1", eng::CVarSource::Code));
    const std::vector<std::string> lines = eng::renderer::applyReferenceMode();
    CHECK(reg.find("r.Exposure")->getFloat() == 1.0f);
    CHECK(reg.find("r.Exposure")->source() == eng::CVarSource::Code);
    bool reported = false;
    for (const std::string& l : lines) {
        reported = reported || l.find("r.Exposure 2 (CommandLine) -> 1") != std::string::npos;
    }
    CHECK_MESSAGE(reported, "lines: " << (lines.empty() ? std::string("(none)") : lines.front()));
    // A Console value outranks Code (spec 8): kept, reported.
    REQUIRE(reg.set("r.IBL.Intensity", "3", eng::CVarSource::Console));
    const std::vector<std::string> lines2 = eng::renderer::applyReferenceMode();
    CHECK(reg.find("r.IBL.Intensity")->getFloat() == 3.0f);
    bool kept = false;
    for (const std::string& l : lines2) {
        kept = kept || l.find("r.IBL.Intensity stays 3 (set by Console") != std::string::npos;
    }
    CHECK(kept);
    // Reference mode off: nothing is forced.
    REQUIRE(reg.set("r.ReferenceMode", "0", eng::CVarSource::Console));
    CHECK(eng::renderer::applyReferenceMode().empty());
    // Restore (the registry is process-wide): Console values stay Console, so reset them at Console.
    REQUIRE(reg.set("r.IBL.Intensity", "1", eng::CVarSource::Console));
}


TEST_CASE("CPU and GPU IBL prefilter agree within 2 / 255 on every texel (reduced size)") {
    if (g_backendName == "d3d11-fl10_0") {
        MESSAGE("T0: no GPU prefilter (the CPU path is the only one)");
        return;
    }
    namespace ibl = eng::renderer::ibl;
    ibl::Params p;
    p.textureSize = 64;
    p.ggxSampleCount = 128;
    p.lambertianSampleCount = 256;
    p.lutResolution = 64;
    p.lutSampleCount = 128;
    REQUIRE(p.mipmapLevels() == 3);
    auto pano = ibl::loadHdr(cannon());
    REQUIRE_MESSAGE(pano.has_value(), (pano ? std::string() : pano.error().message));
    CHECK(pano->width == 1024);
    CHECK(pano->height == 512);
    const ibl::Cube source = ibl::panoramaToCube(*pano, p.textureSize);
    const ibl::Cube lambert = ibl::cubeMapToLambertian(source, p);
    const ibl::Cube ggx = ibl::cubeMapToGGX(source, p);
    const std::vector<float> lut = ibl::sampleGGXLut(p);
    REQUIRE(ggx.levels.size() == p.ggxLevels());

    eng::rhi::DeviceDesc dd;
    dd.backend = g_desc.backend;
    dd.validation = g_desc.validation;
    dd.warp = g_desc.warp;
    dd.d3d11FeatureLevelCap = g_desc.d3d11FeatureLevelCap;
    auto dev = eng::rhi::createDevice(dd);
    REQUIRE_MESSAGE(dev.has_value(), (dev ? std::string() : dev.error().message));
    auto gpu = ibl::buildGpu(**dev, u8(ENG_TEST_SHADER_LIB_DIR), *pano, p, /*copySrc=*/true);
    REQUIRE_MESSAGE(gpu.has_value(), (gpu ? std::string() : gpu.error().message));
    CHECK(gpu->mipCount == 3);
    constexpr double kTol = 2.0 / 255.0;
    Diff spec, irr, lutDiff;
    for (uint32_t mip = 0; mip < p.ggxLevels(); ++mip) {
        const uint32_t s = p.textureSize >> mip;
        for (uint32_t face = 0; face < 6; ++face) {
            auto bytes = (*dev)->readbackSubresource(gpu->specular, mip, face);
            REQUIRE_MESSAGE(bytes.has_value(), (bytes ? std::string() : bytes.error().message));
            const size_t n = size_t(s) * s * 4;
            const std::vector<float> cpu(ggx.levels[mip].begin() + face * n, ggx.levels[mip].begin() + (face + 1) * n);
            Diff level;
            accumulate(level, floats(*bytes), cpu, kTol);
            accumulate(spec, floats(*bytes), cpu, kTol);
            MESSAGE("GGX level " << mip << " face " << face << ": worst " << level.worst * 255.0 << " / 255");
        }
    }
    for (uint32_t face = 0; face < 6; ++face) {
        auto bytes = (*dev)->readbackSubresource(gpu->irradiance, 0, face);
        REQUIRE(bytes.has_value());
        const size_t n = size_t(p.textureSize) * p.textureSize * 4;
        const std::vector<float> cpu(lambert.levels[0].begin() + face * n, lambert.levels[0].begin() + (face + 1) * n);
        accumulate(irr, floats(*bytes), cpu, kTol);
    }
    auto lutBytes = (*dev)->readbackSubresource(gpu->lut, 0, 0);
    REQUIRE(lutBytes.has_value());
    accumulate(lutDiff, floats(*lutBytes), lut, kTol);
    MESSAGE("worst |GPU - CPU| (relative above 1), x 255: GGX " << spec.worst * 255.0 << ", Lambertian "
                                                                << irr.worst * 255.0 << ", LUT " << lutDiff.worst * 255.0);
    CHECK(spec.over == 0);
    CHECK(irr.over == 0);
    CHECK(lutDiff.over == 0);
    ibl::destroy(**dev, *gpu);
    REQUIRE((*dev)->submitAndWait().has_value());
    CHECK((*dev)->validationErrorCount() == 0);
}

TEST_CASE("Review Focus 5: degenerate scenes give a finite fit camera and a background-only frame") {
    const Frame point = renderReference(assets() / "f03" / "degenerate" / "single_point.gltf");
    const Frame empty = renderReference(assets() / "f03" / "degenerate" / "empty_scene.gltf");
    CHECK(finite(point.camera));
    CHECK(finite(empty.camera));
    // Nothing drawn (a zero-area triangle; no node): the environment background only, the same camera.
    CHECK(point.image.rgba8 == empty.image.rgba8);
    CHECK(stdDev(empty.image) > 2.0); // the blurred environment, not a cleared or NaN-black frame
    const Frame cameras =
        renderReference(u8(ENG_TEST_SAMPLE_MODELS) / "Cameras" / "glTF" / "Cameras.gltf");
    CHECK(finite(cameras.camera));
    CHECK(cameras.image.rgba8 != empty.image.rgba8);
}

int main(int argc, char** argv) {
    std::vector<char*> args;
    std::string backend;
    for (int i = 0; i < argc; ++i) {
        if (std::strncmp(argv[i], "--backend=", 10) == 0) {
            backend = argv[i] + 10;
        } else {
            args.push_back(argv[i]);
        }
    }
    if (backend != "vulkan" && backend != "d3d11" && backend != "d3d11-fl10_0") {
        std::printf("usage: renderer_reference_tests --backend=vulkan|d3d11|d3d11-fl10_0 [doctest options]\n");
        return 2;
    }
    g_backendName = backend;
    g_desc.backend = backend == "vulkan" ? eng::rhi::Backend::Vulkan : eng::rhi::Backend::D3D11;
    g_desc.d3d11FeatureLevelCap = backend == "d3d11-fl10_0" ? 0xa000 : 0;
    {
        eng::RendererDesc d = g_desc;
        d.shaderDir = u8(ENG_TEST_SHADER_LIB_DIR);
        d.assetDir = assets();
        auto r = eng::Renderer::create(d);
        if (!r) {
            if (r.error().exitCode == 3) {
                std::printf("SKIP: backend %s not available: %s\n", backend.c_str(), r.error().message.c_str());
                return 77;
            }
            std::printf("FAIL: backend %s: exit %d: %s\n", backend.c_str(), r.error().exitCode, r.error().message.c_str());
            return 1;
        }
    }
    doctest::Context ctx(static_cast<int>(args.size()), args.data());
    return ctx.run();
}
