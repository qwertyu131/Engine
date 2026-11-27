// Vulkan device: instance, validation, physical / logical device, frames, submission, readback,
// deferred destruction (plan F2 Task 2).
#include "VkCommon.h"

#include "core/Log.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace eng::rhi {
Result<std::unique_ptr<IDevice>> createVulkanDevice(const DeviceDesc& desc) {
    return vk::VulkanDevice::create(desc);
}
} // namespace eng::rhi

namespace eng::rhi::vk {

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

std::string versionString(uint32_t v) {
    return std::format("{}.{}.{}", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

bool hasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    return std::any_of(exts.begin(), exts.end(),
                       [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

} // namespace

std::string vkResultString(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_VALIDATION_FAILED_EXT: return "VK_ERROR_VALIDATION_FAILED_EXT";
    default: return std::format("VkResult({})", static_cast<int>(r));
    }
}

Error VulkanDevice::vkError(const char* what, VkResult r) {
    if (r == VK_ERROR_DEVICE_LOST) {
        deviceLost_ = true;
        return Error{kExitDeviceLost, std::format("vulkan: {} failed: VK_ERROR_DEVICE_LOST", what)};
    }
    if (r == VK_ERROR_VALIDATION_FAILED_EXT || validationErrors_.load() > 0) {
        Error e = validationError();
        e.message = std::format("vulkan: {} failed ({}); {}", what, vkResultString(r), e.message);
        return e;
    }
    return Error{kExitDeviceLost, std::format("vulkan: {} failed: {}", what, vkResultString(r))};
}

Result<std::unique_ptr<IDevice>> VulkanDevice::create(const DeviceDesc& desc) {
    std::unique_ptr<VulkanDevice> dev(new VulkanDevice());
    if (auto r = dev->init(desc); !r) {
        return std::unexpected(r.error());
    }
    return std::unique_ptr<IDevice>(std::move(dev));
}

Result<void> VulkanDevice::createInstance(const DeviceDesc& desc) {
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) {
        vkEnumerateInstanceVersion(&loaderVersion);
    }
    if (loaderVersion < VK_API_VERSION_1_2) {
        return std::unexpected(Error{kExitNoBackend, "vulkan: loader supports Vulkan " + versionString(loaderVersion) +
                                                         ", need 1.2 or newer"});
    }
    std::vector<const char*> layers;
    std::vector<const char*> extensions;
    validation_ = desc.validation;
    if (validation_) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n);
        vkEnumerateInstanceLayerProperties(&n, props.data());
        const bool found = std::any_of(props.begin(), props.end(), [](const VkLayerProperties& p) {
            return std::strcmp(p.layerName, kValidationLayer) == 0;
        });
        if (!found) {
            return std::unexpected(
                Error{kExitValidation, std::string("validation layer not available: ") + kValidationLayer});
        }
        layers.push_back(kValidationLayer);
        uint32_t en = 0;
        vkEnumerateInstanceExtensionProperties(kValidationLayer, &en, nullptr);
        std::vector<VkExtensionProperties> layerExts(en);
        vkEnumerateInstanceExtensionProperties(kValidationLayer, &en, layerExts.data());
        uint32_t gn = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &gn, nullptr);
        std::vector<VkExtensionProperties> exts(gn);
        vkEnumerateInstanceExtensionProperties(nullptr, &gn, exts.data());
        if (!hasExtension(exts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) &&
            !hasExtension(layerExts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            return std::unexpected(Error{kExitValidation, "validation layer not available: " +
                                                              std::string(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)});
        }
        if (!hasExtension(layerExts, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME)) {
            return std::unexpected(Error{kExitValidation, std::string("validation layer not available: ") +
                                                              kValidationLayer + " without " +
                                                              VK_EXT_LAYER_SETTINGS_EXTENSION_NAME +
                                                              " (synchronization validation)"});
        }
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        extensions.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "eng";
    app.pEngineName = "eng";
    app.apiVersion = VK_API_VERSION_1_3;

    // Synchronization validation (plan F2 Global Constraints) through VK_EXT_layer_settings.
    const VkBool32 on = VK_TRUE;
    const VkLayerSettingEXT settings[] = {
        {kValidationLayer, "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &on},
        {kValidationLayer, "validate_core", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &on},
    };
    VkLayerSettingsCreateInfoEXT layerSettings{VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT};
    layerSettings.settingCount = static_cast<uint32_t>(std::size(settings));
    layerSettings.pSettings = settings;
    // Messages during vkCreateInstance / vkDestroyInstance go through this chained messenger too.
    VkDebugUtilsMessengerCreateInfoEXT messenger = messengerCreateInfo(this);
    messenger.pNext = &layerSettings;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    if (validation_) {
        ci.pNext = &messenger;
    }
    const VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
    if (r == VK_ERROR_INCOMPATIBLE_DRIVER || r == VK_ERROR_INITIALIZATION_FAILED) {
        return std::unexpected(Error{kExitNoBackend, "vulkan: vkCreateInstance failed: " + vkResultString(r)});
    }
    if (r != VK_SUCCESS) {
        return std::unexpected(vkError("vkCreateInstance", r));
    }
    volkLoadInstanceOnly(instance_);
    if (validation_) {
        VkDebugUtilsMessengerCreateInfoEXT mci = messengerCreateInfo(this);
        if (const VkResult mr = vkCreateDebugUtilsMessengerEXT(instance_, &mci, nullptr, &messenger_); mr != VK_SUCCESS) {
            return std::unexpected(vkError("vkCreateDebugUtilsMessengerEXT", mr));
        }
    }
    return {};
}

Result<void> VulkanDevice::pickPhysicalDevice(const DeviceDesc& desc) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(instance_, &n, devices.data());
    if (devices.empty()) {
        return std::unexpected(Error{kExitNoBackend, "vulkan: no physical device"});
    }
    if (desc.adapterIndex >= static_cast<int>(devices.size())) {
        return std::unexpected(Error{kExitUsage, std::format("vulkan: --adapter {} out of range ({} device(s))",
                                                             desc.adapterIndex, devices.size())});
    }

    struct Candidate {
        VkPhysicalDevice device;
        VkPhysicalDeviceProperties props;
        uint32_t queueFamily;
        bool khrDynamicRendering;
        std::string missing; // empty = usable
    };
    auto evaluate = [](VkPhysicalDevice pd) {
        Candidate c{pd, {}, UINT32_MAX, false, {}};
        vkGetPhysicalDeviceProperties(pd, &c.props);
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
        for (uint32_t i = 0; i < qn; ++i) {
            if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                c.queueFamily = i;
                break;
            }
        }
        if (c.queueFamily == UINT32_MAX) {
            c.missing = "graphics queue";
            return c;
        }
        if (c.props.apiVersion < VK_API_VERSION_1_2) {
            c.missing = "Vulkan 1.2 (device has " + versionString(c.props.apiVersion) + ")";
            return c;
        }
        if (c.props.apiVersion >= VK_API_VERSION_1_3) {
            VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            f2.pNext = &f13;
            vkGetPhysicalDeviceFeatures2(pd, &f2);
            if (!f13.dynamicRendering) {
                c.missing = "dynamicRendering";
            }
            return c;
        }
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> exts(en);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, exts.data());
        if (!hasExtension(exts, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) {
            c.missing = "dynamicRendering (Vulkan 1.3 or VK_KHR_dynamic_rendering)";
            return c;
        }
        c.khrDynamicRendering = true;
        return c;
    };

