#pragma once
// Draw lists (plan F3 Task 4): one item per node-primitive of the scene, sorted as glTF-Sample-Renderer v1.1.0
// renderer.js drawScene does: opaque + mask front-to-back, then (after the skybox) blend back-to-front by the distance
// from the camera to the node's world-space primitive centre (Sample-Renderer sorts transparent primitives by the
// view-space depth of the node's centre; the bounds centre of each primitive is used here, one level finer).
#include "GpuModel.h"
#include "scene/Scene.h"

#include <vector>

namespace eng::renderer {

struct DrawItem {
    uint32_t draw = 0;      // index into SceneInstance::draws
    uint32_t primitive = 0; // index into the mesh's primitives
    const GpuPrimitive* prim = nullptr;
    const GpuMaterial* material = nullptr;
    bool skinned = false;   // the node has a skin and the primitive JOINTS_0 / WEIGHTS_0
    bool morph = false;     // the primitive has morph targets
    bool flipWinding = false; // negative-determinant world transform
    float distance = 0.0f;  // camera -> world bounds centre
};

struct DrawLists {
    std::vector<DrawItem> opaque; // OPAQUE + MASK, front to back
    std::vector<DrawItem> blend;  // BLEND, back to front
};

DrawLists buildDrawLists(const scene::SceneInstance& scene, const asset::ModelData& model, const GpuModel& gpu,
                         const scene::Vec3& cameraPosition);

} // namespace eng::renderer
