// glTF loader on Khronos glTF-Sample-Assets + the F3 fixtures (plan F3 Task 2).
//
// Suites:
//   asset.loader   Sample-Assets: Box / BoxTextured (.gltf with external .bin, .glb, data-URI embedded), the
//                  hand values of SimpleSparseAccessor, the 7 modes of MeshPrimitiveModes (fan / loop converted),
//                  MultiUVTest, VertexColorTest, MikkTSpace tangents vs the file's on NormalTangentMirrorTest,
//                  generated tangents on NormalTangentTest, Cameras / SimpleSkin / SimpleMorph parsed counts;
//                  the caps (MaxVertices, MaxTextureSize) give warnings, never errors.
//   asset.missing  Review Focus 4 (tests/assets/f03/missing): no normals -> flat normals, no tangents with a normal
//                  map -> MikkTSpace, no UVs with a texture -> warning + UV (0, 0), no material -> default (-1);
//                  spec §7.2: unknown extension in extensionsUsed -> warning, later-phase one -> info + raw JSON kept.
//   asset.paths    Review Focus 2 (tests/assets/f03/paths): external .bin / .png in the same and in a sibling
//                  directory, non-ASCII names with spaces, percent-encoded and raw UTF-8 URIs.
// Expected values come from the files themselves (accessor counts, the SimpleSparseAccessor .bin decoded by hand,
// see the plan F3 Task 2 worklist row), never from a loader run.
#include "AssetTestEnv.h"

#include "asset/Geometry.h"

#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <string>

using namespace eng::asset;
using namespace eng::asset::test;

