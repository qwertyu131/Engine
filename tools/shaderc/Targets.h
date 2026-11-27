#pragma once
// shaderc targets, stages and their names (plan F1 Task 2).
#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace eng::sc {

enum class Target { DXIL, DXBC, SPIRV, MSL, GLSL_ES300, GLSL_ES320, WGSL };
enum class Stage { Vertex, Fragment, Compute };
// D3D11 bytecode route: Direct = HLSL source -> FXC (registers flattened by shaderc);
// Cross = DXC -> SPIR-V -> SPIRV-Cross HLSL SM5 -> FXC. Default decided in F1 Task 5.
enum class DxbcPath { Direct, Cross };

inline constexpr std::array<Target, 7> kAllTargets = {Target::DXIL,       Target::DXBC,       Target::SPIRV, Target::MSL,
                                                       Target::GLSL_ES300, Target::GLSL_ES320, Target::WGSL};

std::string toString(Target t);   // dxil dxbc spirv msl glsl-es300 glsl-es320 wgsl
std::string toString(Stage s);    // vs fs cs
std::string toString(DxbcPath p); // direct cross
std::optional<Target> parseTarget(std::string_view s);
std::optional<Stage> parseStage(std::string_view s);
std::optional<DxbcPath> parseDxbcPath(std::string_view s);

std::string extension(Target t); // output file extension without dot
bool isTextTarget(Target t);     // MSL / GLSL / WGSL: CompileResult::text is the deliverable

} // namespace eng::sc
