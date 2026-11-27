#include <doctest/doctest.h>

#include "Batch.h"
#include "Compare.h"
#include "Image.h"
#include "Utf8.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <ostream> // doctest stringifies std::string via operator<<
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using namespace eng::golden;

namespace {

fs::path fixture(const char* name) {
    return fromUtf8(ENG_GOLDEN_FIXTURE_DIR) / name;
}

Image load(const char* name) {
    auto img = loadPng(fixture(name));
    REQUIRE_MESSAGE(img.has_value(), (img ? std::string() : img.error()));
    return *img;
}

// A fresh, empty temp directory whose leaf name contains non-ASCII characters and spaces.
struct TempDir {
    fs::path path;
    explicit TempDir(const char* tag) {
        path = fs::temp_directory_path() / (std::string("eng-golden-") + tag) / fromUtf8("ç ğ ü test");
        std::error_code ec;
        fs::remove_all(path.parent_path(), ec);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path.parent_path(), ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

nlohmann::json readJson(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    REQUIRE(in.good());
    return nlohmann::json::parse(in);
}

void writeText(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

} // namespace

TEST_SUITE("golden") {

TEST_CASE("loadPng expands to RGBA8 and keeps the original channel count") {
    Image rgb = load("solid_red_64.png");
    CHECK(rgb.width == 64);
    CHECK(rgb.height == 64);
    CHECK(rgb.channels == 3);
    REQUIRE(rgb.rgba8.size() == 64u * 64u * 4u);
    CHECK(rgb.rgba8[0] == 255);
    CHECK(rgb.rgba8[1] == 0);
    CHECK(rgb.rgba8[2] == 0);
    CHECK(rgb.rgba8[3] == 255);

    Image gray = load("gray_64_l8.png");
    CHECK(gray.channels == 1);
    REQUIRE(gray.rgba8.size() == 64u * 64u * 4u);
    CHECK(gray.rgba8[0] == 128);
    CHECK(gray.rgba8[1] == 128);
    CHECK(gray.rgba8[2] == 128);
    CHECK(gray.rgba8[3] == 255);
}

TEST_CASE("loadPng reports missing and corrupt files as errors") {
    auto missing = loadPng(fixture("does_not_exist.png"));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().find("cannot open") != std::string::npos);

    TempDir dir("corrupt");
    writeText(dir.path / "bad.png", "this is not a png");
    auto bad = loadPng(dir.path / "bad.png");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().find("decode") != std::string::npos);
}

TEST_CASE("identical images: flip 0, psnr infinite") {
    auto r = compare(load("solid_red_64.png"), load("solid_red_64_copy.png"));
    REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error()));
    CHECK(r->flipMean < 1e-6);
    CHECK((std::isinf(r->psnr) || r->psnr >= 99.0));
    CHECK(r->diffHeatmap.width == 64);
    CHECK(r->diffHeatmap.height == 64);
    CHECK(r->diffHeatmap.rgba8.size() == 64u * 64u * 4u);
}

TEST_CASE("small local difference: flip above 0 and below 0.05") {
    auto r = compare(load("red_with_dot_64.png"), load("solid_red_64.png"));
    REQUIRE(r.has_value());
    CHECK(r->flipMean > 0.0);
    CHECK(r->flipMean < 0.05);
    CHECK(std::isfinite(r->psnr));
}

TEST_CASE("large difference: flip above 0.05") {
    auto r = compare(load("gray_64.png"), load("solid_red_64.png"));
    REQUIRE(r.has_value());
    CHECK(r->flipMean > 0.05);
}

TEST_CASE("size mismatch is an error, never a resize") {
    auto r = compare(load("solid_red_64.png"), load("solid_red_32.png"));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().find("size mismatch") != std::string::npos);
    CHECK(r.error().find("64x64 vs 32x32") != std::string::npos);
}

TEST_CASE("channel-count mismatch is an error") {
    auto r = compare(load("solid_red_64.png"), load("gray_64_l8.png"));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().find("channel mismatch") != std::string::npos);
}

