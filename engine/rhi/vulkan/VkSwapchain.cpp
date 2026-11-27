// Vulkan swapchain (plan F2 Task 6): VK_KHR_win32_surface + VK_KHR_swapchain.
//   - present mode: FIFO with vsync; MAILBOX, else IMMEDIATE, else FIFO without;
//   - images registered as RHI textures (RenderTarget | CopySrc, so --screenshot can read the back buffer);
//   - one acquire semaphore per frame slot, one render-finished semaphore per image;
//   - a zero-size surface (minimised window) gives a swapchain without images: acquireNextImage returns a
//     null handle and present() only submits the frame.
#include <windows.h> // GetModuleHandleW, HWND

#include "VkCommon.h"

#include "core/Assert.h"
#include "core/Log.h"

#include <algorithm>
#include <format>

namespace eng::rhi::vk {

namespace {

VkFormat swapchainFormat(Format f) {
    switch (f) {
    case Format::BGRA8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case Format::BGRA8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

const char* presentModeName(VkPresentModeKHR m) {
    switch (m) {
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    default: return "other";
    }
}

} // namespace

Result<SwapchainHandle> VulkanDevice::createSwapchain(const SwapchainDesc& desc) {
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    if (!swapchainExtension_) {
        return std::unexpected(Error{kExitNoBackend, "vulkan: VK_KHR_surface / VK_KHR_win32_surface / VK_KHR_swapchain "
                                                     "not available"});
    }
    if (!desc.nativeWindow) {
        return std::unexpected(Error{kExitUsage, "vulkan: createSwapchain without a native window"});
    }
    Swapchain sc;
    sc.desc = desc;
    sc.format = swapchainFormat(desc.format);
    if (sc.format == VK_FORMAT_UNDEFINED) {
        return std::unexpected(Error{kExitUsage, std::string("vulkan: swapchain format ") + toString(desc.format) +
                                                     " not supported (RGBA8 / BGRA8 UNORM or SRGB)"});
    }
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = static_cast<HWND>(desc.nativeWindow);
    if (const VkResult r = vkCreateWin32SurfaceKHR(instance_, &sci, nullptr, &sc.surface); r != VK_SUCCESS) {
        return std::unexpected(vkError("vkCreateWin32SurfaceKHR", r));
    }
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physical_, queueFamily_, sc.surface, &supported);
    if (!supported) {
        vkDestroySurfaceKHR(instance_, sc.surface, nullptr);
        return std::unexpected(Error{kExitNoBackend, "vulkan: the graphics queue cannot present to this window"});
    }
    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, sc.surface, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, sc.surface, &fn, formats.data());
    const bool formatOk = std::any_of(formats.begin(), formats.end(), [&](const VkSurfaceFormatKHR& f) {
        return f.format == sc.format && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    if (!formatOk) {
        vkDestroySurfaceKHR(instance_, sc.surface, nullptr);
        return std::unexpected(Error{kExitNoBackend, std::string("vulkan: surface does not support ") +
                                                         toString(desc.format) + " / SRGB_NONLINEAR"});
    }
    for (VkSemaphore& s : sc.acquired) {
        VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device_, &ci, nullptr, &s);
    }
    if (auto r = buildSwapchain(sc, desc.width, desc.height); !r) {
        destroySwapchain(sc);
        return std::unexpected(r.error());
    }
    return SwapchainHandle{swapchains_.add(std::move(sc))};
#else
    (void)desc;
    return std::unexpected(Error{kExitNoBackend, "vulkan: no window surface support on this platform yet"});
#endif
}

void VulkanDevice::releaseSwapchainImages(Swapchain& sc) {
    for (TextureHandle h : sc.images) {
        if (auto t = textures_.remove(h.id)) {
            vkDestroyImageView(device_, t->view, nullptr);
        }
    }
    sc.images.clear();
    for (VkSemaphore s : sc.renderFinished) {
        vkDestroySemaphore(device_, s, nullptr);
    }
    sc.renderFinished.clear();
}

void VulkanDevice::destroySwapchain(Swapchain& sc) {
    releaseSwapchainImages(sc);
    if (sc.swapchain) {
        vkDestroySwapchainKHR(device_, sc.swapchain, nullptr);
        sc.swapchain = VK_NULL_HANDLE;
    }
    for (VkSemaphore& s : sc.acquired) {
        if (s) {
            vkDestroySemaphore(device_, s, nullptr);
            s = VK_NULL_HANDLE;
        }
    }
    if (sc.surface) {
        vkDestroySurfaceKHR(instance_, sc.surface, nullptr);
        sc.surface = VK_NULL_HANDLE;
    }
}

Result<void> VulkanDevice::buildSwapchain(Swapchain& sc, uint32_t width, uint32_t height) {
    ENG_ASSERT_MSG(sc.acquiredIndex < 0, "swapchain rebuilt while an image is acquired");
    // Every frame that could use the old images or semaphores is finished first (resize is rare).
    vkDeviceWaitIdle(device_);
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        (void)waitFrame(i);
    }
    runDeferred(false);
    releaseSwapchainImages(sc);
    // The acquire semaphores may be signalled by an acquire that was never waited on (out of date): fresh ones.
    sc.pendingWait = VK_NULL_HANDLE;
    for (VkSemaphore& s : sc.acquired) {
        vkDestroySemaphore(device_, s, nullptr);
        VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device_, &ci, nullptr, &s);
    }

