#pragma once
// Paths of the asset tests (plan F3 Task 2), from UTF-8 compile definitions (CMake passes source-tree paths).
#include "asset/GltfLoader.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace eng::asset::test {

inline std::filesystem::path u8path(std::string_view utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

inline std::string toUtf8(const std::filesystem::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

// tests/assets/f03 (fixtures of scripts/make_f03_assets.py).
inline std::filesystem::path f03Dir() {
    return u8path(ENG_TEST_F03_ASSETS);
}

// tests/assets/glTF-Sample-Assets/Models (scripts/fetch_assets.py). A missing checkout fails the test (never a skip).
inline std::filesystem::path sampleModels() {
    const std::filesystem::path p = u8path(ENG_TEST_SAMPLE_MODELS);
    REQUIRE_MESSAGE(std::filesystem::is_directory(p),
                    "glTF-Sample-Assets missing: " << toUtf8(p) << " (run python scripts/fetch_assets.py)");
    return p;
}

// Sample-Assets model file: Models/<model>/<variant>/<model>.gltf|.glb (.glb for glTF-Binary*).
inline std::filesystem::path sampleFile(std::string_view model, std::string_view variant) {
    const std::filesystem::path dir = sampleModels() / u8path(model) / u8path(variant);
    const std::string ext = variant.starts_with("glTF-Binary") ? ".glb" : ".gltf";
    const std::filesystem::path p = dir / u8path(std::string(model) + ext);
    REQUIRE_MESSAGE(std::filesystem::is_regular_file(p), "missing sample asset " << toUtf8(p)
                                                                                    << " (run python scripts/fetch_assets.py)");
    return p;
}

struct Loaded {
    Result<ModelData> model;
    std::vector<Diagnostic> diags;
    bool has(Diagnostic::Level level, std::string_view text) const {
        for (const Diagnostic& d : diags) {
            if (d.level == level && d.message.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    std::string all() const {
        std::string s;
        for (const Diagnostic& d : diags) {
            s += (d.level == Diagnostic::Warning ? "[warning] " : "[info] ") + d.message + "\n";
        }
        return s;
    }
};

inline Loaded load(const std::filesystem::path& p, const LoadOptions& o = {}) {
    Loaded l{loadGltf(p, o, nullptr), {}};
    l.model = loadGltf(p, o, &l.diags);
    return l;
}

// The loaded model, or a failed REQUIRE with the error.
inline const ModelData& ok(const Loaded& l) {
    REQUIRE_MESSAGE(l.model.has_value(), (l.model ? std::string() : l.model.error().message));
    return *l.model;
}
const ModelData& ok(const Loaded&&) = delete; // the model would dangle: keep the Loaded in a variable

} // namespace eng::asset::test