TEST_CASE("UTF-8 path with spaces and non-ASCII: load and save") {
    TempDir dir("utf8");
    fs::path copy = dir.path / fromUtf8("kırmızı görüntü.png");
    fs::copy_file(fixture("solid_red_64.png"), copy);

    auto img = loadPng(copy);
    REQUIRE_MESSAGE(img.has_value(), (img ? std::string() : img.error()));
    CHECK(img->width == 64);

    fs::path saved = dir.path / fromUtf8("çıktı.png");
    REQUIRE(savePng(saved, *img));
    auto back = loadPng(saved);
    REQUIRE(back.has_value());
    CHECK(back->channels == 3);
    CHECK(back->rgba8 == img->rgba8);
}

TEST_CASE("batch: PASS, over-threshold, missing-reference, missing-test, size-mismatch") {
    TempDir dir("batch");
    fs::copy_file(fixture("solid_red_64.png"), dir.path / "red.png");
    fs::copy_file(fixture("solid_red_64_copy.png"), dir.path / "red_copy.png");
    fs::copy_file(fixture("gray_64.png"), dir.path / "gray.png");
    fs::copy_file(fixture("solid_red_32.png"), dir.path / "red32.png");

    const std::string manifest = R"({
      "phase": "f00",
      "cases": [
        {"name": "same", "test": "red.png", "ref": "red_copy.png", "thresholdFlip": 0.05},
        {"name": "different", "test": "gray.png", "ref": "red.png", "thresholdFlip": 0.05},
        {"name": "no_ref", "test": "red.png", "ref": "ref/does_not_exist.png"},
        {"name": "no_test", "test": "nothing.png", "ref": "red.png"},
        {"name": "sizes", "test": "red.png", "ref": "red32.png"}
      ]
    })";
    writeText(dir.path / "manifest.json", manifest);

    fs::path out = dir.path / "out";
    auto summary = runBatch(dir.path / "manifest.json", out);
    REQUIRE_MESSAGE(summary.has_value(), (summary ? std::string() : summary.error()));
    CHECK(summary->passed == 1);
    CHECK(summary->failed == 4);

    nlohmann::json j = readJson(out / "results.json");
    CHECK(j["phase"] == "f00");
    REQUIRE(j["cases"].size() == 5);
    auto c = j["cases"];

    CHECK(c[0]["name"] == "same");
    CHECK(c[0]["status"] == "PASS");
    CHECK(c[0]["reason"] == "ok");
    CHECK(c[0]["flipMean"].get<double>() < 1e-6);
    CHECK(c[0]["psnr"] == "inf");
    CHECK(c[0]["frameTimeMs"].is_null());
    REQUIRE(c[0]["diff"].is_string());
    CHECK(fs::exists(out / fromUtf8(c[0]["diff"].get<std::string>())));
    // Paths in results.json are relative to the results.json directory.
    CHECK(fs::exists(out / fromUtf8(c[0]["test"].get<std::string>())));

    CHECK(c[1]["status"] == "FAIL");
    CHECK(c[1]["reason"] == "over-threshold");
    CHECK(c[1]["flipMean"].get<double>() > 0.05);

    CHECK(c[2]["status"] == "FAIL");
    CHECK(c[2]["reason"] == "missing-reference");
    CHECK(c[2]["flipMean"].is_null());
    CHECK(c[2]["psnr"].is_null());
    CHECK(c[2]["diff"].is_null());

    CHECK(c[3]["status"] == "FAIL");
    CHECK(c[3]["reason"] == "missing-test");

    CHECK(c[4]["status"] == "FAIL");
    CHECK(c[4]["reason"] == "size-mismatch");
}

TEST_CASE("batch: non-ASCII case names and paths inside the manifest (UTF-8 JSON)") {
    TempDir dir("batchutf8");
    fs::create_directories(dir.path / fromUtf8("referans ğ"));
    fs::copy_file(fixture("solid_red_64.png"), dir.path / fromUtf8("görüntü ş.png"));
    fs::copy_file(fixture("solid_red_64_copy.png"), dir.path / fromUtf8("referans ğ") / fromUtf8("kırmızı.png"));
    // Source is compiled with /utf-8 (MSVC) or UTF-8 by default: these literals are UTF-8 bytes.
    writeText(dir.path / "manifest.json", R"({"phase":"f00","cases":[
        {"name":"kırmızı_çift","test":"görüntü ş.png","ref":"referans ğ/kırmızı.png"}]})");

    fs::path out = dir.path / fromUtf8("çıktı");
    auto summary = runBatch(dir.path / "manifest.json", out);
    REQUIRE_MESSAGE(summary.has_value(), (summary ? std::string() : summary.error()));
    CHECK(summary->passed == 1);

    nlohmann::json j = readJson(out / "results.json");
    REQUIRE(j["cases"].size() == 1);
    const auto& c = j["cases"][0];
    CHECK(c["name"] == "kırmızı_çift");
    CHECK(c["status"] == "PASS");
    CHECK(c["diff"] == "kırmızı_çift.diff.png");
    CHECK(c["test"] == "../görüntü ş.png");
    CHECK(c["ref"] == "../referans ğ/kırmızı.png");
    CHECK(fs::exists(out / fromUtf8("kırmızı_çift.diff.png")));
    CHECK(fs::exists(out / fromUtf8(c["ref"].get<std::string>())));
}