    VkSurfaceCapabilitiesKHR caps{};
    if (const VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, sc.surface, &caps); r != VK_SUCCESS) {
        return std::unexpected(vkError("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", r));
    }
    VkExtent2D extent{width, height};
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    sc.outOfDate = false;
    if (extent.width == 0 || extent.height == 0) {
        // Minimised: no images until a resize to a non-zero size.
        if (sc.swapchain) {
            vkDestroySwapchainKHR(device_, sc.swapchain, nullptr);
            sc.swapchain = VK_NULL_HANDLE;
        }
        sc.desc.width = sc.desc.height = 0;
        return {};
    }

    uint32_t pn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, sc.surface, &pn, nullptr);
    std::vector<VkPresentModeKHR> modes(pn);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, sc.surface, &pn, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!sc.desc.vsync) {
        mode = has(VK_PRESENT_MODE_MAILBOX_KHR)     ? VK_PRESENT_MODE_MAILBOX_KHR
               : has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                    : VK_PRESENT_MODE_FIFO_KHR;
    }
    uint32_t count = std::max(sc.desc.imageCount, caps.minImageCount);
    if (caps.maxImageCount > 0) {
        count = std::min(count, caps.maxImageCount);
    }
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const bool copySrc = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (copySrc) {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = sc.surface;
    ci.minImageCount = count;
    ci.imageFormat = sc.format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = usage;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = sc.swapchain;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    const VkResult r = vkCreateSwapchainKHR(device_, &ci, nullptr, &created);
    if (sc.swapchain) {
        vkDestroySwapchainKHR(device_, sc.swapchain, nullptr);
        sc.swapchain = VK_NULL_HANDLE;
    }
    if (r != VK_SUCCESS) {
        return std::unexpected(vkError("vkCreateSwapchainKHR", r));
    }
    sc.swapchain = created;
    sc.desc.width = extent.width;
    sc.desc.height = extent.height;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(device_, sc.swapchain, &n, nullptr);
    std::vector<VkImage> images(n);
    vkGetSwapchainImagesKHR(device_, sc.swapchain, &n, images.data());
    for (VkImage img : images) {
        Texture t;
        t.image = img;
        t.format = sc.format;
        t.swapchainImage = true;
        t.desc.width = extent.width;
        t.desc.height = extent.height;
        t.desc.format = sc.desc.format;
        t.desc.usage = TextureUsage::RenderTarget | (copySrc ? TextureUsage::CopySrc : TextureUsage::None);
        t.desc.debugName = "swapchain image";
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = sc.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (const VkResult vr = vkCreateImageView(device_, &vci, nullptr, &t.view); vr != VK_SUCCESS) {
            return std::unexpected(vkError("vkCreateImageView (swapchain)", vr));
        }
        sc.images.push_back(TextureHandle{textures_.add(std::move(t))});
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore sem = VK_NULL_HANDLE;
        vkCreateSemaphore(device_, &sci, nullptr, &sem);
        sc.renderFinished.push_back(sem);
    }
    ENG_LOG_INFO("vulkan: swapchain {}x{}, {} images, {}, present mode {}", extent.width, extent.height, n,
                 toString(sc.desc.format), presentModeName(mode));
    if (validationErrors_.load() > 0) {
        return std::unexpected(validationError());
    }
    return {};
}

