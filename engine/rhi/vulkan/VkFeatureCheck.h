#pragma once
// Required-feature check of a Vulkan physical device (plan F2 Task 2 rules, Task 7 Review Focus 4). Kept free of
// Vulkan types so the decision and its error text are unit-tested without a device that lacks the feature.
//
// Required: a graphics queue; Vulkan 1.2; dynamicRendering (Vulkan 1.3 core feature, or the
// VK_KHR_dynamic_rendering extension on 1.2). A device without one is not usable: createDevice exits 3
// with the feature named, "vulkan: device '<name>' lacks required feature: <feature>".
#include "rhi/Types.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace eng::rhi::vk {

struct PhysicalDeviceSupport {
    bool graphicsQueue = false;
    uint32_t apiMajor = 0, apiMinor = 0, apiPatch = 0;
    bool dynamicRenderingFeature = false;   // VkPhysicalDeviceVulkan13Features::dynamicRendering (1.3+)
    bool khrDynamicRenderingExtension = false; // VK_KHR_dynamic_rendering is offered (1.2)
};

// Empty = usable; otherwise the first missing requirement, in the order above.
std::string missingRequiredFeature(const PhysicalDeviceSupport& s);

// Error{3, "vulkan: device '<name>' lacks required feature: <missing>"}.
Error missingFeatureError(std::string_view deviceName, std::string_view missing);

} // namespace eng::rhi::vk
