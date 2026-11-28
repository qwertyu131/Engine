// Draw lists (plan F3 Task 4). See DrawList.h.
#include "DrawList.h"

#include <algorithm>
#include <cmath>

namespace eng::renderer {

DrawLists buildDrawLists(const scene::SceneInstance& scene, const asset::ModelData& model, const GpuModel& gpu,
                         const scene::Vec3& cameraPosition) {
    DrawLists out;
    for (uint32_t d = 0; d < scene.draws.size(); ++d) {
        const scene::DrawNode& dn = scene.draws[d];
        const auto& prims = gpu.meshes()[dn.mesh];
        const bool hasSkin = model.nodes[dn.node].skin >= 0 && !dn.joints.empty();
        for (uint32_t p = 0; p < prims.size(); ++p) {
            const GpuPrimitive& g = prims[p];
            if (g.indexCount == 0) {
                continue;
            }
            DrawItem it;
            it.draw = d;
            it.primitive = p;
            it.prim = &g;
            it.material = &gpu.material(g.material);
            it.skinned = hasSkin && g.skinned;
            it.morph = g.targetCount > 0;
            it.flipWinding = dn.negativeScale;
            scene::Vec3 centre{0, 0, 0};
            if (g.bounds.valid) {
                centre = {(g.bounds.min[0] + g.bounds.max[0]) * 0.5f, (g.bounds.min[1] + g.bounds.max[1]) * 0.5f,
                          (g.bounds.min[2] + g.bounds.max[2]) * 0.5f};
            }
            const scene::Vec3 wc = scene::transformPoint(dn.world, centre);
            const scene::Vec3 dv = scene::sub(wc, cameraPosition);
            it.distance = std::sqrt(scene::dot(dv, dv));
            (it.material->alphaMode == asset::AlphaMode::Blend ? out.blend : out.opaque).push_back(it);
        }
    }
    std::stable_sort(out.opaque.begin(), out.opaque.end(),
                     [](const DrawItem& a, const DrawItem& b) { return a.distance < b.distance; });
    std::stable_sort(out.blend.begin(), out.blend.end(),
                     [](const DrawItem& a, const DrawItem& b) { return a.distance > b.distance; });
    return out;
}

} // namespace eng::renderer
