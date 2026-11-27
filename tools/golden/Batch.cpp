#include "Batch.h"

#include "Compare.h"
#include "Image.h"
#include "Utf8.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <exception>
#include <fstream>
#include <set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;

namespace eng::golden {

namespace {

std::string relativeUtf8(const fs::path& p, const fs::path& baseDir) {
    std::error_code ec;
    const fs::path absP = fs::absolute(p, ec);
    if (ec) {
        return toUtf8Generic(p);
    }
    const fs::path absBase = fs::absolute(baseDir, ec);
    if (ec) {
        return toUtf8Generic(absP);
    }
    const fs::path rel = absP.lexically_normal().lexically_relative(absBase.lexically_normal());
    return rel.empty() ? toUtf8Generic(absP.lexically_normal()) : toUtf8Generic(rel);
}

json caseJsonObject(const CaseResult& c, const fs::path& baseDir) {
    json j;
    j["name"] = c.name;
    j["status"] = c.pass ? "PASS" : "FAIL";
    j["reason"] = c.reason;
    j["detail"] = c.detail;
    j["flipMean"] = c.flipMean ? json(*c.flipMean) : json(nullptr);
    if (!c.psnr) {
        j["psnr"] = nullptr;
    } else if (std::isinf(*c.psnr)) {
        j["psnr"] = "inf";
    } else {
        j["psnr"] = *c.psnr;
    }
    j["thresholdFlip"] = c.thresholdFlip;
    j["test"] = relativeUtf8(c.test, baseDir);
    j["ref"] = relativeUtf8(c.ref, baseDir);
    j["diff"] = c.diff ? json(relativeUtf8(*c.diff, baseDir)) : json(nullptr);
    j["frameTimeMs"] = c.frameTimeMs ? json(*c.frameTimeMs) : json(nullptr);
    return j;
}

// Case names become file names: reject anything that is not a plain, portable file name.
bool validCaseName(const std::string& name) {
    if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ') {
        return false;
    }
    for (const char ch : name) {
        const auto u = static_cast<unsigned char>(ch);
        if (u < 0x20 || u == 0x7F) {
            return false;
        }
        switch (ch) {
            case '<': case '>': case ':': case '"': case '/': case '\\': case '|': case '?': case '*':
                return false;
            default:
                break;
        }
    }
    return true;
}

std::string lowerAscii(std::string s) {
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return s;
}

struct ManifestCase {
    std::string name;
    fs::path test;
    fs::path ref;
    double thresholdFlip = kDefaultThresholdFlip;
    std::optional<double> frameTimeMs;
};

std::expected<std::pair<std::string, std::vector<ManifestCase>>, std::string> readManifest(const fs::path& manifest) {
    std::ifstream in(manifest, std::ios::binary);
    if (!in) {
        return std::unexpected("cannot open manifest '" + toUtf8(manifest) + "'");
    }
    json j;
    try {
        j = json::parse(in);
    } catch (const json::exception& e) {
        return std::unexpected("invalid manifest JSON '" + toUtf8(manifest) + "': " + e.what());
    }
    if (!j.is_object() || !j.contains("phase") || !j["phase"].is_string() || !j.contains("cases") ||
        !j["cases"].is_array()) {
        return std::unexpected(std::string("manifest must be an object with string 'phase' and array 'cases'"));
    }
    const fs::path base = manifest.parent_path();
    std::vector<ManifestCase> cases;
    std::set<std::string> seen;
    for (const json& c : j["cases"]) {
        if (!c.is_object() || !c.contains("name") || !c["name"].is_string() || !c.contains("test") ||
            !c["test"].is_string() || !c.contains("ref") || !c["ref"].is_string()) {
            return std::unexpected(std::string("each case needs string 'name', 'test' and 'ref'"));
        }
        ManifestCase mc;
        mc.name = c["name"].get<std::string>();
        if (!validCaseName(mc.name)) {
            return std::unexpected("invalid case name '" + mc.name + "' (must be a plain file name)");
        }
        if (!seen.insert(lowerAscii(mc.name)).second) {
            return std::unexpected("duplicate case name '" + mc.name + "'");
        }
        mc.test = base / fromUtf8(c["test"].get<std::string>());
        mc.ref = base / fromUtf8(c["ref"].get<std::string>());
        if (c.contains("thresholdFlip")) {
            if (!c["thresholdFlip"].is_number() || !std::isfinite(c["thresholdFlip"].get<double>()) ||
                c["thresholdFlip"].get<double>() < 0.0) {
                return std::unexpected("case '" + mc.name + "': thresholdFlip must be a finite number >= 0");
            }
            mc.thresholdFlip = c["thresholdFlip"].get<double>();
        }
        if (c.contains("frameTimeMs") && !c["frameTimeMs"].is_null()) {
            if (!c["frameTimeMs"].is_number()) {
                return std::unexpected("case '" + mc.name + "': frameTimeMs must be a number or null");
            }
            mc.frameTimeMs = c["frameTimeMs"].get<double>();
        }
        cases.push_back(std::move(mc));
    }
    return std::pair{j["phase"].get<std::string>(), std::move(cases)};
}

} // namespace

