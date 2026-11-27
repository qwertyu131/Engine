// Vulkan staging uploads (plan F2 Task 3): device-local buffers and textures (all mips / layers) get
// their initial data through a host-visible staging buffer and a one-shot submission. Uploaded textures
// end in ResourceState::ShaderRead; the barriers make the copies visible to every later submission.
#include "VkCommon.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace eng::rhi::vk {

namespace {

struct Staging {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void* mapped = nullptr;
};

} // namespace

Result<void> VulkanDevice::uploadBuffer(const Buffer& dst, std::span<const std::byte> data) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = data.size();
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    Staging st;
    VmaAllocationInfo info{};
    if (const VkResult r = vmaCreateBuffer(allocator_, &bci, &aci, &st.buffer, &st.allocation, &info); r != VK_SUCCESS) {
        return std::unexpected(vkError("vmaCreateBuffer (staging)", r));
    }
    std::memcpy(info.pMappedData, data.data(), data.size());
    vmaFlushAllocation(allocator_, st.allocation, 0, VK_WHOLE_SIZE);
    auto r = immediateSubmit([&](VkCommandBuffer cmd) {
        const VkBufferCopy region{0, 0, data.size()};
        vkCmdCopyBuffer(cmd, st.buffer, dst.buffer, 1, &region);
        // Visible to every later use (vertex / index / uniform / storage access) in later submissions.
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
    });
    vmaDestroyBuffer(allocator_, st.buffer, st.allocation);
    return r;
}

Result<void> VulkanDevice::uploadTexture(Texture& dst, std::span<const SubresourceData> init) {
    const TextureDesc& d = dst.desc;
    const uint32_t bpp = formatBytes(d.format);
    // Tightly packed copy of every subresource, in SubresourceData order (layer-major, then mip).
    std::vector<VkBufferImageCopy> regions;
    VkDeviceSize total = 0;
    for (uint32_t layer = 0; layer < d.arrayLayers; ++layer) {
        for (uint32_t mip = 0; mip < d.mipLevels; ++mip) {
            const uint32_t w = std::max(1u, d.width >> mip), h = std::max(1u, d.height >> mip);
            const SubresourceData& sd = init[size_t(layer) * d.mipLevels + mip];
            const uint32_t pitch = sd.rowPitch ? sd.rowPitch : w * bpp;
            if (pitch < w * bpp || sd.data.size() < size_t(pitch) * (h - 1) + size_t(w) * bpp) {
                return std::unexpected(Error{kExitUsage, std::format("vulkan: createTexture: initial data of layer {} "
                                                                     "mip {} is too small ({} bytes for {}x{})",
                                                                     layer, mip, sd.data.size(), w, h)});
            }
            VkBufferImageCopy region{};
            region.bufferOffset = total;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1};
            region.imageExtent = {w, h, 1};
            regions.push_back(region);
            total += VkDeviceSize(w) * h * bpp;
        }
    }
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = total;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    Staging st;
    VmaAllocationInfo info{};
    if (const VkResult r = vmaCreateBuffer(allocator_, &bci, &aci, &st.buffer, &st.allocation, &info); r != VK_SUCCESS) {
        return std::unexpected(vkError("vmaCreateBuffer (texture staging)", r));
    }
    auto* out = static_cast<std::byte*>(info.pMappedData);
    size_t i = 0;
    for (uint32_t layer = 0; layer < d.arrayLayers; ++layer) {
        for (uint32_t mip = 0; mip < d.mipLevels; ++mip, ++i) {
            const uint32_t w = std::max(1u, d.width >> mip), h = std::max(1u, d.height >> mip);
            const SubresourceData& sd = init[i];
            const uint32_t pitch = sd.rowPitch ? sd.rowPitch : w * bpp;
            for (uint32_t y = 0; y < h; ++y) {
                std::memcpy(out + regions[i].bufferOffset + size_t(y) * w * bpp, sd.data.data() + size_t(y) * pitch,
                            size_t(w) * bpp);
            }
        }
    }
    vmaFlushAllocation(allocator_, st.allocation, 0, VK_WHOLE_SIZE);
    auto r = immediateSubmit([&](VkCommandBuffer cmd) {
        const StateInfo copy = stateInfo(ResourceState::CopyDst, false);
        const StateInfo read = stateInfo(ResourceState::ShaderRead, false);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = dst.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, d.mipLevels, 0, d.arrayLayers};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = copy.layout;
        b.srcAccessMask = 0;
        b.dstAccessMask = copy.access;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, copy.stage, 0, 0, nullptr, 0, nullptr, 1, &b);
        vkCmdCopyBufferToImage(cmd, st.buffer, dst.image, copy.layout, static_cast<uint32_t>(regions.size()),
                               regions.data());
        b.oldLayout = copy.layout;
        b.newLayout = read.layout;
        b.srcAccessMask = copy.access;
        b.dstAccessMask = read.access;
        vkCmdPipelineBarrier(cmd, copy.stage, read.stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    });
    vmaDestroyBuffer(allocator_, st.buffer, st.allocation);
    if (r) {
        dst.state = ResourceState::ShaderRead;
    }
    return r;
}

} // namespace eng::rhi::vk