    std::vector<Candidate> cands;
    for (VkPhysicalDevice pd : devices) {
        cands.push_back(evaluate(pd));
    }
    const Candidate* chosen = nullptr;
    if (desc.adapterIndex >= 0) {
        chosen = &cands[static_cast<size_t>(desc.adapterIndex)];
    } else {
        for (const Candidate& c : cands) {
            if (c.missing.empty() && c.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                chosen = &c;
                break;
            }
        }
        if (!chosen) {
            for (const Candidate& c : cands) {
                if (c.missing.empty()) {
                    chosen = &c;
                    break;
                }
            }
        }
        if (!chosen) {
            chosen = &cands[0];
        }
    }
    if (!chosen->missing.empty()) {
        return std::unexpected(Error{kExitNoBackend, std::format("vulkan: device '{}' lacks required feature: {}",
                                                                 chosen->props.deviceName, chosen->missing)});
    }
    physical_ = chosen->device;
    queueFamily_ = chosen->queueFamily;
    khrDynamicRendering_ = chosen->khrDynamicRendering;
    apiVersion_ = std::min<uint32_t>(chosen->props.apiVersion, VK_API_VERSION_1_3);
    caps_.backend = Backend::Vulkan;
    caps_.tier = 2;
    caps_.apiVersion = versionString(chosen->props.apiVersion);
    caps_.adapterName = chosen->props.deviceName;
    caps_.vendorId = chosen->props.vendorID;
    caps_.software = chosen->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    return {};
}

