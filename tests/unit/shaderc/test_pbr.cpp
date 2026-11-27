// shaderc: glTF core PBR shader (shaders/pbr/pbr.hlsl) and its permutations (plan F1 Task 4).
//   - one permutation per tier compiles + validates for every target that can express it (a superset
//     of the shaders/pbr/permutations.json matrix, which is checked to be covered);
//   - reflection (name, kind, space, index) and vertex inputs identical across targets;
//   - T0 vertex attributes <= 16 (GLES 3.0 / WebGL2 / D3D10 minimum; no validator checks it);
//   - compiling twice gives byte-identical outputs (Review Focus 4);
//   - the reserved feature groups compile as no-ops.
#include "Compiler.h"
#include "Targets.h"
#include "Validate.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ShaderTestTools.h"

namespace fs = std::filesystem;
using namespace eng::sc;

namespace {

using Binding = std::tuple<std::string, std::string, uint32_t, uint32_t>; // name, kind, space, index

struct Perm {
    int tier;
    int skin;
    int morph;
    int unlit;
};

const std::vector<std::pair<std::string, Stage>> kEntries = {{"VSMain", Stage::Vertex}, {"PSMain", Stage::Fragment}};

fs::path pbrSrc() {
    return test::shaderSrc("pbr/pbr.hlsl");
}

CompileRequest request(const std::string& entry, Stage stage, Target target, const Perm& p) {
    CompileRequest req;
    req.src = pbrSrc();
    req.entry = entry;
    req.stage = stage;
    req.target = target;
    // Canonical order, as scripts/compile_shaders.py passes them.
    req.defines = {{"TIER", std::to_string(p.tier)},
                   {"FG_SKIN", std::to_string(p.skin)},
                   {"FG_MORPH", std::to_string(p.morph)},
                   {"FG_UNLIT", std::to_string(p.unlit)},
                   {"FG_CLEARCOAT_SHEEN_SPEC", "0"},
                   {"FG_TRANSMISSION_VOLUME", "0"}};
    return req;
}

std::set<Binding> bindingSet(const nlohmann::json& refl) {
    std::set<Binding> out;
    for (const auto& r : refl.at("resources")) {
        out.emplace(r.at("name").get<std::string>(), r.at("kind").get<std::string>(), r.at("space").get<uint32_t>(),
                    r.at("index").get<uint32_t>());
    }
    return out;
}

std::set<Binding> expectedBindings(Stage stage, const Perm& p) {
    std::set<Binding> s;
    if (stage == Stage::Vertex) {
        s = {{"Frame", "cbuffer", 0, 0}, {"Object", "cbuffer", 3, 0}};
        if (p.skin) {
            s.insert(p.tier == 0 ? Binding{"Skin", "cbuffer", 3, 1} : Binding{"jointMatrices", "storage_ro", 3, 2});
        }
        if (p.morph && p.tier >= 1) {
            s.insert({"morphDeltas", "storage_ro", 3, 3});
            s.insert({"morphTargetWeights", "storage_ro", 3, 4});
        }
        return s;
    }
    s = {{"Frame", "cbuffer", 0, 0}, {"Material", "cbuffer", 2, 0}, {"baseColorTexture", "texture", 2, 1},
         {"baseColorSampler", "sampler", 2, 6}};
    if (!p.unlit) {
        s.insert({{"specularEnvTexture", "texture", 0, 1},
                  {"irradianceEnvTexture", "texture", 0, 2},
                  {"brdfLutTexture", "texture", 0, 3},
                  {"envSampler", "sampler", 0, 4},
                  {"brdfLutSampler", "sampler", 0, 5},
                  {"metallicRoughnessTexture", "texture", 2, 2},
                  {"normalTexture", "texture", 2, 3},
                  {"occlusionTexture", "texture", 2, 4},
                  {"emissiveTexture", "texture", 2, 5},
                  {"metallicRoughnessSampler", "sampler", 2, 7},
                  {"normalSampler", "sampler", 2, 8},
                  {"occlusionSampler", "sampler", 2, 9},
                  {"emissiveSampler", "sampler", 2, 10}});
    }
    return s;
}

// GLSL ES 3.00 has no storage buffers: T1/T2 skin and morph live in StructuredBuffers (vertex stage).
bool expressible(Target t, const Perm& p, Stage stage) {
    return !(t == Target::GLSL_ES300 && stage == Stage::Vertex && p.tier >= 1 && (p.skin || p.morph));
}

nlohmann::json loadPermutations() {
    std::ifstream f(test::shaderSrc("pbr/permutations.json"));
    REQUIRE(f.good());
    return nlohmann::json::parse(f);
}

std::string what(const std::expected<CompileResult, std::string>& r) {
    return r.has_value() ? std::string() : r.error();
}

} // namespace

