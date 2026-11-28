// Renderer facade (plan F3 Task 4, spec K-D). Uses ONLY include/engine/*.h (the K-D rule the apps follow; the target
// has no engine-internal include path) plus nlohmann/json for the test's own reading of the glTF files.
//
//   renderer_unit_tests --backend=vulkan|d3d11|d3d11-fl10_0 [doctest options]
//   (one ctest entry per backend configuration, exit 77 = absent; d3d11-fl10_0 = D3D11 capped at FL10_0 = T0: SM 4.0
//   shaders, the skin cbuffer, morph targets as vertex attributes)
//
// Every case renders offscreen with r.Background=Color (magenta, r.Background.Color) so "background" is a known colour,
// and checks the validation message count is 0. Probes come from the assets' design, never from a render:
//   BoxTextured          the centre pixel is not background (the box covers the fit camera's centre);
//   OrientationTest      each arrow seen from outside its face: a point on its shaft (the node transform applied to a
//                        point of its mesh, the camera's projection computed here) has the arrow's colour; quaternion
//                        arrows (X1, Y1, Z1) and matrix arrows (X2, Y2, Z2);
//   NegativeScaleTest    CheckAndX.png: green checks show on correctly culled faces, red X marks only on wrongly culled
//                        ones (README); red-X pixels ~ 0, green pixels present;
//   AlphaBlendModeTest   AlphaBlendLabels.png: green checks / red X decals show checks only when OPAQUE / BLEND / MASK
//                        are right (README): no red-X pixel, every test column shows green;
//   f03/alpha            MASK and BLEND over the bare background (AlphaBlendModeTest's cut-outs show its marble bed,
//                        not the background): the cut-out is the background, the blend a half / half mix;
//   SimpleSkin, RecursiveSkeletons  rest pose with skinning renders (something drawn), validation 0;
//   f03/rest             a quad displaced +2 in x by a skin (joint translated) or a morph target (weight 0.5 of +4):
//                        drawn at the displaced place, nothing at the origin;
//   f03/colorspace/emissive_view  the renderer's slot -> view table (Review Focus 3): an emissive texture sampled
//                        midway equals the emissiveFactor of its sRGB-decoded midpoint;
//   f03/colorspace/slot_views  the same for baseColor (sRGB view: texture quad = factor quad) and occlusion (linear
//                        view: a 2 x 1 texture's midpoint = a 1 x 1 texture of that value);
//   f03/colorspace/mr_normal_views  the same for metallicRoughness and normal (linear views: a 2 x 1 texture's
//                        midpoint = a 1 x 1 texture of that value);
//   f03/missing          Review Focus 4: no material (default material), no normals, no tangents with a normal map, no
//                        UVs with a texture: each drawn; non-finite tangent input: renders with validation 0.
// Every case ends with finish() (every GPU object released, GPU idle) and validation 0.
#include <engine/Renderer.h>

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace {

eng::RendererDesc g_desc; // backend configuration of this run (main)

std::filesystem::path u8(std::string_view s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}
std::filesystem::path sample(const char* model, const char* variant = "glTF") {
    const std::string ext = std::string(variant) == "glTF-Binary" ? ".glb" : ".gltf";
    return u8(ENG_TEST_SAMPLE_MODELS) / model / variant / (std::string(model) + ext);
}

constexpr uint32_t kSize = 384;
constexpr std::array<uint8_t, 3> kMagenta{255, 0, 255};

struct Fixture {
    std::unique_ptr<eng::Renderer> r;
    eng::SceneHandle scene;
    eng::Camera camera;
};

Fixture open(const std::filesystem::path& file) {
    eng::RendererDesc d = g_desc;
    d.width = d.height = kSize;
    d.shaderDir = u8(ENG_TEST_SHADER_LIB_DIR);
    d.assetDir = u8(ENG_TEST_ASSET_DIR);
    auto r = eng::Renderer::create(d);
    REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
    Fixture f{std::move(*r), {}, {}};
    REQUIRE(f.r->settings().set("r.Background", "Color"));
    REQUIRE(f.r->settings().set("r.Background.Color", "1,0,1"));
    auto a = f.r->loadAsset(file);
    REQUIRE_MESSAGE(a.has_value(), (a ? std::string() : a.error().message));
    auto s = f.r->instantiate(*a);
    REQUIRE_MESSAGE(s.has_value(), (s ? std::string() : s.error().message));
    f.scene = *s;
    f.camera = f.r->fitCamera(f.scene, 1.0f);
    f.r->setCamera(f.camera);
    return f;
}

// finish(): releases every GPU object and waits; the teardown's validation messages count too.
void done(Fixture& f) {
    auto r = f.r->finish();
    REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
    CHECK(f.r->validationErrorCount() == 0);
}

eng::Image render(Fixture& f, uint32_t size = kSize) {
    auto rf = f.r->renderFrame();
    REQUIRE_MESSAGE(rf.has_value(), (rf ? std::string() : rf.error().message));
    auto img = f.r->readback();
    REQUIRE(img.has_value());
    REQUIRE(img->width == size);
    REQUIRE(img->height == size);
    CHECK(f.r->validationErrorCount() == 0);
    return std::move(*img);
}

std::array<int, 3> pixel(const eng::Image& img, int x, int y) {
    x = std::clamp(x, 0, int(img.width) - 1);
    y = std::clamp(y, 0, int(img.height) - 1);
    const uint8_t* p = &img.rgba8[(size_t(y) * img.width + x) * 4];
    return {p[0], p[1], p[2]};
}

bool isBackground(const std::array<int, 3>& p) {
    return std::abs(p[0] - kMagenta[0]) <= 2 && std::abs(p[1] - kMagenta[1]) <= 2 && std::abs(p[2] - kMagenta[2]) <= 2;
}

// Colour classes of the test textures (CheckAndX.png, AlphaBlendLabels.png: X ~ (221, 40, 0), check ~ (0, 200, 0)):
// a channel at least twice each other one and bright enough; lighting / tonemapping keep the dominant channel.
bool isRed(const std::array<int, 3>& p) {
    return p[0] > 60 && p[0] > 2 * p[1] && p[0] > 2 * p[2];
}
bool isGreen(const std::array<int, 3>& p) {
    return p[1] > 60 && p[1] > 2 * p[0] && p[1] > 2 * p[2];
}
size_t count(const eng::Image& img, bool (*cls)(const std::array<int, 3>&)) {
    size_t n = 0;
    for (uint32_t y = 0; y < img.height; ++y) {
        for (uint32_t x = 0; x < img.width; ++x) {
            n += cls(pixel(img, int(x), int(y))) ? 1 : 0;
        }
    }
    return n;
}

// --- the test's own camera model (independent of the renderer): right-handed look-at + perspective, clip z [0, 1] --
using V3 = std::array<double, 3>;
using M4 = std::array<double, 16>; // column-major
M4 mul(const M4& a, const M4& b) {
    M4 r{};
    for (int c = 0; c < 4; ++c) {
        for (int rr = 0; rr < 4; ++rr) {
            for (int k = 0; k < 4; ++k) {
                r[c * 4 + rr] += a[k * 4 + rr] * b[c * 4 + k];
            }
        }
    }
    return r;
}
V3 sub(V3 a, V3 b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
V3 cross(V3 a, V3 b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double dot(V3 a, V3 b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
V3 norm(V3 a) {
    const double l = std::sqrt(dot(a, a));
    return {a[0] / l, a[1] / l, a[2] / l};
}
M4 viewProj(const eng::Camera& c, double aspect) {
    const V3 eye{c.position[0], c.position[1], c.position[2]}, tgt{c.target[0], c.target[1], c.target[2]};
    const V3 f = norm(sub(tgt, eye)), s = norm(cross(f, {c.up[0], c.up[1], c.up[2]})), u = cross(s, f);
    const M4 view{s[0], u[0], -f[0], 0, s[1], u[1], -f[1], 0, s[2], u[2], -f[2], 0, -dot(s, eye), -dot(u, eye), dot(f, eye), 1};
    const double t = 1.0 / std::tan(c.yfov / 2.0), n = c.znear, fa = c.zfar;
    const M4 proj{t / aspect, 0, 0, 0, 0, t, 0, 0, 0, 0, fa / (n - fa), -1, 0, 0, n * fa / (n - fa), 0};
    return mul(proj, view);
}
// World point -> pixel (x right, y down) of a kSize x kSize target.
std::array<int, 2> project(const M4& vp, V3 p, uint32_t size = kSize) {
    const double x = vp[0] * p[0] + vp[4] * p[1] + vp[8] * p[2] + vp[12];
    const double y = vp[1] * p[0] + vp[5] * p[1] + vp[9] * p[2] + vp[13];
    const double w = vp[3] * p[0] + vp[7] * p[1] + vp[11] * p[2] + vp[15];
    return {int(std::floor((x / w * 0.5 + 0.5) * size)), int(std::floor((0.5 - y / w * 0.5) * size))};
}
// glTF node -> local matrix (matrix or T * R * S).
M4 nodeMatrix(const nlohmann::json& n) {
    if (n.contains("matrix")) {
        M4 m{};
        for (int i = 0; i < 16; ++i) {
            m[i] = n["matrix"][i].get<double>();
        }
        return m;
    }
    const auto t = n.value("translation", std::vector<double>{0, 0, 0});
    const auto q = n.value("rotation", std::vector<double>{0, 0, 0, 1});
    const auto s = n.value("scale", std::vector<double>{1, 1, 1});
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    return {(1 - 2 * (y * y + z * z)) * s[0], 2 * (x * y + z * w) * s[0], 2 * (x * z - y * w) * s[0], 0,
            2 * (x * y - z * w) * s[1], (1 - 2 * (x * x + z * z)) * s[1], 2 * (y * z + x * w) * s[1], 0,
            2 * (x * z + y * w) * s[2], 2 * (y * z - x * w) * s[2], (1 - 2 * (x * x + y * y)) * s[2], 0,
            t[0], t[1], t[2], 1};
}
V3 xform(const M4& m, V3 p) {
    return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}
nlohmann::json readJson(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    REQUIRE(in.good());
    return nlohmann::json::parse(in);
}

} // namespace

int main(int argc, char** argv) {
    std::vector<char*> args;
    std::string backend;
    for (int i = 0; i < argc; ++i) {
        if (std::strncmp(argv[i], "--backend=", 10) == 0) {
            backend = argv[i] + 10;
        } else {
            args.push_back(argv[i]);
        }
    }
    if (backend != "vulkan" && backend != "d3d11" && backend != "d3d11-fl10_0") {
        std::printf("usage: renderer_unit_tests --backend=vulkan|d3d11|d3d11-fl10_0 [doctest options]\n");
        return 2;
    }
    g_desc.backend = backend == "vulkan" ? eng::rhi::Backend::Vulkan : eng::rhi::Backend::D3D11;
    g_desc.d3d11FeatureLevelCap = backend == "d3d11-fl10_0" ? 0xa000 : 0;
    g_desc.shaderDir = u8(ENG_TEST_SHADER_LIB_DIR);
    g_desc.assetDir = u8(ENG_TEST_ASSET_DIR);
    {
        eng::RendererDesc d = g_desc;
        d.shaderDir = u8(ENG_TEST_SHADER_LIB_DIR);
        d.assetDir = u8(ENG_TEST_ASSET_DIR);
        auto r = eng::Renderer::create(d);
        if (!r) {
            if (r.error().exitCode == 3) {
                std::printf("SKIP: backend %s not available: %s\n", backend.c_str(), r.error().message.c_str());
                return 77;
            }
            std::printf("FAIL: backend %s: exit %d: %s\n", backend.c_str(), r.error().exitCode, r.error().message.c_str());
            return 1;
        }
        std::printf("renderer backend: %s (%s, %s, tier %d)\n", backend.c_str(), (*r)->caps().adapterName.c_str(),
                    (*r)->caps().apiVersion.c_str(), (*r)->caps().tier);
        std::fflush(stdout);
    }
    doctest::Context ctx(static_cast<int>(args.size()), args.data());
    return ctx.run();
}

TEST_SUITE("renderer.facade") {

TEST_CASE("BoxTextured.glb: create -> load -> instantiate -> fit -> render -> readback; the centre is the box") {
    Fixture f = open(sample("BoxTextured", "glTF-Binary"));
    const eng::SceneInfo info = f.r->sceneInfo(f.scene);
    CHECK(info.drawCount == 1);
    CHECK(info.extentsValid);
    const eng::Image img = render(f);
    CHECK_FALSE(isBackground(pixel(img, kSize / 2, kSize / 2)));
    CHECK(isBackground(pixel(img, 1, 1))); // the fit camera leaves a margin around the box
    done(f);
}

TEST_CASE("OrientationTest: every arrow's shaft is where its node transform puts it (quaternions and matrices)") {
    const std::filesystem::path file = sample("OrientationTest");
    Fixture f = open(file);
    const nlohmann::json d = readJson(file);
    struct Arrow {
        const char* node;
        std::array<double, 3> shaft; // a point inside the arrow mesh's shaft, mesh space (accessor min / max: the
                                     // X / Z arrows run along +Y from -1 to 3, the Y arrows along Z)
        bool (*colour)(const std::array<int, 3>&);
    };
    const auto red = [](const std::array<int, 3>& p) { return p[0] > 40 && p[0] > 2 * p[1] && p[0] > 2 * p[2]; };
    const auto green = [](const std::array<int, 3>& p) { return p[1] > 40 && p[1] > 2 * p[0] && p[1] > 2 * p[2]; };
    const auto blue = [](const std::array<int, 3>& p) { return p[2] > 40 && p[2] > 2 * p[0] && p[2] > 2 * p[1]; };
    const auto cyan = [](const std::array<int, 3>& p) { return p[1] > 40 && p[2] > 40 && p[1] > 2 * p[0] && p[2] > 2 * p[0]; };
    const auto magenta = [](const std::array<int, 3>& p) { return p[0] > 40 && p[2] > 40 && p[0] > 2 * p[1] && p[2] > 2 * p[1]; };
    const auto yellow = [](const std::array<int, 3>& p) { return p[0] > 40 && p[1] > 40 && p[0] > 2 * p[2] && p[1] > 2 * p[2]; };
    // MatX1 red, MatX2 cyan, MatY1 green, MatY2 magenta, MatZ1 blue, MatZ2 yellow (materials of the file).
    const Arrow arrows[] = {{"ArrowX1", {0, 1.5, 0}, red},   {"ArrowX2", {0, 1.5, 0}, cyan},
                            {"ArrowY1", {0, 0, -1.5}, green}, {"ArrowY2", {0, 0, 1.5}, magenta},
                            {"ArrowZ1", {0, 1.5, 0}, blue},  {"ArrowZ2", {0, 1.5, 0}, yellow}};
    for (const Arrow& a : arrows) {
        const std::string arrow = a.node;
        CAPTURE(arrow);
        const auto& nodes = d.at("nodes");
        const auto it = std::find_if(nodes.begin(), nodes.end(),
                                     [&](const nlohmann::json& n) { return n.value("name", "") == a.node; });
        REQUIRE(it != nodes.end());
        const M4 world = nodeMatrix(*it); // arrows are root nodes of the scene
        const V3 shaftWorld = xform(world, {a.shaft[0], a.shaft[1], a.shaft[2]});
        // Look at the arrow's cube face from outside: from 4 x the face centre (the node's translation), at the cube.
        const V3 face{world[12], world[13], world[14]};
        eng::Camera c = f.camera;
        c.position = {float(face[0] * 4), float(face[1] * 4), float(face[2] * 4)};
        c.target = {0, 0, 0};
        c.up = std::abs(face[1]) > 0.5 ? std::array<float, 3>{0, 0, -1} : std::array<float, 3>{0, 1, 0};
        f.r->setCamera(c);
        const eng::Image img = render(f);
        const auto px = project(viewProj(c, 1.0), shaftWorld);
        CAPTURE(px[0]);
        CAPTURE(px[1]);
        const auto p = pixel(img, px[0], px[1]);
        CAPTURE(p[0]);
        CAPTURE(p[1]);
        CAPTURE(p[2]);
        CHECK(a.colour(p));
    }
    done(f);
}

TEST_CASE("NegativeScaleTest: green checks shown, no red X (negative determinant flips the front face)") {
    Fixture f = open(sample("NegativeScaleTest"));
    const eng::Image img = render(f);
    const size_t red = count(img, isRed), green = count(img, isGreen);
    MESSAGE("NegativeScaleTest: red-X pixels " << red << ", green-check pixels " << green);
    CHECK(red == 0);
    CHECK(green >= 200); // six checks (front / back / sample x scale 1 / -1), about 80 pixels each at 384 x 384
    done(f);
}

TEST_CASE("AlphaBlendModeTest: every column shows its green check, no red X") {
    // AlphaBlendLabels.png (README): under each test quad a decal shows a green check when OPAQUE / BLEND / MASK
    // (cutoff 0.25, default 0.5, 0.75) is applied right, and a red X otherwise.
    // 1024 x 1024: the label checks are a few pixels wide at the fit camera's distance.
    constexpr uint32_t size = 1024;
    const std::filesystem::path file = sample("AlphaBlendModeTest");
    Fixture f = open(file);
    f.r->resize(size, size);
    const eng::Image img = render(f, size);
    const size_t red = count(img, isRed);
    MESSAGE("AlphaBlendModeTest: red-X pixels " << red << ", green-check pixels " << count(img, isGreen));
    CHECK(red == 0);
    // One column per test node (translation x = -3, -1.5, 0, 1.5, 3): a band +-0.75 around it, projected.
    const nlohmann::json d = readJson(file);
    const M4 vp = viewProj(f.camera, 1.0);
    for (const char* nodeName : {"TestOpaque", "TestBlend", "TestCutoff25", "TestCutoffDefault", "TestCutoff75"}) {
        const std::string node = nodeName;
        CAPTURE(node);
        const auto& nodes = d.at("nodes");
        const auto it = std::find_if(nodes.begin(), nodes.end(),
                                     [&](const nlohmann::json& n) { return n.value("name", "") == node; });
        REQUIRE(it != nodes.end());
        const double x = it->value("translation", std::vector<double>{0, 0, 0})[0];
        const int x0 = project(vp, {x - 0.75, 0.0, 0.0}, size)[0], x1 = project(vp, {x + 0.75, 0.0, 0.0}, size)[0];
        size_t green = 0;
        for (int px = std::max(0, x0); px < std::min(int(size), x1); ++px) {
            for (uint32_t py = 0; py < size; ++py) {
                green += isGreen(pixel(img, px, int(py))) ? 1 : 0;
            }
        }
        CAPTURE(green);
        CHECK(green >= 8);
    }
    done(f);
}

TEST_CASE("MASK cuts below alphaCutoff to the background; BLEND mixes with it (tests/assets/f03/alpha)") {
    // scripts/make_f03_assets.py: quad A (x -1.1 .. -0.1) MASK 0.5, left texel alpha 0, right texel alpha 1 (green);
    // quad B (x 0.1 .. 1.1) BLEND, baseColorFactor (0, 0, 1, 0.5). Over the magenta background (nothing behind).
    Fixture f = open(u8(ENG_TEST_ASSET_DIR) / "f03" / "alpha" / "mask_and_blend.gltf");
    const eng::Image img = render(f);
    const M4 vp = viewProj(f.camera, 1.0);
    const auto at = [&](double x, double y) {
        const auto px = project(vp, {x, y, 0.0});
        return pixel(img, px[0], px[1]);
    };
    const auto cut = at(-0.85, 0.0), kept = at(-0.35, 0.0), mixed = at(0.6, 0.0);
    CAPTURE(cut[0]);
    CAPTURE(kept[1]);
    CAPTURE(mixed[0]);
    CAPTURE(mixed[1]);
    CAPTURE(mixed[2]);
    CHECK(isBackground(cut));
    CHECK(isGreen(kept));
    // 50 % blend of a blue surface over magenta (255, 0, 255) in the target's encoded values: red ~ 255 / 2.
    CHECK_FALSE(isBackground(mixed));
    CHECK(mixed[0] >= 90);
    CHECK(mixed[0] <= 170);
    CHECK(mixed[2] >= 200);
    CHECK(mixed[1] <= 40);
    done(f);
}

TEST_CASE("skinned rest pose renders: SimpleSkin, RecursiveSkeletons") {
    for (const char* model : {"SimpleSkin", "RecursiveSkeletons"}) {
        const std::string name = model;
        CAPTURE(name);
        Fixture f = open(sample(model));
        const eng::Image img = render(f);
        size_t drawn = 0;
        for (uint32_t y = 0; y < img.height; ++y) {
            for (uint32_t x = 0; x < img.width; ++x) {
                drawn += isBackground(pixel(img, int(x), int(y))) ? 0 : 1;
            }
        }
        MESSAGE(std::string(model) << ": " << drawn << " non-background pixels");
        CHECK(drawn > 500);
        done(f);
    }
}

TEST_CASE("Review Focus 4: missing material / normals / tangents / UVs each render") {
    // f03/missing (scripts/make_f03_assets.py): one triangle (0, 0, 0) (1, 0, 0) (0, 1, 0) facing +Z; the fit camera
    // looks at it from +Z, so its centroid is drawn whatever the loader generated for it.
    for (const char* name : {"no_material", "no_normals", "no_tangents_normal_map", "no_uv_with_texture"}) {
        const std::string file = std::string(name) + ".gltf";
        CAPTURE(file);
        Fixture f = open(u8(ENG_TEST_ASSET_DIR) / "f03" / "missing" / u8(file));
        const eng::Image img = render(f);
        const M4 vp = viewProj(f.camera, 1.0);
        const auto px = project(vp, {1.0 / 3.0, 1.0 / 3.0, 0.0});
        const auto p = pixel(img, px[0], px[1]);
        CAPTURE(p[0]);
        CAPTURE(p[1]);
        CAPTURE(p[2]);
        CHECK_FALSE(isBackground(p));
        if (file == "no_material.gltf") {
            // The glTF default material (white base colour, metallic 1, roughness 1): grey, lit by the IBL.
            CHECK(std::abs(p[0] - p[1]) <= 24);
            CHECK(std::abs(p[1] - p[2]) <= 24);
        }
        done(f);
    }
    // NaN / Inf position and UV of one vertex with a normal map and no TANGENT: loads, renders, validation 0.
    Fixture f = open(u8(ENG_TEST_ASSET_DIR) / "f03" / "missing" / "nonfinite_tangent_input.gltf");
    (void)render(f);
    done(f);
}

TEST_CASE("rest pose: a skin and a morph target move the quad +2 in x (tests/assets/f03/rest)") {
    for (const char* name : {"skin_quad.gltf", "morph_quad.gltf"}) {
        const std::string file = name;
        CAPTURE(file);
        Fixture f = open(u8(ENG_TEST_ASSET_DIR) / "f03" / "rest" / u8(file));
        // The fit camera frames the undeformed quad (Sample-Viewer extents ignore skins and morphs); this camera sees
        // both the origin and x = 2.
        eng::Camera c;
        c.position = {1.0f, 0.0f, 6.0f};
        c.target = {1.0f, 0.0f, 0.0f};
        c.yfov = 0.785398163f;
        c.znear = 0.1f;
        c.zfar = 100.0f;
        f.r->setCamera(c);
        const eng::Image img = render(f);
        const M4 vp = viewProj(c, 1.0);
        const auto moved = project(vp, {2.0, 0.0, 0.0}), origin = project(vp, {0.0, 0.0, 0.0});
        const auto pm = pixel(img, moved[0], moved[1]), po = pixel(img, origin[0], origin[1]);
        CAPTURE(pm[0]);
        CAPTURE(po[0]);
        CHECK_FALSE(isBackground(pm));
        CHECK(isBackground(po));
        done(f);
    }
}

TEST_CASE("Review Focus 3 in the renderer: the emissive texture's view decodes sRGB before filtering") {
    // f03/colorspace/emissive_view.gltf: quad A emits emissiveFactor (0, srgbToLinear(188 / 255) / 2, 0); quad B emits
    // its 2 x 1 texture (0, 0, 0) - (0, 188, 0) sampled midway with emissiveFactor 1. Same black base, same normal: both
    // must give the same pixel. GpuModel's slot table makes emissive an sRGB view; a linear view makes B brighter.
    Fixture f = open(u8(ENG_TEST_ASSET_DIR) / "f03" / "colorspace" / "emissive_view.gltf");
    const eng::Image img = render(f);
    const M4 vp = viewProj(f.camera, 1.0);
    const auto pa = project(vp, {-0.6, 0.0, 0.0}), pb = project(vp, {0.6, 0.0, 0.0});
    const auto a = pixel(img, pa[0], pa[1]), b = pixel(img, pb[0], pb[1]);
    MESSAGE("emissive factor quad " << a[0] << "," << a[1] << "," << a[2] << " / texture quad " << b[0] << "," << b[1]
                                    << "," << b[2]);
    CHECK_FALSE(isBackground(a));
    CHECK(std::abs(a[1] - b[1]) <= 3);
    CHECK(std::abs(a[0] - b[0]) <= 3);
    CHECK(std::abs(a[2] - b[2]) <= 3);
    done(f);
}

TEST_CASE("Review Focus 3 in the renderer: baseColor is an sRGB view, occlusion a linear view") {
    // f03/colorspace/slot_views.gltf (scripts/make_f03_assets.py): top row = baseColor factor quad A vs baseColor
    // texture quad B, bottom row = 1 x 1 occlusion quad C vs 2 x 1 occlusion midpoint quad D; each pair matches only
    // when GpuModel's slot table gives baseColor an sRGB view and occlusion a linear (UNORM) view. A distant
    // camera with a narrow field keeps the view vectors of the probes (+-0.6, +-0.6) nearly equal.
    Fixture f = open(u8(ENG_TEST_ASSET_DIR) / "f03" / "colorspace" / "slot_views.gltf");
    eng::Camera c;
    c.position = {0.0f, 0.0f, 40.0f};
    c.target = {0.0f, 0.0f, 0.0f};
    c.yfov = 2.0f * std::atan(1.4f / 40.0f);
    c.znear = 1.0f;
    c.zfar = 100.0f;
    f.r->setCamera(c);
    const eng::Image img = render(f);
    const M4 vp = viewProj(c, 1.0);
    const auto probe = [&](double x, double y) {
        const auto p = project(vp, {x, y, 0.0});
        return pixel(img, p[0], p[1]);
    };
    const auto a = probe(-0.6, 0.6), b = probe(0.6, 0.6), cq = probe(-0.6, -0.6), d = probe(0.6, -0.6);
    MESSAGE("baseColor factor " << a[0] << "," << a[1] << "," << a[2] << " / texture " << b[0] << "," << b[1] << ","
                                << b[2]);
    MESSAGE("occlusion 1 x 1 " << cq[0] << "," << cq[1] << "," << cq[2] << " / 2 x 1 midpoint " << d[0] << ","
                               << d[1] << "," << d[2]);
    for (const auto& p : {a, b, cq, d}) {
        CHECK_FALSE(isBackground(p));
    }
    for (int ch = 0; ch < 3; ++ch) {
        CAPTURE(ch);
        CHECK(std::abs(a[ch] - b[ch]) <= 3);
        CHECK(std::abs(cq[ch] - d[ch]) <= 3);
    }
    done(f);
}

TEST_CASE("Review Focus 3 in the renderer: metallicRoughness and normal are linear views") {
    // f03/colorspace/mr_normal_views.gltf (scripts/make_f03_assets.py): top row = 1 x 1 metallicRoughness quad A vs
    // 2 x 1 metallicRoughness midpoint quad B (metallic 1, coloured base), bottom row = 1 x 1 normal quad C vs 2 x 1
    // normal midpoint quad D; each pair matches only when GpuModel's slot table gives the slot a linear (UNORM)
    // view. Same distant, narrow camera as the baseColor / occlusion case.
    Fixture f = open(u8(ENG_TEST_ASSET_DIR) / "f03" / "colorspace" / "mr_normal_views.gltf");
    eng::Camera c;
    c.position = {0.0f, 0.0f, 40.0f};
    c.target = {0.0f, 0.0f, 0.0f};
    c.yfov = 2.0f * std::atan(1.4f / 40.0f);
    c.znear = 1.0f;
    c.zfar = 100.0f;
    f.r->setCamera(c);
    const eng::Image img = render(f);
    const M4 vp = viewProj(c, 1.0);
    const auto probe = [&](double x, double y) {
        const auto p = project(vp, {x, y, 0.0});
        return pixel(img, p[0], p[1]);
    };
    const auto a = probe(-0.6, 0.6), b = probe(0.6, 0.6), cq = probe(-0.6, -0.6), d = probe(0.6, -0.6);
    MESSAGE("metallicRoughness 1 x 1 " << a[0] << "," << a[1] << "," << a[2] << " / 2 x 1 midpoint " << b[0] << ","
                                       << b[1] << "," << b[2]);
    MESSAGE("normal 1 x 1 " << cq[0] << "," << cq[1] << "," << cq[2] << " / 2 x 1 midpoint " << d[0] << "," << d[1]
                            << "," << d[2]);
    for (const auto& p : {a, b, cq, d}) {
        CHECK_FALSE(isBackground(p));
    }
    for (int ch = 0; ch < 3; ++ch) {
        CAPTURE(ch);
        CHECK(std::abs(a[ch] - b[ch]) <= 3);
        CHECK(std::abs(cq[ch] - d[ch]) <= 3);
    }
    done(f);
}

} // TEST_SUITE
