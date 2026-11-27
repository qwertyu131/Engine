#pragma once
// shaderc core: one HLSL entry -> one target (plan F1 Task 2).
//   DXIL        DXC (SM 6.0)
//   SPIR-V      DXC -spirv (Vulkan 1.1)
//   MSL         SPIR-V -> SPIRV-Cross (MSL 3.0, macOS)
//   GLSL ES     SPIR-V -> SPIRV-Cross (#version 300 es / 320 es)
//   WGSL        SPIR-V -> Tint
//   DXBC        Direct: HLSL -> DXC preprocessor -> registers flattened -> FXC (SM 5.0)
//               Cross:  SPIR-V -> SPIRV-Cross HLSL SM 5.0 -> FXC
// Before any target is emitted (plan F1 Task 3): the binding convention is checked on the declared
// registers ("binding collision in space<S>: b<i> and t<i>") and features the target cannot express
// are rejected ("target <t> does not support <feature> (TIER=<n>)").
#include "Reflection.h"
#include "Targets.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eng::sc {

// Absolute tool paths. Empty or non-existent -> "tool not found: <name> (run scripts/fetch_shader_tools.py)".
struct ToolPaths {
    std::filesystem::path dxc;      // pinned DXC (third_party/tools/dxc), with dxil.dll for signing
    std::filesystem::path fxc;      // Windows SDK fxc.exe
    std::filesystem::path tint;     // third_party/tools/tint/tint.exe
    std::filesystem::path spirvVal; // Vulkan SDK spirv-val
    std::filesystem::path glslang;  // Vulkan SDK glslangValidator
};

struct CompileRequest {
    std::filesystem::path src;
    std::string entry;
    Stage stage = Stage::Vertex;
    Target target = Target::SPIRV;
    std::vector<std::pair<std::string, std::string>> defines;
    std::vector<std::filesystem::path> includeDirs;
    DxbcPath dxbcPath = DxbcPath::Cross; // only for Target::DXBC; default decided in plan F1 Task 5 (docs/spikes)
};

struct CompileResult {
    std::vector<uint8_t> bytecode; // DXIL / DXBC / SPIR-V
    std::string text;              // MSL / GLSL / WGSL; for DXBC-cross the SM5 HLSL fed to FXC
    nlohmann::json reflection;
    std::vector<std::string> diagnostics; // tool warnings / notes
};

std::string toolNotFound(std::string_view name);
// Error text if `path` is not an existing file.
std::expected<std::filesystem::path, std::string> requireTool(const std::filesystem::path& path, std::string_view name);

std::expected<CompileResult, std::string> compile(const CompileRequest& req, const ToolPaths& tools);

// Writes <stem>.<entry>.<target>.<ext> and <stem>.<entry>.<target>.refl.json into outDir (created).
std::expected<std::vector<std::filesystem::path>, std::string> writeOutputs(const CompileResult& r,
                                                                            const std::filesystem::path& outDir,
                                                                            const std::filesystem::path& src,
                                                                            const std::string& entry, Target target);

} // namespace eng::sc
