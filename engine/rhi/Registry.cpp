// Backend registry (plan F2 Task 2): createDevice dispatches to the backends compiled into this build.
#include "Rhi.h"

#include <vector>

namespace eng::rhi {

#if defined(ENG_RHI_HAS_VULKAN)
Result<std::unique_ptr<IDevice>> createVulkanDevice(const DeviceDesc& desc); // vulkan/VkDevice.cpp
#endif
#if defined(ENG_RHI_HAS_D3D11)
Result<std::unique_ptr<IDevice>> createD3D11Device(const DeviceDesc& desc); // d3d11/D3D11Device.cpp
#endif

std::span<const Backend> compiledBackends() {
    static const std::vector<Backend> backends = {
#if defined(ENG_RHI_HAS_VULKAN)
        Backend::Vulkan,
#endif
#if defined(ENG_RHI_HAS_D3D11)
        Backend::D3D11,
#endif
    };
    return backends;
}

Result<std::unique_ptr<IDevice>> createDevice(const DeviceDesc& desc) {
    switch (desc.backend) {
    case Backend::Vulkan:
#if defined(ENG_RHI_HAS_VULKAN)
        return createVulkanDevice(desc);
#else
        break;
#endif
    case Backend::D3D11:
#if defined(ENG_RHI_HAS_D3D11)
        return createD3D11Device(desc);
#else
        break;
#endif
    }
    return std::unexpected(
        Error{kExitNoBackend, std::string("backend '") + toString(desc.backend) + "' not available in this build"});
}

} // namespace eng::rhi
