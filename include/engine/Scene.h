#pragma once
// Facade scene / asset information (spec K-D, plan F3 Task 4): what the host can read about a loaded asset and an
// instantiated scene. The scene graph itself stays inside the engine.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace eng {

struct AssetInfo {
    std::string source;                      // UTF-8 path or "builtin:<name>"
    bool builtin = false;
    uint32_t sceneCount = 0;                 // glTF scenes (1 for a built-in scene)
    int defaultScene = -1;                   // gltf.scene, -1 when absent
    std::vector<std::string> extensionsUsed; // glTF extensionsUsed
    std::vector<std::string> messages;       // loader / upload diagnostics: "[info] ..." / "[warning] ..."
};

struct SceneInfo {
    uint32_t drawCount = 0;    // node-primitive draws
    bool extentsValid = false; // false: nothing with bounds (empty scene)
    std::array<float, 3> extentMin{0, 0, 0};
    std::array<float, 3> extentMax{0, 0, 0};
};

} // namespace eng
