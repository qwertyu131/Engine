// headless: offscreen render -> PNG (spec §10, plan F0 Task 5).
//
//   headless --backend <name> --scene <path|builtin:...> --out <png>
//            [--width N=256] [--height N=256] [--cvar name=value]... [--bench <frames>]
//            [--warp] [--adapter <index>] [--shader-dir <dir>]
//
// Backends:
//   cpu     F0: `--scene builtin:test-pattern` (deterministic CPU pattern, see TestPattern.h).
//   vulkan  F2: through engine/rhi, with validation (VK_LAYER_KHRONOS_validation + sync validation).
//   d3d11   F2: through engine/rhi, with the debug layer; --warp = WARP software rasterizer.
// GPU scenes (Scenes.h): builtin:triangle. --adapter selects the adapter by index (default: the
// backend's default). --shader-dir is the shader library of the `shaders` build step (default: the
// build tree's, compiled in).
//
// --cvar   applied with CVarSource::CommandLine, in order. An unknown cvar or a rejected value is
//          a usage error (a test gate must not silently run with a typo'd setting).
// --bench  renders r.Bench.WarmupFrames untimed frames, then N timed frames, and prints
//          `frame_ms_avg=<float> frame_ms_p95=<float>` on stdout (p95 = nearest rank).
//          The PNG written is the last frame.
//
// Exit codes: 0 = image written, 2 = usage / IO error, 3 = backend not available in this build or on
// this machine, 4 = validation error (also: validation layer missing), 5 = device lost / GPU error.
// All path arguments are UTF-8 (on Windows they are taken from the wide command line).

#include "Scenes.h"
#include "TestPattern.h"

#include "Image.h"
#include "Utf8.h"
#include "core/CVar.h"
#include "core/Log.h"
#include "rhi/Rhi.h"
#include "rhi/ShaderLibrary.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
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

constexpr std::string_view kBuiltinTestPattern = "builtin:test-pattern";
constexpr int kMaxBenchFrames = 1'000'000;

void printUsage(std::ostream& os) {
    os << "usage:\n"
          "  headless --backend <name> --scene <path|builtin:...> --out <png>\n"
          "           [--width N=256] [--height N=256] [--cvar name=value]... [--bench <frames>]\n"
          "           [--warp] [--adapter <index>] [--shader-dir <dir>]\n"
          "backends in this build: cpu";
    for (const eng::rhi::Backend b : eng::rhi::compiledBackends()) {
        os << ", " << eng::rhi::toString(b);
    }
    os << "\nscenes: builtin:test-pattern (cpu), builtin:triangle (gpu)\n"
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
};

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
    if (!haveBackend || !haveScene || !haveOut) {
        std::cerr << "headless: --backend, --scene and --out are required\n";
        return std::nullopt;
    }
    return o;
}

