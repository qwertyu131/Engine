// CPU path of the IBL (plan F3 Task 5, T0 devices): glTF-Sample-Renderer v1.1.0 (d59d1d6) iblSampler
// (source/ibl_sampler.js) + panorama_to_cubemap.frag + ibl_filtering.frag, ported by value; the GPU twin is
// shaders/ibl/*.hlsl (Ibl.cpp). Function names follow ibl_filtering.frag. See Ibl.h.
//
// Texture sampling as the GPU does it:
//   panorama  texture(u_panorama, uv): bilinear on level 0 (LINEAR, no mips), MIRRORED_REPEAT; uv stays in [0, 1]
//             (dirToUV), where mirrored repeat and clamp pick the same edge texels;
//   cube      textureLod(uCubeMap, dir, lod): lod clamped to [0, levels - 1], trilinear between floor / ceil levels,
//             seamless bilinear (a tap past a face edge reads the neighbouring face's texel along that direction, a
//             tap past a corner the mean of the three texels meeting there, as the D3D11 / Vulkan rules).
// Loops over samples precompute what depends only on the sample index (Hammersley point, local direction, pdf, lod).
#include "Ibl.h"

#include "core/Log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <thread>

#include <bit>
#include <fstream>
#include <iterator>

// stb_image's Radiance .hdr decoder, private to this file (STB_IMAGE_STATIC: engine/asset and tools/golden compile
// their own PNG-only copies). Its RGBE decode is Sample-Renderer's _rgbeToFloat: byte * 2^(e - 136).
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_HDR
#define STBI_NO_STDIO
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace eng::renderer::ibl {

namespace {

constexpr float MATH_PI = 3.1415926535897932384626433832795f;

struct V3 {
    float x, y, z;
};
inline float dot(V3 a, V3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline V3 cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline V3 normalize(V3 v) {
    const float l = std::sqrt(dot(v, v));
    return {v.x / l, v.y / l, v.z / l};
}

// ibl_filtering.frag / panorama_to_cubemap.frag uvToXYZ.
inline V3 uvToXYZ(int face, float u, float v) {
    switch (face) {
    case 0: return {1.0f, v, -u};
    case 1: return {-1.0f, v, u};
    case 2: return {u, -1.0f, v};
    case 3: return {u, 1.0f, -v};
    case 4: return {u, v, 1.0f};
    default: return {-u, v, -1.0f};
    }
}

// dirToUV (acos argument clamped: a normalized y can exceed 1 by an ulp).
inline void dirToUV(V3 d, float& u, float& v) {
    u = 0.5f + 0.5f * std::atan2(d.z, d.x) / MATH_PI;
    v = 1.0f - std::acos(std::clamp(d.y, -1.0f, 1.0f)) / MATH_PI;
}

float radicalInverse_VdC(uint32_t bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

inline void generateTBN(V3 normal, V3& tangent, V3& bitangent) {
    bitangent = {0.0f, 1.0f, 0.0f};
    const float NdotUp = normal.y;
    const float epsilon = 0.0000001f;
    if (1.0f - std::abs(NdotUp) <= epsilon) {
        bitangent = NdotUp > 0.0f ? V3{0.0f, 0.0f, 1.0f} : V3{0.0f, 0.0f, -1.0f};
    }
    tangent = normalize(cross(bitangent, normal));
    bitangent = cross(normal, tangent);
}

float D_GGX(float NdotH, float roughness) {
    const float a = NdotH * roughness;
    const float k = roughness / (1.0f - NdotH * NdotH + a * a);
    return k * k * (1.0f / MATH_PI);
}

float V_SmithGGXCorrelated(float NoV, float NoL, float roughness) {
    const float a2 = std::pow(roughness, 4.0f);
    const float GGXV = NoL * std::sqrt(NoV * NoV * (1.0f - a2) + a2);
    const float GGXL = NoV * std::sqrt(NoL * NoL * (1.0f - a2) + a2);
    return 0.5f / (GGXV + GGXL);
}

// One importance sample in tangent space: the normalized local direction + pdf (GGX / Lambertian of
// ibl_filtering.frag, with getImportanceSample's normalize).
struct LocalSample {
    V3 dir;
    float pdf;
};
LocalSample localSample(int i, int sampleCount, float roughness, bool lambertian) {
    const float xiX = float(i) / float(sampleCount);
    const float xiY = radicalInverse_VdC(static_cast<uint32_t>(i));
    float cosTheta, sinTheta, pdf;
    const float phi = 2.0f * MATH_PI * xiX;
    if (lambertian) {
        cosTheta = std::sqrt(1.0f - xiY);
        sinTheta = std::sqrt(xiY);
        pdf = cosTheta / MATH_PI;
    } else {
        const float alpha = roughness * roughness;
        cosTheta = std::clamp(std::sqrt((1.0f - xiY) / (1.0f + (alpha * alpha - 1.0f) * xiY)), 0.0f, 1.0f);
        sinTheta = std::sqrt(1.0f - cosTheta * cosTheta);
        pdf = D_GGX(cosTheta, alpha) / 4.0f;
    }
    return {normalize({sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta}), pdf};
}

float computeLod(float pdf, uint32_t width, uint32_t sampleCount) {
    return 0.5f * std::log2(6.0f * float(width) * float(width) / (float(sampleCount) * pdf));
}

// Runs fn(i) for i in [0, n) on every hardware thread.
template <class Fn>
void parallelFor(uint32_t n, Fn fn) {
    const uint32_t workers = std::max(1u, std::min(n, std::thread::hardware_concurrency()));
    std::atomic<uint32_t> next{0};
    const auto run = [&] {
        for (uint32_t i = next++; i < n; i = next++) {
            fn(i);
        }
    };
    std::vector<std::thread> threads;
    for (uint32_t t = 1; t < workers; ++t) {
        threads.emplace_back(run);
    }
    run();
    for (std::thread& t : threads) {
        t.join();
    }
}

// --- cube sampling ---------------------------------------------------------------------------------------------

// Direction of face coordinates (sc, tc) (the inverse of the face-selection table below).
inline V3 faceDir(int face, float sc, float tc) {
    switch (face) {
    case 0: return {1.0f, -tc, -sc};
    case 1: return {-1.0f, -tc, sc};
    case 2: return {sc, 1.0f, tc};
    case 3: return {sc, -1.0f, -tc};
    case 4: return {sc, -tc, 1.0f};
    default: return {-sc, -tc, -1.0f};
    }
}

// Major-axis face selection (GL / D3D / Vulkan cube table): face and s, t in [0, 1].
inline int selectFace(V3 d, float& s, float& t) {
    const float ax = std::abs(d.x), ay = std::abs(d.y), az = std::abs(d.z);
    int face;
    float sc, tc, ma;
    if (ax >= ay && ax >= az) {
        ma = ax;
        face = d.x >= 0.0f ? 0 : 1;
        sc = d.x >= 0.0f ? -d.z : d.z;
        tc = -d.y;
    } else if (ay >= az) {
        ma = ay;
        face = d.y >= 0.0f ? 2 : 3;
        sc = d.x;
        tc = d.y >= 0.0f ? d.z : -d.z;
    } else {
        ma = az;
        face = d.z >= 0.0f ? 4 : 5;
        sc = d.z >= 0.0f ? d.x : -d.x;
        tc = -d.y;
    }
    s = (sc / ma + 1.0f) * 0.5f;
    t = (tc / ma + 1.0f) * 0.5f;
    return face;
}

inline const float* texel(const float* level, uint32_t size, int face, int x, int y) {
    return level + ((size_t(face) * size + size_t(y)) * size + size_t(x)) * 4u;
}

// Texel (x, y) of `face` past at most one edge: the neighbouring face's texel along that direction.
inline const float* fetchAcross(const float* level, uint32_t size, int face, int x, int y) {
    const int n = static_cast<int>(size);
    if (x >= 0 && y >= 0 && x < n && y < n) {
        return texel(level, size, face, x, y);
    }
    const float sc = 2.0f * (float(x) + 0.5f) / float(n) - 1.0f;
    const float tc = 2.0f * (float(y) + 0.5f) / float(n) - 1.0f;
    float s, t;
    const int f2 = selectFace(faceDir(face, sc, tc), s, t);
    const int x2 = std::clamp(static_cast<int>(std::floor(s * float(n))), 0, n - 1);
    const int y2 = std::clamp(static_cast<int>(std::floor(t * float(n))), 0, n - 1);
    return texel(level, size, f2, x2, y2);
}

// Seamless fetch (D3D11 / Vulkan / GL seamless cube filtering): past one edge, the neighbouring face's texel; past a
// corner (both coordinates outside), the mean of the three texels that meet there.
inline void fetchSeamless(const float* level, uint32_t size, int face, int x, int y, float out[3]) {
    const int n = static_cast<int>(size);
    const bool outX = x < 0 || x >= n, outY = y < 0 || y >= n;
    if (outX && outY) {
        const int cx = std::clamp(x, 0, n - 1), cy = std::clamp(y, 0, n - 1);
        const float* a = texel(level, size, face, cx, cy);
        const float* b = fetchAcross(level, size, face, x, cy);
        const float* c = fetchAcross(level, size, face, cx, y);
        for (int k = 0; k < 3; ++k) {
            out[k] = (a[k] + b[k] + c[k]) / 3.0f;
        }
        return;
    }
    const float* t = fetchAcross(level, size, face, x, y);
    out[0] = t[0];
    out[1] = t[1];
    out[2] = t[2];
}

void bilinearCube(const Cube& c, uint32_t mip, V3 dir, float out[3]) {
    const uint32_t size = std::max(1u, c.size >> mip);
    const float* level = c.levels[mip].data();
    float s, t;
    const int face = selectFace(dir, s, t);
    const float x = s * float(size) - 0.5f, y = t * float(size) - 0.5f;
    const float x0f = std::floor(x), y0f = std::floor(y);
    const float fx = x - x0f, fy = y - y0f;
    const int x0 = static_cast<int>(x0f), y0 = static_cast<int>(y0f);
    float a[3], b[3], cc[3], d[3];
    fetchSeamless(level, size, face, x0, y0, a);
    fetchSeamless(level, size, face, x0 + 1, y0, b);
    fetchSeamless(level, size, face, x0, y0 + 1, cc);
    fetchSeamless(level, size, face, x0 + 1, y0 + 1, d);
    for (int k = 0; k < 3; ++k) {
        const float top = a[k] + (b[k] - a[k]) * fx;
        const float bottom = cc[k] + (d[k] - cc[k]) * fx;
        out[k] = top + (bottom - top) * fy;
    }
}

void sampleCube(const Cube& c, V3 dir, float lod, float out[3]) {
    const float maxLevel = float(c.levels.size() - 1);
    if (!(lod > 0.0f)) {
        lod = 0.0f; // also NaN
    }
    lod = std::min(lod, maxLevel);
    const float l0 = std::floor(lod);
    const float f = lod - l0;
    bilinearCube(c, static_cast<uint32_t>(l0), dir, out);
    if (f > 0.0f) {
        float b[3];
        bilinearCube(c, static_cast<uint32_t>(l0) + 1u, dir, b);
        for (int k = 0; k < 3; ++k) {
            out[k] += (b[k] - out[k]) * f;
        }
    }
}

// Panorama bilinear, level 0, edge texels clamped (= MIRRORED_REPEAT for uv in [0, 1]).
void samplePanorama(const Panorama& p, float u, float v, float out[3]) {
    const float x = u * float(p.width) - 0.5f, y = v * float(p.height) - 0.5f;
    const float x0f = std::floor(x), y0f = std::floor(y);
    const float fx = x - x0f, fy = y - y0f;
    const int w = static_cast<int>(p.width), h = static_cast<int>(p.height);
    const int x0 = std::clamp(static_cast<int>(x0f), 0, w - 1), x1 = std::clamp(static_cast<int>(x0f) + 1, 0, w - 1);
    const int y0 = std::clamp(static_cast<int>(y0f), 0, h - 1), y1 = std::clamp(static_cast<int>(y0f) + 1, 0, h - 1);
    const float* d = p.rgb.data();
    const float* a = d + (size_t(y0) * w + x0) * 3;
    const float* b = d + (size_t(y0) * w + x1) * 3;
    const float* c = d + (size_t(y1) * w + x0) * 3;
    const float* e = d + (size_t(y1) * w + x1) * 3;
    for (int k = 0; k < 3; ++k) {
        const float top = a[k] + (b[k] - a[k]) * fx;
        const float bottom = c[k] + (e[k] - c[k]) * fx;
        out[k] = top + (bottom - top) * fy;
    }
}

// filterColor of ibl_filtering.frag into level `mip` of `dst` (size = textureSize >> mip).
void filterLevel(const Cube& source, const Params& p, bool lambertian, float roughness, uint32_t sampleCount,
                 uint32_t size, std::vector<float>& dst) {
    dst.assign(size_t(6) * size * size * 4, 0.0f);
    std::vector<LocalSample> samples(sampleCount);
    std::vector<float> lods(sampleCount);
    for (uint32_t i = 0; i < sampleCount; ++i) {
        samples[i] = localSample(static_cast<int>(i), static_cast<int>(sampleCount), roughness, lambertian);
        lods[i] = computeLod(samples[i].pdf, p.textureSize, sampleCount) + p.lodBias;
        if (!lambertian && roughness == 0.0f) {
            lods[i] = p.lodBias; // "without this the roughness = 0 distribution gets blurry"
        }
    }
    const LocalSample* smp = samples.data();
    const float* lod = lods.data();
    float* out = dst.data();
    parallelFor(6u * size, [&](uint32_t row) {
        const int face = static_cast<int>(row / size);
        const uint32_t y = row % size;
        for (uint32_t x = 0; x < size; ++x) {
            const float u = (float(x) + 0.5f) / float(size) * 2.0f - 1.0f;
            const float v = (float(y) + 0.5f) / float(size) * 2.0f - 1.0f;
            V3 N = normalize(uvToXYZ(face, u, v));
            N.y = -N.y;
            V3 T, B;
            generateTBN(N, T, B);
            float color[3] = {0.0f, 0.0f, 0.0f};
            float weight = 0.0f;
            // GGX at roughness 0: every sample is H = N, L = N, lod = lodBias, so the mean of the identical samples is
            // the one sample (the GPU sums sampleCount copies and divides).
            const uint32_t n = (!lambertian && roughness == 0.0f) ? 1u : sampleCount;
            for (uint32_t i = 0; i < n; ++i) {
                const V3 l = smp[i].dir;
                const V3 H{T.x * l.x + B.x * l.y + N.x * l.z, T.y * l.x + B.y * l.y + N.y * l.z,
                           T.z * l.x + B.z * l.y + N.z * l.z};
                float c[3];
                if (lambertian) {
                    sampleCube(source, H, lod[i], c);
                    color[0] += c[0];
                    color[1] += c[1];
                    color[2] += c[2];
                } else {
                    const float NdotH = dot(N, H);
                    const V3 L = normalize({2.0f * NdotH * H.x - N.x, 2.0f * NdotH * H.y - N.y, 2.0f * NdotH * H.z - N.z});
                    const float NdotL = dot(N, L);
                    if (NdotL > 0.0f) {
                        sampleCube(source, L, lod[i], c);
                        color[0] += c[0] * NdotL;
                        color[1] += c[1] * NdotL;
                        color[2] += c[2] * NdotL;
                        weight += NdotL;
                    }
                }
            }
            const float div = weight != 0.0f ? weight : float(n);
            float* o = out + ((size_t(face) * size + y) * size + x) * 4;
            o[0] = color[0] / div;
            o[1] = color[1] / div;
            o[2] = color[2] / div;
            o[3] = 1.0f;
        }
    });
}

} // namespace

uint32_t Params::mipmapLevels() const {
    const uint32_t log2 = static_cast<uint32_t>(std::floor(std::log2(double(std::max(1u, textureSize)))));
    const int levels = int(log2) + 1 - int(lowestMipLevel);
    return static_cast<uint32_t>(std::max(2, levels));
}

Result<Panorama> loadHdr(const std::filesystem::path& path) {
    const std::u8string u = path.u8string();
    const std::string name(u.begin(), u.end());
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return std::unexpected(Error{2, std::format("IBL environment '{}' not found (run scripts/fetch_assets.py "
                                                    "--only environments)",
                                                    name)});
    }
    const std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() > (1u << 30)) {
        return std::unexpected(Error{2, std::format("IBL environment '{}': larger than 1 GiB", name)});
    }
    int w = 0, h = 0, comp = 0;
    float* data = stbi_loadf_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()),
                                         &w, &h, &comp, 3);
    if (!data) {
        return std::unexpected(Error{2, std::format("IBL environment '{}': cannot decode: {}", name,
                                                    stbi_failure_reason() ? stbi_failure_reason() : "unknown")});
    }
    Panorama p;
    p.width = static_cast<uint32_t>(w);
    p.height = static_cast<uint32_t>(h);
    p.rgb.assign(data, data + size_t(w) * h * 3);
    stbi_image_free(data);
    return p;
}

