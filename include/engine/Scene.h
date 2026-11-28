#pragma once
// Facade scene / asset information (spec K-D, plan F3 Task 4): what the host can read about a loaded asset and an
// instantiated scene. The scene graph itself stays inside the engine.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace eng {

// What the renderer does with one extensionsUsed entry of an asset (plan F3 Task 8: the viewer's asset panel).
struct ExtensionStatus {
    std::string name;
    bool loaded = false; // true: handled in this build; false: parsed and ignored
    std::string note;    // what it does, or why it is ignored (e.g. "material extension, supported from F6")
};

struct AssetInfo {
    std::string source;                      // UTF-8 path or "builtin:<name>"
    bool builtin = false;
    uint32_t sceneCount = 0;                 // glTF scenes (1 for a built-in scene)
    int defaultScene = -1;                   // gltf.scene, -1 when absent
    std::vector<std::string> extensionsUsed; // glTF extensionsUsed
    std::vector<ExtensionStatus> extensions; // one per extensionsUsed entry, same order
    std::vector<std::string> messages;       // loader / upload diagnostics: "[info] ..." / "[warning] ..."
};

// A punctual light of a scene (plan F3 Task 7): a KHR_lights_punctual light of the asset (in the scene's node
// hierarchy, world space at t = 0) or one added by the host (Renderer::addLight). Units as KHR_lights_punctual:
// intensity in lux (directional) or candela (point / spot), range 0 = infinite, cone angles in radians.
enum class LightType : uint8_t { Directional, Point, Spot };

struct LightInfo {
    std::string name;
    LightType type = LightType::Point;
    std::array<float, 3> color{1.0f, 1.0f, 1.0f}; // linear
    float intensity = 1.0f;
    float range = 0.0f;
    float innerConeAngle = 0.0f;
    float outerConeAngle = 0.785398163f;          // pi / 4
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> direction{0.0f, 0.0f, -1.0f}; // the way the light points (the node's -Z), normalized
    bool enabled = true;
    bool fromAsset = true; // false: added with Renderer::addLight (ignored by addLight)
};

struct SceneInfo {
    uint32_t drawCount = 0;    // node-primitive draws
    bool extentsValid = false; // false: nothing with bounds (empty scene)
    std::array<float, 3> extentMin{0, 0, 0};
    std::array<float, 3> extentMax{0, 0, 0};
};

} // namespace eng
