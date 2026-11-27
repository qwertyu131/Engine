#include "Validate.h"

#include "Process.h"

#include <algorithm>

namespace fs = std::filesystem;

namespace eng::sc {

std::string toString(ValidateStatus s) {
    return s == ValidateStatus::PendingCI ? "PENDING-CI" : "passed";
}

namespace {

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

// DXBC container: "DXBC" + 16-byte digest. An all-zero digest = unsigned (DXIL without dxil.dll).
std::expected<void, std::string> checkContainer(const std::vector<uint8_t>& bc, const char* what) {
    if (bc.size() < 32 || bc[0] != 'D' || bc[1] != 'X' || bc[2] != 'B' || bc[3] != 'C') {
        return std::unexpected(std::string(what) + ": not a DXBC container");
    }
    if (std::all_of(bc.begin() + 4, bc.begin() + 20, [](uint8_t b) { return b == 0; })) {
        return std::unexpected(std::string(what) + ": container is not signed (zero digest; dxil.dll missing next to dxcompiler.dll?)");
    }
    return {};
}

std::expected<void, std::string> runValidator(const fs::path& exe, const char* name, const std::vector<Arg>& args,
                                              const fs::path& cwd) {
    auto r = runProcess(exe, args, cwd);
    if (!r) {
        return std::unexpected(r.error());
    }
    if (r->exitCode != 0) {
        return std::unexpected(std::string(name) + " rejected the output (exit " + std::to_string(r->exitCode) +
                               "):\n" + trim(r->output));
    }
    return {};
}

} // namespace

std::expected<ValidateStatus, std::string> validate(Target target, const CompileResult& result, const ToolPaths& tools) {
    if (target == Target::MSL) {
        return ValidateStatus::PendingCI;
    }
    TempDir tmp;
    switch (target) {
    case Target::SPIRV: {
        auto exe = requireTool(tools.spirvVal, "spirv-val");
        if (!exe) {
            return std::unexpected(exe.error());
        }
        const fs::path f = tmp.path() / "shader.spv";
        if (auto w = writeFile(f, result.bytecode.data(), result.bytecode.size()); !w) {
            return std::unexpected(w.error());
        }
        if (auto v = runValidator(*exe, "spirv-val", {"--target-env", "vulkan1.1", f}, tmp.path()); !v) {
            return std::unexpected(v.error());
        }
        return ValidateStatus::Passed;
    }
    case Target::DXIL: {
        auto exe = requireTool(tools.dxc, "dxc");
        if (!exe) {
            return std::unexpected(exe.error());
        }
        if (auto c = checkContainer(result.bytecode, "DXIL"); !c) {
            return std::unexpected(c.error());
        }
        const fs::path f = tmp.path() / "shader.dxil";
        if (auto w = writeFile(f, result.bytecode.data(), result.bytecode.size()); !w) {
            return std::unexpected(w.error());
        }
        if (auto v = runValidator(*exe, "dxc -dumpbin", {"-dumpbin", f}, tmp.path()); !v) {
            return std::unexpected(v.error());
        }
        return ValidateStatus::Passed;
    }
    case Target::DXBC: {
        if (auto c = checkContainer(result.bytecode, "DXBC"); !c) {
            return std::unexpected(c.error());
        }
        return ValidateStatus::Passed;
    }
    case Target::GLSL_ES300:
    case Target::GLSL_ES320: {
        auto exe = requireTool(tools.glslang, "glslangValidator");
        if (!exe) {
            return std::unexpected(exe.error());
        }
        const std::string stage = result.reflection.value("stage", std::string());
        const char* glslStage = stage == "vs" ? "vert" : stage == "fs" ? "frag" : "comp";
        const fs::path f = tmp.path() / (std::string("shader.") + glslStage);
        if (auto w = writeFile(f, result.text); !w) {
            return std::unexpected(w.error());
        }
        if (auto v = runValidator(*exe, "glslangValidator", {"-S", glslStage, f}, tmp.path()); !v) {
            return std::unexpected(v.error());
        }
        return ValidateStatus::Passed;
    }
    case Target::WGSL: {
        auto exe = requireTool(tools.tint, "tint");
        if (!exe) {
            return std::unexpected(exe.error());
        }
        const fs::path f = tmp.path() / "shader.wgsl";
        if (auto w = writeFile(f, result.text); !w) {
            return std::unexpected(w.error());
        }
        if (auto v = runValidator(*exe, "tint", {"--validate", "--format", "wgsl", f, "-o", tmp.path() / "roundtrip.wgsl"},
                                  tmp.path());
            !v) {
            return std::unexpected(v.error());
        }
        return ValidateStatus::Passed;
    }
    case Target::MSL: break;
    }
    return ValidateStatus::PendingCI;
}

} // namespace eng::sc
