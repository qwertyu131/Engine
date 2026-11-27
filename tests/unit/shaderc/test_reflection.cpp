// shaderc: unified reflection JSON + binding-convention enforcement (plan F1 Task 3).
//   - bindings_ok.hlsl: the same (name, kind, space, index) set on every target, target bindings
//     derived from it as the convention says (Vulkan/WGSL set=space binding=index, GLSL ES 3.20
//     space*16+index, D3D12 native registers, ...).
//   - bindings_collide.hlsl: b0 and t0 in one space -> error naming both registers (Review Focus 1).
//   - needs_storage.hlsl: StructuredBuffer on GLSL ES 3.00 -> error naming target + feature (Review Focus 2).
#include "Compiler.h"
#include "Targets.h"
#include "Validate.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <filesystem>
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

CompileRequest request(const fs::path& src, const std::string& entry, Stage stage, Target target, int tier = -1) {
    CompileRequest req;
    req.src = src;
    req.entry = entry;
    req.stage = stage;
    req.target = target;
    if (tier >= 0) {
        req.defines.emplace_back("TIER", std::to_string(tier));
    }
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

// targetBinding of every resource follows the convention for `target`.
void checkTargetBindings(Target target, const nlohmann::json& refl) {
    for (const auto& r : refl.at("resources")) {
        const auto& tb = r.at("targetBinding");
        const std::string kind = r.at("kind");
        const uint32_t space = r.at("space"), index = r.at("index");
        CAPTURE(toString(target));
        CAPTURE(r.dump());
        switch (target) {
        case Target::SPIRV:
        case Target::WGSL:
            CHECK(tb.at("set") == space);
            CHECK(tb.at("binding") == index);
            break;
        case Target::GLSL_ES320:
            CHECK(!tb.at("glslName").get<std::string>().empty());
            if (kind != "sampler") {
                CHECK(tb.at("glslBinding") == space * 16 + index);
            }
            break;
        case Target::GLSL_ES300:
            CHECK(!tb.at("glslName").get<std::string>().empty());
            CHECK_FALSE(tb.contains("glslBinding"));
            break;
        case Target::DXIL:
            CHECK(tb.at("register") == std::string(1, registerClass(kind)) + std::to_string(index));
            break;
        case Target::DXBC:
            CHECK(tb.at("register").get<std::string>().front() == registerClass(kind));
            break;
        case Target::MSL: CHECK(tb.contains("mslIndex")); break;
        }
    }
}

const std::vector<std::pair<std::string, Stage>> kEntries = {{"VSMain", Stage::Vertex}, {"PSMain", Stage::Fragment}};

} // namespace

