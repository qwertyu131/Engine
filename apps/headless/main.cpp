// headless: offscreen render -> PNG (spec §10, plan F0 Task 5).
//
//   headless --backend <name> --scene <path|builtin:test-pattern> --out <png>
//            [--width N=256] [--height N=256] [--cvar name=value]... [--bench <frames>]
//
// F0: only `--backend cpu` with `--scene builtin:test-pattern` (deterministic CPU pattern, see
// TestPattern.h). Real GPU backends register in F2.
//
// --cvar   applied with CVarSource::CommandLine, in order. An unknown cvar or a rejected value is
//          a usage error (a test gate must not silently run with a typo'd setting).
// --bench  renders r.Bench.WarmupFrames untimed frames, then N timed frames, and prints
//          `frame_ms_avg=<float> frame_ms_p95=<float>` on stdout (p95 = nearest rank).
//          The PNG written is the last frame.
//
// Exit codes: 0 = image written, 2 = usage / IO error, 3 = backend not available in this build.
// All path arguments are UTF-8 (on Windows they are taken from the wide command line).

#include "TestPattern.h"

#include "Image.h"
#include "Utf8.h"
#include "core/CVar.h"
#include "core/Log.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
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
          "  headless --backend <name> --scene <path|builtin:test-pattern> --out <png>\n"
          "           [--width N=256] [--height N=256] [--cvar name=value]... [--bench <frames>]\n"
          "backends in this build: cpu\n"
          "exit codes: 0 = ok, 2 = usage / IO error, 3 = backend not available in this build\n";
}

struct Options {
    std::string backend;
    std::string scene;
    std::string out;
    int width = 256;
    int height = 256;
    std::vector<std::string> cvars;
    std::optional<int> benchFrames;
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

    // Real backends (d3d11, d3d12, vulkan, ...) register here in F2.
    if (opts->backend != "cpu") {
        std::cerr << "backend '" << opts->backend << "' not available in this build\n";
        return kExitNoBackend;
    }

    eng::CVar* warmup = eng::CVarRegistry::get().registerInt(
        "r.Bench.WarmupFrames", 0, "Untimed frames rendered before --bench timing starts", eng::CVarFlags::None, 0,
        1000);
    if (!applyCVars(opts->cvars)) {
        return kExitError;
    }

    if (opts->scene != kBuiltinTestPattern) {
        std::cerr << "headless: scene '" << opts->scene << "' not supported in this build (only "
                  << kBuiltinTestPattern << ")\n";
        return kExitError;
    }

    const auto renderFrame = [&] { return eng::makeTestPattern(opts->width, opts->height); };
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
