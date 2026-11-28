// Shadow planning (plan F3 Task 7). See Shadows.h.
#include "Shadows.h"

#include <algorithm>
#include <cmath>

namespace eng::renderer {

namespace {

using scene::Mat4;
using scene::Vec3;

Vec3 add(const Vec3& a, const Vec3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
Vec3 mul(const Vec3& a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}

std::array<Vec3, 8> corners(const ShadowSceneBounds& b) {
    std::array<Vec3, 8> c{};
    for (int i = 0; i < 8; ++i) {
        c[i] = {(i & 1) ? b.max[0] : b.min[0], (i & 2) ? b.max[1] : b.min[1], (i & 4) ? b.max[2] : b.min[2]};
    }
    return c;
}

// A look-at whose up is not parallel to the view direction.
Mat4 lookTowards(const Vec3& eye, const Vec3& dir) {
    const Vec3 up = std::abs(dir[1]) > 0.99f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
    return scene::lookAt(eye, add(eye, dir), up);
}

// Gribb / Hartmann planes of a clip-space [0, 1]-depth view-projection: left, right, bottom, top, near, far.
std::array<std::array<float, 4>, 6> frustumPlanes(const Mat4& m) {
    std::array<std::array<float, 4>, 6> p{};
    for (int k = 0; k < 4; ++k) {
        const float r0 = m.at(0, k), r1 = m.at(1, k), r2 = m.at(2, k), r3 = m.at(3, k);
        p[0][k] = r3 + r0;
        p[1][k] = r3 - r0;
        p[2][k] = r3 + r1;
        p[3][k] = r3 - r1;
        p[4][k] = r2;
        p[5][k] = r3 - r2;
    }
    for (auto& pl : p) {
        const float l = std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
        if (l > 0.0f) {
            for (float& v : pl) {
                v /= l;
            }
        }
    }
    return p;
}

// Distance from `from` to the farthest scene corner (a light's far plane when it has no range).
float farthest(const ShadowSceneBounds& b, const Vec3& from) {
    float d = 0.0f;
    for (const Vec3& c : corners(b)) {
        const Vec3 v = scene::sub(c, from);
        d = std::max(d, std::sqrt(scene::dot(v, v)));
    }
    return d;
}

ShadowTile perspectiveTile(const Vec3& eye, const Vec3& dir, float halfTan, float zNear, float zFar,
                           uint32_t tileSize) {
    ShadowTile t;
    const float fov = 2.0f * std::atan(halfTan);
    t.viewProj = scene::perspectiveRhZo(fov, 1.0f, zNear, zFar) * lookTowards(eye, dir);
    t.params = {2.0f * halfTan / float(tileSize), 1.0f, zNear, zFar};
    t.planes = frustumPlanes(t.viewProj);
    return t;
}

} // namespace

bool sphereInTile(const ShadowTile& tile, const Vec3& c, float radius) {
    for (const auto& p : tile.planes) {
        if (p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + p[3] < -radius) {
            return false;
        }
    }
    return true;
}

ShadowPlan planShadows(const std::vector<SceneLight>& lights, const LightingSettings& s, const ShadowCamera& cam,
                       const ShadowSceneBounds& bounds, uint32_t maxAtlasSize) {
    ShadowPlan plan;
    plan.perLight.assign(lights.size(), LightShadow{});
    if (!s.shadows || !s.punctual || !bounds.valid || s.maxLights == 0) {
        return plan;
    }
    // Directional range: the camera's near plane to min(r.Shadow.Distance, far, the farthest scene view depth).
    float sceneMinDepth = 1e30f, sceneMaxDepth = -1e30f;
    for (const Vec3& c : corners(bounds)) {
        const float d = scene::dot(scene::sub(c, cam.position), cam.forward);
        sceneMinDepth = std::min(sceneMinDepth, d);
        sceneMaxDepth = std::max(sceneMaxDepth, d);
    }
    const float csmFar = std::min({s.shadowDistance, cam.zfar, sceneMaxDepth});
    const float csmNear = std::max({cam.znear, sceneMinDepth, csmFar * 1e-3f});

    // Tile count per light, in light order, within kMaxShadowTiles.
    std::vector<uint32_t> want(lights.size(), 0);
    uint32_t total = 0;
    for (size_t i = 0; i < lights.size(); ++i) {
        const SceneLight& l = lights[i];
        uint32_t n = 0;
        if (l.enabled) {
            if (l.type == asset::Light::Type::Directional) {
                n = csmFar > csmNear ? s.cascadeCount : 0;
            } else if (l.type == asset::Light::Type::Spot) {
                n = 1;
            } else if (s.pointShadows) {
                n = 6;
            }
        }
        if (total + n > kMaxShadowTiles) {
            plan.truncated = plan.truncated || n > 0;
            n = 0;
        }
        want[i] = n;
        total += n;
    }
    if (total == 0) {
        return plan;
    }
    const uint32_t cols = static_cast<uint32_t>(std::ceil(std::sqrt(double(total))));
    const uint32_t cap = std::max(16u, maxAtlasSize);
    plan.tileSize = std::max(16u, std::min(s.shadowResolution, cap / cols));
    plan.atlasSize = cols * plan.tileSize;
    const auto place = [&](ShadowTile t, float cascadeFar) {
        const uint32_t i = static_cast<uint32_t>(plan.tiles.size());
        t.rect = {float((i % cols) * plan.tileSize), float((i / cols) * plan.tileSize), float(plan.tileSize),
                  cascadeFar};
        plan.tiles.push_back(t);
    };
    const float tileF = float(plan.tileSize);
    for (size_t i = 0; i < lights.size(); ++i) {
        if (want[i] == 0) {
            continue;
        }
        const SceneLight& l = lights[i];
        plan.perLight[i] = {static_cast<int>(plan.tiles.size()), static_cast<int>(want[i])};
        if (l.type == asset::Light::Type::Directional) {
            // Practical split scheme between csmNear and csmFar.
            const uint32_t n = want[i];
            const Vec3 right = scene::normalize(scene::cross(cam.forward, cam.up));
            const Vec3 up = scene::cross(right, cam.forward);
            const float th = std::tan(cam.yfov * 0.5f), tw = th * cam.aspect;
            float a = csmNear;
            for (uint32_t c = 0; c < n; ++c) {
                const float f = float(c + 1) / float(n);
                const float logSplit = csmNear * std::pow(csmFar / csmNear, f);
                const float uniSplit = csmNear + (csmFar - csmNear) * f;
                const float b = c + 1 == n ? csmFar : 0.75f * logSplit + 0.25f * uniSplit;
                // Bounding sphere of the slice [a, b]: centre = mean of the 8 corners, radius = farthest corner.
                std::array<Vec3, 8> pts{};
                int k = 0;
                for (const float d : {a, b}) {
                    const Vec3 centre = add(cam.position, mul(cam.forward, d));
                    for (const float sx : {-1.0f, 1.0f}) {
                        for (const float sy : {-1.0f, 1.0f}) {
                            pts[k++] = add(centre, add(mul(right, sx * tw * d), mul(up, sy * th * d)));
                        }
                    }
                }
                Vec3 centre{0, 0, 0};
                for (const Vec3& p : pts) {
                    centre = add(centre, mul(p, 1.0f / 8.0f));
                }
                float radius = 0.0f;
                for (const Vec3& p : pts) {
                    const Vec3 v = scene::sub(p, centre);
                    radius = std::max(radius, std::sqrt(scene::dot(v, v)));
                }
                radius = std::max(radius, 1e-4f);
                // Light view through the slice centre; the centre snapped to whole texels (stable when the camera
                // moves); depth range = the whole scene (and the slice) along the light direction.
                const Mat4 view0 = lookTowards({0, 0, 0}, l.direction);
                const float texel = 2.0f * radius / tileF;
                Vec3 lc = scene::transformPoint(view0, centre);
                lc[0] = std::floor(lc[0] / texel) * texel;
                lc[1] = std::floor(lc[1] / texel) * texel;
                float dmin = -lc[2] - radius, dmax = -lc[2] + radius; // view depth = -z
                for (const Vec3& corner : corners(bounds)) {
                    const float d = -scene::transformPoint(view0, corner)[2];
                    dmin = std::min(dmin, d);
                    dmax = std::max(dmax, d);
                }
                const float margin = 0.01f * (dmax - dmin) + texel;
                ShadowTile t;
                t.viewProj = scene::orthoRhZo(lc[0] - radius, lc[0] + radius, lc[1] - radius, lc[1] + radius,
                                              dmin - margin, dmax + margin) *
                             view0;
                t.params = {texel, 0.0f, dmin - margin, dmax + margin};
                t.planes = frustumPlanes(t.viewProj);
                place(t, b);
                a = b;
            }
        } else {
            const float zFar = l.range > 0.0f ? l.range : farthest(bounds, l.position) * 1.01f + 1e-3f;
            const float zNear = std::max(zFar * 1e-3f, 1e-4f);
            if (l.type == asset::Light::Type::Spot) {
                const float half = std::min(l.outerConeAngle, 1.48f); // < 85 deg
                const float halfTan = std::tan(half) * (1.0f + 2.0f / tileF) + 2.0f / tileF;
                place(perspectiveTile(l.position, l.direction, halfTan, zNear, zFar, plan.tileSize), 0.0f);
            } else {
                const Vec3 axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                const float halfTan = 1.0f + 2.0f / tileF; // 90 deg + 2 texels
                for (const Vec3& axis : axes) {
                    place(perspectiveTile(l.position, axis, halfTan, zNear, zFar, plan.tileSize), 0.0f);
                }
            }
        }
    }
    return plan;
}

} // namespace eng::renderer
