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

using Path = asset::AnimationChannel::Path;
using Interp = asset::AnimationSampler::Interpolation;

// The animated property of a channel (gltf.js computeDisjointAnimations getAnimationProperty): weights of a node
// without node.weights animate its mesh's weights. -1 node = no property (pointer channels: F4).
struct Property {
    int node = -1;
    Path path = Path::Translation;
    int mesh = -1; // weights: the mesh whose weights are animated, -1 = the node's own weights
    bool operator==(const Property&) const = default;
};
Property propertyOf(const asset::ModelData& model, const asset::AnimationChannel& c) {
    if (c.node < 0 || static_cast<size_t>(c.node) >= model.nodes.size() || c.path == Path::Pointer) {
        return {};
    }
    Property p{c.node, c.path, -1};
    if (c.path == Path::Weights && model.nodes[c.node].weights.empty()) {
        p.mesh = model.nodes[c.node].mesh;
    }
    return p;
}

// gltfInterpolator.interpolate (interpolator.js) at time t, `stride` components; normalized quaternions for rotation
// as Sample-Renderer (slerpQuat / normalize).
std::vector<float> interpolate(const asset::AnimationSampler& smp, Path path, uint32_t stride, float t, float maxTime) {
    const std::vector<float>& in = smp.input;
    const std::vector<float>& out = smp.output;
    std::vector<float> r(stride, 0.0f);
    if (in.empty() || stride == 0) {
        return r;
    }
    const bool cubic = smp.interpolation == Interp::CubicSpline;
    const size_t keySize = size_t(stride) * (cubic ? 3 : 1);
    const auto value = [&](size_t key, size_t i) {
        const size_t idx = key * keySize + (cubic ? stride : 0) + i;
        return idx < out.size() ? out[idx] : 0.0f;
    };
    if (out.size() == stride) { // a single keyframe
        for (uint32_t i = 0; i < stride; ++i) {
            r[i] = out[i];
        }
        return r;
    }
    if (maxTime > 0.0f) {
        t = std::fmod(t, maxTime);
    }
    t = std::clamp(t, in.front(), in.back());
    size_t nextKey = in.size() - 1;
    for (size_t i = 0; i < in.size(); ++i) {
        if (t <= in[i]) {
            nextKey = std::clamp<size_t>(i, 1, in.size() - 1);
            break;
        }
    }
    const size_t prevKey = nextKey > 0 ? nextKey - 1 : 0;
    const float keyDelta = in[nextKey] - in[prevKey];
    const float tn = keyDelta != 0.0f ? (t - in[prevKey]) / keyDelta : 0.0f;
    const auto normalize4 = [](std::vector<float>& q) {
        const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (l > 0.0f) {
            for (float& v : q) {
                v /= l;
            }
        }
    };
    if (smp.interpolation == Interp::Step) {
        for (uint32_t i = 0; i < stride; ++i) {
            r[i] = value(prevKey, i);
        }
        return r;
    }
    if (cubic) {
        const float tSq = tn * tn, tCub = tSq * tn;
        for (uint32_t i = 0; i < stride; ++i) {
            const size_t pi = prevKey * keySize, ni = nextKey * keySize;
            const float v0 = out[pi + stride + i];
            const float a = keyDelta * out[ni + i];
            const float b = keyDelta * out[pi + 2 * stride + i];
            const float v1 = out[ni + stride + i];
            r[i] = (2 * tCub - 3 * tSq + 1) * v0 + (tCub - 2 * tSq + tn) * b + (-2 * tCub + 3 * tSq) * v1 +
                   (tCub - tSq) * a;
        }
        if (path == Path::Rotation && stride == 4) {
            normalize4(r);
        }
        return r;
    }
    if (path == Path::Rotation && stride == 4) {
        // slerpQuat: normalized q0, q1, gl-matrix slerp, normalized.
        std::vector<float> q0(4), q1(4);
        for (int i = 0; i < 4; ++i) {
            q0[i] = value(prevKey, i);
            q1[i] = value(nextKey, i);
        }
        normalize4(q0);
        normalize4(q1);
        float cosom = q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3];
        if (cosom < 0.0f) {
            cosom = -cosom;
            for (float& v : q1) {
                v = -v;
            }
        }
        float s0 = 1.0f - tn, s1 = tn;
        if (1.0f - cosom > 0.000001f) {
            const float omega = std::acos(cosom), sinom = std::sin(omega);
            s0 = std::sin((1.0f - tn) * omega) / sinom;
            s1 = std::sin(tn * omega) / sinom;
        }
        for (int i = 0; i < 4; ++i) {
            r[i] = s0 * q0[i] + s1 * q1[i];
        }
        normalize4(r);
        return r;
    }
    for (uint32_t i = 0; i < stride; ++i) {
        r[i] = value(prevKey, i) * (1 - tn) + value(nextKey, i) * tn;
    }
    return r;
}