Cube panoramaToCube(const Panorama& pano, uint32_t size) {
    Cube c;
    c.size = size;
    std::vector<float> mip0(size_t(6) * size * size * 4);
    float* out = mip0.data();
    parallelFor(6u * size, [&](uint32_t row) {
        const int face = static_cast<int>(row / size);
        const uint32_t y = row % size;
        for (uint32_t x = 0; x < size; ++x) {
            const float u = (float(x) + 0.5f) / float(size) * 2.0f - 1.0f;
            const float v = (float(y) + 0.5f) / float(size) * 2.0f - 1.0f;
            const V3 direction = normalize(uvToXYZ(face, u, v));
            float su, sv;
            dirToUV(direction, su, sv);
            float* o = out + ((size_t(face) * size + y) * size + x) * 4;
            samplePanorama(pano, su, sv, o);
            o[3] = 1.0f;
        }
    });
    c.levels.push_back(std::move(mip0));
    // generateMipmap: 2 x 2 box filter per level down to 1 x 1.
    for (uint32_t s = size / 2; s >= 1; s /= 2) {
        const std::vector<float>& src = c.levels.back();
        const uint32_t ps = s * 2;
        std::vector<float> dst(size_t(6) * s * s * 4);
        for (uint32_t face = 0; face < 6; ++face) {
            for (uint32_t y = 0; y < s; ++y) {
                for (uint32_t x = 0; x < s; ++x) {
                    for (int k = 0; k < 4; ++k) {
                        const auto at = [&](uint32_t xx, uint32_t yy) {
                            return src[((size_t(face) * ps + yy) * ps + xx) * 4 + k];
                        };
                        dst[((size_t(face) * s + y) * s + x) * 4 + k] =
                            (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1)) *
                            0.25f;
                    }
                }
            }
        }
        c.levels.push_back(std::move(dst));
        if (s == 1) {
            break;
        }
    }
    return c;
}