Result<void> VulkanDevice::createLogicalDevice() {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    std::vector<const char*> exts;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR fdr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    if (khrDynamicRendering_) {
        exts.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        fdr.dynamicRendering = VK_TRUE;
        f2.pNext = &fdr;
    } else {
        f13.dynamicRendering = VK_TRUE;
        f2.pNext = &f13;
    }
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    if (const VkResult r = vkCreateDevice(physical_, &ci, nullptr, &device_); r != VK_SUCCESS) {
        if (r == VK_ERROR_FEATURE_NOT_PRESENT || r == VK_ERROR_EXTENSION_NOT_PRESENT) {
            return std::unexpected(Error{kExitNoBackend, "vulkan: vkCreateDevice failed: " + vkResultString(r)});
        }
        return std::unexpected(vkError("vkCreateDevice", r));
    }
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    cmdBeginRendering = khrDynamicRendering_ ? vkCmdBeginRenderingKHR : vkCmdBeginRendering;
    cmdEndRendering = khrDynamicRendering_ ? vkCmdEndRenderingKHR : vkCmdEndRendering;

    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci{};
    aci.vulkanApiVersion = apiVersion_;
    aci.physicalDevice = physical_;
    aci.device = device_;
    aci.instance = instance_;
    aci.pVulkanFunctions = &fns;
    if (const VkResult r = vmaCreateAllocator(&aci, &allocator_); r != VK_SUCCESS) {
        return std::unexpected(vkError("vmaCreateAllocator", r));
    }

    for (Frame& f : frames_) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = queueFamily_;
        if (const VkResult r = vkCreateCommandPool(device_, &pci, nullptr, &f.pool); r != VK_SUCCESS) {
            return std::unexpected(vkError("vkCreateCommandPool", r));
        }
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (const VkResult r = vkAllocateCommandBuffers(device_, &ai, &f.cmd); r != VK_SUCCESS) {
            return std::unexpected(vkError("vkAllocateCommandBuffers", r));
        }
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (const VkResult r = vkCreateFence(device_, &fci, nullptr, &f.fence); r != VK_SUCCESS) {
            return std::unexpected(vkError("vkCreateFence", r));
        }
    }
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = queueFamily_;
    if (const VkResult r = vkCreateCommandPool(device_, &pci, nullptr, &transferPool_); r != VK_SUCCESS) {
        return std::unexpected(vkError("vkCreateCommandPool", r));
    }
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (const VkResult r = vkCreateFence(device_, &fci, nullptr, &transferFence_); r != VK_SUCCESS) {
        return std::unexpected(vkError("vkCreateFence", r));
    }
    cmdList_ = std::make_unique<VulkanCommandList>(*this);
    return {};
}

Result<void> VulkanDevice::init(const DeviceDesc& desc) {
    if (desc.warp) {
        return std::unexpected(Error{kExitUsage, "vulkan: --warp is a D3D11 option"});
    }
    if (const VkResult r = volkInitialize(); r != VK_SUCCESS) {
        return std::unexpected(Error{kExitNoBackend, "vulkan: Vulkan loader not found (" + vkResultString(r) + ")"});
    }
    if (auto r = createInstance(desc); !r) {
        return r;
    }
    if (auto r = pickPhysicalDevice(desc); !r) {
        return r;
    }
    if (auto r = createLogicalDevice(); !r) {
        return r;
    }
    if (validationErrors_.load() > 0) {
        return std::unexpected(validationError());
    }
    ENG_LOG_INFO("vulkan: {} (API {}){}", caps_.adapterName, caps_.apiVersion,
                 validation_ ? ", VK_LAYER_KHRONOS_validation + sync validation" : "");
    return {};
}

VulkanDevice::~VulkanDevice() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        runDeferred(true);
        pipelines_.clear([&](Pipeline& p) {
            vkDestroyPipeline(device_, p.pipeline, nullptr);
            vkDestroyPipelineLayout(device_, p.layout, nullptr);
        });
        shaders_.clear([&](Shader& s) { vkDestroyShaderModule(device_, s.module, nullptr); });
        textures_.clear([&](Texture& t) {
            vkDestroyImageView(device_, t.view, nullptr);
            vmaDestroyImage(allocator_, t.image, t.allocation);
        });
        buffers_.clear([&](Buffer& b) { vmaDestroyBuffer(allocator_, b.buffer, b.allocation); });
        for (Frame& f : frames_) {
            vkDestroyFence(device_, f.fence, nullptr);
            vkDestroyCommandPool(device_, f.pool, nullptr);
        }
        vkDestroyFence(device_, transferFence_, nullptr);
        vkDestroyCommandPool(device_, transferPool_, nullptr);
        if (allocator_) {
            vmaDestroyAllocator(allocator_);
        }
        vkDestroyDevice(device_, nullptr);
    }
    if (messenger_) {
        vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    }
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
    }
}

