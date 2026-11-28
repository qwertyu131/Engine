// Conformance over glTF-Sample-Assets (plan F3 Task 2, spec §7.3.1).
//
// For every model under tests/assets/glTF-Sample-Assets/Models whose variant directory is glTF, glTF-Binary or
// glTF-Embedded:
//   - the pinned Khronos glTF-Validator (tests/assets/tools, scripts/fetch_assets.py) validates the file;
//   - the loader loads it. Expected outcome, decided here from the file's own extensionsRequired (independent of
//     the loader's extension table):
//       * every required extension is one the F3 Task 2 loader supports (KHR_lights_punctual, KHR_xmp_json_ld)
//         -> loads with 0 errors;
//       * otherwise -> exit 2 naming the first such extension (plan F3 Global Constraints: a later-phase or
//         unknown REQUIRED extension is an error). This overrides "loads with 0 errors" for those files; they are
//         listed in conformance.json with expected = "exit2" and the reason;
//   - a validator error is a loader error, case by case (a file the validator rejects must not load);
//   - an extension planned for F4-F16 that is only in extensionsUsed loads with an info message naming it.
// The list and results go to build/f03/conformance.json. Missing assets or validator: failure, never a skip.
#include "AssetTestEnv.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <stdio.h>
#endif

using namespace eng::asset;
using namespace eng::asset::test;
using eng::Result;
namespace fs = std::filesystem;

namespace {

struct Case {
    std::string model, variant;
    fs::path file;
    std::vector<std::string> required, used;
    int validatorErrors = -1; // -1 = the validator did not run / report
    std::string validatorFirst;
    bool loaded = false;
    std::string loaderMessage;
    std::vector<std::string> infos, warnings;
    std::string expected; // "load" | "exit2"
    std::string expectedReason;
    bool pass = false;
    std::string why;
};

// Extensions the Task 2 loader supports when REQUIRED (spec §7.2 / plan F3): KHR_lights_punctual (parsed into
// ModelData::lights) and KHR_xmp_json_ld (metadata, no render effect). Every other required extension is an exit 2.
bool supportedRequired(const std::string& e) {
    return e == "KHR_lights_punctual" || e == "KHR_xmp_json_ld";
}

// Extensions with a planned phase F4..F16 (spec §11 / §7.2): in extensionsUsed they must give an info message.
bool laterPhase(const std::string& e) {
    if (e == "KHR_materials_pbrSpecularGlossiness" || e == "KHR_materials_scatter") {
        return false; // archived / not in the registry: unknown to the loader (warning, not info)
    }
    if (e.rfind("KHR_materials_", 0) == 0) {
        return true; // F6
    }
    return e == "KHR_animation_pointer" || e == "KHR_texture_transform" || e == "EXT_mesh_gpu_instancing" ||
           e == "KHR_node_visibility" || e == "KHR_interactivity" || e == "KHR_node_hoverability" ||
           e == "KHR_node_selectability" || e == "KHR_gaussian_splatting" || e == "EXT_lights_image_based";
}

std::vector<std::string> stringList(const nlohmann::json& j, const char* key) {
    std::vector<std::string> out;
    if (j.contains(key) && j[key].is_array()) {
        for (const auto& v : j[key]) {
            if (v.is_string()) {
                out.push_back(v.get<std::string>());
            }
        }
    }
    return out;
}

// The JSON of a .gltf / .glb (GLB: chunk 0), for extensionsRequired / extensionsUsed.
nlohmann::json readJson(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<char> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (b.size() >= 20 && std::string(b.data(), 4) == "glTF") {
        uint32_t len = 0;
        std::memcpy(&len, b.data() + 12, 4);
        if (20ull + len <= b.size()) {
            return nlohmann::json::parse(b.begin() + 20, b.begin() + 20 + len, nullptr, false);
        }
        return nlohmann::json();
    }
    return nlohmann::json::parse(b.begin(), b.end(), nullptr, false);
}

// Runs the validator with -o (JSON report on stdout).
void validate(Case& c) {
#if defined(_WIN32)
    const std::wstring cmd = L"\"\"" + u8path(ENG_TEST_GLTF_VALIDATOR).wstring() + L"\" -o \"" + c.file.wstring() + L"\"\"";
    FILE* f = _wpopen(cmd.c_str(), L"rb");
#else
    const std::string cmd = "\"" + std::string(ENG_TEST_GLTF_VALIDATOR) + "\" -o \"" + c.file.string() + "\"";
    FILE* f = popen(cmd.c_str(), "r");
#endif
    if (!f) {
        c.validatorFirst = "cannot start the validator";
        return;
    }
    std::string out;
    char buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) {
        out.append(buf, n);
    }
#if defined(_WIN32)
    _pclose(f);
#else
    pclose(f);
#endif
    const nlohmann::json r = nlohmann::json::parse(out, nullptr, false);
    if (!r.is_object() || !r.contains("issues")) {
        c.validatorFirst = "no JSON report: " + out.substr(0, 200);
        return;
    }
    c.validatorErrors = r["issues"].value("numErrors", -1);
    for (const auto& m : r["issues"].value("messages", nlohmann::json::array())) {
        if (m.value("severity", 9) == 0) {
            c.validatorFirst = m.value("code", std::string()) + ": " + m.value("message", std::string());
            break;
        }
    }
}

} // namespace

