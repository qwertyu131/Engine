#pragma once
// Shadow planning (plan F3 Task 7, normal mode): which shadow-atlas tiles the punctual lights get this frame and their
// light view-projections. Passes.cpp renders the casters of every tile into one D32 atlas (a depth-only pass) and the
// PBR shader reads it (shaders/pbr/lights.hlsli).
//
//   directional  cascaded shadow maps: r.Shadow.CascadeCount slices of the camera frustum between the near plane and
//                min(r.Shadow.Distance, camera far, the scene's farthest view depth), split with the practical scheme
//                (lambda 0.75 between logarithmic and uniform); each slice's bounding sphere gets an orthographic tile
//                (texel-snapped centre, depth range covering the whole scene so off-slice casters still cast);
//   spot         one perspective tile: the light's cone (2 x outer angle + 2 texels), near = far / 1000, far = range
//                or the farthest scene corner;
//   point        six 90-degree perspective tiles (+X, -X, +Y, -Y, +Z, -Z, widened by 2 texels so PCF stays inside the
//                face), gated by r.Shadow.PointLights.
// Tiles are square, r.Shadow.MapResolution texels, packed in a grid of ceil(sqrt(n)) columns; the atlas is at most
// min(8192, maxTexture2D) wide, smaller tiles when needed. At most kMaxShadowTiles (64) tiles: lights beyond that cast
// no shadow (warned once per renderer).
#include "Lights.h"
#include "RenderSettings.h"
#include "scene/Math.h"

#include <array>
#include <cstdint>
#include <vector>

namespace eng::renderer {

struct ShadowCamera {
    scene::Vec3 position{0, 0, 0};
    scene::Vec3 forward{0, 0, -1};
    scene::Vec3 up{0, 1, 0};
    float yfov = 0.785398163f;
    float aspect = 1.0f;
    float znear = 0.01f;
    float zfar = 100.0f;
};

struct ShadowTile {
    scene::Mat4 viewProj;               // world -> tile clip
    std::array<float, 4> rect{};        // atlas texel x, y, edge, cascade far view depth (directional)
    std::array<float, 4> params{};      // texel world size (perspective: per unit distance), perspective, near, far
    std::array<std::array<float, 4>, 6> planes{}; // frustum planes (a, b, c, d), inside: a x + b y + c z + d >= 0
};

struct ShadowPlan {
    std::vector<ShadowTile> tiles;
    std::vector<LightShadow> perLight; // per scene light (lights index)
    uint32_t tileSize = 0;
    uint32_t atlasSize = 0;            // square atlas edge; 0 = no tile
    bool truncated = false;            // some light got no tile (kMaxShadowTiles)
};

// Scene bounds: world AABB of every drawn primitive (valid = false: nothing to shadow).
struct ShadowSceneBounds {
    scene::Vec3 min{0, 0, 0}, max{0, 0, 0};
    bool valid = false;
};

ShadowPlan planShadows(const std::vector<SceneLight>& lights, const LightingSettings& settings,
                       const ShadowCamera& camera, const ShadowSceneBounds& bounds, uint32_t maxAtlasSize);

// True when the sphere (centre, radius) is not fully outside the tile's frustum.
bool sphereInTile(const ShadowTile& tile, const scene::Vec3& centre, float radius);

} // namespace eng::renderer
