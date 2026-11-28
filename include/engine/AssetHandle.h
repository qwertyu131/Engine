#pragma once
// Facade handles (spec K-D, plan F3 Task 4). Plain ids issued by eng::Renderer; 0 = null.
#include <cstdint>

namespace eng {

// A loaded asset: a glTF file (.gltf / .glb) or a built-in scene ("builtin:<name>").
struct AssetHandle {
    uint32_t id = 0;
    explicit operator bool() const { return id != 0; }
    friend bool operator==(AssetHandle, AssetHandle) = default;
};

// An instantiated scene of an asset (one glTF scene, rest pose in F3).
struct SceneHandle {
    uint32_t id = 0;
    explicit operator bool() const { return id != 0; }
    friend bool operator==(SceneHandle, SceneHandle) = default;
};

} // namespace eng
