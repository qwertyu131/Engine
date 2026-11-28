// headless: offscreen render -> PNG (spec §10, plan F0 Task 5).
//
//   headless --backend <name> --scene <path|builtin:...> --out <png>
//            [--width N=256] [--height N=256] [--cvar name=value]... [--bench <frames>]
//            [--warp] [--adapter <index>] [--shader-dir <dir>] [--asset-dir <dir>]
//            [--d3d11-feature-level 10_0|10_1|11_0|11_1] [--dump-caps <json>]
//
// Backends:
//   cpu     F0: `--scene builtin:test-pattern` (deterministic CPU pattern, see TestPattern.h).
//   vulkan  F2: through engine/rhi, with validation (VK_LAYER_KHRONOS_validation + sync validation).
//   d3d11   F2: through engine/rhi, with the debug layer; --warp = WARP software rasterizer.
// GPU scenes through the library facade (include/engine, spec K-D, plan F3 Task 4): builtin:triangle,
// builtin:textured-cube, builtin:pbr-spheres, or a .gltf / .glb file (fit camera of its default scene, rest pose).
// --adapter selects the adapter by index
// (default: the backend's default). --shader-dir is the shader library of the `shaders` build step
// (default: the build tree's, compiled in). --asset-dir is the test asset root (default: the source
// tree's tests/assets, compiled in).
//
// --d3d11-feature-level  caps the D3D11 feature level (DeviceDesc::d3d11FeatureLevelCap; 10_x = T0, plan F2
//          Task 7). Default: the highest the adapter offers.
// --dump-caps  writes the device's DeviceCaps as JSON (rhi::capsToJson, with a "note" on where the values
//          come from) and prints `caps: <backend> tier <n> (<apiVersion>, <adapter>) -> <json>`. With
//          --dump-caps, --scene and --out are optional (both or neither): without them nothing is rendered.
// --cvar   applied with CVarSource::CommandLine, in order. An unknown cvar or a rejected value is
//          a usage error (a test gate must not silently run with a typo'd setting).
// --bench  renders r.Bench.WarmupFrames untimed frames, then N timed frames, and prints
//          `frame_ms_avg=<float> frame_ms_p95=<float>` on stdout (p95 = nearest rank).
//          The PNG written is the last frame. GPU backends (plan F2 Task 8) first print
//          `bench: <backend> <scene> <W>x<H>, <N> timed frames after <M> warm-up, one offscreen target, ...`:
//          the scene and one target are created once; a timed frame = record + submit + GPU wait
//          (renderFrameInto, frames serialised, validation on as in every gate run), no readback; the last
//          frame is read back once after the timing.
//
// Exit codes: 0 = image written, 2 = usage / IO error, 3 = backend not available in this build or on
// this machine, 4 = validation error (also: validation layer missing), 5 = device lost / GPU error.
// All path arguments are UTF-8 (on Windows they are taken from the wide command line).

#include "TestPattern.h"

#include "Image.h"
#include "Utf8.h"

#include <engine/Renderer.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 2;
constexpr int kExitNoBackend = 3;
constexpr int kExitValidation = 4;

constexpr std::string_view kBuiltinTestPattern = "builtin:test-pattern";
constexpr int kMaxBenchFrames = 1'000'000;

void printUsage(std::ostream& os) {
    os << "usage:\n"
          "  headless --backend <name> --scene <path|builtin:...> --out <png>\n"
          "           [--width N=256] [--height N=256] [--cvar name=value]... [--bench <frames>]\n"
          "           [--warp] [--adapter <index>] [--shader-dir <dir>] [--asset-dir <dir>]\n"
          "           [--d3d11-feature-level 10_0|10_1|11_0|11_1] [--dump-caps <json>]\n"
          "  headless --backend <gpu> --dump-caps <json> [--warp] [--adapter <index>] [--d3d11-feature-level ...]\n"
          "backends in this build: cpu";
    for (const eng::rhi::Backend b : eng::Renderer::compiledBackends()) {
        os << ", " << eng::rhi::toString(b);
    }
    os << "\nscenes: builtin:test-pattern (cpu), builtin:triangle, builtin:textured-cube, builtin:pbr-spheres, "
          "<file>.gltf / .glb (gpu)\n"
          "exit codes: 0 = ok, 2 = usage / IO error, 3 = backend not available, 4 = validation error,\n"
          "            5 = device lost / GPU error\n";
}