std::expected<CaseResult, std::string> evaluateCase(std::string name, const fs::path& test, const fs::path& ref,
                                                    double thresholdFlip, const fs::path& diffOut) {
    CaseResult r;
    r.name = std::move(name);
    r.test = test;
    r.ref = ref;
    r.thresholdFlip = thresholdFlip;

    auto testImg = loadPng(test);
    if (!testImg) {
        r.reason = "missing-test";
        r.detail = testImg.error();
        return r;
    }
    auto refImg = loadPng(ref);
    if (!refImg) {
        r.reason = "missing-reference";
        r.detail = refImg.error();
        return r;
    }
    auto cmp = compare(*testImg, *refImg);
    if (!cmp) {
        r.reason = "size-mismatch";
        r.detail = cmp.error();
        return r;
    }
    r.flipMean = cmp->flipMean;
    r.psnr = cmp->psnr;
    if (!savePng(diffOut, cmp->diffHeatmap)) {
        return std::unexpected("cannot write diff image '" + toUtf8(diffOut) + "'");
    }
    r.diff = diffOut;
    r.pass = cmp->flipMean <= thresholdFlip;
    r.reason = r.pass ? "ok" : "over-threshold";
    if (!r.pass) {
        r.detail = "flipMean " + std::to_string(cmp->flipMean) + " > threshold " + std::to_string(thresholdFlip);
    }
    return r;
}

std::string caseToJson(const CaseResult& c, const fs::path& baseDir) {
    return caseJsonObject(c, baseDir).dump(2);
}

std::expected<BatchSummary, std::string> runBatch(const fs::path& manifest, const fs::path& outDir) {
    try {
        auto parsed = readManifest(manifest);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        auto& [phase, cases] = *parsed;

        std::error_code ec;
        fs::create_directories(outDir, ec);
        if (ec || !fs::is_directory(outDir)) {
            return std::unexpected("cannot create output directory '" + toUtf8(outDir) + "'");
        }

        BatchSummary summary;
        json results;
        results["phase"] = phase;
        results["cases"] = json::array();
        for (ManifestCase& mc : cases) {
            const fs::path diffOut = outDir / fromUtf8(mc.name + ".diff.png");
            fs::remove(diffOut, ec); // never leave a stale heatmap from a previous run
            auto r = evaluateCase(mc.name, mc.test, mc.ref, mc.thresholdFlip, diffOut);
            if (!r) {
                return std::unexpected(r.error());
            }
            r->frameTimeMs = mc.frameTimeMs;
            (r->pass ? summary.passed : summary.failed) += 1;
            results["cases"].push_back(caseJsonObject(*r, outDir));
        }

        summary.resultsJson = outDir / "results.json";
        std::ofstream out(summary.resultsJson, std::ios::binary | std::ios::trunc);
        out << results.dump(2) << '\n';
        out.close();
        if (!out) {
            return std::unexpected("cannot write '" + toUtf8(summary.resultsJson) + "'");
        }
        return summary;
    } catch (const std::exception& e) {
        return std::unexpected(std::string("batch failed: ") + e.what());
    }
}

} // namespace eng::golden