Result<TextureHandle> VulkanDevice::acquireNextImage(SwapchainHandle handle) {
    ENG_ASSERT_MSG(recording_, "acquireNextImage outside beginFrame / present");
    Swapchain* sc = swapchains_.get(handle.id);
    if (!sc) {
        return std::unexpected(Error{kExitUsage, "vulkan: acquireNextImage with an invalid swapchain"});
    }
    if (sc->acquiredIndex >= 0) {
        return sc->images[static_cast<size_t>(sc->acquiredIndex)];
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (sc->outOfDate || (!sc->swapchain && sc->desc.width > 0)) {
            // Rebuild at the surface's current size (only between frames on the GPU side: waits idle).
            if (auto r = buildSwapchain(*sc, sc->desc.width, sc->desc.height); !r) {
                return std::unexpected(r.error());
            }
        }
        if (!sc->swapchain) {
            return TextureHandle{};
        }
        VkSemaphore sem = sc->acquired[frameIndex_];
        uint32_t index = 0;
        const VkResult r = vkAcquireNextImageKHR(device_, sc->swapchain, UINT64_MAX, sem, VK_NULL_HANDLE, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            sc->outOfDate = true;
            continue;
        }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            return std::unexpected(vkError("vkAcquireNextImageKHR", r));
        }
        if (r == VK_SUBOPTIMAL_KHR) {
            sc->outOfDate = true; // rebuilt after this frame's present
        }
        sc->acquiredIndex = static_cast<int32_t>(index);
        sc->pendingWait = sem;
        return sc->images[index];
    }
    return TextureHandle{}; // still out of date (window changing size): skip this frame
}

Result<void> VulkanDevice::resizeSwapchain(SwapchainHandle handle, uint32_t width, uint32_t height) {
    ENG_ASSERT_MSG(!recording_, "resizeSwapchain inside a frame");
    Swapchain* sc = swapchains_.get(handle.id);
    if (!sc) {
        return std::unexpected(Error{kExitUsage, "vulkan: resizeSwapchain with an invalid swapchain"});
    }
    if (width == 0 || height == 0) {
        return {}; // minimised: nothing to do, rendering skips
    }
    if (sc->swapchain && !sc->outOfDate && sc->desc.width == width && sc->desc.height == height) {
        return {};
    }
    return buildSwapchain(*sc, width, height);
}

Result<void> VulkanDevice::setSwapchainVsync(SwapchainHandle handle, bool vsync) {
    ENG_ASSERT_MSG(!recording_, "setSwapchainVsync inside a frame");
    Swapchain* sc = swapchains_.get(handle.id);
    if (!sc) {
        return std::unexpected(Error{kExitUsage, "vulkan: setSwapchainVsync with an invalid swapchain"});
    }
    if (sc->desc.vsync == vsync) {
        return {};
    }
    sc->desc.vsync = vsync;
    if (!sc->swapchain) {
        return {};
    }
    return buildSwapchain(*sc, sc->desc.width, sc->desc.height);
}

Result<void> VulkanDevice::present(SwapchainHandle handle) {
    if (deviceLost_) {
        return std::unexpected(Error{kExitDeviceLost, "vulkan: device lost"});
    }
    if (!recording_) {
        return std::unexpected(Error{kExitUsage, "vulkan: present without beginFrame"});
    }
    Swapchain* sc = swapchains_.get(handle.id);
    if (!sc) {
        return std::unexpected(Error{kExitUsage, "vulkan: present with an invalid swapchain"});
    }
    const int32_t index = sc->acquiredIndex;
    VkSemaphore signal = index >= 0 ? sc->renderFinished[static_cast<size_t>(index)] : VK_NULL_HANDLE;
    if (auto r = submitFrame(signal); !r) {
        return r;
    }
    if (index >= 0) {
        sc->acquiredIndex = -1;
        const auto idx = static_cast<uint32_t>(index);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &signal;
        pi.swapchainCount = 1;
        pi.pSwapchains = &sc->swapchain;
        pi.pImageIndices = &idx;
        const VkResult r = vkQueuePresentKHR(queue_, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            sc->outOfDate = true;
        } else if (r != VK_SUCCESS) {
            return std::unexpected(vkError("vkQueuePresentKHR", r));
        }
    }
    if (validationErrors_.load() > 0) {
        return std::unexpected(validationError());
    }
    return {};
}

} // namespace eng::rhi::vk
