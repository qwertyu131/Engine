// RHI tests (plan F2 Task 2). One ctest entry per backend configuration:
//   rhi_unit_tests --rhi-backend=vulkan|d3d11|d3d11-warp|d3d11-fl10_0|d3d11-warp-fl10_0|cross [doctest options]
// `cross` (plan F2 Task 4) runs the backend-vs-backend suite rhi.cross and needs Vulkan, D3D11 and WARP.
// `d3d11-fl10_0` / `d3d11-warp-fl10_0` (plan F2 Task 7) cap the D3D11 feature level at 10_0 (T0) and run only
// suite rhi.t0: the FL11 suites use SM 5.0 shaders, which a T0 device rejects by design.
// The backend is created once up front:
//   - created          -> "RHI backend: <name> (<adapter>, <api>)" is printed and the cases run;
//   - exit 3 (absent)  -> "SKIP: backend <name> not available: <reason>", exit 77 = ctest Skipped
//                         (visible in the ctest summary, never a silent pass);
//   - any other error  -> printed, exit 1 (a missing validation layer is exit 4 from createDevice and a
//                         failure here, plan F2 Global Constraints).
// Without --rhi-backend only the backend-independent suites run (rhi.shaderlib, rhi.layout, rhi.ibl, rhi.caps).
// --caps-json=<file> (plan F3 Task 1) runs only suite rhi.dumpcaps on that `headless --dump-caps` output.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "RhiTestEnv.h"

namespace eng::rhi::test {
namespace {
BackendConfig g_backend;
std::string g_capsJson;
}
const BackendConfig& currentBackend() {
    return g_backend;
}
const std::string& capsJsonPath() {
    return g_capsJson;
}
} // namespace eng::rhi::test

int main(int argc, char** argv) {
    using namespace eng::rhi;
    std::vector<char*> args;
    std::string backend;
    for (int i = 0; i < argc; ++i) {
        if (std::strncmp(argv[i], "--rhi-backend=", 14) == 0) {
            backend = argv[i] + 14;
        } else if (std::strncmp(argv[i], "--caps-json=", 12) == 0) {
            test::g_capsJson = argv[i] + 12;
        } else {
            args.push_back(argv[i]);
        }
    }
    doctest::Context ctx(static_cast<int>(args.size()), args.data());
    if (!test::g_capsJson.empty()) {
        ctx.setOption("test-suite", "rhi.dumpcaps");
    } else if (backend == "cross") {
        for (const char* name : {"vulkan", "d3d11", "d3d11-warp"}) {
            DeviceDesc d;
            d.backend = std::string(name) == "vulkan" ? Backend::Vulkan : Backend::D3D11;
            d.warp = std::string(name) == "d3d11-warp";
            auto dev = createDevice(d);
            if (!dev) {
                if (dev.error().exitCode == kExitNoBackend) {
                    std::printf("SKIP: backend %s not available: %s\n", name, dev.error().message.c_str());
                    return 77;
                }
                std::printf("FAIL: backend %s: createDevice exit %d: %s\n", name, dev.error().exitCode,
                            dev.error().message.c_str());
                return 1;
            }
            std::printf("RHI backend: %s (adapter '%s', API %s)\n", name, (*dev)->caps().adapterName.c_str(),
                        (*dev)->caps().apiVersion.c_str());
        }
        std::fflush(stdout);
        ctx.setOption("test-suite", "rhi.cross");
    } else if (!backend.empty()) {
        test::BackendConfig& cfg = const_cast<test::BackendConfig&>(test::currentBackend());
        cfg.name = backend;
        if (backend == "vulkan") {
            cfg.desc.backend = Backend::Vulkan;
        } else if (backend == "d3d11" || backend == "d3d11-warp" || backend == "d3d11-fl10_0" ||
                   backend == "d3d11-warp-fl10_0") {
            cfg.desc.backend = Backend::D3D11;
            cfg.desc.warp = backend.rfind("d3d11-warp", 0) == 0;
            cfg.desc.d3d11FeatureLevelCap = backend.ends_with("-fl10_0") ? 0xa000 : 0;
        } else {
            std::printf("unknown --rhi-backend=%s\n", backend.c_str());
            return 2;
        }
        auto dev = createDevice(cfg.desc);
        if (!dev) {
            if (dev.error().exitCode == kExitNoBackend) {
                std::printf("SKIP: backend %s not available: %s\n", backend.c_str(), dev.error().message.c_str());
                return 77;
            }
            std::printf("FAIL: backend %s: createDevice exit %d: %s\n", backend.c_str(), dev.error().exitCode,
                        dev.error().message.c_str());
            return 1;
        }
        std::printf("RHI backend: %s (adapter '%s', API %s, tier %d)\n", backend.c_str(),
                    (*dev)->caps().adapterName.c_str(), (*dev)->caps().apiVersion.c_str(), (*dev)->caps().tier);
        std::fflush(stdout);
        // Only the suite(s) of this backend configuration.
        if (cfg.desc.d3d11FeatureLevelCap != 0) {
            ctx.setOption("test-suite", "rhi.t0");
        } else {
            ctx.setOption("test-suite", ("rhi.device," + std::string(backend == "vulkan" ? "rhi.vulkan" : "rhi.d3d11")).c_str());
        }
    } else {
        ctx.setOption("test-suite", "rhi.shaderlib,rhi.layout,rhi.ibl,rhi.caps");
    }
    return ctx.run();
}
