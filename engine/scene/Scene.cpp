// Scene instance (plan F3 Task 4). See Scene.h; the formulas follow glTF-Sample-Renderer v1.1.0 (d59d1d6):
// scene.js applyTransformHierarchy, skin.js computeJoints, node.js getWeights, gltf_utils.js getSceneExtents.
#include "Scene.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace eng::scene {

namespace {

Mat4 localOf(const asset::Node& n) {
    if (n.hasMatrix) {
        Mat4 m;
        for (size_t i = 0; i < 16; ++i) {
            m.m[i] = n.matrix[i];
        }
        return m;
    }
    return trs({n.translation[0], n.translation[1], n.translation[2]},
               {n.rotation[0], n.rotation[1], n.rotation[2], n.rotation[3]}, {n.scale[0], n.scale[1], n.scale[2]});
}

} // namespace

Result<SceneInstance> instantiate(const asset::ModelData& model, int sceneIndex) {
    SceneInstance s;
    const int idx = sceneIndex >= 0 ? sceneIndex : (model.defaultScene >= 0 ? model.defaultScene : 0);
    if (!model.scenes.empty() && (idx < 0 || static_cast<size_t>(idx) >= model.scenes.size())) {
        return std::unexpected(Error{2, std::format("scene {} does not exist ({} scenes)", idx, model.scenes.size())});
    }
    if (model.scenes.empty() && sceneIndex > 0) {
        return std::unexpected(Error{2, std::format("scene {} does not exist (the model has no scenes)", sceneIndex)});
    }
    s.sceneIndex = idx;
    s.world.assign(model.nodes.size(), Mat4{});
    // World transforms of every hierarchy (scene.js applyTransformHierarchy), iteratively (deep chains).
    for (size_t r = 0; r < model.nodes.size(); ++r) {
        if (model.nodes[r].parent >= 0) {
            continue;
        }
        std::vector<std::pair<int, Mat4>> stack{{static_cast<int>(r), Mat4{}}};
        while (!stack.empty()) {
            auto [n, parent] = stack.back();
            stack.pop_back();
            s.world[n] = parent * localOf(model.nodes[n]);
            for (const int c : model.nodes[n].children) {
                stack.emplace_back(c, s.world[n]);
            }
        }
    }
    if (model.scenes.empty()) {
        return s;
    }
    Vec3 lo{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::infinity()};
    Vec3 hi{-lo[0], -lo[1], -lo[2]};
    // Depth-first over the scene's roots, in order.
    std::vector<int> stack(model.scenes[idx].nodes.rbegin(), model.scenes[idx].nodes.rend());
    while (!stack.empty()) {
        const int n = stack.back();
        stack.pop_back();
        const asset::Node& node = model.nodes[n];
        for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
            stack.push_back(*it);
        }
        if (node.mesh < 0) {
            continue;
        }
        const asset::Mesh& mesh = model.meshes[node.mesh];
        DrawNode d;
        d.node = n;
        d.mesh = node.mesh;
        d.world = s.world[n];
        d.normalMatrix = transpose(inverse(d.world));
        d.negativeScale = determinant3(d.world) < 0.0f;
        if (node.skin >= 0) {
            const asset::Skin& skin = model.skins[node.skin];
            const Mat4 invMesh = inverse(d.world);
            for (size_t j = 0; j < skin.joints.size(); ++j) {
                Mat4 ibm;
                for (size_t k = 0; k < 16; ++k) {
                    ibm.m[k] = skin.inverseBindMatrices[j][k];
                }
                const Mat4 jm = invMesh * s.world[skin.joints[j]] * ibm;
                d.joints.push_back(jm);
                d.joints.push_back(transpose(inverse(jm)));
            }
        }
        size_t targets = 0;
        for (const asset::Primitive& p : mesh.primitives) {
            targets = std::max(targets, p.targets.size());
        }
        const std::vector<float>& w = !node.weights.empty() ? node.weights : mesh.weights;
        d.morphWeights.assign(targets, 0.0f);
        for (size_t t = 0; t < targets && t < w.size(); ++t) {
            d.morphWeights[t] = w[t];
        }
        // Extents (gltf_utils.js getExtentsFromAccessor): accessor box -> world -> re-box -> centre +- radius.
        for (const asset::Primitive& p : mesh.primitives) {
            if (!p.bounds.valid) {
                continue;
            }
            Vec3 bmin{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::infinity()};
            Vec3 bmax{-bmin[0], -bmin[1], -bmin[2]};
            for (int corner = 0; corner < 8; ++corner) {
                const Vec3 c{(corner & 4) ? p.bounds.max[0] : p.bounds.min[0], (corner & 2) ? p.bounds.max[1] : p.bounds.min[1],
                             (corner & 1) ? p.bounds.max[2] : p.bounds.min[2]};
                const Vec3 wc = transformPoint(d.world, c);
                for (int k = 0; k < 3; ++k) {
                    bmin[k] = std::min(bmin[k], wc[k]);
                    bmax[k] = std::max(bmax[k], wc[k]);
                }
            }
            const Vec3 centre{(bmin[0] + bmax[0]) * 0.5f, (bmin[1] + bmax[1]) * 0.5f, (bmin[2] + bmax[2]) * 0.5f};
            const Vec3 half = sub(bmax, centre);
            const float radius = std::sqrt(dot(half, half));
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], centre[k] - radius);
                hi[k] = std::max(hi[k], centre[k] + radius);
            }
        }
        s.draws.push_back(std::move(d));
    }
    if (std::isfinite(lo[0]) && std::isfinite(lo[1]) && std::isfinite(lo[2]) && std::isfinite(hi[0]) &&
        std::isfinite(hi[1]) && std::isfinite(hi[2])) {
        s.extentMin = lo;
        s.extentMax = hi;
        s.extentsValid = true;
    }
    return s;
}

} // namespace eng::scene