TEST_CASE("batch: invalid manifest is an error") {
    TempDir dir("badmanifest");
    writeText(dir.path / "manifest.json", "{ not json");
    auto r = runBatch(dir.path / "manifest.json", dir.path / "out");
    CHECK_FALSE(r.has_value());

    writeText(dir.path / "manifest2.json", R"({"phase":"f00","cases":[{"name":"../x","test":"a","ref":"b"}]})");
    auto r2 = runBatch(dir.path / "manifest2.json", dir.path / "out");
    CHECK_FALSE(r2.has_value());

    auto r3 = runBatch(dir.path / "missing.json", dir.path / "out");
    CHECK_FALSE(r3.has_value());
}

TEST_CASE("alpha: compared by default when an image has non-opaque alpha, ignored on request") {
    const Image opaque = load("alpha_opaque_64.png");
    const Image half = load("alpha_half_64.png");
    REQUIRE(opaque.channels == 4);
    REQUIRE(half.channels == 4);
    auto byDefault = compare(half, opaque);
    REQUIRE_MESSAGE(byDefault.has_value(), (byDefault ? std::string() : byDefault.error()));
    CHECK(byDefault->flipMean > kDefaultThresholdFlip); // FAIL at the default threshold

    CompareOptions ignore;
    ignore.alpha = CompareOptions::Alpha::Ignore;
    auto ignored = compare(half, opaque, ignore);
    REQUIRE(ignored.has_value());
    CHECK(ignored->flipMean < 1e-6); // PASS: RGB is identical

    CompareOptions explicitCompare;
    explicitCompare.alpha = CompareOptions::Alpha::Compare;
    auto compared = compare(half, opaque, explicitCompare);
    REQUIRE(compared.has_value());
    CHECK(compared->flipMean == doctest::Approx(byDefault->flipMean));
}

TEST_CASE("FLIP mean matches NVIDIA's reference implementation within 1e-3") {
    auto a = loadPng(fixture("flip_upstream/a.png"));
    auto b = loadPng(fixture("flip_upstream/b.png"));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    const nlohmann::json expected = readJson(fixture("flip_upstream/expected.json"));
    const double upstream = expected.at("flipMean").get<double>();
    auto r = compare(*b, *a); // test = b, reference = a, as upstream was run
    REQUIRE(r.has_value());
    MESSAGE("ours " << r->flipMean << " upstream " << upstream);
    CHECK(std::abs(r->flipMean - upstream) <= 1e-3);
}

TEST_CASE("batch: detail messages use repo-relative paths") {
    TempDir dir("detail");
    const fs::path repo = dir.path / "repo";
    fs::create_directories(repo / ".git");
    fs::create_directories(repo / "cases");
    fs::copy_file(fixture("solid_red_64.png"), repo / "cases" / "red.png");
    writeText(repo / "cases" / "manifest.json",
              R"({"phase":"f00","cases":[{"name":"no_ref","test":"red.png","ref":"ref/does_not_exist.png"}]})");
    auto summary = runBatch(repo / "cases" / "manifest.json", repo / "out");
    REQUIRE_MESSAGE(summary.has_value(), (summary ? std::string() : summary.error()));
    const nlohmann::json j = readJson(repo / "out" / "results.json");
    const std::string detail = j["cases"][0]["detail"].get<std::string>();
    CAPTURE(detail);
    CHECK(detail.find("'cases/ref/does_not_exist.png'") != std::string::npos);
    CHECK(detail.find(toUtf8Generic(dir.path)) == std::string::npos);
    CHECK(detail.find(toUtf8(dir.path)) == std::string::npos);
}

} // TEST_SUITE