struct Options {
    std::string backend;
    std::string scene;
    std::string out;
    int width = 256;
    int height = 256;
    std::vector<std::string> cvars;
    std::optional<int> benchFrames;
    bool warp = false;
    int adapter = -1;
    std::string shaderDir;
    std::string assetDir;
    int d3d11FeatureLevel = 0; // 0 = highest; 0xa000 = 10_0 ...
    std::string featureLevelName;
    std::string dumpCaps;
};

// "10_0" -> 0xa000 ... "11_1" -> 0xb100; 0 for anything else.
int parseFeatureLevel(const std::string& s) {
    if (s == "10_0") {
        return 0xa000;
    }
    if (s == "10_1") {
        return 0xa100;
    }
    if (s == "11_0") {
        return 0xb000;
    }
    if (s == "11_1") {
        return 0xb100;
    }
    return 0;
}

std::optional<int> parseInt(const std::string& s, int min, int max) {
    int v = 0;
    const char* end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(s.data(), end, v);
    if (ec != std::errc() || ptr != end || v < min || v > max) {
        return std::nullopt;
    }
    return v;
}

// Returns nullopt (after printing why) on unknown options, missing values or invalid numbers.
std::optional<Options> parseArgs(const std::vector<std::string>& args) {
    Options o;
    bool haveBackend = false;
    bool haveScene = false;
    bool haveOut = false;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& key = args[i];
        if (key == "--warp") { // flag, no value
            o.warp = true;
            continue;
        }
        if (i + 1 >= args.size()) {
            std::cerr << "headless: option '" << key << "' needs a value (or is unknown)\n";
            return std::nullopt;
        }
        const std::string& val = args[++i];
        if (key == "--backend") {
            o.backend = val;
            haveBackend = true;
        } else if (key == "--scene") {
            o.scene = val;
            haveScene = true;
        } else if (key == "--out") {
            o.out = val;
            haveOut = true;
        } else if (key == "--width" || key == "--height") {
            auto v = parseInt(val, 1, eng::kMaxTestPatternSize);
            if (!v) {
                std::cerr << "headless: " << key << " must be an integer in [1, " << eng::kMaxTestPatternSize
                          << "] (got '" << val << "')\n";
                return std::nullopt;
            }
            (key == "--width" ? o.width : o.height) = *v;
        } else if (key == "--adapter") {
            auto v = parseInt(val, 0, 64);
            if (!v) {
                std::cerr << "headless: --adapter must be an integer in [0, 64] (got '" << val << "')\n";
                return std::nullopt;
            }
            o.adapter = *v;
        } else if (key == "--shader-dir") {
            o.shaderDir = val;
        } else if (key == "--asset-dir") {
            o.assetDir = val;
        } else if (key == "--d3d11-feature-level") {
            o.d3d11FeatureLevel = parseFeatureLevel(val);
            if (o.d3d11FeatureLevel == 0) {
                std::cerr << "headless: --d3d11-feature-level must be one of 10_0, 10_1, 11_0, 11_1 (got '" << val
                          << "')\n";
                return std::nullopt;
            }
            o.featureLevelName = val;
        } else if (key == "--dump-caps") {
            o.dumpCaps = val;
        } else if (key == "--cvar") {
            o.cvars.push_back(val);
        } else if (key == "--bench") {
            auto v = parseInt(val, 1, kMaxBenchFrames);
            if (!v) {
                std::cerr << "headless: --bench must be an integer in [1, " << kMaxBenchFrames << "] (got '" << val
                          << "')\n";
                return std::nullopt;
            }
            o.benchFrames = *v;
        } else {
            std::cerr << "headless: unknown option '" << key << "'\n";
            return std::nullopt;
        }
    }
    const bool capsOnly = !o.dumpCaps.empty() && !haveScene && !haveOut;
    if (!haveBackend || (!capsOnly && (!haveScene || !haveOut))) {
        std::cerr << "headless: --backend, --scene and --out are required (--scene / --out are optional with "
                     "--dump-caps)\n";
        return std::nullopt;
    }
    return o;
}

