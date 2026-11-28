#pragma once
// Punctual lights of a glTF scene (plan F3 Task 7): KHR_lights_punctual lights placed by the scene's nodes, plus lights
// the host adds (Renderer::addLight), and their space-1 constants (shaders/common/bindings.hlsli cbuffer Lights).
//
// World values follow glTF-Sample-Renderer v1.1.0 light.js toUniform: the scale is divided out of the node's world
// matrix, direction = its rotation applied to (0, 0, -1), position = its translation, range = the glTF range (absent:
// -1, i.e. no cut-off; punctual.glsl treats range <= 0 as unlimited), cone cosines of the spot angles. Order = renderer.js
// getVisibleLights (scene::SceneInstance::lights). No unit conversion (Sample-Renderer uses the glTF intensity as is).
//
// Per object (forward light loop): up to `maxLights` lights (r.Lighting.MaxLights, capped by the tier: T0 8, T1 / T2
// 64). When a scene has more, each object keeps the strongest at its bounds centre (directional first, then intensity x
// range / distance attenuation, spot cone included), in getVisibleLights order; otherwise every light, in that order, as
// Sample-Renderer (LIGHT_COUNT = every visible light).
#include "asset/ModelData.h"
#include "scene/Scene.h"

#include <cstdint>
#include <string>
#include <vector>

namespace eng::renderer {

struct SceneLight {
    std::string name;
    asset::Light::Type type = asset::Light::Type::Point;
    scene::Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = -1.0f; // light.js: -1 when the glTF light has none (<= 0 = unlimited)
    float innerConeAngle = 0.0f;
    float outerConeAngle = 0.785398163f;
    scene::Vec3 position{0.0f, 0.0f, 0.0f};
    scene::Vec3 direction{0.0f, 0.0f, -1.0f};
    bool enabled = true;
    bool fromAsset = true;
};

// The scene's lights in getVisibleLights order.
std::vector<SceneLight> sceneLights(const asset::ModelData& model, const scene::SceneInstance& scene);

// Tier limit of the light loop (bindings.hlsli MAX_LIGHTS).
inline constexpr uint32_t kMaxLightsT0 = 8;
inline constexpr uint32_t kMaxLightsT12 = 64;
inline constexpr uint32_t kMaxShadowTiles = 64; // bindings.hlsli MAX_SHADOW_TILES

// Indices into `lights` of the enabled lights an object with bounding sphere (centre, radius) is lit by.
std::vector<uint32_t> selectLights(const std::vector<SceneLight>& lights, uint32_t maxLights, const scene::Vec3& centre,
                                   float radius);

// cbuffer Lights (bindings.hlsli): header + `lights` float4 x 4 per light; the array length is the tier's MAX_LIGHTS.
struct LightsHeader {
    uint32_t lightInfo[4];
    float shadowInfo[4];
    float viewForward[4];
};
static_assert(sizeof(LightsHeader) == 48);
inline constexpr size_t lightsCBSize(uint32_t maxLightsOfTier) {
    return sizeof(LightsHeader) + size_t(maxLightsOfTier) * 64;
}

// cbuffer Shadows (bindings.hlsli).
struct ShadowsCB {
    float matrices[kMaxShadowTiles][16];
    float rects[kMaxShadowTiles][4];
    float params[kMaxShadowTiles][4];
};
static_assert(sizeof(ShadowsCB) == kMaxShadowTiles * 96);

// Shadow tiles of one light (Shadows.cpp): first tile and count, -1 = none.
struct LightShadow {
    int firstTile = -1;
    int tileCount = 0;
};

// Writes the Lights cbuffer for `selection` (indices into `lights`) into `out` (lightsCBSize(maxLightsOfTier) bytes).
void packLights(const std::vector<SceneLight>& lights, const std::vector<LightShadow>& shadows,
                const std::vector<uint32_t>& selection, const LightsHeader& header, uint32_t maxLightsOfTier,
                std::byte* out);

} // namespace eng::renderer
