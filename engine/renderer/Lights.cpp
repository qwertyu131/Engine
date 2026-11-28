// Punctual lights (plan F3 Task 7). See Lights.h.
#include "Lights.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace eng::renderer {

std::vector<SceneLight> sceneLights(const asset::ModelData& model, const scene::SceneInstance& scene) {
    std::vector<SceneLight> out;
    for (const scene::LightNode& ln : scene.lights) {
        const asset::Light& l = model.lights[ln.light];
        SceneLight s;
        s.name = l.name;
        s.type = l.type;
        s.color = l.color;
        s.intensity = l.intensity;
        s.range = l.range > 0.0f ? l.range : -1.0f;
        s.innerConeAngle = l.innerConeAngle;
        s.outerConeAngle = l.outerConeAngle;
        // light.js toUniform: translation; rotation of the scale-free matrix applied to (0, 0, -1). For a rotation x
        // positive scale, that is -column 2 normalized (getScaling / getRotation / transformQuat). A mirroring
        // (negative determinant) matrix is not a rotation; gl-matrix's getRotation of it is not reproduced.
        const scene::Mat4& w = ln.world;
        s.position = {w.at(0, 3), w.at(1, 3), w.at(2, 3)};
        const scene::Vec3 dir = scene::normalize({-w.at(0, 2), -w.at(1, 2), -w.at(2, 2)});
        s.direction = (dir[0] == 0.0f && dir[1] == 0.0f && dir[2] == 0.0f) ? scene::Vec3{0.0f, 0.0f, -1.0f} : dir;
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<uint32_t> selectLights(const std::vector<SceneLight>& lights, uint32_t maxLights, const scene::Vec3& centre,
                                   float radius) {
    std::vector<uint32_t> enabled;
    for (uint32_t i = 0; i < lights.size(); ++i) {
        if (lights[i].enabled) {
            enabled.push_back(i);
        }
    }
    if (enabled.size() <= maxLights) {
        return enabled;
    }
    // Strongest at the object's bounds: directional first (infinite), then intensity x attenuation at the nearest
    // point of the bounding sphere (punctual.glsl range / spot terms; the colour's largest channel).
    std::vector<float> score(lights.size(), 0.0f);
    for (uint32_t i : enabled) {
        const SceneLight& l = lights[i];
        const float c = std::max({l.color[0], l.color[1], l.color[2]}) * l.intensity;
        if (l.type == asset::Light::Type::Directional) {
            score[i] = std::numeric_limits<float>::infinity();
            continue;
        }
        const scene::Vec3 toObj = scene::sub(centre, l.position);
        const float dist = std::max(std::sqrt(scene::dot(toObj, toObj)) - radius, 1e-3f);
        float att = 1.0f / (dist * dist);
        if (l.range > 0.0f) {
            att *= std::clamp(1.0f - std::pow(dist / l.range, 4.0f), 0.0f, 1.0f);
        }
        if (l.type == asset::Light::Type::Spot && std::sqrt(scene::dot(toObj, toObj)) > radius) {
            const scene::Vec3 d = scene::normalize(toObj);
            const float cosA = scene::dot(d, l.direction);
            const float sinR = std::min(1.0f, radius / std::sqrt(scene::dot(toObj, toObj)));
            // the sphere's angular radius widens the cone: lit if any of it is inside the outer cone
            const float cosOuter = std::cos(std::min(3.14159265f, l.outerConeAngle + std::asin(sinR)));
            att *= cosA > cosOuter ? 1.0f : 0.0f;
        }
        score[i] = c * att;
    }
    std::stable_sort(enabled.begin(), enabled.end(), [&](uint32_t a, uint32_t b) { return score[a] > score[b]; });
    enabled.resize(maxLights);
    std::sort(enabled.begin(), enabled.end()); // getVisibleLights order inside the kept set
    return enabled;
}

void packLights(const std::vector<SceneLight>& lights, const std::vector<LightShadow>& shadows,
                const std::vector<uint32_t>& selection, const LightsHeader& header, uint32_t maxLightsOfTier,
                std::byte* out) {
    std::memset(out, 0, lightsCBSize(maxLightsOfTier));
    LightsHeader h = header;
    const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(selection.size()), maxLightsOfTier);
    h.lightInfo[0] = n;
    std::memcpy(out, &h, sizeof(h));
    auto* f = reinterpret_cast<float*>(out + sizeof(LightsHeader));
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t i = selection[k];
        const SceneLight& l = lights[i];
        float* p = f + k * 16;
        p[0] = l.direction[0];
        p[1] = l.direction[1];
        p[2] = l.direction[2];
        p[3] = l.range;
        p[4] = l.color[0];
        p[5] = l.color[1];
        p[6] = l.color[2];
        p[7] = l.intensity;
        p[8] = l.position[0];
        p[9] = l.position[1];
        p[10] = l.position[2];
        p[11] = std::cos(l.innerConeAngle);
        p[12] = std::cos(l.outerConeAngle);
        p[13] = l.type == asset::Light::Type::Directional ? 0.0f : l.type == asset::Light::Type::Point ? 1.0f : 2.0f;
        const LightShadow sh = i < shadows.size() ? shadows[i] : LightShadow{};
        p[14] = float(sh.firstTile);
        p[15] = float(sh.tileCount);
    }
}

} // namespace eng::renderer