TEST_SUITE("asset.conformance") {

TEST_CASE("glTF-Sample-Assets glTF / glTF-Binary / glTF-Embedded: loader result = validator + extension policy") {
    const fs::path validator = u8path(ENG_TEST_GLTF_VALIDATOR);
    REQUIRE_MESSAGE(fs::is_regular_file(validator),
                    "glTF-Validator missing: " << toUtf8(validator) << " (run python scripts/fetch_assets.py)");
    std::vector<Case> cases;
    std::vector<fs::path> models;
    for (const auto& e : fs::directory_iterator(sampleModels())) {
        if (e.is_directory()) {
            models.push_back(e.path());
        }
    }
    std::sort(models.begin(), models.end());
    for (const fs::path& m : models) {
        for (const char* variant : {"glTF", "glTF-Binary", "glTF-Embedded"}) {
            const fs::path dir = m / variant;
            if (!fs::is_directory(dir)) {
                continue;
            }
            std::vector<fs::path> files;
            for (const auto& f : fs::directory_iterator(dir)) {
                const auto ext = f.path().extension();
                if (ext == ".gltf" || ext == ".glb") {
                    files.push_back(f.path());
                }
            }
            std::sort(files.begin(), files.end());
            for (const fs::path& f : files) {
                Case c;
                c.model = toUtf8(m.filename());
                c.variant = variant;
                c.file = f;
                const nlohmann::json j = readJson(f);
                c.required = stringList(j, "extensionsRequired");
                c.used = stringList(j, "extensionsUsed");
                cases.push_back(std::move(c));
            }
        }
    }
    REQUIRE(cases.size() > 250); // 308 files at the pinned commit
    // The malformed fixtures (tests/assets/f03/bad) go through the same rule: every file the validator rejects must
    // fail to load (all Sample-Assets files are validator-clean at the pinned commit, so these are the cases that
    // exercise "validator error = loader error").
    const size_t sampleCases = cases.size();
    {
        std::vector<fs::path> bad;
        for (const auto& f : fs::directory_iterator(f03Dir() / "bad")) {
            const auto ext = f.path().extension();
            if (ext == ".gltf" || ext == ".glb") {
                bad.push_back(f.path());
            }
        }
        std::sort(bad.begin(), bad.end());
        for (const fs::path& f : bad) {
            Case c;
            c.model = "f03/bad";
            c.variant = "fixture";
            c.file = f;
            const nlohmann::json j = readJson(f);
            c.required = stringList(j, "extensionsRequired");
            c.used = stringList(j, "extensionsUsed");
            cases.push_back(std::move(c));
        }
    }

    // Validator: parallel (one process per file).
    {
        const size_t workers = std::max(2u, std::thread::hardware_concurrency());
        std::vector<std::future<void>> jobs;
        size_t next = 0;
        std::mutex mu;
        for (size_t w = 0; w < workers; ++w) {
            jobs.push_back(std::async(std::launch::async, [&] {
                for (;;) {
                    size_t i;
                    {
                        std::lock_guard lock(mu);
                        if (next >= cases.size()) {
                            return;
                        }
                        i = next++;
                    }
                    validate(cases[i]);
                }
            }));
        }
        for (auto& j : jobs) {
            j.get();
        }
    }

    size_t failed = 0, expectedExit2 = 0, infoChecked = 0, validatorRejected = 0;
    for (Case& c : cases) {
        std::vector<Diagnostic> diags;
        const Result<ModelData> r = loadGltf(c.file, LoadOptions{}, &diags);
        c.loaded = r.has_value();
        c.loaderMessage = r ? std::string() : r.error().message;
        for (const Diagnostic& d : diags) {
            (d.level == Diagnostic::Info ? c.infos : c.warnings).push_back(d.message);
        }
        const auto unsupported = std::find_if(c.required.begin(), c.required.end(),
                                              [](const std::string& e) { return !supportedRequired(e); });
        if (c.model == "f03/bad") {
            // Every malformed fixture must be rejected (test_bad_files checks the messages).
            c.expected = "exit2";
            c.expectedReason = "malformed fixture (tests/assets/f03/bad)";
            c.pass = !c.loaded && r.error().exitCode == 2;
            if (!c.pass) {
                c.why = "a malformed fixture loaded";
            }
        } else if (unsupported != c.required.end()) {
            c.expected = "exit2";
            c.expectedReason = "extensionsRequired has " + *unsupported + " (not supported by the F3 Task 2 loader)";
            ++expectedExit2;
            c.pass = !c.loaded && r.error().exitCode == 2 && c.loaderMessage.find(*unsupported) != std::string::npos;
            if (!c.pass) {
                c.why = c.loaded ? "loaded" : "message does not name " + *unsupported;
            }
        } else {
            c.expected = "load";
            c.pass = c.loaded;
            if (!c.loaded) {
                c.why = "loader error: " + c.loaderMessage;
            }
        }
        if (c.validatorErrors < 0) {
            c.pass = false;
            c.why = "validator did not report: " + c.validatorFirst;
        } else if (c.validatorErrors > 0 && c.loaded) {
            c.pass = false;
            c.why = "the validator rejects it (" + c.validatorFirst + ") but it loaded";
        }
        if (c.loaded) {
            for (const std::string& e : c.used) {
                if (laterPhase(e) && std::find(c.required.begin(), c.required.end(), e) == c.required.end()) {
                    const bool named = std::any_of(c.infos.begin(), c.infos.end(),
                                                   [&](const std::string& m) { return m.find(e) != std::string::npos; });
                    ++infoChecked;
                    if (!named) {
                        c.pass = false;
                        c.why = "no info message for the later-phase extension " + e;
                    }
                }
            }
        }
        validatorRejected += c.validatorErrors > 0 ? 1 : 0;
        failed += c.pass ? 0 : 1;
    }

    nlohmann::ordered_json out;
    out["comment"] = "plan F3 Task 2 test_conformance: every glTF / glTF-Binary / glTF-Embedded file of "
                     "glTF-Sample-Assets, pinned glTF-Validator result vs loader result";
    out["total"] = cases.size();
    out["failed"] = failed;
    out["expectedExit2"] = expectedExit2;
    out["laterPhaseInfoChecks"] = infoChecked;
    out["sampleAssetFiles"] = sampleCases;
    out["badFixtures"] = cases.size() - sampleCases;
    out["validatorRejected"] = validatorRejected;
    for (const Case& c : cases) {
        nlohmann::ordered_json e;
        e["model"] = c.model;
        e["variant"] = c.variant;
        e["file"] = toUtf8(c.file.filename());
        e["extensionsRequired"] = c.required;
        e["validatorErrors"] = c.validatorErrors;
        if (!c.validatorFirst.empty()) {
            e["validatorFirstError"] = c.validatorFirst;
        }
        e["expected"] = c.expected;
        if (!c.expectedReason.empty()) {
            e["expectedReason"] = c.expectedReason;
        }
        e["loaded"] = c.loaded;
        if (!c.loaderMessage.empty()) {
            e["loaderError"] = c.loaderMessage;
        }
        e["warnings"] = c.warnings;
        e["infos"] = c.infos;
        e["pass"] = c.pass;
        if (!c.why.empty()) {
            e["why"] = c.why;
        }
        out["cases"].push_back(std::move(e));
    }
    const fs::path outFile = u8path(ENG_TEST_CONFORMANCE_OUT);
    fs::create_directories(outFile.parent_path());
    std::ofstream(outFile, std::ios::binary) << out.dump(1) << "\n";
    MESSAGE("conformance: " << cases.size() << " files, " << failed << " failed, " << expectedExit2
                            << " expected exit 2 (required extension), " << infoChecked
                            << " later-phase info checks -> " << toUtf8(outFile));
    for (const Case& c : cases) {
        CHECK_MESSAGE(c.pass, c.model << "/" << c.variant << "/" << toUtf8(c.file.filename()) << ": " << c.why);
    }
    CHECK(infoChecked > 0);
    CHECK(validatorRejected > 0); // the validator-error rule was exercised (on the bad fixtures)
}

} // TEST_SUITE
