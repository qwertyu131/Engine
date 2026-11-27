// golden: image diff CLI (NVIDIA FLIP + PSNR).
//
//   golden compare --test <png> --ref <png> --out-diff <png> [--threshold-flip <float=0.05>] [--json <file>]
//   golden batch --manifest <json> --out <dir>
//
// Exit codes: 0 = all PASS, 1 = at least one FAIL, 2 = usage / IO error.
// In `compare`, an unreadable input or a size / channel-count mismatch is an IO/usage error (2);
// in `batch` the same conditions are per-case FAIL results (missing-test, missing-reference,
// size-mismatch) so one bad case does not hide the others.
// All path arguments are UTF-8 (on Windows they are taken from the wide command line).

#include "Batch.h"
#include "Utf8.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace eng::golden;

namespace {

constexpr int kExitPass = 0;
constexpr int kExitFail = 1;
constexpr int kExitError = 2;

void printUsage(std::ostream& os) {
    os << "usage:\n"
          "  golden compare --test <png> --ref <png> --out-diff <png> [--threshold-flip <float=0.05>] [--json <file>]\n"
          "  golden batch --manifest <json> --out <dir>\n"
          "exit codes: 0 = all PASS, 1 = at least one FAIL, 2 = usage / IO error\n";
}

// Parses "--key value" pairs. Returns nullopt (after printing why) on unknown or valueless keys.
std::optional<std::map<std::string, std::string>> parseOptions(const std::vector<std::string>& args, size_t first,
                                                               const std::vector<std::string_view>& allowed) {
    std::map<std::string, std::string> opts;
    for (size_t i = first; i < args.size(); ++i) {
        const std::string& key = args[i];
        bool known = false;
        for (std::string_view a : allowed) {
            known = known || key == a;
        }
        if (!known) {
            std::cerr << "golden: unknown option '" << key << "'\n";
            return std::nullopt;
        }
        if (i + 1 >= args.size()) {
            std::cerr << "golden: option '" << key << "' needs a value\n";
            return std::nullopt;
        }
        opts[key] = args[++i];
    }
    return opts;
}

std::optional<double> parseDouble(const std::string& s) {
    double v = 0.0;
    const char* end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(s.data(), end, v); // locale-independent ('.' decimal)
    if (ec != std::errc() || ptr != end || !std::isfinite(v)) {
        return std::nullopt;
    }
    return v;
}

std::string formatPsnr(const std::optional<double>& psnr) {
    if (!psnr) {
        return "n/a";
    }
    return std::isinf(*psnr) ? "inf" : std::to_string(*psnr);
}

int runCompare(const std::vector<std::string>& args) {
    auto opts = parseOptions(args, 2, {"--test", "--ref", "--out-diff", "--threshold-flip", "--json"});
    if (!opts) {
        printUsage(std::cerr);
        return kExitError;
    }
    for (const char* req : {"--test", "--ref", "--out-diff"}) {
        if (!opts->contains(req)) {
            std::cerr << "golden compare: missing required option " << req << "\n";
            printUsage(std::cerr);
            return kExitError;
        }
    }
    double threshold = kDefaultThresholdFlip;
    if (opts->contains("--threshold-flip")) {
        auto t = parseDouble(opts->at("--threshold-flip"));
        if (!t || *t < 0.0) {
            std::cerr << "golden compare: --threshold-flip must be a number >= 0 (got '"
                      << opts->at("--threshold-flip") << "')\n";
            return kExitError;
        }
        threshold = *t;
    }

    auto r = evaluateCase("compare", fromUtf8(opts->at("--test")), fromUtf8(opts->at("--ref")), threshold,
                          fromUtf8(opts->at("--out-diff")));
    if (!r) {
        std::cerr << "golden compare: " << r.error() << "\n";
        return kExitError;
    }
    if (r->reason == "missing-test" || r->reason == "missing-reference" || r->reason == "size-mismatch") {
        std::cerr << "golden compare: " << r->reason << ": " << r->detail << "\n";
        return kExitError;
    }

    std::cout << (r->pass ? "PASS" : "FAIL") << " reason=" << r->reason << " flipMean=" << *r->flipMean
              << " psnr=" << formatPsnr(r->psnr) << " threshold=" << threshold << "\n";

    if (opts->contains("--json")) {
        const fs::path jsonPath = fromUtf8(opts->at("--json"));
        std::ofstream out(jsonPath, std::ios::binary | std::ios::trunc);
        out << caseToJson(*r, jsonPath.parent_path()) << '\n';
        out.close();
        if (!out) {
            std::cerr << "golden compare: cannot write '" << toUtf8(jsonPath) << "'\n";
            return kExitError;
        }
    }
    return r->pass ? kExitPass : kExitFail;
}

int runBatchCommand(const std::vector<std::string>& args) {
    auto opts = parseOptions(args, 2, {"--manifest", "--out"});
    if (!opts || !opts->contains("--manifest") || !opts->contains("--out")) {
        if (opts) {
            std::cerr << "golden batch: --manifest and --out are required\n";
        }
        printUsage(std::cerr);
        return kExitError;
    }
    auto summary = runBatch(fromUtf8(opts->at("--manifest")), fromUtf8(opts->at("--out")));
    if (!summary) {
        std::cerr << "golden batch: " << summary.error() << "\n";
        return kExitError;
    }
    std::cout << "passed=" << summary->passed << " failed=" << summary->failed
              << " results=" << toUtf8(summary->resultsJson) << "\n";
    return summary->failed == 0 ? kExitPass : kExitFail;
}

int run(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        printUsage(std::cerr);
        return kExitError;
    }
    const std::string& cmd = args[1];
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        printUsage(std::cout);
        return kExitPass;
    }
    if (cmd == "compare") {
        return runCompare(args);
    }
    if (cmd == "batch") {
        return runBatchCommand(args);
    }
    std::cerr << "golden: unknown command '" << cmd << "'\n";
    printUsage(std::cerr);
    return kExitError;
}

int guardedRun(const std::vector<std::string>& args) {
    try {
        return run(args);
    } catch (const std::exception& e) {
        std::cerr << "golden: error: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "golden: unknown error\n";
    }
    return kExitError;
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    try {
        for (int i = 0; i < argc; ++i) {
            args.push_back(toUtf8(fs::path(argv[i]))); // UTF-16 -> UTF-8, independent of the ANSI codepage
        }
    } catch (const std::exception& e) {
        std::cerr << "golden: cannot decode command line: " << e.what() << "\n";
        return kExitError;
    }
    return guardedRun(args);
}
#else
int main(int argc, char** argv) {
    return guardedRun(std::vector<std::string>(argv, argv + argc));
}
#endif
