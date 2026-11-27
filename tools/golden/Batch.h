#pragma once
// Case evaluation shared by `golden compare` and `golden batch`, plus the batch runner.
//
// results.json schema (one object per case, in manifest order):
//   {"phase": str, "cases": [{
//       "name": str, "status": "PASS"|"FAIL",
//       "reason": "ok"|"over-threshold"|"missing-reference"|"missing-test"|"size-mismatch",
//       "detail": str,                     // human-readable message ("" when ok)
//       "flipMean": number|null,           // null when no comparison happened
//       "psnr": number|"inf"|null,         // "inf" = identical images
//       "thresholdFlip": number,
//       "test": str, "ref": str,           // UTF-8, '/' separated
//       "diff": str|null,                  // FLIP magma heatmap PNG, null when not produced
//       "frameTimeMs": number|null }]}
// Path rule: every path in results.json is relative to the directory containing results.json
// (absolute only when no relative form exists, e.g. another drive). Manifest paths are
// relative to the manifest's directory (or absolute).

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace eng::golden {

inline constexpr double kDefaultThresholdFlip = 0.05;

struct CaseResult {
    std::string name;
    bool pass = false;
    std::string reason; // see schema above
    std::string detail;
    std::optional<double> flipMean;
    std::optional<double> psnr; // may be +infinity
    double thresholdFlip = kDefaultThresholdFlip;
    std::filesystem::path test;
    std::filesystem::path ref;
    std::optional<std::filesystem::path> diff; // set only when the heatmap was written
    std::optional<double> frameTimeMs;
};

// Loads both images, compares them and writes the heatmap to `diffOut`.
// Unreadable test -> missing-test; unreadable ref -> missing-reference; size or channel count
// differ -> size-mismatch; flipMean > threshold -> over-threshold. Returns an error only when
// the heatmap cannot be written (an IO error, not a test outcome).
std::expected<CaseResult, std::string> evaluateCase(std::string name, const std::filesystem::path& test,
                                                    const std::filesystem::path& ref, double thresholdFlip,
                                                    const std::filesystem::path& diffOut);

// Serializes one case as a JSON object (see schema), paths relative to `baseDir`.
std::string caseToJson(const CaseResult& c, const std::filesystem::path& baseDir);

struct BatchSummary {
    int passed = 0;
    int failed = 0;
    std::filesystem::path resultsJson;
};

// Runs every manifest case, writes <outDir>/<name>.diff.png and <outDir>/results.json.
// Error (no results written) for an unreadable/invalid manifest, an invalid or duplicate case
// name, or an IO failure in outDir.
std::expected<BatchSummary, std::string> runBatch(const std::filesystem::path& manifest,
                                                  const std::filesystem::path& outDir);

} // namespace eng::golden
