// RHI tests (plan F2 Task 2). One ctest entry per backend configuration:
//   rhi_unit_tests --rhi-backend=vulkan|d3d11|d3d11-warp|cross [doctest options]
// `cross` (plan F2 Task 4) runs the backend-vs-backend suite rhi.cross and needs Vulkan, D3D11 and WARP.
// The backend is created once up front:
//   - created          -> "RHI backend: <name> (<adapter>, <api>)" is printed and the cases run;
//   - exit 3 (absent)  -> "SKIP: backend <name> not available: <reason>", exit 77 = ctest Skipped
//                         (visible in the ctest summary, never a silent pass);
//   - any other error  -> printed, exit 1 (a missing validation layer is exit 4 from createDevice and a
//                         failure here, plan F2 Global Constraints).
// Without --rhi-backend only the backend-independent suites run (rhi.shaderlib, rhi.layout, rhi.ibl).
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
}
const BackendConfig& currentBackend() {
    return g_backend;
}
} // namespace eng::rhi::test

int main(int argc, char** argv) {
    using namespace eng::rhi;
    std::vector<char*> args;
    std::string backend;
    for (int i = 0; i < argc; ++i) {
        if (std::strncmp(argv[i], "--rhi-backend=", 14) == 0) {
            backend = argv[i] + 14;
        } else {
            args.push_back(argv[i]);
        }
    }
    doctest::Context ctx(static_cast<int>(args.size()), args.data());
    if (backend == "cross") {
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
        } else if (backend == "d3d11" || backend == "d3d11-warp") {
            cfg.desc.backend = Backend::D3D11;
            cfg.desc.warp = backend == "d3d11-warp";
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
        // Only the suite of this backend + the backend-independent rhi suites.
        ctx.setOption("test-suite", ("rhi.device," + std::string(backend == "vulkan" ? "rhi.vulkan" : "rhi.d3d11")).c_str());
    } else {
        ctx.setOption("test-suite", "rhi.shaderlib,rhi.layout,rhi.ibl");
    }
    return ctx.run();
}
