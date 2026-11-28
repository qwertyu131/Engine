// shaderc: glTF core PBR shader (shaders/pbr/pbr.hlsl) and its permutations (plan F1 Task 4).
//   - one permutation per tier compiles + validates for every target that can express it (a superset
//     of the shaders/pbr/permutations.json matrix, which is checked to be covered);
//   - reflection (name, kind, space, index) and vertex inputs identical across targets;
//   - T0 vertex attributes <= 16 (GLES 3.0 / WebGL2 / D3D10 minimum; no validator checks it);
//   - compiling twice gives byte-identical outputs (Review Focus 4);
//   - the reserved feature groups compile as no-ops;
//   - D3D11 (DXBC) VS -> PS linkage on both --dxbc-path values: every PS input has the same semantic,
//     index and register in the VS output; the default path is cross (plan F1 Task 5 decision);
//   - DXBC shader model by tier (plan F2 Task 7): TIER=0 -> vs_4_0 / ps_4_0 (D3D11 FL10_x, T0), else
//     vs_5_0 / ps_5_0; recorded as reflection "shaderModel" and checked in the bytecode itself.
#include "Compiler.h"
#include "Targets.h"
#include "Validate.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
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
                  {"emissiveSampler", "sampler", 2, 10},
                  // space1: punctual lights and shadows (plan F3 Task 7, shaders/pbr/lights.hlsli)
                  {"Lights", "cbuffer", 1, 0},
                  {"Shadows", "cbuffer", 1, 1},
                  {"shadowAtlas", "texture", 1, 2},
                  {"shadowSampler", "sampler", 1, 3}});
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

// One element of a DXBC ISGN / OSGN chunk (the D3D11 stage-linkage signature).
struct SigElement {
    std::string semantic;
    uint32_t semanticIndex = 0;
    uint32_t systemValue = 0; // D3D_NAME: 0 = none (user semantic), 1 = POSITION, others = system values
    uint32_t reg = 0;
    uint8_t mask = 0;
};