Cube cubeMapToLambertian(const Cube& source, const Params& p) {
    Cube c;
    c.size = p.textureSize;
    c.levels.resize(1);
    filterLevel(source, p, true, 0.0f, p.lambertianSampleCount, p.textureSize, c.levels[0]);
    return c;
}

Cube cubeMapToGGX(const Cube& source, const Params& p) {
    Cube c;
    c.size = p.textureSize;
    const uint32_t mipmapLevels = p.mipmapLevels();
    c.levels.resize(p.ggxLevels());
    for (uint32_t level = 0; level <= mipmapLevels; ++level) {
        const float roughness = float(level) / float(mipmapLevels - 1);
        filterLevel(source, p, false, roughness, p.ggxSampleCount, std::max(1u, p.textureSize >> level),
                    c.levels[level]);
    }
    return c;
}

std::vector<float> sampleGGXLut(const Params& p) {
    const uint32_t res = p.lutResolution, n = p.lutSampleCount;
    std::vector<float> out(size_t(res) * res * 4);
    float* o = out.data();
    parallelFor(res, [&](uint32_t row) {
        const float roughness = (float(row) + 0.5f) / float(res);
        // N = (0, 0, 1): generateTBN gives T = (1, 0, 0), B = (0, 1, 0), so H is the local direction.
        std::vector<V3> hs(n);
        for (uint32_t i = 0; i < n; ++i) {
            hs[i] = localSample(static_cast<int>(i), static_cast<int>(n), roughness, false).dir;
        }
        for (uint32_t x = 0; x < res; ++x) {
            const float NdotV = (float(x) + 0.5f) / float(res);
            const V3 V{std::sqrt(1.0f - NdotV * NdotV), 0.0f, NdotV};
            float A = 0.0f, B = 0.0f;
            for (uint32_t i = 0; i < n; ++i) {
                const V3 H = hs[i];
                const float VdotHraw = dot(V, H);
                const V3 L = normalize({2.0f * VdotHraw * H.x - V.x, 2.0f * VdotHraw * H.y - V.y,
                                        2.0f * VdotHraw * H.z - V.z});
                const float NdotL = std::clamp(L.z, 0.0f, 1.0f);
                const float NdotH = std::clamp(H.z, 0.0f, 1.0f);
                const float VdotH = std::clamp(VdotHraw, 0.0f, 1.0f);
                if (NdotL > 0.0f) {
                    const float V_pdf = V_SmithGGXCorrelated(NdotV, NdotL, roughness) * VdotH * NdotL / NdotH;
                    const float Fc = std::pow(1.0f - VdotH, 5.0f);
                    A += (1.0f - Fc) * V_pdf;
                    B += Fc * V_pdf;
                }
            }
            float* t = o + (size_t(row) * res + x) * 4;
            t[0] = 4.0f * A / float(n);
            t[1] = 4.0f * B / float(n);
            t[2] = 0.0f;
            t[3] = 1.0f;
        }
    });
    return out;
}

