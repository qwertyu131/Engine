// shaderc CLI (plan F1 Task 2).
//   shaderc compile --src <f> --entry <e> --stage vs|fs|cs --target <t>|all [--define K=V]... [-I dir]...
//                   --out <dir> [--validate] [--dxbc-path direct|cross] [--tool <name>=<path>]...
// Targets: dxil dxbc spirv msl glsl-es300 glsl-es320 wgsl. Outputs: <stem>.<entry>.<target>.<ext> and
// <stem>.<entry>.<target>.refl.json. Exit: 0 ok, 1 compile/validate failure, 2 usage error or tool missing.
// Tool names for --tool: dxc fxc tint spirv-val glslangValidator (defaults: paths found at configure time).
#include "Compiler.h"
#include "Process.h"
#include "Targets.h"
#include "Validate.h"

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace eng::sc;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFail = 1;
constexpr int kExitUsage = 2;

void print(FILE* f, const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), f);
    std::fputc('\n', f);
}

int usage(const std::string& msg) {
    print(stderr, "shaderc: " + msg);
    print(stderr, "usage: shaderc compile --src <file> --entry <name> --stage vs|fs|cs --target <t>|all "
                  "[--define K=V]... [-I dir]... --out <dir> [--validate] [--dxbc-path direct|cross] "
                  "[--tool <name>=<path>]...");
    print(stderr, "targets: dxil dxbc spirv msl glsl-es300 glsl-es320 wgsl");
    return kExitUsage;
}

bool isToolMissing(const std::string& err) {
    return err.rfind("tool not found:", 0) == 0;
}

ToolPaths defaultTools() {
    ToolPaths t;
    t.dxc = fromUtf8(ENG_DEFAULT_DXC);
    t.fxc = fromUtf8(ENG_DEFAULT_FXC);
    t.tint = fromUtf8(ENG_DEFAULT_TINT);
    t.spirvVal = fromUtf8(ENG_DEFAULT_SPIRV_VAL);
    t.glslang = fromUtf8(ENG_DEFAULT_GLSLANG);
    return t;
}

int run(const std::vector<std::string>& args) {
    if (args.empty() || args[0] != "compile") {
        return usage(args.empty() ? "missing command" : "unknown command '" + args[0] + "'");
    }
    CompileRequest base;
    std::optional<std::string> src, entry, stage, target, out;
    bool doValidate = false;
    ToolPaths tools = defaultTools();
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&](std::optional<std::string>& dst) {
            if (i + 1 >= args.size()) {
                return false;
            }
            dst = args[++i];
            return true;
        };
        std::optional<std::string> v;
        if (a == "--src") {
            if (!value(src)) return usage("--src needs a value");
        } else if (a == "--entry") {
            if (!value(entry)) return usage("--entry needs a value");
        } else if (a == "--stage") {
            if (!value(stage)) return usage("--stage needs a value");
        } else if (a == "--target") {
            if (!value(target)) return usage("--target needs a value");
        } else if (a == "--out") {
            if (!value(out)) return usage("--out needs a value");
        } else if (a == "--validate") {
            doValidate = true;
        } else if (a == "--define" || a == "-D") {
            if (!value(v)) return usage(a + " needs K=V");
            const size_t eq = v->find('=');
            base.defines.emplace_back(v->substr(0, eq), eq == std::string::npos ? std::string() : v->substr(eq + 1));
        } else if (a == "-I") {
            if (!value(v)) return usage("-I needs a directory");
            base.includeDirs.push_back(fromUtf8(*v));
        } else if (a == "--dxbc-path") {
            if (!value(v)) return usage("--dxbc-path needs direct|cross");
            auto p = parseDxbcPath(*v);
            if (!p) return usage("unknown --dxbc-path '" + *v + "'");
            base.dxbcPath = *p;
        } else if (a == "--tool") {
            if (!value(v)) return usage("--tool needs <name>=<path>");
            const size_t eq = v->find('=');
            const std::string name = v->substr(0, eq);
            const fs::path path = eq == std::string::npos ? fs::path() : fromUtf8(v->substr(eq + 1));
            if (name == "dxc") tools.dxc = path;
            else if (name == "fxc") tools.fxc = path;
            else if (name == "tint") tools.tint = path;
            else if (name == "spirv-val") tools.spirvVal = path;
            else if (name == "glslangValidator") tools.glslang = path;
            else return usage("unknown tool '" + name + "'");
        } else {
            return usage("unknown argument '" + a + "'");
        }
    }
    if (!src || !entry || !stage || !target || !out) {
        return usage("--src, --entry, --stage, --target and --out are required");
    }
    const auto st = parseStage(*stage);
    if (!st) {
        return usage("unknown stage '" + *stage + "'");
    }
    std::vector<Target> targets;
    if (*target == "all") {
        targets.assign(kAllTargets.begin(), kAllTargets.end());
    } else if (auto t = parseTarget(*target)) {
        targets.push_back(*t);
    } else {
        return usage("unknown target '" + *target + "'");
    }

    base.src = fromUtf8(*src);
    base.entry = *entry;
    base.stage = *st;
    const fs::path outDir = fromUtf8(*out);
    int exitCode = kExitOk;
    for (Target t : targets) {
        CompileRequest req = base;
        req.target = t;
        const std::string label = toString(t) + " " + req.entry;
        auto r = compile(req, tools);
        if (!r) {
            print(stderr, "shaderc: " + label + ": " + r.error());
            if (isToolMissing(r.error())) {
                return kExitUsage;
            }
            exitCode = kExitFail;
            continue;
        }
        for (const std::string& d : r->diagnostics) {
            print(stderr, "shaderc: " + label + ": " + d);
        }
        auto written = writeOutputs(*r, outDir, base.src, req.entry, t);
        if (!written) {
            print(stderr, "shaderc: " + label + ": " + written.error());
            exitCode = kExitFail;
            continue;
        }
        std::string line = "shaderc: " + label + " -> " + toUtf8(written->front());
        if (doValidate) {
            auto v = validate(t, *r, tools);
            if (!v) {
                print(stderr, "shaderc: " + label + ": validation failed: " + v.error());
                if (isToolMissing(v.error())) {
                    return kExitUsage;
                }
                exitCode = kExitFail;
                continue;
            }
            line += " [" + toString(*v) + "]";
        }
        print(stdout, line);
    }
    return exitCode;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.push_back(toUtf8(fs::path(argv[i])));
    }
    try {
        return run(args);
    } catch (const std::exception& e) {
        print(stderr, std::string("shaderc: internal error: ") + e.what());
        return kExitFail;
    }
}