bool applyCVars(eng::Settings& reg, const std::vector<std::string>& cvars) {
    for (const std::string& kv : cvars) {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) {
            std::cerr << "headless: --cvar expects name=value (got '" << kv << "')\n";
            return false;
        }
        const std::string_view name(kv.data(), eq);
        const std::string_view value(kv.data() + eq + 1, kv.size() - eq - 1);
        if (reg.find(name) == nullptr) {
            std::cerr << "headless: unknown cvar '" << name << "'\n";
            return false;
        }
        if (!reg.set(name, value, eng::CVarSource::CommandLine)) {
            std::cerr << "headless: cvar '" << name << "' rejected value '" << value << "'\n";
            return false;
        }
        std::cerr << "[info] cvar " << name << " = " << value << " (CommandLine)\n";
    }
    return true;
}

// `frame_ms_avg=<float> frame_ms_p95=<float>` (p95 = nearest rank) on stdout.
void printBenchLine(const std::vector<double>& ms) {
    double sum = 0.0;
    for (const double v : ms) {
        sum += v;
    }
    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    const auto rank = static_cast<size_t>(std::ceil(0.95 * static_cast<double>(sorted.size())));
    const double p95 = sorted[std::max<size_t>(rank, 1) - 1];
    char line[128];
    std::snprintf(line, sizeof(line), "frame_ms_avg=%.6f frame_ms_p95=%.6f", sum / static_cast<double>(ms.size()), p95);
    std::cout << line << "\n";
}

// Where the numbers come from, written into every caps JSON (plan F2 Task 7).
std::string capsNote(const eng::rhi::DeviceCaps& c, const Options& o) {
    const std::string warp = c.software ? " (WARP software rasterizer)" : "";
    std::string n;
    if (c.backend == eng::rhi::Backend::D3D11) {
        if (o.d3d11FeatureLevel != 0) {
            n = "D3D11 device created with its feature level capped at " + o.featureLevelName + " on '" + c.adapterName +
                "'" + warp +
                ": these are the API limits (Windows SDK constants of that feature level) and the format / feature "
                "support this driver reports at that feature level, NOT measurements of real FL" +
                o.featureLevelName + " hardware.";
        } else {
            n = "D3D11 device at the adapter's highest feature level (" + c.apiVersion + ") on '" + c.adapterName + "'" +
                warp +
                ": limits are the Windows SDK constants of that feature level; format and feature support as the "
                "driver reports it.";
        }
    } else {
        n = "Vulkan physical device '" + c.adapterName + "' (API " + c.apiVersion +
            "): limits, formats and features the device SUPPORTS (vkGetPhysicalDevice*); the RHI enables "
            "dynamicRendering and the texture compression features the device has (plan F3 Task 3).";
    }
    return n + " Written by headless --dump-caps (plan F2 Task 7).";
}

int dumpCaps(const eng::rhi::DeviceCaps& c, const Options& o) {
    const fs::path path = eng::golden::fromUtf8(o.dumpCaps);
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
    }
    std::ofstream f(path, std::ios::binary);
    f << eng::rhi::capsToJson(c, capsNote(c, o));
    f.close();
    if (!f) {
        std::cerr << "headless: cannot write '" << o.dumpCaps << "'\n";
        return kExitError;
    }
    std::cout << "caps: " << eng::rhi::toString(c.backend) << " tier " << c.tier << " (" << c.apiVersion << ", "
              << c.adapterName << ") -> " << o.dumpCaps << "\n";
    return kExitOk;
}