namespace {

float len3(const Vec3& v) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

float dot3(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 xyz(const Vec4& v) {
    return {v[0], v[1], v[2]};
}

} // namespace

TEST_SUITE("asset.loader") {

TEST_CASE("Box: .gltf (external .bin), .glb and data-URI embedded give the same model") {
    std::vector<ModelData> models;
    for (const char* variant : {"glTF", "glTF-Binary", "glTF-Embedded"}) {
        CAPTURE(variant);
        const Loaded l = load(sampleFile("Box", variant));
        const ModelData& m = ok(l);
        REQUIRE(m.meshes.size() == 1);
        REQUIRE(m.meshes[0].primitives.size() == 1);
        const Primitive& p = m.meshes[0].primitives[0];
        CHECK(p.topology == Topology::TriangleList);
        CHECK(p.vertexCount == 24);
        CHECK(p.indices.size() == 36);
        CHECK(p.normal.size() == 24);
        CHECK_FALSE(p.generatedNormals);
        CHECK(p.bounds.valid);
        CHECK(p.bounds.min == Vec3{-0.5f, -0.5f, -0.5f});
        CHECK(p.bounds.max == Vec3{0.5f, 0.5f, 0.5f});
        REQUIRE(m.materials.size() == 1);
        CHECK(p.material == 0);
        CHECK(m.scenes.size() == 1);
        CHECK(m.defaultScene == 0);
        models.push_back(m);
    }
    for (size_t i = 1; i < models.size(); ++i) {
        const Primitive& a = models[0].meshes[0].primitives[0];
        const Primitive& b = models[i].meshes[0].primitives[0];
        CHECK(a.position == b.position);
        CHECK(a.normal == b.normal);
        CHECK(a.indices == b.indices);
    }
}

TEST_CASE("BoxTextured: PNG decoded (external, GLB bufferView, data URI), baseColor image is sRGB") {
    for (const char* variant : {"glTF", "glTF-Binary", "glTF-Embedded"}) {
        CAPTURE(variant);
        const Loaded l = load(sampleFile("BoxTextured", variant));
        const ModelData& m = ok(l);
        REQUIRE(m.images.size() == 1);
        const Image& img = m.images[0];
        CHECK(img.decoded);
        CHECK(img.width == 256);
        CHECK(img.height == 256);
        CHECK(img.rgba8.size() == size_t{256} * 256 * 4);
        CHECK(img.colorSpace == ColorSpace::Srgb);
        CHECK(img.usedAsSrgb);
        CHECK_FALSE(img.usedAsLinear);
        REQUIRE(m.materials.size() == 1);
        CHECK(m.materials[0].baseColor.image == 0);
        CHECK(m.materials[0].baseColor.texCoord == 0);
        const Primitive& p = m.meshes[0].primitives[0];
        CHECK(p.texCoord[0].size() == p.vertexCount);
        CHECK(l.diags.empty());
    }
}

TEST_CASE("SimpleSparseAccessor: sparse positions equal the file's base + substituted values") {
    // The file has no NORMAL, so the loader de-indexes for flat normals: generated vertex k is the file's vertex
    // indices[k]. The 36 uint16 indices are bytes [0, 72) of SimpleSparseAccessor.bin, read here by hand.
    std::vector<uint16_t> fileIndices(36);
    {
        std::ifstream bin(sampleModels() / "SimpleSparseAccessor" / "glTF" / "SimpleSparseAccessor.bin", std::ios::binary);
        bin.read(reinterpret_cast<char*>(fileIndices.data()), 72);
        REQUIRE(bin.gcount() == 72);
    }
    // Base grid: vertex i = (i % 7, i / 7, 0) for i < 14; sparse indices 8, 10, 12 -> (1, 2, 0), (3, 3, 0), (5, 4, 0).
    const auto want = [](uint32_t i) {
        if (i == 8) {
            return Vec3{1.0f, 2.0f, 0.0f};
        }
        if (i == 10) {
            return Vec3{3.0f, 3.0f, 0.0f};
        }
        if (i == 12) {
            return Vec3{5.0f, 4.0f, 0.0f};
        }
        return Vec3{static_cast<float>(i % 7), static_cast<float>(i / 7), 0.0f};
    };
    for (const char* variant : {"glTF", "glTF-Embedded"}) {
        CAPTURE(variant);
        const Loaded l = load(sampleFile("SimpleSparseAccessor", variant));
        const ModelData& m = ok(l);
        const Primitive& p = m.meshes.at(0).primitives.at(0);
        CHECK(p.generatedNormals);
        REQUIRE(p.vertexCount == 36);
        for (uint32_t k = 0; k < 36; ++k) {
            CAPTURE(k);
            REQUIRE(fileIndices[k] < 14);
            CHECK(p.position[k] == want(fileIndices[k]));
        }
        CHECK(p.bounds.max == Vec3{6.0f, 4.0f, 0.0f});
    }
}

TEST_CASE("MeshPrimitiveModes: all 7 modes; LINE_LOOP and TRIANGLE_FAN become index lists") {
    const Loaded l = load(sampleFile("MeshPrimitiveModes", "glTF"));
    const ModelData& m = ok(l);
    REQUIRE(m.meshes.size() == 7);
    // Mesh i has one primitive of mode i; index counts in the file: 7 12 7 7 18 6 8 (7 vertices each).
    // Loop of 7 indices -> 7 segments = 14 indices; fan of 8 indices -> 6 triangles = 18. The file has no NORMAL:
    // the triangle modes get flat normals, which de-indexes them (strip -> list first: 6 strip indices = 4 triangles
    // = 12), so their vertex count is their index count.
    const Topology topo[7] = {Topology::PointList,    Topology::LineList,     Topology::LineList, Topology::LineStrip,
                              Topology::TriangleList, Topology::TriangleList, Topology::TriangleList};
    const size_t count[7] = {7, 12, 14, 7, 18, 12, 18};
    for (size_t i = 0; i < 7; ++i) {
        CAPTURE(i);
        REQUIRE(m.meshes[i].primitives.size() == 1);
        const Primitive& p = m.meshes[i].primitives[0];
        CHECK(p.topology == topo[i]);
        CHECK(p.indices.size() == count[i]);
        CHECK(p.vertexCount == (i >= 4 ? count[i] : 7));
        CHECK(p.generatedNormals == (i >= 4));
        for (const uint32_t idx : p.indices) {
            CHECK(idx < p.vertexCount);
        }
    }
}

TEST_CASE("MultiUVTest: TEXCOORD_0 and TEXCOORD_1, a texture on set 1") {
    const Loaded l = load(sampleFile("MultiUVTest", "glTF"));
    const ModelData& m = ok(l);
    const Primitive& p = m.meshes.at(0).primitives.at(0);
    CHECK(p.texCoord[0].size() == p.vertexCount);
    CHECK(p.texCoord[1].size() == p.vertexCount);
    CHECK(p.texCoord[2].empty());
    bool set1 = false;
    for (const Material& mat : m.materials) {
        for (const TextureRef* t : {&mat.baseColor, &mat.metallicRoughness, &mat.normal, &mat.occlusion, &mat.emissive}) {
            set1 = set1 || (t->valid() && t->texCoord == 1);
        }
    }
    CHECK(set1);
}

TEST_CASE("VertexColorTest: COLOR_0 on the second primitive only, VEC4 float colours") {
    const Loaded l = load(sampleFile("VertexColorTest", "glTF"));
    const ModelData& m = ok(l);
    size_t withColor = 0, total = 0;
    for (const Mesh& mesh : m.meshes) {
        for (const Primitive& p : mesh.primitives) {
            ++total;
            if (!p.color0.empty()) {
                ++withColor;
                CHECK(p.color0.size() == p.vertexCount);
                for (const Vec4& c : p.color0) {
                    for (const float v : c) {
                        CHECK(v >= 0.0f);
                        CHECK(v <= 1.0f);
                    }
                }
            }
        }
    }
    CHECK(total == 2);
    CHECK(withColor == 1);
}

TEST_CASE("MikkTSpace on NormalTangentMirrorTest with its tangents stripped equals the file's within 1e-3 on >= 99%") {
    // Plan deviation (recorded): NormalTangentTest has no TANGENT attribute at the pinned commit, so the strip /
    // compare runs on NormalTangentMirrorTest (POSITION NORMAL TANGENT TEXCOORD_0, mirrored UVs).
    const Loaded l = load(sampleFile("NormalTangentMirrorTest", "glTF"));
    const ModelData& m = ok(l);
    const Primitive& file = m.meshes.at(0).primitives.at(0);
    REQUIRE(file.tangent.size() == file.vertexCount);
    REQUIRE_FALSE(file.generatedTangents);
    Primitive stripped = file;
    stripped.tangent.clear();
    std::string why;
    REQUIRE_MESSAGE(generateTangents(stripped, 0, &why), why);
    CHECK(stripped.generatedTangents);
    // generateTangents de-indexes: generated vertex k is the file's vertex file.indices[k].
    REQUIRE(stripped.vertexCount == file.indices.size());
    size_t close = 0;
    for (size_t k = 0; k < file.indices.size(); ++k) {
        const Vec4& want = file.tangent[file.indices[k]];
        const Vec4& got = stripped.tangent[k];
        bool same = got[3] == want[3];
        for (int c = 0; c < 3; ++c) {
            same = same && std::abs(got[c] - want[c]) <= 1e-3f;
        }
        if (!same && close + 8 > k) { // the first mismatches, for the log
            MESSAGE("vertex " << k << ": file (" << want[0] << ", " << want[1] << ", " << want[2] << ", " << want[3]
                              << ") MikkTSpace (" << got[0] << ", " << got[1] << ", " << got[2] << ", " << got[3] << ")");
        }
        close += same ? 1u : 0u;
    }
    const double ratio = static_cast<double>(close) / static_cast<double>(file.indices.size());
    MESSAGE("MikkTSpace vs file tangents: " << close << " / " << file.indices.size() << " within 1e-3");
    CHECK(ratio >= 0.99);
}

TEST_CASE("NormalTangentTest (normal map, no TANGENT): MikkTSpace tangents generated, unit and orthogonal to N") {
    const Loaded l = load(sampleFile("NormalTangentTest", "glTF"));
    const ModelData& m = ok(l);
    const Primitive& p = m.meshes.at(0).primitives.at(0);
    CHECK(p.generatedTangents);
    REQUIRE(p.tangent.size() == p.vertexCount);
    REQUIRE(p.normal.size() == p.vertexCount);
    size_t good = 0;
    for (uint32_t i = 0; i < p.vertexCount; ++i) {
        const Vec3 t = xyz(p.tangent[i]);
        good += std::abs(len3(t) - 1.0f) < 1e-3f && std::abs(dot3(t, p.normal[i])) < 1e-3f &&
                        std::abs(std::abs(p.tangent[i][3]) - 1.0f) == 0.0f
                    ? 1u
                    : 0u;
    }
    CHECK(good == p.vertexCount);
    CHECK(l.has(Diagnostic::Info, "MikkTSpace"));
}

TEST_CASE("Cameras, SimpleSkin, SimpleMorph: parsed counts and values") {
    {
        const Loaded l1 = load(sampleFile("Cameras", "glTF"));
        const ModelData& m = ok(l1);
        REQUIRE(m.cameras.size() == 2);
        CHECK(m.cameras[0].type == Camera::Type::Perspective);
        CHECK(m.cameras[0].yfov == doctest::Approx(0.7f));
        CHECK(m.cameras[0].aspectRatio == doctest::Approx(1.0f));
        CHECK(m.cameras[0].znear == doctest::Approx(0.01f));
        CHECK(m.cameras[0].zfar == doctest::Approx(100.0f));
        CHECK(m.cameras[1].type == Camera::Type::Orthographic);
        CHECK(m.cameras[1].xmag == doctest::Approx(1.0f));
        CHECK(m.cameras[1].ymag == doctest::Approx(1.0f));
        CHECK(m.nodes.size() == 3);
    }
    {
        const Loaded l2 = load(sampleFile("SimpleSkin", "glTF"));
        const ModelData& m = ok(l2);
        REQUIRE(m.skins.size() == 1);
        CHECK(m.skins[0].joints.size() == 2);
        CHECK(m.skins[0].inverseBindMatrices.size() == 2);
        CHECK(m.animations.size() == 1);
        const Primitive& p = m.meshes.at(0).primitives.at(0);
        CHECK(p.joints[0].size() == p.vertexCount);
        CHECK(p.weights[0].size() == p.vertexCount);
        CHECK(m.nodes.size() == 3);
        int skinned = 0;
        for (const Node& n : m.nodes) {
            skinned += n.skin == 0 ? 1 : 0;
        }
        CHECK(skinned == 1);
        CHECK(m.nodes[2].parent == 1); // node 1 (joint) -> child 2 (joint)
    }
    {
        const Loaded l3 = load(sampleFile("SimpleMorph", "glTF"));
        const ModelData& m = ok(l3);
        REQUIRE(m.meshes.size() == 1);
        CHECK(m.meshes[0].weights == std::vector<float>{0.5f, 0.5f});
        const Primitive& p = m.meshes[0].primitives.at(0);
        REQUIRE(p.targets.size() == 2);
        CHECK(p.targets[0].position.size() == 3);
        CHECK(p.targets[1].position.size() == 3);
        CHECK(p.targets[0].normal.empty());
        REQUIRE(m.animations.size() == 1);
        REQUIRE(m.animations[0].channels.size() == 1);
        CHECK(m.animations[0].channels[0].path == AnimationChannel::Path::Weights);
    }
}

TEST_CASE("caps: MaxVertices drops a primitive, MaxTextureSize downscales, both with a warning") {
    LoadOptions o;
    o.maxVertices = 23; // Box has 24
    const Loaded box = load(sampleFile("Box", "glTF"), o);
    const ModelData& m = ok(box);
    CHECK(m.meshes.at(0).primitives.empty());
    CHECK(box.has(Diagnostic::Warning, "r.Asset.MaxVertices"));

    LoadOptions t;
    t.maxTextureSize = 64; // BoxTextured's image is 256 x 256
    const Loaded tex = load(sampleFile("BoxTextured", "glTF-Binary"), t);
    const ModelData& mt = ok(tex);
    CHECK(mt.images.at(0).width == 64);
    CHECK(mt.images.at(0).height == 64);
    CHECK(mt.images.at(0).rgba8.size() == size_t{64} * 64 * 4);
    CHECK(tex.has(Diagnostic::Warning, "r.Asset.MaxTextureSize"));

    LoadOptions f;
    f.maxFileBytes = 100; // Box.gltf is larger
    const Loaded big = load(sampleFile("Box", "glTF"), f);
    REQUIRE_FALSE(big.model.has_value());
    CHECK(big.model.error().exitCode == 2);
    CHECK(big.model.error().message.find("r.Asset.MaxFileBytes") != std::string::npos);
}

} // TEST_SUITE

TEST_SUITE("asset.missing") {

TEST_CASE("no NORMAL -> flat normals (+Z for the counter-clockwise triangle)") {
    const Loaded l4 = load(f03Dir() / "missing" / "no_normals.gltf");
    const ModelData& m = ok(l4);
    const Primitive& p = m.meshes.at(0).primitives.at(0);
    CHECK(p.generatedNormals);
    REQUIRE(p.normal.size() == p.vertexCount);
    for (const Vec3& n : p.normal) {
        CHECK(n == Vec3{0.0f, 0.0f, 1.0f});
    }
}

TEST_CASE("normal map without TANGENT -> MikkTSpace tangents") {
    const Loaded l = load(f03Dir() / "missing" / "no_tangents_normal_map.gltf");
    const ModelData& m = ok(l);
    const Primitive& p = m.meshes.at(0).primitives.at(0);
    CHECK(p.generatedTangents);
    REQUIRE(p.tangent.size() == p.vertexCount);
    // UV u grows along +X, v along +Y in position space: tangent +X, bitangent sign +1.
    for (const Vec4& t : p.tangent) {
        CHECK(t[0] == doctest::Approx(1.0f));
        CHECK(t[1] == doctest::Approx(0.0f));
        CHECK(t[2] == doctest::Approx(0.0f));
        CHECK(std::abs(t[3]) == 1.0f);
    }
    CHECK(m.images.at(0).colorSpace == ColorSpace::Linear); // a normal map
}

TEST_CASE("texture without its TEXCOORD -> warning, UV (0, 0)") {
    const Loaded l = load(f03Dir() / "missing" / "no_uv_with_texture.gltf");
    const ModelData& m = ok(l);
    const Primitive& p = m.meshes.at(0).primitives.at(0);
    CHECK(p.generatedTexCoord);
    REQUIRE(p.texCoord[0].size() == p.vertexCount);
    for (const Vec2& uv : p.texCoord[0]) {
        CHECK(uv == Vec2{0.0f, 0.0f});
    }
    CHECK_MESSAGE(l.has(Diagnostic::Warning, "TEXCOORD_0"), l.all());
}

TEST_CASE("non-finite POSITION / UV with a normal map and no TANGENT: loads, finite generated tangents") {
    const Loaded l = load(f03Dir() / "missing" / "nonfinite_tangent_input.gltf");
    const ModelData& m = ok(l);
    const Primitive& p = m.meshes.at(0).primitives.at(0);
    CHECK(p.generatedTangents);
    REQUIRE(p.tangent.size() == p.vertexCount);
    for (const Vec4& t : p.tangent) {
        for (const float v : t) {
            CHECK(std::isfinite(v));
        }
    }
}

TEST_CASE("no material -> the default material (-1), loads without diagnostics") {
    const Loaded l = load(f03Dir() / "missing" / "no_material.gltf");
    const ModelData& m = ok(l);
    CHECK(m.materials.empty());
    CHECK(m.meshes.at(0).primitives.at(0).material == -1);
    CHECK_MESSAGE(l.diags.empty(), l.all());
}

TEST_CASE("unknown extension in extensionsUsed -> warning, ignored") {
    const Loaded l = load(f03Dir() / "missing" / "unknown_used_extension.gltf");
    const ModelData& m = ok(l);
    CHECK(m.extensionsUsed == std::vector<std::string>{"EXT_does_not_exist"});
    CHECK_MESSAGE(l.has(Diagnostic::Warning, "EXT_does_not_exist"), l.all());
}

TEST_CASE("later-phase extension in extensionsUsed -> info, parsed and ignored, raw JSON kept for F6") {
    const Loaded l = load(f03Dir() / "missing" / "later_phase_used_extension.gltf");
    const ModelData& m = ok(l);
    CHECK_MESSAGE(l.has(Diagnostic::Info, "KHR_materials_clearcoat"), l.all());
    CHECK_MESSAGE(l.has(Diagnostic::Info, "F6"), l.all());
    CHECK_FALSE(l.has(Diagnostic::Warning, "KHR_materials_clearcoat"));
    REQUIRE(m.materials.size() == 1);
    CHECK(m.materials[0].extensionsJson.find("clearcoatFactor") != std::string::npos);
}

} // TEST_SUITE

TEST_SUITE("asset.paths") {

TEST_CASE("external .bin / .png: non-ASCII names with spaces, percent-encoded and raw UTF-8, same and sibling dir") {
    for (const char* rel : {"paths/çıktı klasörü/model with spaces.gltf", "paths/çıktı klasörü/raw utf8 uri.gltf",
                            "paths/sahne ı/sibling dir.gltf"}) {
        CAPTURE(rel);
        const Loaded l = load(f03Dir() / u8path(rel));
        const ModelData& m = ok(l);
        const Primitive& p = m.meshes.at(0).primitives.at(0);
        CHECK(p.vertexCount == 3);
        CHECK(p.position.at(1) == Vec3{1.0f, 0.0f, 0.0f});
        REQUIRE(m.images.size() == 1);
        CHECK(m.images[0].decoded);
        CHECK(m.images[0].width == 2);
        CHECK(m.images[0].rgba8.at(0) == 255); // first pixel red
        CHECK(m.images[0].rgba8.at(1) == 0);
        CHECK_MESSAGE(l.diags.empty(), l.all());
    }
}

} // TEST_SUITE