void sampleCubeLod(const Cube& c, const float dir[3], float lod, float out[3]) {
    sampleCube(c, {dir[0], dir[1], dir[2]}, lod, out);
}

Result<GpuIbl> buildCpu(rhi::IDevice& dev, const std::filesystem::path& hdr, const Params& p, bool copySrc) {
    struct Data {
        Cube specular, irradiance;
        std::vector<float> lut;
    };
    static std::mutex mutex;
    static std::map<std::pair<std::filesystem::path, std::vector<uint32_t>>, std::shared_ptr<const Data>> cache;
    const std::vector<uint32_t> key{p.textureSize, p.ggxSampleCount, p.lambertianSampleCount,
                                    std::bit_cast<uint32_t>(p.lodBias), p.lowestMipLevel, p.lutResolution,
                                    p.lutSampleCount};
    std::shared_ptr<const Data> data;
    {
        std::lock_guard lock(mutex);
        if (auto it = cache.find({hdr, key}); it != cache.end()) {
            data = it->second;
        }
    }
    if (!data) {
        auto pano = loadHdr(hdr);
        if (!pano) {
            return std::unexpected(pano.error());
        }
        auto d = std::make_shared<Data>();
        const Cube source = panoramaToCube(*pano, p.textureSize);
        d->irradiance = cubeMapToLambertian(source, p);
        d->specular = cubeMapToGGX(source, p);
        d->lut = sampleGGXLut(p);
        std::lock_guard lock(mutex);
        cache[{hdr, key}] = d;
        data = d;
    }
    return upload(dev, data->specular, data->irradiance, data->lut, p.lutResolution, p.mipmapLevels(), copySrc);
}

} // namespace eng::renderer::ibl