int run(const std::vector<std::string>& args) {
    if (args.size() == 2 && (args[1] == "--help" || args[1] == "-h")) {
        printUsage(std::cout);
        return kExitOk;
    }
    auto opts = parseArgs(args);
    if (!opts) {
        printUsage(std::cerr);
        return kExitError;
    }

    std::optional<eng::rhi::Backend> gpu;
    for (const eng::rhi::Backend b : eng::Renderer::compiledBackends()) {
        if (opts->backend == eng::rhi::toString(b)) {
            gpu = b;
        }
    }
    if (opts->backend != "cpu" && !gpu) {
        std::cerr << "backend '" << opts->backend << "' not available in this build\n";
        return kExitNoBackend;
    }
    if (!gpu && (opts->warp || opts->adapter >= 0 || !opts->dumpCaps.empty())) {
        std::cerr << "headless: --warp / --adapter / --dump-caps need a GPU backend\n";
        return kExitError;
    }
    if (opts->d3d11FeatureLevel != 0 && gpu != eng::rhi::Backend::D3D11) {
        std::cerr << "headless: --d3d11-feature-level needs --backend d3d11\n";
        return kExitError;
    }
    const bool capsOnly = opts->scene.empty();

    eng::Settings settings;
    eng::CVar* warmup = settings.registry().registerInt(
        "r.Bench.WarmupFrames", 0, "Untimed frames rendered before --bench timing starts", eng::CVarFlags::None, 0,
        1000);
    if (!gpu && !applyCVars(settings, opts->cvars)) {
        return kExitError;
    }
    if (!gpu && opts->scene != kBuiltinTestPattern) {
        std::cerr << "headless: scene '" << opts->scene << "' not supported in this build (only " << kBuiltinTestPattern
                  << ")\n";
        return kExitError;
    }

    // GPU: everything through the facade (spec K-D, plan F3 Task 4).
    std::unique_ptr<eng::Renderer> renderer;
    if (gpu) {
        if (!capsOnly && opts->scene.rfind("builtin:", 0) == 0) {
            bool known = false;
            std::string names;
            for (const std::string_view n : eng::Renderer::builtinScenes()) {
                known = known || opts->scene == n;
                names += (names.empty() ? "" : ", ") + std::string(n);
            }
            if (!known) {
                std::cerr << "headless: scene '" << opts->scene << "' not supported on backend " << opts->backend
                          << " in this build (" << names << ", or a .gltf / .glb file)\n";
                return kExitError;
            }
        }
        eng::RendererDesc rd;
        rd.backend = *gpu;
        rd.warp = opts->warp;
        rd.adapterIndex = opts->adapter;
        rd.d3d11FeatureLevelCap = opts->d3d11FeatureLevel;
        rd.width = static_cast<uint32_t>(opts->width);
        rd.height = static_cast<uint32_t>(opts->height);
#if defined(ENG_DEFAULT_SHADER_DIR)
        rd.shaderDir = eng::golden::fromUtf8(opts->shaderDir.empty() ? ENG_DEFAULT_SHADER_DIR : opts->shaderDir);
#else
        if (!capsOnly && opts->shaderDir.empty()) {
            std::cerr << "headless: --shader-dir is required (no shader library in this build)\n";
            return kExitError;
        }
        rd.shaderDir = eng::golden::fromUtf8(opts->shaderDir);
#endif
#if defined(ENG_DEFAULT_ASSET_DIR)
        rd.assetDir = eng::golden::fromUtf8(opts->assetDir.empty() ? ENG_DEFAULT_ASSET_DIR : opts->assetDir);
#else
        rd.assetDir = eng::golden::fromUtf8(opts->assetDir);
#endif
        auto r = eng::Renderer::create(rd);
        if (!r) {
            // Only device creation's exit 3 is "backend not available" (cmake/TestSkip.cmake skips on that text).
            std::cerr << "headless: " << (r.error().exitCode == kExitNoBackend ? "backend not available: " : "")
                      << r.error().message << "\n";
            return r.error().exitCode;
        }
        renderer = std::move(*r);
        std::cerr << "[info] backend " << opts->backend << ": " << renderer->caps().adapterName << " (API "
                  << renderer->caps().apiVersion << ")\n";
        if (!applyCVars(settings, opts->cvars)) { // after create: the renderer registers its cvars there
            return kExitError;
        }
        if (!opts->dumpCaps.empty()) {
            if (const int rc = dumpCaps(renderer->caps(), *opts); rc != kExitOk) {
                return rc;
            }
            if (capsOnly) {
                return kExitOk;
            }
        }
        auto asset = renderer->loadAsset(eng::golden::fromUtf8(opts->scene));
        if (!asset) {
            std::cerr << "headless: " << asset.error().message << "\n";
            return asset.error().exitCode;
        }
        auto scene = renderer->instantiate(*asset);
        if (!scene) {
            std::cerr << "headless: " << scene.error().message << "\n";
            return scene.error().exitCode;
        }
        renderer->setCamera(renderer->fitCamera(*scene, float(opts->width) / float(opts->height)));
    }

    int gpuError = kExitOk;
    std::string gpuMessage;
    // One frame: the CPU pattern, or a GPU frame (record + submit + GPU wait, no readback).
    const auto gpuFrame = [&]() {
        if (gpuError != kExitOk) {
            return;
        }
        if (auto r = renderer->renderFrame(); !r) {
            gpuError = r.error().exitCode;
            gpuMessage = r.error().message;
        }
    };
    eng::Image8 frame;
    if (opts->benchFrames) {
        if (renderer) {
            std::cout << "bench: " << opts->backend << " " << opts->scene << " " << opts->width << "x" << opts->height
                      << ", " << *opts->benchFrames << " timed frames after " << warmup->getInt()
                      << " warm-up, one offscreen target, frame = record + submit + GPU wait (no readback), validation "
                      << "on (" << renderer->caps().adapterName << ")\n";
        }
        for (int i = 0; i < warmup->getInt(); ++i) {
            if (renderer) {
                gpuFrame();
            } else {
                frame = eng::makeTestPattern(opts->width, opts->height);
            }
        }
        std::vector<double> ms;
        ms.reserve(static_cast<size_t>(*opts->benchFrames));
        for (int i = 0; i < *opts->benchFrames; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            if (renderer) {
                gpuFrame();
            } else {
                frame = eng::makeTestPattern(opts->width, opts->height);
            }
            const auto t1 = std::chrono::steady_clock::now();
            ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        printBenchLine(ms);
    } else if (renderer) {
        gpuFrame();
    } else {
        frame = eng::makeTestPattern(opts->width, opts->height);
    }
    if (renderer && gpuError == kExitOk) {
        if (auto img = renderer->readback(); img) {
            frame.width = static_cast<int>(img->width);
            frame.height = static_cast<int>(img->height);
            frame.channels = 4;
            frame.rgba8 = std::move(img->rgba8);
        } else {
            gpuError = img.error().exitCode;
            gpuMessage = img.error().message;
        }
    }
    if (renderer) {
        if (auto r = renderer->finish(); !r && gpuError == kExitOk) { // runs the deferred destruction
            gpuError = r.error().exitCode;
            gpuMessage = r.error().message;
        }
    }
    if (gpuError != kExitOk) {
        std::cerr << "headless: " << gpuMessage << "\n";
        return gpuError;
    }
    if (renderer && renderer->validationErrorCount() > 0) {
        std::cerr << "headless: " << renderer->validationErrorCount() << " validation message(s)\n";
        return kExitValidation;
    }

    const fs::path out = eng::golden::fromUtf8(opts->out);
    if (out.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
    }
    if (!eng::golden::savePng(out, frame)) {
        std::cerr << "headless: cannot write '" << opts->out << "'\n";
        return kExitError;
    }
    std::cerr << "[info] wrote " << opts->out << " (" << opts->width << "x" << opts->height << ", backend "
              << opts->backend << ", scene " << opts->scene << ")\n";
    return kExitOk;
}

int guardedRun(const std::vector<std::string>& args) {
    try {
        return run(args);
    } catch (const std::exception& e) {
        std::cerr << "headless: error: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "headless: unknown error\n";
    }
    return kExitError;
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    try {
        for (int i = 0; i < argc; ++i) {
            args.push_back(eng::golden::toUtf8(fs::path(argv[i]))); // UTF-16 -> UTF-8, ANSI codepage independent
        }
    } catch (const std::exception& e) {
        std::cerr << "headless: cannot decode command line: " << e.what() << "\n";
        return kExitError;
    }
    return guardedRun(args);
}
#else
int main(int argc, char** argv) {
    return guardedRun(std::vector<std::string>(argv, argv + argc));
}
#endif
