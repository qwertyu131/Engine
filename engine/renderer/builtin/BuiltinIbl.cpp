// CPU IBL inputs of builtin:pbr-spheres (plan F2 Task 5; see Ibl.h).
#include "Ibl.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>

namespace eng::scenes::ibl {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

Vec3 operator+(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 operator*(Vec3 a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 lerp(Vec3 a, Vec3 b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

constexpr Vec3 kSkyHorizon{0.85f, 0.80f, 0.70f};
constexpr Vec3 kSkyZenith{0.20f, 0.40f, 0.85f};
constexpr Vec3 kGroundNear{0.30f, 0.25f, 0.20f};
constexpr Vec3 kGroundFar{0.12f, 0.10f, 0.08f};
constexpr float kGradientScale = 2.5f;
constexpr float kXTint = 0.5f;

Vec3 faceDirection(uint32_t face, float s, float t) {
    switch (face) {
    case 0: return {1.0f, -t, -s};
    case 1: return {-1.0f, -t, s};
    case 2: return {s, 1.0f, t};
    case 3: return {s, -1.0f, -t};
    case 4: return {s, -t, 1.0f};
    default: return {-s, -t, -1.0f};
    }
}

// Solid angle of texel (x, y) of a size x size face (exact, Driscoll's area element).
float texelSolidAngle(uint32_t x, uint32_t y, uint32_t size) {
    auto area = [](float a, float b) { return std::atan2(a * b, std::sqrt(a * a + b * b + 1.0f)); };
    const float inv = 1.0f / static_cast<float>(size);
    const float x0 = 2.0f * static_cast<float>(x) * inv - 1.0f, x1 = x0 + 2.0f * inv;
    const float y0 = 2.0f * static_cast<float>(y) * inv - 1.0f, y1 = y0 + 2.0f * inv;
    return area(x0, y0) - area(x0, y1) - area(x1, y0) + area(x1, y1);
}

struct Texel {
    Vec3 dir;
    float solidAngle;
    Vec3 radiance;
};

std::vector<Texel> texels(const Cube& env) {
    std::vector<Texel> out;
    const uint32_t n = env.size;
    out.reserve(size_t(6) * n * n);
    const std::vector<float>& l0 = env.levels[0];
    for (uint32_t f = 0; f < 6; ++f) {
        for (uint32_t y = 0; y < n; ++y) {
            for (uint32_t x = 0; x < n; ++x) {
                const size_t i = ((size_t(f) * n + y) * n + x) * 4u;
                out.push_back({cubeTexelDirection(f, x, y, n), texelSolidAngle(x, y, n), {l0[i], l0[i + 1], l0[i + 2]}});
            }
        }
    }
    return out;
}

float dGgx(float nDotH, float alpha) {
    const float a2 = alpha * alpha;
    const float f = (nDotH * nDotH) * (a2 - 1.0f) + 1.0f;
    return a2 / (kPi * f * f);
}

float vSmithGgxCorrelated(float nDotV, float nDotL, float roughness) {
    const float a2 = std::pow(roughness, 4.0f);
    const float ggxv = nDotL * std::sqrt(nDotV * nDotV * (1.0f - a2) + a2);
    const float ggxl = nDotV * std::sqrt(nDotL * nDotL * (1.0f - a2) + a2);
    return 0.5f / (ggxv + ggxl);
}

float radicalInverseVdC(uint32_t bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

} // namespace

Vec3 normalize(Vec3 v) {
    const float l = std::sqrt(dot(v, v));
    return l > 0.0f ? v * (1.0f / l) : v;
}

Vec3 environmentRadiance(Vec3 d) {
    if (dot(d, normalize(kSunDirection)) >= std::cos(kSunRadiusDeg * kPi / 180.0f)) {
        return kSunRadiance;
    }
    const float t = std::min(1.0f, std::abs(d.y) * kGradientScale);
    const Vec3 c = d.y >= 0.0f ? lerp(kSkyHorizon, kSkyZenith, t) : lerp(kGroundNear, kGroundFar, t);
    return c * std::max(0.0f, 1.0f + kXTint * d.x);
}

Vec3 cubeTexelDirection(uint32_t face, uint32_t x, uint32_t y, uint32_t size) {
    const float s = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
    const float t = (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
    return normalize(faceDirection(face, s, t));
}

Cube environmentCube(uint32_t size) {
    Cube c;
    c.size = size;
    std::vector<float> l0(size_t(6) * size * size * 4u);
    for (uint32_t f = 0; f < 6; ++f) {
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                const Vec3 r = environmentRadiance(cubeTexelDirection(f, x, y, size));
                float* p = &l0[((size_t(f) * size + y) * size + x) * 4u];
                p[0] = r.x;
                p[1] = r.y;
                p[2] = r.z;
                p[3] = 1.0f;
            }
        }
    }
    c.levels.push_back(std::move(l0));
    return c;
}

Cube prefilterGgx(const Cube& env, uint32_t mipCount) {
    Cube out;
    out.size = env.size;
    out.levels.push_back(env.levels[0]);
    const std::vector<Texel> src = texels(env);
    for (uint32_t m = 1; m < mipCount; ++m) {
        const uint32_t s = std::max(1u, env.size >> m);
        const float roughness = static_cast<float>(m) / static_cast<float>(mipCount - 1);
        const float alpha = roughness * roughness;
        std::vector<float> level(size_t(6) * s * s * 4u);
        for (uint32_t f = 0; f < 6; ++f) {
            for (uint32_t y = 0; y < s; ++y) {
                for (uint32_t x = 0; x < s; ++x) {
                    const Vec3 n = cubeTexelDirection(f, x, y, s);
                    Vec3 sum{};
                    float weight = 0.0f;
                    for (const Texel& t : src) {
                        const float nDotL = dot(n, t.dir);
                        if (nDotL <= 0.0f) {
                            continue;
                        }
                        const Vec3 h = normalize(n + t.dir);
                        const float w = dGgx(std::max(0.0f, dot(n, h)), alpha) * nDotL * t.solidAngle;
                        sum = sum + t.radiance * w;
                        weight += w;
                    }
                    float* p = &level[((size_t(f) * s + y) * s + x) * 4u];
                    const float inv = weight > 0.0f ? 1.0f / weight : 0.0f;
                    p[0] = sum.x * inv;
                    p[1] = sum.y * inv;
                    p[2] = sum.z * inv;
                    p[3] = 1.0f;
                }
            }
        }
        out.levels.push_back(std::move(level));
    }
    return out;
}

Cube irradiance(const Cube& env, uint32_t size) {
    Cube out;
    out.size = size;
    const std::vector<Texel> src = texels(env);
    std::vector<float> level(size_t(6) * size * size * 4u);
    for (uint32_t f = 0; f < 6; ++f) {
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                const Vec3 n = cubeTexelDirection(f, x, y, size);
                Vec3 sum{};
                float weight = 0.0f;
                for (const Texel& t : src) {
                    const float nDotL = dot(n, t.dir);
                    if (nDotL <= 0.0f) {
                        continue;
                    }
                    const float w = nDotL * t.solidAngle;
                    sum = sum + t.radiance * w;
                    weight += w;
                }
                float* p = &level[((size_t(f) * size + y) * size + x) * 4u];
                const float inv = weight > 0.0f ? 1.0f / weight : 0.0f;
                p[0] = sum.x * inv;
                p[1] = sum.y * inv;
                p[2] = sum.z * inv;
                p[3] = 1.0f;
            }
        }
    }
    out.levels.push_back(std::move(level));
    return out;
}

std::vector<float> brdfLut(uint32_t size, uint32_t samples) {
    std::vector<float> lut(size_t(size) * size * 4u);
    const Vec3 n{0.0f, 0.0f, 1.0f};
    for (uint32_t j = 0; j < size; ++j) {
        const float roughness = (static_cast<float>(j) + 0.5f) / static_cast<float>(size);
        const float alpha = roughness * roughness;
        for (uint32_t i = 0; i < size; ++i) {
            const float nDotV = (static_cast<float>(i) + 0.5f) / static_cast<float>(size);
            const Vec3 v{std::sqrt(1.0f - nDotV * nDotV), 0.0f, nDotV};
            float a = 0.0f, b = 0.0f;
            for (uint32_t k = 0; k < samples; ++k) {
                // Sample-Viewer GGX(xi, roughness): xi = hammersley2d(k, samples)
                const float xi0 = static_cast<float>(k) / static_cast<float>(samples);
                const float xi1 = radicalInverseVdC(k);
                const float cosTheta =
                    std::clamp(std::sqrt((1.0f - xi1) / (1.0f + (alpha * alpha - 1.0f) * xi1)), 0.0f, 1.0f);
                const float sinTheta = std::sqrt(1.0f - cosTheta * cosTheta);
                const float phi = 2.0f * kPi * xi0;
                // generateTBN(N = +Z): bitangent +Y, tangent = normalize(cross(bitangent, N)) = +X.
                const Vec3 tangent = normalize(cross({0.0f, 1.0f, 0.0f}, n));
                const Vec3 bitangent = cross(n, tangent);
                const Vec3 h = normalize(tangent * (sinTheta * std::cos(phi)) + bitangent * (sinTheta * std::sin(phi)) +
                                         n * cosTheta);
                const Vec3 l = normalize(h * (2.0f * dot(v, h)) + v * -1.0f); // reflect(-V, H)
                const float nDotL = std::clamp(l.z, 0.0f, 1.0f);
                const float nDotH = std::clamp(h.z, 0.0f, 1.0f);
                const float vDotH = std::clamp(dot(v, h), 0.0f, 1.0f);
                if (nDotL > 0.0f) {
                    const float vPdf = vSmithGgxCorrelated(nDotV, nDotL, roughness) * vDotH * nDotL / nDotH;
                    const float fc = std::pow(1.0f - vDotH, 5.0f);
                    a += (1.0f - fc) * vPdf;
                    b += fc * vPdf;
                }
            }
            float* p = &lut[(size_t(j) * size + i) * 4u];
            p[0] = 4.0f * a / static_cast<float>(samples);
            p[1] = 4.0f * b / static_cast<float>(samples);
            p[2] = 0.0f;
            p[3] = 1.0f;
        }
    }
    return lut;
}

uint16_t floatToHalf(float f) {
    const uint32_t x = std::bit_cast<uint32_t>(f);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exp = static_cast<int32_t>((x >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFFu) == 0xFFu) { // inf / nan
        return static_cast<uint16_t>(sign | 0x7C00u | (mant ? 0x200u : 0u));
    }
    if (exp >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    if (exp <= 0) {
        if (exp < -10) {
            return static_cast<uint16_t>(sign);
        }
        mant |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exp);
        uint32_t half = mant >> shift;
        const uint32_t rem = mant & ((1u << shift) - 1u), halfway = 1u << (shift - 1u);
        if (rem > halfway || (rem == halfway && (half & 1u))) {
            ++half;
        }
        return static_cast<uint16_t>(sign | half);
    }
    uint32_t half = (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
    const uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) {
        ++half; // may carry into the exponent (correct rounding up to inf)
    }
    return static_cast<uint16_t>(sign | half);
}

} // namespace eng::scenes::ibl
