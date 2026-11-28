#pragma once
// Helpers shared by the built-in scenes (internal to headless_core): column-major 4x4 matrices, sRGB
// conversion, handle ownership, shader / pipeline loading.
#include "rhi/Rhi.h"
#include "rhi/ShaderLibrary.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

namespace eng::scenes::detail {

// Column-major 4x4 (element (row r, column c) at m[c * 4 + r]): the layout of an HLSL column_major
// float4x4, used with mul(M, v) in the shaders.
struct Mat4 {
    std::array<float, 16> m{};
    float& at(int r, int c) { return m[static_cast<size_t>(c * 4 + r)]; }
    float at(int r, int c) const { return m[static_cast<size_t>(c * 4 + r)]; }
    static Mat4 identity() {
        Mat4 x;
        for (int i = 0; i < 4; ++i) {
            x.at(i, i) = 1.0f;
        }
        return x;
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 x;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) {
                s += a.at(r, k) * b.at(k, c);
            }
            x.at(r, c) = s;
        }
    }
    return x;
}

inline Mat4 rotationX(float rad) {
    Mat4 x = Mat4::identity();
    x.at(1, 1) = std::cos(rad);
    x.at(1, 2) = -std::sin(rad);
    x.at(2, 1) = std::sin(rad);
    x.at(2, 2) = std::cos(rad);
    return x;
}

inline Mat4 rotationY(float rad) {
    Mat4 x = Mat4::identity();
    x.at(0, 0) = std::cos(rad);
    x.at(0, 2) = std::sin(rad);
    x.at(2, 0) = -std::sin(rad);
    x.at(2, 2) = std::cos(rad);
    return x;
}

inline Mat4 translation(float tx, float ty, float tz) {
    Mat4 x = Mat4::identity();
    x.at(0, 3) = tx;
    x.at(1, 3) = ty;
    x.at(2, 3) = tz;
    return x;
}

// Right-handed perspective, clip Z in [0, 1] (RHI convention), +Y up.
inline Mat4 perspectiveRhZo(float fovY, float aspect, float zNear, float zFar) {
    const float f = 1.0f / std::tan(fovY * 0.5f);
    Mat4 x;
    x.at(0, 0) = f / aspect;
    x.at(1, 1) = f;
    x.at(2, 2) = zFar / (zNear - zFar);
    x.at(2, 3) = zNear * zFar / (zNear - zFar);
    x.at(3, 2) = -1.0f;
    return x;
}

inline constexpr float kDeg = std::numbers::pi_v<float> / 180.0f;

inline float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
inline float linearToSrgb(float c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
inline bool isSrgb(rhi::Format f) {
    return f == rhi::Format::RGBA8_SRGB || f == rhi::Format::BGRA8_SRGB;
}

// Destroys the handles it owns, in reverse order, when released or destroyed.
struct Owned {
    rhi::IDevice& dev;
    std::vector<rhi::Handle> handles;
    explicit Owned(rhi::IDevice& d) : dev(d) {}
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    template <class H>
    H keep(H h) {
        handles.push_back(h);
        return h;
    }
    void release() {
        for (auto it = handles.rbegin(); it != handles.rend(); ++it) {
            dev.destroy(*it);
        }
        handles.clear();
    }
    ~Owned() { release(); }
};

// Loads and creates the VS + PS of `shader` / `key` (entries VSMain / PSMain).
struct ShaderPair {
    rhi::ShaderHandle vs, ps;
    rhi::ShaderReflection vsReflection, psReflection;
};
// Key of a shader without a TIER axis for this device (plan F2 Task 7): a D3D11 T0 device (FL10_x) takes
// the SM 4.0 build "<key>-t0" (permutation file "tier0Variant"); every other device takes "<key>".
inline std::string tierKey(const rhi::IDevice& dev, std::string_view key) {
    const bool t0 = dev.caps().backend == rhi::Backend::D3D11 && dev.caps().tier == 0;
    return std::string(key) + (t0 ? "-t0" : "");
}

inline rhi::Result<ShaderPair> loadShaderPair(rhi::IDevice& dev, Owned& own, const std::filesystem::path& dir,
                                              std::string_view shader, std::string_view key) {
    const rhi::Backend backend = dev.caps().backend;
    auto vsBlob = rhi::ShaderLibrary::load(dir, shader, "VSMain", backend, key);
    if (!vsBlob) {
        return std::unexpected(vsBlob.error());
    }
    auto psBlob = rhi::ShaderLibrary::load(dir, shader, "PSMain", backend, key);
    if (!psBlob) {
        return std::unexpected(psBlob.error());
    }
    auto vs = dev.createShader(*vsBlob);
    if (!vs) {
        return std::unexpected(vs.error());
    }
    own.keep(*vs);
    auto ps = dev.createShader(*psBlob);
    if (!ps) {
        return std::unexpected(ps.error());
    }
    own.keep(*ps);
    return ShaderPair{*vs, *ps, vsBlob->reflection, psBlob->reflection};
}

} // namespace eng::scenes::detail
