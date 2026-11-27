// Vulkan validation hook (plan F2 Task 2, Global Constraints): VK_EXT_debug_utils messenger on
// VK_LAYER_KHRONOS_validation. Every message of severity WARNING or ERROR is logged through eng::log
// and counted; submitAndWait() then returns exit 4 with the first message (which carries the VUID).
// An ERROR from the validation layer returns VK_TRUE: the layer skips the invalid call instead of
// passing it to the driver (a draw without a pipeline must not reach the driver).
#include "VkCommon.h"

#include "core/Log.h"

namespace eng::rhi::vk {

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL messengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT types,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto* dev = static_cast<VulkanDevice*>(user);
    const bool error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    const bool warning = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0;
    if (!error && !warning) {
        return VK_FALSE;
    }
    const char* msg = data && data->pMessage ? data->pMessage : "(no message)";
    if (dev) {
        dev->onValidationMessage(error, msg);
    }
    return (error && (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)) ? VK_TRUE : VK_FALSE;
}

} // namespace

VkDebugUtilsMessengerCreateInfoEXT messengerCreateInfo(void* user) {
    VkDebugUtilsMessengerCreateInfoEXT ci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = messengerCallback;
    ci.pUserData = user;
    return ci;
}

void VulkanDevice::onValidationMessage(bool error, const char* message) {
    validationErrors_.fetch_add(1);
    {
        std::lock_guard lock(messageMutex_);
        if (firstMessage_.empty()) {
            firstMessage_ = message;
        }
    }
    if (error) {
        ENG_LOG_ERROR("vulkan validation: {}", message);
    } else {
        ENG_LOG_WARN("vulkan validation: {}", message);
    }
}

Error VulkanDevice::validationError() const {
    std::lock_guard lock(messageMutex_);
    return Error{kExitValidation, "vulkan: " + std::to_string(validationErrors_.load()) +
                                      " validation message(s) of severity warning or above; first: " + firstMessage_};
}

} // namespace eng::rhi::vk