TEST_SUITE("shaderc") {

TEST_CASE("pbr: one permutation per tier compiles and validates on every target that can express it") {
    const nlohmann::json perms = loadPermutations();
    for (int tier : {0, 1, 2}) {
        for (const Perm& p : {Perm{tier, 1, 1, 0}, Perm{tier, 0, 0, 1}}) {
            for (const auto& [entry, stage] : kEntries) {
                CAPTURE(tier);
                CAPTURE(p.skin);
                CAPTURE(p.morph);
                CAPTURE(p.unlit);
                CAPTURE(entry);
                std::map<Target, nlohmann::json> refl;
                for (Target t : kAllTargets) {
                    CAPTURE(toString(t));
                    auto r = compile(request(entry, stage, t, p), test::tools());
                    if (!expressible(t, p, stage)) {
                        REQUIRE_FALSE(r.has_value());
                        CHECK(r.error() == "target glsl-es300 does not support storage buffers (TIER=" +
                                               std::to_string(tier) + ")");
                        // ... and the matrix never asks for it.
                        const auto& tiers = perms.at("targets").at(toString(t)).at("tiers");
                        CHECK(std::find(tiers.begin(), tiers.end(), tier) == tiers.end());
                        continue;
                    }
                    REQUIRE_MESSAGE(r.has_value(), what(r));
                    auto v = validate(t, *r, test::tools());
                    REQUIRE_MESSAGE(v.has_value(), (v.has_value() ? std::string() : v.error()));
                    CHECK(*v == (t == Target::MSL ? ValidateStatus::PendingCI : ValidateStatus::Passed));
                    refl[t] = r->reflection;
                }
                // Same (name, kind, space, index) and vertex inputs on every target; the expected set itself.
                const nlohmann::json& first = refl.begin()->second;
                for (const auto& [t, j] : refl) {
                    CAPTURE(toString(t));
                    CHECK(bindingSet(j) == bindingSet(first));
                    CHECK(j.at("vertexInputs") == first.at("vertexInputs"));
                }
                CHECK(bindingSet(first) == expectedBindings(stage, p));
                if (stage == Stage::Vertex) {
                    const size_t inputs = first.at("vertexInputs").size();
                    CHECK(inputs <= 16);
                    // POSITION NORMAL TANGENT TEXCOORD0/1 COLOR0 + JOINTS/WEIGHTS + (T0) 8 morph positions.
                    const size_t expected = 6 + (p.skin ? 2 : 0) + (p.morph && tier == 0 ? 8 : 0);
                    CHECK(inputs == expected);
                }
            }
        }
    }
}

TEST_CASE("pbr: the permutation matrix is 8 define sets per target per stage and every set is expressible") {
    const nlohmann::json perms = loadPermutations();
    REQUIRE(perms.at("targets").size() == kAllTargets.size());
    for (Target t : kAllTargets) {
        CAPTURE(toString(t));
        const auto& tiers = perms.at("targets").at(toString(t)).at("tiers");
        size_t count = 0;
        for (int tier : tiers) {
            for (int skin : {0, 1}) {
                for (int morph : {0, 1}) {
                    for (int unlit : {0, 1}) {
                        CHECK(expressible(t, Perm{tier, skin, morph, unlit}, Stage::Vertex));
                        ++count;
                    }
                }
            }
        }
        CHECK(count == 8);
    }
}

TEST_CASE("pbr: compiling the same source + defines twice is byte-identical on every target (Review Focus 4)") {
    for (int tier : {0, 1, 2}) {
        const Perm p{tier, 1, 1, 0};
        for (const auto& [entry, stage] : kEntries) {
            for (Target t : kAllTargets) {
                if (!expressible(t, p, stage)) {
                    continue;
                }
                CAPTURE(tier);
                CAPTURE(entry);
                CAPTURE(toString(t));
                auto a = compile(request(entry, stage, t, p), test::tools());
                auto b = compile(request(entry, stage, t, p), test::tools());
                REQUIRE_MESSAGE(a.has_value(), what(a));
                REQUIRE_MESSAGE(b.has_value(), what(b));
                CHECK((!a->bytecode.empty() || !a->text.empty()));
                CHECK(a->bytecode == b->bytecode);
                CHECK(a->text == b->text);
                CHECK(a->reflection.dump() == b->reflection.dump());
            }
            // The direct FXC path too (Task 5 compares the two DXBC paths).
            CompileRequest req = request(entry, stage, Target::DXBC, p);
            req.dxbcPath = DxbcPath::Direct;
            auto a = compile(req, test::tools());
            auto b = compile(req, test::tools());
            REQUIRE_MESSAGE(a.has_value(), what(a));
            REQUIRE_MESSAGE(b.has_value(), what(b));
            CHECK(a->bytecode == b->bytecode);
        }
    }
}

TEST_CASE("pbr: reserved feature groups FG_CLEARCOAT_SHEEN_SPEC / FG_TRANSMISSION_VOLUME compile as no-ops") {
    for (Target t : {Target::SPIRV, Target::DXIL, Target::GLSL_ES300, Target::WGSL}) {
        CAPTURE(toString(t));
        const Perm p{t == Target::GLSL_ES300 ? 0 : 2, 0, 0, 0};
        CompileRequest base = request("PSMain", Stage::Fragment, t, p);
        CompileRequest reserved = base;
        for (auto& [k, v] : reserved.defines) {
            if (k == "FG_CLEARCOAT_SHEEN_SPEC" || k == "FG_TRANSMISSION_VOLUME") {
                v = "1";
            }
        }
        auto a = compile(base, test::tools());
        auto b = compile(reserved, test::tools());
        REQUIRE_MESSAGE(a.has_value(), what(a));
        REQUIRE_MESSAGE(b.has_value(), what(b));
        CHECK(a->bytecode == b->bytecode);
        CHECK(a->text == b->text);
    }
}

} // TEST_SUITE
