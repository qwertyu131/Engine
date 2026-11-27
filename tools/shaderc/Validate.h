#pragma once
// Per-target validation of a CompileResult (plan F1 Task 2; dxil.dll check plan F2 Task 1):
//   SPIR-V      spirv-val --target-env vulkan1.1
//   DXIL        dxil.dll next to dxc.exe ("tool not found: dxil.dll (...)"), signed container (non-zero
//               DXBC digest) + `dxc -dumpbin` succeeds
//   DXBC        FXC succeeded (compile step) + DXBC container with a non-zero digest
//   GLSL ES     glslangValidator -S <stage> (the source carries #version 300 es / 320 es)
//   WGSL        tint --validate (WGSL -> WGSL round trip: Tint's resolver = WGSL validation)
//   MSL         PendingCI: `xcrun metal` runs only on the macOS CI job (spec K2)
#include "Compiler.h"
#include "Targets.h"

#include <expected>
#include <string>

namespace eng::sc {

enum class ValidateStatus { Passed, PendingCI };

std::string toString(ValidateStatus s); // "passed" | "PENDING-CI"

// Error = validation failure or missing tool (message starts with "tool not found:").
std::expected<ValidateStatus, std::string> validate(Target target, const CompileResult& result, const ToolPaths& tools);

} // namespace eng::sc