NativeHandles VulkanDevice::nativeHandles() const {
    NativeHandles h;
    h.backend = Backend::Vulkan;
    h.vkInstance = instance_;
    h.vkPhysicalDevice = physical_;
    h.vkDevice = device_;
    h.vkQueue = queue_;
    h.vkQueueFamily = queueFamily_;
    return h;
}

// --- frames -----------------------------------------------------------------------------------------

Result<void> VulkanDevice::waitFrame(uint32_t index) {
    Frame& f = frames_[index];
    if (f.serial == 0) {
        return {};
    }
    if (const VkResult r = vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX); r != VK_SUCCESS) {
        return std::unexpected(vkError("vkWaitForFences", r));
    }
    vkResetFences(device_, 1, &f.fence);
    completedSerial_ = std::max(completedSerial_, f.serial);
    f.serial = 0;
    return {};
}

ICommandList& VulkanDevice::beginFrame() {
    ENG_ASSERT_MSG(!recording_, "beginFrame while a frame is open");
    Frame& f = frames_[frameIndex_];
    if (auto r = waitFrame(frameIndex_); !r) {
        ENG_LOG_ERROR("{}", r.error().message);
    }
    runDeferred(false);
    vkResetCommandPool(device_, f.pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);
    cmdList_->begin(f.cmd);
    recording_ = true;
    return *cmdList_;
}

Result<void> VulkanDevice::submitAndWait() {
    if (deviceLost_) {
        return std::unexpected(Error{kExitDeviceLost, "vulkan: device lost"});
    }
    if (recording_) {
        Frame& f = frames_[frameIndex_];
        recording_ = false;
        if (const VkResult r = vkEndCommandBuffer(f.cmd); r != VK_SUCCESS) {
            return std::unexpected(vkError("vkEndCommandBuffer", r));
        }
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cmd;
        if (const VkResult r = vkQueueSubmit(queue_, 1, &si, f.fence); r != VK_SUCCESS) {
            return std::unexpected(vkError("vkQueueSubmit", r));
        }
        f.serial = nextSerial_++;
        const uint32_t submitted = frameIndex_;
        frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
        if (auto r = waitFrame(submitted); !r) {
            return r;
        }
    }
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (auto r = waitFrame(i); !r) {
            return r;
        }
    }
    runDeferred(false);
    if (validationErrors_.load() > 0) {
        return std::unexpected(validationError());
    }
    return {};
}

void VulkanDevice::destroy(Handle h) {
    // The object leaves its pool now (the handle is stale at once) and is destroyed when every
    // submission that could reference it has finished.
    const uint64_t serial = recording_ ? nextSerial_ : nextSerial_ - 1;
    std::function<void()> fn;
    switch (h.type) {
    case HandleType::Buffer:
        if (auto b = buffers_.remove(h.id)) {
            fn = [this, b = *b] { vmaDestroyBuffer(allocator_, b.buffer, b.allocation); };
        }
        break;
    case HandleType::Texture:
        if (auto t = textures_.remove(h.id)) {
            fn = [this, t = *t] {
                vkDestroyImageView(device_, t.view, nullptr);
                vmaDestroyImage(allocator_, t.image, t.allocation);
            };
        }
        break;
    case HandleType::Shader:
        if (auto s = shaders_.remove(h.id)) {
            fn = [this, m = s->module] { vkDestroyShaderModule(device_, m, nullptr); };
        }
        break;
    case HandleType::Pipeline:
        if (auto p = pipelines_.remove(h.id)) {
            fn = [this, p = *p] {
                vkDestroyPipeline(device_, p.pipeline, nullptr);
                vkDestroyPipelineLayout(device_, p.layout, nullptr);
            };
        }
        break;
    default: ENG_ASSERT_MSG(false, "destroy: handle type not supported by the Vulkan backend yet"); break;
    }
    if (fn) {
        deferred_.emplace_back(serial, std::move(fn));
    }
}

void VulkanDevice::runDeferred(bool all) {
    auto it = std::stable_partition(deferred_.begin(), deferred_.end(),
                                    [&](const auto& d) { return !all && d.first > completedSerial_; });
    for (auto i = it; i != deferred_.end(); ++i) {
        i->second();
    }
    deferred_.erase(it, deferred_.end());
}

// --- not yet (later plan tasks) ---------------------------------------------------------------------