uint32_t le32(const std::vector<uint8_t>& b, size_t off) {
    REQUIRE(off + 4 <= b.size());
    uint32_t v = 0;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

// DXBC container: "DXBC", 16-byte hash, version, total size, chunk count, chunk offsets; a chunk is
// fourcc + size + data. ISGN/OSGN data: element count, 8, then 24-byte elements (name offset, semantic
// index, system value, component type, register, mask, rw mask, 2 pad); name offsets are relative to the
// chunk data.
std::vector<SigElement> dxbcSignature(const std::vector<uint8_t>& b, const char (&fourcc)[5]) {
    REQUIRE(b.size() >= 32);
    REQUIRE(std::memcmp(b.data(), "DXBC", 4) == 0);
    const uint32_t chunks = le32(b, 28);
    for (uint32_t c = 0; c < chunks; ++c) {
        const size_t chunk = le32(b, 32 + 4 * size_t(c));
        REQUIRE(chunk + 8 <= b.size());
        if (std::memcmp(b.data() + chunk, fourcc, 4) != 0) {
            continue;
        }
        const size_t data = chunk + 8;
        const uint32_t count = le32(b, data);
        std::vector<SigElement> out;
        for (uint32_t i = 0; i < count; ++i) {
            const size_t e = data + 8 + 24 * size_t(i);
            REQUIRE(e + 24 <= b.size());
            SigElement s;
            const size_t name = data + le32(b, e);
            REQUIRE(name < b.size());
            s.semantic = reinterpret_cast<const char*>(b.data() + name);
            s.semanticIndex = le32(b, e + 4);
            s.systemValue = le32(b, e + 8);
            s.reg = le32(b, e + 16);
            s.mask = b[e + 20];
            out.push_back(s);
        }
        return out;
    }
    FAIL("DXBC chunk " << fourcc << " not found");
    return {};
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

TEST_CASE("pbr: the permutation matrix is 8 define sets per target per stage (dxbc: 16) and every set is expressible") {
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
        // dxbc carries two tiers since plan F2 Task 7: T0 (SM 4.0, D3D11 FL10_x) and T1 (SM 5.0, FL11_x).
        CHECK(count == (t == Target::DXBC ? 16u : 8u));
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

TEST_CASE("pbr: D3D11 VS -> PS signatures link on both DXBC paths; the default path is cross") {
    // Task 5 (docs/spikes/f01-shader-pipeline.md): without -fspv-preserve-interface the cross path drops
    // the PS inputs an FG_UNLIT=1 pixel shader does not read, and FXC re-packs the rest from register 0;
    // the D3D11 debug layer rejects that pair at Draw ("Semantic 'TEXCOORD' is defined for mismatched
    // hardware registers between the output stage and input stage").
    CHECK(CompileRequest{}.dxbcPath == DxbcPath::Cross);
    for (DxbcPath path : {DxbcPath::Cross, DxbcPath::Direct}) {
        for (int skin : {0, 1}) {
            for (int morph : {0, 1}) {
                for (int unlit : {0, 1}) {
                    for (int tier : {0, 1}) { // dxbc tiers (plan F2 Task 7: T0 = SM 4.0)
                        const Perm p{tier, skin, morph, unlit};
                        CAPTURE(tier);
                        CAPTURE(toString(path));
                        CAPTURE(skin);
                        CAPTURE(morph);
                        CAPTURE(unlit);
                        CompileRequest vreq = request("VSMain", Stage::Vertex, Target::DXBC, p);
                        CompileRequest preq = request("PSMain", Stage::Fragment, Target::DXBC, p);
                        vreq.dxbcPath = path;
                        preq.dxbcPath = path;
                        auto vs = compile(vreq, test::tools());
                        auto ps = compile(preq, test::tools());
                        REQUIRE_MESSAGE(vs.has_value(), what(vs));
                        REQUIRE_MESSAGE(ps.has_value(), what(ps));
                        const auto vsOut = dxbcSignature(vs->bytecode, "OSGN");
                        const auto psIn = dxbcSignature(ps->bytecode, "ISGN");
                        REQUIRE_FALSE(psIn.empty());
                        for (const SigElement& in : psIn) {
                            // D3D_NAME_UNDEFINED (0) and D3D_NAME_POSITION (1) link to the VS output; every
                            // other system value (front face, sample index, coverage, primitive id, ...) is
                            // generated by the rasterizer. Decided by the ISGN field, not by the semantic name.
                            constexpr uint32_t kNameUndefined = 0, kNamePosition = 1;
                            if (in.systemValue != kNameUndefined && in.systemValue != kNamePosition) {
                                continue;
                            }
                            CAPTURE(in.semantic);
                            CAPTURE(in.semanticIndex);
                            auto it = std::find_if(vsOut.begin(), vsOut.end(), [&](const SigElement& o) {
                                return o.semantic == in.semantic && o.semanticIndex == in.semanticIndex;
                            });
                            REQUIRE(it != vsOut.end());
                            CHECK(it->reg == in.reg);
                            CHECK((in.mask & ~it->mask) == 0);
                        }
                    }
                }
            }
        }
    }
}

// Shader model of a DXBC container: the program chunk is SHDR for SM 4.x and SHEX for SM 5.x; its first
// token holds the program type (bits 16..31), major (4..7) and minor (0..3) version.
static std::string dxbcShaderModel(const std::vector<uint8_t>& b, std::string& chunkName) {
    REQUIRE(b.size() >= 32);
    REQUIRE(std::memcmp(b.data(), "DXBC", 4) == 0);
    const uint32_t chunks = le32(b, 28);
    for (uint32_t c = 0; c < chunks; ++c) {
        const size_t chunk = le32(b, 32 + 4 * size_t(c));
        REQUIRE(chunk + 12 <= b.size());
        if (std::memcmp(b.data() + chunk, "SHDR", 4) == 0 || std::memcmp(b.data() + chunk, "SHEX", 4) == 0) {
            chunkName.assign(reinterpret_cast<const char*>(b.data() + chunk), 4);
            const uint32_t token = le32(b, chunk + 8);
            return std::to_string((token >> 4) & 0xF) + "_" + std::to_string(token & 0xF);
        }
    }
    FAIL("DXBC has no SHDR / SHEX chunk");
    return {};
}

TEST_CASE("pbr: dxbc TIER=0 is SM 4.0 (vs_4_0 / ps_4_0), TIER=1 is SM 5.0, on both DXBC paths") {
    for (DxbcPath path : {DxbcPath::Cross, DxbcPath::Direct}) {
        for (int tier : {0, 1}) {
            for (const auto& [entry, stage] : kEntries) {
                CAPTURE(toString(path));
                CAPTURE(tier);
                CAPTURE(entry);
                CompileRequest req = request(entry, stage, Target::DXBC, Perm{tier, 1, 1, 0});
                req.dxbcPath = path;
                auto r = compile(req, test::tools());
                REQUIRE_MESSAGE(r.has_value(), what(r));
                const std::string sm = tier == 0 ? "4_0" : "5_0";
                CHECK(r->reflection.at("shaderModel") == sm);
                std::string chunk;
                CHECK(dxbcShaderModel(r->bytecode, chunk) == sm);
                CHECK(chunk == (tier == 0 ? "SHDR" : "SHEX"));
                auto v = validate(Target::DXBC, *r, test::tools());
                REQUIRE_MESSAGE(v.has_value(), (v.has_value() ? std::string() : v.error()));
            }
        }
    }
    // No TIER define: SM 5.0 (the default tier of bindings.hlsli is 2; D3D11 is never below SM 5.0 then).
    CompileRequest req;
    req.src = test::triangleSrc();
    req.entry = "PSMain";
    req.stage = Stage::Fragment;
    req.target = Target::DXBC;
    auto r = compile(req, test::tools());
    REQUIRE_MESSAGE(r.has_value(), what(r));
    CHECK(r->reflection.at("shaderModel") == "5_0");
    // Other targets do not record a DXBC shader model.
    req.target = Target::SPIRV;
    auto spv = compile(req, test::tools());
    REQUIRE_MESSAGE(spv.has_value(), what(spv));
    CHECK_FALSE(spv->reflection.contains("shaderModel"));
}

} // TEST_SUITE
