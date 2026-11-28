#include "VkFeatureCheck.h"

#include <format>

namespace eng::rhi::vk {

std::string missingRequiredFeature(const PhysicalDeviceSupport& s) {
    if (!s.graphicsQueue) {
        return "graphics queue";
    }
    if (s.apiMajor < 1 || (s.apiMajor == 1 && s.apiMinor < 2)) {
        return std::format("Vulkan 1.2 (device has {}.{}.{})", s.apiMajor, s.apiMinor, s.apiPatch);
    }
    if (s.apiMajor > 1 || s.apiMinor >= 3) {
        return s.dynamicRenderingFeature ? std::string() : std::string("dynamicRendering");
    }
    return s.khrDynamicRenderingExtension ? std::string()
                                          : std::string("dynamicRendering (Vulkan 1.3 or VK_KHR_dynamic_rendering)");
}

Error missingFeatureError(std::string_view deviceName, std::string_view missing) {
    return Error{kExitNoBackend, std::format("vulkan: device '{}' lacks required feature: {}", deviceName, missing)};
}

} // namespace eng::rhi::vk
