#pragma once
// Scene instance (plan F3 Task 4): the node hierarchy of one glTF scene in its rest pose, world transforms, the draw
// list input (mesh nodes), skins and morph weights at t = 0. Animation playback is F4.
//
// Conventions follow glTF-Sample-Renderer v1.1.0 (d59d1d6), as the reference mode needs them:
//   world        GltfScene.applyTransformHierarchy (scene.js): world = parent world * local (TRS or matrix);
//   joints       Skin.computeJoints (skin.js): joint matrix = inverse(world(mesh node)) * world(joint) *
//                inverseBindMatrix, joint normal matrix = transpose(inverse(joint matrix)); the shader then applies
//                the mesh node's world matrix (primitive.vert: u_ModelMatrix * skinMatrix). The loader gives identity
//                inverse bind matrices when a skin has none (glTF 2.0 §3.7.3.2); Sample-Renderer 1.1.0 instead
//                skips the parent-inverse product in that case (a quirk not reproduced; logged as a difference);
//   morph        Node.getWeights (node.js): node.weights, else mesh.weights, else zeros;
//   extents      getSceneExtents / getExtentsFromAccessor (gltf_utils.js): per mesh primitive, the POSITION
//                accessor min / max box transformed by the node's world matrix, re-boxed, then replaced by the cube
//                centre +- |half diagonal| (a bounding sphere's box); skinning and morphing are ignored.
// Every node is reached once (the loader rejects cycles and multi-parent nodes).
#include "Math.h"
#include "asset/ModelData.h"
#include "core/Result.h"

#include <vector>

namespace eng::scene {

struct DrawNode {
    int node = -1;
    int mesh = -1;
    Mat4 world;
    Mat4 normalMatrix;        // transpose(inverse(world))
    bool negativeScale = false; // det(world) < 0: the triangle winding flips (glTF 2.0 §3.7.4)
    // [2 * j] joint matrix, [2 * j + 1] joint normal matrix (the shader's jointMatrices layout); empty = no skin.
    std::vector<Mat4> joints;
    std::vector<float> morphWeights; // one per morph target of the mesh (zeros when the file has none)
};

struct SceneInstance {
    int sceneIndex = 0;
    std::vector<Mat4> world;     // every node of the model (nodes outside the scene: their own hierarchy)
    std::vector<DrawNode> draws; // mesh nodes of the scene, in depth-first scene order
    Vec3 extentMin{0, 0, 0}, extentMax{0, 0, 0};
    bool extentsValid = false; // false: no mesh with POSITION bounds (empty scene)
};

// sceneIndex -1 = gltf.scene ?? 0. Error{2} for a scene index that does not exist (a model without scenes has
// none: scene 0 of an empty scene list is an empty instance).
Result<SceneInstance> instantiate(const asset::ModelData& model, int sceneIndex);

// Sample-Renderer UserCamera.resetView (user_camera.js, v1.1.0): looks down -Z at the extents' centre from the
// distance that fits max(width, height) of the extents into yfov (and xfov = yfov * aspect), near / far from
// fitCameraPlanesToExtents (MaxNearFarRatio 10000). A degenerate scene (no extents, a single point) still gives a
// finite camera (Review Focus 5, Task 5 tests it).
struct FitCamera {
    Vec3 position{0, 0, 1};
    Vec3 target{0, 0, 0};
    Vec3 up{0, 1, 0};
    float yfov = 0.785398163f; // 45 deg
    float znear = 0.01f;
    float zfar = 100.0f;
};
FitCamera fitCamera(const SceneInstance& scene, float aspect, float yfov = 0.785398163f);

} // namespace eng::scene
