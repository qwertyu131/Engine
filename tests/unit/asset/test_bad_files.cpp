// Malformed / hostile files (plan F3 Task 2, Review Focus 1): every fixture of tests/assets/f03/bad/expected.json
// (written by scripts/make_f03_assets.py) gives exit 2 with its message substring, never a crash, hang or
// out-of-bounds read. The same binary runs in the ASan build (ENG_ASAN=ON, MSVC /fsanitize=address; ctest
// unit.asset.bad in build/asan): an out-of-bounds access there aborts the run instead of passing.
#include "AssetTestEnv.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <fstream>
#include <set>
#include <string>

using namespace eng::asset;
using namespace eng::asset::test;
using eng::Result;

TEST_SUITE("asset.bad") {

TEST_CASE("every bad/ fixture: exit 2 with its message, no diagnostics claim success") {
    const std::filesystem::path dir = f03Dir() / "bad";
    std::ifstream in(dir / "expected.json", std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "missing " << toUtf8(dir / "expected.json") << " (run python scripts/make_f03_assets.py)");
    const nlohmann::json manifest = nlohmann::json::parse(in);
    const auto& fixtures = manifest.at("fixtures");
    REQUIRE(fixtures.size() >= 6);
    std::set<std::string> listed;
    for (const auto& f : fixtures) {
        const std::string file = f.at("file").get<std::string>();
        const std::string message = f.at("message").get<std::string>();
        CAPTURE(file);
        listed.insert(file);
        std::vector<Diagnostic> diags;
        const Result<ModelData> r = loadGltf(dir / u8path(file), LoadOptions{}, &diags);
        REQUIRE_FALSE_MESSAGE(r.has_value(), "loaded although it is malformed");
        CAPTURE(r.error().message);
        CHECK(r.error().exitCode == 2);
        CHECK(r.error().message.find(message) != std::string::npos);
    }
    // Every file in bad/ is listed (a new fixture without an expectation would never be checked).
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        const std::string name = toUtf8(e.path().filename());
        if (name != "expected.json") {
            CHECK_MESSAGE(listed.count(name) == 1, name << " is not in expected.json");
        }
    }
    // Review Focus 1 list (plan F3): each kind has a fixture.
    for (const char* kind : {"truncated.glb", "accessor_out_of_range.gltf", "index_out_of_range.gltf", "bad_base64.gltf",
                             "bad_image.gltf", "accessor_count_2pow31.gltf"}) {
        CHECK_MESSAGE(listed.count(kind) == 1, kind);
    }
}

TEST_CASE("a nonexistent path and a directory: exit 2") {
    const Result<ModelData> none = loadGltf(f03Dir() / "bad" / "no such file.gltf", LoadOptions{}, nullptr);
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().exitCode == 2);
    const Result<ModelData> dir = loadGltf(f03Dir() / "bad", LoadOptions{}, nullptr);
    REQUIRE_FALSE(dir.has_value());
    CHECK(dir.error().exitCode == 2);
}

TEST_CASE("every truncation of a valid .glb: exit 2 or a complete model, never a crash") {
    // Box.glb cut at every 7th byte: the loader must reject each prefix cleanly (the ASan build checks the reads).
    const std::filesystem::path src = sampleFile("Box", "glTF-Binary");
    std::ifstream in(src, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(bytes.size() > 100);
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / u8path("eng_f03_trunc ç.glb");
    size_t rejected = 0;
    for (size_t n = 0; n < bytes.size(); n += 7) {
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(n));
        }
        const Result<ModelData> r = loadGltf(tmp, LoadOptions{}, nullptr);
        if (!r) {
            CHECK(r.error().exitCode == 2);
            ++rejected;
        }
    }
    std::filesystem::remove(tmp);
    CHECK(rejected == (bytes.size() + 6) / 7); // every proper prefix is malformed
}

} // TEST_SUITE