TEST_SUITE("shaderc") {

TEST_CASE("bindings_ok: identical (name, kind, space, index) on every target, convention-derived target bindings") {
    const fs::path src = test::shaderSrc("test/bindings_ok.hlsl");
    // TIER=0: all 7 targets. TIER=1 adds storage buffers: every target except GLSL ES 3.00.
    for (int tier : {0, 1}) {
        for (const auto& [entry, stage] : kEntries) {
            CAPTURE(tier);
            CAPTURE(entry);
            std::set<Binding> first;
            bool haveFirst = false;
            for (Target t : kAllTargets) {
                if (tier >= 1 && t == Target::GLSL_ES300) {
                    continue;
                }
                CAPTURE(toString(t));
                auto r = compile(request(src, entry, stage, t, tier), test::tools());
                REQUIRE_MESSAGE(r.has_value(), (r.has_value() ? std::string() : r.error()));
                auto v = validate(t, *r, test::tools());
                REQUIRE_MESSAGE(v.has_value(), (v.has_value() ? std::string() : v.error()));
                const nlohmann::json& refl = r->reflection;
                CHECK(refl.at("stage") == toString(stage));
                CHECK(refl.at("entry") == entry);
                CHECK(refl.at("target") == toString(t));
                CHECK(refl.at("pushConstants").is_null());
                CHECK(refl.at("vertexInputs").is_array());
                for (const auto& res : refl.at("resources")) {
                    CHECK(res.at("arraySize") == 1);
                    CHECK(res.contains("sizeBytes") == (res.at("kind") == "cbuffer"));
                }
                checkTargetBindings(t, refl);
                const std::set<Binding> set = bindingSet(refl);
                if (!haveFirst) {
                    first = set;
                    haveFirst = true;
                } else {
                    CHECK(set == first);
                }
            }
            // The expected set itself (per stage: only the resources that stage uses).
            std::set<Binding> expected;
            if (stage == Stage::Vertex) {
                expected = {{"Frame", "cbuffer", 0, 0}, {"Object", "cbuffer", 3, 0}};
                if (tier >= 1) {
                    expected.insert({"offsets", "storage_ro", 3, 1});
                }
            } else {
                expected = {{"Pass", "cbuffer", 1, 0},
                            {"Material", "cbuffer", 2, 0},
                            {"baseColorTexture", "texture", 2, 1},
                            {"baseColorSampler", "sampler", 2, 2},
                            {"envTexture", "texture", 2, 3},
                            {"envSampler", "sampler", 2, 4}};
                if (tier >= 1) {
                    expected.insert({"fragmentCounter", "storage_rw", 1, 1});
                }
            }
            CHECK(first == expected);
        }
    }
}

TEST_CASE("bindings_ok: emitted WGSL and GLSL ES 3.20 carry the convention's bindings") {
    const fs::path src = test::shaderSrc("test/bindings_ok.hlsl");
    auto wgsl = compile(request(src, "PSMain", Stage::Fragment, Target::WGSL, 0), test::tools());
    REQUIRE_MESSAGE(wgsl.has_value(), (wgsl.has_value() ? std::string() : wgsl.error()));
    CHECK(wgsl->text.find("@group(2u) @binding(1u) var baseColorTexture") != std::string::npos);
    CHECK(wgsl->text.find("@group(2u) @binding(4u) var envSampler") != std::string::npos);
    CHECK(wgsl->text.find("@group(1u) @binding(0u) var<uniform> Pass") != std::string::npos);

    auto es = compile(request(src, "PSMain", Stage::Fragment, Target::GLSL_ES320, 0), test::tools());
    REQUIRE_MESSAGE(es.has_value(), (es.has_value() ? std::string() : es.error()));
    // space2 t1 -> 2*16+1 = 33 (combined with its sampler), space2 b0 -> 32, space1 b0 -> 16.
    CHECK(es->text.find("layout(binding = 33) uniform highp sampler2D baseColorTexture_baseColorSampler") !=
          std::string::npos);
    CHECK(es->text.find("layout(binding = 35) uniform highp samplerCube envTexture_envSampler") != std::string::npos);
    CHECK(es->text.find("layout(binding = 32, std140) uniform type_Material") != std::string::npos);
    CHECK(es->text.find("layout(binding = 16, std140) uniform type_Pass") != std::string::npos);
}

TEST_CASE("bindings_collide: b0 and t0 in one space is rejected on every target, for both stages") {
    const fs::path src = test::shaderSrc("test/bindings_collide.hlsl");
    for (const auto& [entry, stage] : kEntries) {
        for (Target t : kAllTargets) {
            CAPTURE(entry);
            CAPTURE(toString(t));
            auto r = compile(request(src, entry, stage, t), test::tools());
            REQUIRE_FALSE(r.has_value());
            CHECK(r.error() == "binding collision in space2: b0 and t0");
        }
    }
}

TEST_CASE("needs_storage: StructuredBuffer is rejected on GLSL ES 3.00 by name, accepted elsewhere") {
    const fs::path src = test::shaderSrc("test/needs_storage.hlsl");
    auto es300 = compile(request(src, "VSMain", Stage::Vertex, Target::GLSL_ES300, 1), test::tools());
    REQUIRE_FALSE(es300.has_value());
    CHECK(es300.error() == "target glsl-es300 does not support storage buffers (TIER=1)");

    auto noTier = compile(request(src, "VSMain", Stage::Vertex, Target::GLSL_ES300), test::tools());
    REQUIRE_FALSE(noTier.has_value());
    CHECK(noTier.error() == "target glsl-es300 does not support storage buffers (TIER undefined)");

    for (Target t : {Target::SPIRV, Target::DXIL, Target::DXBC, Target::MSL, Target::GLSL_ES320, Target::WGSL}) {
        CAPTURE(toString(t));
        auto r = compile(request(src, "VSMain", Stage::Vertex, t, 1), test::tools());
        REQUIRE_MESSAGE(r.has_value(), (r.has_value() ? std::string() : r.error()));
        auto v = validate(t, *r, test::tools());
        REQUIRE_MESSAGE(v.has_value(), (v.has_value() ? std::string() : v.error()));
        const std::set<Binding> expected = {{"jointMatrices", "storage_ro", 3, 0}};
        CHECK(bindingSet(r->reflection) == expected);
    }
}

TEST_CASE("needs_storage: a vertex-stage storage write is rejected on WGSL and GLSL ES 3.00") {
    const fs::path src = test::shaderSrc("test/needs_storage.hlsl");
    auto wgsl = compile(request(src, "VSWrite", Stage::Vertex, Target::WGSL, 1), test::tools());
    REQUIRE_FALSE(wgsl.has_value());
    CHECK(wgsl.error() == "target wgsl does not support read-write storage in the vertex stage (TIER=1)");

    auto es300 = compile(request(src, "VSWrite", Stage::Vertex, Target::GLSL_ES300, 1), test::tools());
    REQUIRE_FALSE(es300.has_value());
    CHECK(es300.error() == "target glsl-es300 does not support storage buffers (TIER=1)");

    auto spirv = compile(request(src, "VSWrite", Stage::Vertex, Target::SPIRV, 1), test::tools());
    REQUIRE_MESSAGE(spirv.has_value(), (spirv.has_value() ? std::string() : spirv.error()));
    CHECK(bindingSet(spirv->reflection) == std::set<Binding>{{"vertexOut", "storage_rw", 3, 1}});
}

TEST_CASE("glsl_array_overflow: a range past index 15 and unbounded arrays are rejected on GLSL targets only") {
    const fs::path src = test::shaderSrc("test/glsl_array_overflow.hlsl");
    for (Target t : {Target::GLSL_ES320, Target::GLSL_ES300}) {
        CAPTURE(toString(t));
        auto r = compile(request(src, "PSMain", Stage::Fragment, t, 0), test::tools());
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error() == "binding range t14..t17 exceeds 16 per space for " + toString(t));
        auto u = compile(request(src, "PSUnbounded", Stage::Fragment, t, 0), test::tools());
        REQUIRE_FALSE(u.has_value());
        CHECK(u.error() == "binding range t0..(unbounded) exceeds 16 per space for " + toString(t));
    }
    // The same ranges are fine where (space, index) is not flattened.
    for (Target t : {Target::SPIRV, Target::DXIL}) {
        CAPTURE(toString(t));
        auto r = compile(request(src, "PSMain", Stage::Fragment, t, 0), test::tools());
        REQUIRE_MESSAGE(r.has_value(), (r.has_value() ? std::string() : r.error()));
        auto v = validate(t, *r, test::tools());
        REQUIRE_MESSAGE(v.has_value(), (v.has_value() ? std::string() : v.error()));
        CHECK(bindingSet(r->reflection) ==
              std::set<Binding>{{"layers", "texture", 2, 14}, {"layerSampler", "sampler", 2, 0}});
    }
}

TEST_CASE("no_register: a resource without an explicit register() is rejected on every target") {
    const fs::path src = test::shaderSrc("test/no_register.hlsl");
    for (Target t : kAllTargets) {
        CAPTURE(toString(t));
        auto r = compile(request(src, "PSMain", Stage::Fragment, t, 0), test::tools());
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error() == "resource Material has no explicit register() (binding convention)");
        // VSMain only uses the declared `Frame`: accepted.
        auto vs = compile(request(src, "VSMain", Stage::Vertex, t, 0), test::tools());
        REQUIRE_MESSAGE(vs.has_value(), (vs.has_value() ? std::string() : vs.error()));
    }
}

} // TEST_SUITE