// The model's nodes / mesh weights with the animations refgen plays applied at `time` (tools/refgen/render.mjs: every
// animation not overlapping an earlier one, gltf.js nonDisjointAnimations; animation.js advance).
void animate(const asset::ModelData& model, float time, std::vector<asset::Node>& nodes,
             std::vector<std::vector<float>>& meshWeights) {
    const size_t n = model.animations.size();
    std::vector<std::vector<Property>> props(n);
    for (size_t a = 0; a < n; ++a) {
        for (const asset::AnimationChannel& c : model.animations[a].channels) {
            props[a].push_back(propertyOf(model, c));
        }
    }
    const auto disjoint = [&](size_t i, size_t k) {
        for (const Property& pi : props[i]) {
            for (const Property& pk : props[k]) {
                if (pi == pk) {
                    return false;
                }
            }
        }
        return true;
    };
    std::vector<size_t> selected;
    for (size_t i = 0; i < n; ++i) {
        bool ok = true;
        for (size_t k : selected) {
            ok = ok && (i == k || disjoint(k, i));
        }
        if (ok) {
            selected.push_back(i);
        }
    }
    for (size_t a : selected) {
        const asset::Animation& anim = model.animations[a];
        float maxTime = 0.0f;
        for (const asset::AnimationChannel& c : anim.channels) {
            if (c.sampler >= 0 && static_cast<size_t>(c.sampler) < anim.samplers.size() &&
                !anim.samplers[c.sampler].input.empty()) {
                maxTime = std::max(maxTime, anim.samplers[c.sampler].input.back());
            }
        }
        for (size_t ci = 0; ci < anim.channels.size(); ++ci) {
            const asset::AnimationChannel& c = anim.channels[ci];
            const Property p = props[a][ci];
            if (p.node < 0 || c.sampler < 0 || static_cast<size_t>(c.sampler) >= anim.samplers.size()) {
                continue;
            }
            const asset::AnimationSampler& smp = anim.samplers[c.sampler];
            asset::Node& node = nodes[p.node];
            switch (c.path) {
            case Path::Translation: {
                const auto v = interpolate(smp, c.path, 3, time, maxTime);
                node.translation = {v[0], v[1], v[2]};
                break;
            }
            case Path::Rotation: {
                const auto v = interpolate(smp, c.path, 4, time, maxTime);
                node.rotation = {v[0], v[1], v[2], v[3]};
                break;
            }
            case Path::Scale: {
                const auto v = interpolate(smp, c.path, 3, time, maxTime);
                node.scale = {v[0], v[1], v[2]};
                break;
            }
            case Path::Weights: {
                std::vector<float>& w = p.mesh >= 0 ? meshWeights[p.mesh] : node.weights;
                if (!w.empty()) {
                    w = interpolate(smp, c.path, static_cast<uint32_t>(w.size()), time, maxTime);
                }
                break;
            }
            case Path::Pointer: break;
            }
        }
    }
}

// World transforms of every hierarchy (scene.js applyTransformHierarchy), iteratively (deep chains).
std::vector<Mat4> worldOf(const std::vector<asset::Node>& nodes) {
    std::vector<Mat4> world(nodes.size(), Mat4{});
    for (size_t r = 0; r < nodes.size(); ++r) {
        if (nodes[r].parent >= 0) {
            continue;
        }
        std::vector<std::pair<int, Mat4>> stack{{static_cast<int>(r), Mat4{}}};
        while (!stack.empty()) {
            auto [n, parent] = stack.back();
            stack.pop_back();
            world[n] = parent * localOf(nodes[n]);
            for (const int c : nodes[n].children) {
                stack.emplace_back(c, world[n]);
            }
        }
    }
    return world;
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
    // Rest pose (the fit camera's extents: refgen calls resetView before the first frame animates), then the pose at
    // t = 0 that every frame draws (Sample-Viewer advances the played animations at elapsedSec() = 0).
    const std::vector<Mat4> rest = worldOf(model.nodes);
    std::vector<asset::Node> posed = model.nodes;
    std::vector<std::vector<float>> meshWeights(model.meshes.size());
    for (size_t m = 0; m < model.meshes.size(); ++m) {
        meshWeights[m] = model.meshes[m].weights;
    }
    animate(model, 0.0f, posed, meshWeights);
    s.world = worldOf(posed);
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
        const asset::Node& node = posed[n];
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
        const std::vector<float>& w = !node.weights.empty() ? node.weights : meshWeights[node.mesh];
        d.morphWeights.assign(targets, 0.0f);
        for (size_t t = 0; t < targets && t < w.size(); ++t) {
            d.morphWeights[t] = w[t];
        }
        // Extents (gltf_utils.js getExtentsFromAccessor): accessor box -> world -> re-box -> centre +- radius.
        for (const asset::Primitive& p : mesh.primitives) {
            // The accessor's JSON min / max as Sample-Renderer reads them; the decoded bounds when the file has none.
            const asset::Bounds& pb = p.accessorBounds.valid ? p.accessorBounds : p.bounds;
            if (!pb.valid) {
                continue;
            }
            Vec3 bmin{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::infinity()};
            Vec3 bmax{-bmin[0], -bmin[1], -bmin[2]};
            for (int corner = 0; corner < 8; ++corner) {
                const Vec3 c{(corner & 4) ? pb.max[0] : pb.min[0], (corner & 2) ? pb.max[1] : pb.min[1],
                             (corner & 1) ? pb.max[2] : pb.min[2]};
                const Vec3 wc = transformPoint(rest[n], c);
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
