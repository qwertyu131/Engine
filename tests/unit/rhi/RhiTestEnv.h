#pragma once
// Environment of the RHI tests: the shader library directory (written by the `shaders` build step,
// UTF-8 compile definition) and the backend configurations a test binary run covers.
#include "rhi/Rhi.h"
#include "rhi/ShaderLibrary.h"

#include <filesystem>
#include <string>
#include <vector>

namespace eng::rhi::test {

inline std::filesystem::path shaderLibDir() {
    return pathFromUtf8(ENG_TEST_SHADER_LIB_DIR);
}

// tests/assets of the source tree (cube_faces.png: plan F2 Task 3).
inline std::filesystem::path assetDir() {
    return pathFromUtf8(ENG_TEST_ASSET_DIR);
}

struct BackendConfig {
    std::string name; // vulkan | d3d11 | d3d11-warp
    DeviceDesc desc;
};

// Set by test_main.cpp from --rhi-backend=<name> (one ctest entry per backend configuration).
const BackendConfig& currentBackend();

// Set by test_main.cpp from --caps-json=<file> (suite rhi.dumpcaps, plan F3 Task 1): a JSON file written by
// `headless --dump-caps`. Empty otherwise.
const std::string& capsJsonPath();

} // namespace eng::rhi::test
