#pragma once
// Tool paths for the shaderc unit tests: the absolute paths CMake found (cmake/ShaderTools.cmake),
// passed in as UTF-8 compile definitions.
#include "Compiler.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace eng::sc::test {

inline std::filesystem::path u8path(std::string_view s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

inline ToolPaths tools() {
    ToolPaths t;
    t.dxc = u8path(ENG_TEST_DXC);
    t.fxc = u8path(ENG_TEST_FXC);
    t.tint = u8path(ENG_TEST_TINT);
    t.spirvVal = u8path(ENG_TEST_SPIRV_VAL);
    t.glslang = u8path(ENG_TEST_GLSLANG);
    return t;
}

// `rel` is relative to the repo's shaders/ directory, e.g. "test/triangle.hlsl".
inline std::filesystem::path shaderSrc(std::string_view rel) {
    return u8path(ENG_TEST_SHADER_DIR) / u8path(rel);
}

inline std::filesystem::path triangleSrc() {
    return shaderSrc("test/triangle.hlsl");
}

} // namespace eng::sc::test