bool applyCVars(const std::vector<std::string>& cvars) {
    auto& reg = eng::CVarRegistry::get();
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
        ENG_LOG_INFO("cvar {} = {} (CommandLine)", name, value);
    }
    return true;
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
    for (const eng::rhi::Backend b : eng::rhi::compiledBackends()) {
        if (opts->backend == eng::rhi::toString(b)) {
            gpu = b;
        }
    }
    if (opts->backend != "cpu" && !gpu) {
        std::cerr << "backend '" << opts->backend << "' not available in this build\n";
        return kExitNoBackend;
    }
    if (!gpu && (opts->warp || opts->adapter >= 0)) {
        std::cerr << "headless: --warp / --adapter need a GPU backend\n";
        return kExitError;
    }

    eng::CVar* warmup = eng::CVarRegistry::get().registerInt(
        "r.Bench.WarmupFrames", 0, "Untimed frames rendered before --bench timing starts", eng::CVarFlags::None, 0,
        1000);
    if (!applyCVars(opts->cvars)) {
        return kExitError;
    }

    // GPU backends: device + scene through engine/rhi (plan F2 Task 2).
    std::unique_ptr<eng::rhi::IDevice> device;
    fs::path shaderDir;
    if (gpu) {
        if (opts->scene != eng::scenes::kTriangle) {
            std::cerr << "headless: scene '" << opts->scene << "' not supported on backend " << opts->backend
                      << " in this build (only " << eng::scenes::kTriangle << ")\n";
            return kExitError;
        }
#if defined(ENG_DEFAULT_SHADER_DIR)
        shaderDir = eng::rhi::pathFromUtf8(opts->shaderDir.empty() ? ENG_DEFAULT_SHADER_DIR : opts->shaderDir);
#else
        if (opts->shaderDir.empty()) {
            std::cerr << "headless: --shader-dir is required (no shader library in this build)\n";
            return kExitError;
        }
        shaderDir = eng::rhi::pathFromUtf8(opts->shaderDir);
#endif
        eng::rhi::DeviceDesc dd;
        dd.backend = *gpu;
        dd.warp = opts->warp;
        dd.adapterIndex = opts->adapter;
        auto dev = eng::rhi::createDevice(dd);
        if (!dev) {
            std::cerr << "headless: " << dev.error().message << "\n";
            return dev.error().exitCode;
        }
        device = std::move(*dev);
        ENG_LOG_INFO("backend {}: {} (API {})", opts->backend, device->caps().adapterName, device->caps().apiVersion);
    } else if (opts->scene != kBuiltinTestPattern) {
        std::cerr << "headless: scene '" << opts->scene << "' not supported in this build (only "
                  << kBuiltinTestPattern << ")\n";
        return kExitError;
    }

    int gpuError = kExitOk;
    std::string gpuMessage;
    const auto renderFrame = [&]() -> eng::Image8 {
        if (!device) {
            return eng::makeTestPattern(opts->width, opts->height);
        }
        auto img = eng::scenes::renderTriangle(*device, shaderDir, static_cast<uint32_t>(opts->width),
                                               static_cast<uint32_t>(opts->height));
        eng::Image8 out;
        if (!img) {
            gpuError = img.error().exitCode;
            gpuMessage = img.error().message;
            return out;
        }
        out.width = static_cast<int>(img->width);
        out.height = static_cast<int>(img->height);
        out.channels = 4;
        out.rgba8 = std::move(img->rgba8);
        return out;
    };
    eng::Image8 frame;
    if (opts->benchFrames) {
        for (int i = 0; i < warmup->getInt(); ++i) {
            frame = renderFrame();
        }
        std::vector<double> ms;
        ms.reserve(static_cast<size_t>(*opts->benchFrames));
        for (int i = 0; i < *opts->benchFrames; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            frame = renderFrame();
            const auto t1 = std::chrono::steady_clock::now();
            ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        double sum = 0.0;
        for (const double v : ms) {
            sum += v;
        }
        std::vector<double> sorted = ms;
        std::sort(sorted.begin(), sorted.end());
        const auto rank = static_cast<size_t>(std::ceil(0.95 * static_cast<double>(sorted.size())));
        const double p95 = sorted[std::max<size_t>(rank, 1) - 1];
        char line[128];
        std::snprintf(line, sizeof(line), "frame_ms_avg=%.6f frame_ms_p95=%.6f", sum / static_cast<double>(ms.size()),
                      p95);
        std::cout << line << "\n";
    } else {
        frame = renderFrame();
    }

    if (gpuError != kExitOk) {
        std::cerr << "headless: " << gpuMessage << "\n";
        return gpuError;
    }
    if (device && device->validationErrorCount() > 0) {
        std::cerr << "headless: " << device->validationErrorCount() << " validation message(s)\n";
        return eng::rhi::kExitValidation;
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
    ENG_LOG_INFO("wrote {} ({}x{}, backend {}, scene {})", opts->out, opts->width, opts->height, opts->backend,
                 opts->scene);
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
