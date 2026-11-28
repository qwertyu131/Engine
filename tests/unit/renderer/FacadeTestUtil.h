#pragma once
// Shared helpers of the renderer facade tests (test_facade.cpp, test_lights.cpp): the backend configuration of the run
// (set by main in test_facade.cpp), a fixture that opens one glTF file through the facade (offscreen, magenta
// r.Background=Color), render / readback, pixel access and the tests' own camera model (independent of the renderer).
// Uses ONLY include/engine/*.h (spec K-D) + nlohmann/json + doctest.
#include <engine/Renderer.h>

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace facade_test {

inline eng::RendererDesc g_desc; // backend configuration of this run (main)

inline std::filesystem::path u8(std::string_view s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}
inline std::filesystem::path sample(const char* model, const char* variant = "glTF") {
    const std::string ext = std::string(variant) == "glTF-Binary" ? ".glb" : ".gltf";
    return u8(ENG_TEST_SAMPLE_MODELS) / model / variant / (std::string(model) + ext);
}

constexpr uint32_t kSize = 384;
constexpr std::array<uint8_t, 3> kMagenta{255, 0, 255};

struct Fixture {
    std::unique_ptr<eng::Renderer> r;
    eng::SceneHandle scene;
    eng::Camera camera;
    eng::AssetHandle asset;
};

inline Fixture open(const std::filesystem::path& file) {
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
    f.asset = *a;
    auto s = f.r->instantiate(*a);
    REQUIRE_MESSAGE(s.has_value(), (s ? std::string() : s.error().message));
    f.scene = *s;
    f.camera = f.r->fitCamera(f.scene, 1.0f);
    f.r->setCamera(f.camera);
    return f;
}

// finish(): releases every GPU object and waits; the teardown's validation messages count too.
inline void done(Fixture& f) {
    auto r = f.r->finish();
    REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
    CHECK(f.r->validationErrorCount() == 0);
}

inline eng::Image render(Fixture& f, uint32_t size = kSize) {
    auto rf = f.r->renderFrame();
    REQUIRE_MESSAGE(rf.has_value(), (rf ? std::string() : rf.error().message));
    auto img = f.r->readback();
    REQUIRE(img.has_value());
    REQUIRE(img->width == size);
    REQUIRE(img->height == size);
    CHECK(f.r->validationErrorCount() == 0);
    return std::move(*img);
}

inline std::array<int, 3> pixel(const eng::Image& img, int x, int y) {
    x = std::clamp(x, 0, int(img.width) - 1);
    y = std::clamp(y, 0, int(img.height) - 1);
    const uint8_t* p = &img.rgba8[(size_t(y) * img.width + x) * 4];
    return {p[0], p[1], p[2]};
}

inline bool isBackground(const std::array<int, 3>& p) {
    return std::abs(p[0] - kMagenta[0]) <= 2 && std::abs(p[1] - kMagenta[1]) <= 2 && std::abs(p[2] - kMagenta[2]) <= 2;
}

// Colour classes of the test textures (CheckAndX.png, AlphaBlendLabels.png: X ~ (221, 40, 0), check ~ (0, 200, 0)):
// a channel at least twice each other one and bright enough; lighting / tonemapping keep the dominant channel.
inline bool isRed(const std::array<int, 3>& p) {
    return p[0] > 60 && p[0] > 2 * p[1] && p[0] > 2 * p[2];
}
inline bool isGreen(const std::array<int, 3>& p) {
    return p[1] > 60 && p[1] > 2 * p[0] && p[1] > 2 * p[2];
}
inline size_t count(const eng::Image& img, bool (*cls)(const std::array<int, 3>&)) {
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
inline M4 mul(const M4& a, const M4& b) {
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
inline V3 sub(V3 a, V3 b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
inline V3 cross(V3 a, V3 b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline double dot(V3 a, V3 b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline V3 norm(V3 a) {
    const double l = std::sqrt(dot(a, a));
    return {a[0] / l, a[1] / l, a[2] / l};
}
inline M4 viewProj(const eng::Camera& c, double aspect) {
    const V3 eye{c.position[0], c.position[1], c.position[2]}, tgt{c.target[0], c.target[1], c.target[2]};
    const V3 f = norm(sub(tgt, eye)), s = norm(cross(f, {c.up[0], c.up[1], c.up[2]})), u = cross(s, f);
    const M4 view{s[0], u[0], -f[0], 0, s[1], u[1], -f[1], 0, s[2], u[2], -f[2], 0, -dot(s, eye), -dot(u, eye), dot(f, eye), 1};
    const double t = 1.0 / std::tan(c.yfov / 2.0), n = c.znear, fa = c.zfar;
    const M4 proj{t / aspect, 0, 0, 0, 0, t, 0, 0, 0, 0, fa / (n - fa), -1, 0, 0, n * fa / (n - fa), 0};
    return mul(proj, view);
}
// World point -> pixel (x right, y down) of a kSize x kSize target.
inline std::array<int, 2> project(const M4& vp, V3 p, uint32_t size = kSize) {
    const double x = vp[0] * p[0] + vp[4] * p[1] + vp[8] * p[2] + vp[12];
    const double y = vp[1] * p[0] + vp[5] * p[1] + vp[9] * p[2] + vp[13];
    const double w = vp[3] * p[0] + vp[7] * p[1] + vp[11] * p[2] + vp[15];
    return {int(std::floor((x / w * 0.5 + 0.5) * size)), int(std::floor((0.5 - y / w * 0.5) * size))};
}
// glTF node -> local matrix (matrix or T * R * S).
inline M4 nodeMatrix(const nlohmann::json& n) {
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
inline V3 xform(const M4& m, V3 p) {
    return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}
inline nlohmann::json readJson(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    REQUIRE(in.good());
    return nlohmann::json::parse(in);
}

} // namespace facade_test