Result<SamplerHandle> VulkanDevice::createSampler(const SamplerDesc&) {
    return std::unexpected(Error{kExitUsage, "vulkan: createSampler is implemented in plan F2 Task 3"});
}
Result<BindGroupHandle> VulkanDevice::createBindGroup(const BindGroupDesc&) {
    return std::unexpected(Error{kExitUsage, "vulkan: createBindGroup is implemented in plan F2 Task 3"});
}
Result<SwapchainHandle> VulkanDevice::createSwapchain(const SwapchainDesc&) {
    return std::unexpected(Error{kExitUsage, "vulkan: createSwapchain is implemented in plan F2 Task 6"});
}
Result<void> VulkanDevice::present(SwapchainHandle) {
    return std::unexpected(Error{kExitUsage, "vulkan: present is implemented in plan F2 Task 6"});
}

// --- readback ---------------------------------------------------------------------------------------

Result<Image> VulkanDevice::readback(TextureHandle handle) {
    if (recording_) {
        return std::unexpected(Error{kExitUsage, "vulkan: readback during an open frame (call submitAndWait first)"});
    }
    Texture* t = textures_.get(handle.id);
    if (!t) {
        return std::unexpected(Error{kExitUsage, "vulkan: readback of an invalid texture handle"});
    }
    const Format fmt = t->desc.format;
    if (fmt != Format::RGBA8_UNORM && fmt != Format::RGBA8_SRGB && fmt != Format::BGRA8_UNORM &&
        fmt != Format::BGRA8_SRGB) {
        return std::unexpected(Error{kExitUsage, std::string("vulkan: readback of ") + toString(fmt) + " not supported"});
    }
    if (t->state == ResourceState::Undefined) {
        return std::unexpected(Error{kExitUsage, "vulkan: readback of a texture that was never written (state Undefined)"});
    }
    if (!any(t->desc.usage, TextureUsage::CopySrc)) {
        return std::unexpected(Error{kExitUsage, "vulkan: readback needs TextureUsage::CopySrc"});
    }
    const uint32_t w = t->desc.width, h = t->desc.height;
    const VkDeviceSize size = VkDeviceSize(w) * h * 4u;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VmaAllocationInfo info{};
    if (const VkResult r = vmaCreateBuffer(allocator_, &bci, &aci, &staging, &alloc, &info); r != VK_SUCCESS) {
        return std::unexpected(vkError("vmaCreateBuffer (readback)", r));
    }

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = transferPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &ai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    const bool depth = isDepthFormat(fmt);
    const StateInfo from = stateInfo(t->state, depth);
    const StateInfo copy = stateInfo(ResourceState::CopySrc, depth);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (t->state != ResourceState::CopySrc) {
        b.oldLayout = from.layout;
        b.newLayout = copy.layout;
        b.srcAccessMask = from.access;
        b.dstAccessMask = copy.access;
        vkCmdPipelineBarrier(cmd, from.stage, copy.stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);
    VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hb.buffer = staging;
    hb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0,
                         nullptr);
    if (t->state != ResourceState::CopySrc) {
        // Back to the state the app's barriers expect.
        std::swap(b.oldLayout, b.newLayout);
        b.srcAccessMask = copy.access;
        b.dstAccessMask = from.access;
        vkCmdPipelineBarrier(cmd, copy.stage, from.stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VkResult r = vkQueueSubmit(queue_, 1, &si, transferFence_);
    if (r == VK_SUCCESS) {
        r = vkWaitForFences(device_, 1, &transferFence_, VK_TRUE, UINT64_MAX);
    }
    vkResetFences(device_, 1, &transferFence_);
    vkFreeCommandBuffers(device_, transferPool_, 1, &cmd);
    if (r != VK_SUCCESS) {
        vmaDestroyBuffer(allocator_, staging, alloc);
        return std::unexpected(vkError("readback submit", r));
    }
    vmaInvalidateAllocation(allocator_, alloc, 0, VK_WHOLE_SIZE);
    Image img;
    img.width = w;
    img.height = h;
    img.rgba8.resize(static_cast<size_t>(size));
    std::memcpy(img.rgba8.data(), info.pMappedData, img.rgba8.size());
    vmaDestroyBuffer(allocator_, staging, alloc);
    if (fmt == Format::BGRA8_UNORM || fmt == Format::BGRA8_SRGB) {
        for (size_t i = 0; i < img.rgba8.size(); i += 4) {
            std::swap(img.rgba8[i], img.rgba8[i + 2]);
        }
    }
    if (validationErrors_.load() > 0) {
        return std::unexpected(validationError());
    }
    return img;
}

} // namespace eng::rhi::vk
