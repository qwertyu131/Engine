// Vulkan command recording (plan F2 Task 2): explicit image barriers, dynamic rendering,
// negative-height viewport (NDC +Y up, framebuffer origin top-left).
#include "VkCommon.h"

#include "core/Assert.h"

#include <algorithm>
#include <vector>

namespace eng::rhi::vk {

void VulkanCommandList::begin(VkCommandBuffer cmd) {
    cmd_ = cmd;
    pipeline_ = {};
    inRenderPass_ = false;
}

void VulkanCommandList::barrier(std::span<const TextureBarrier> barriers) {
    std::vector<VkImageMemoryBarrier> imgs;
    VkPipelineStageFlags src = 0, dst = 0;
    for (const TextureBarrier& tb : barriers) {
        Texture* t = dev_.textures().get(tb.texture.id);
        if (!t) {
            continue;
        }
        const bool depth = isDepthFormat(t->desc.format);
        StateInfo a = stateInfo(tb.before, depth);
        if (t->swapchainImage && (tb.before == ResourceState::Undefined || tb.before == ResourceState::Present)) {
            // Chains with the acquire-semaphore wait (COLOR_ATTACHMENT_OUTPUT, VulkanDevice::submitFrame):
            // a TOP_OF_PIPE source would let the layout transition run before the image is acquired.
            a.stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        }
        const StateInfo b = stateInfo(tb.after, depth);
        VkImageMemoryBarrier m{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        m.srcAccessMask = a.access;
        m.dstAccessMask = b.access;
        m.oldLayout = a.layout;
        m.newLayout = b.layout;
        m.srcQueueFamilyIndex = m.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        m.image = t->image;
        m.subresourceRange = {static_cast<VkImageAspectFlags>(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
                              0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        imgs.push_back(m);
        src |= a.stage;
        dst |= b.stage;
        t->state = tb.after;
    }
    if (!imgs.empty()) {
        vkCmdPipelineBarrier(cmd_, src, dst, 0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(imgs.size()), imgs.data());
    }
}

namespace {

VkAttachmentLoadOp toVk(LoadOp op) {
    switch (op) {
    case LoadOp::Load: return VK_ATTACHMENT_LOAD_OP_LOAD;
    case LoadOp::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case LoadOp::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

VkAttachmentStoreOp toVk(StoreOp op) {
    return op == StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

} // namespace

void VulkanCommandList::beginRenderPass(const RenderPassDesc& desc) {
    ENG_ASSERT_MSG(!inRenderPass_, "beginRenderPass inside a render pass");
    std::vector<VkRenderingAttachmentInfo> colors;
    VkExtent2D extent{0, 0};
    for (const ColorAttachment& c : desc.colors) {
        Texture* t = dev_.textures().get(c.texture.id);
        if (!t) {
            continue;
        }
        VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        a.imageView = t->view;
        if (!t->subViews.empty()) {
            ENG_ASSERT_MSG(c.mipLevel < t->desc.mipLevels && c.arrayLayer < t->desc.arrayLayers,
                           "beginRenderPass: attachment mip / layer out of range");
            a.imageView = t->subViews[size_t(c.arrayLayer) * t->desc.mipLevels + c.mipLevel];
        } else {
            ENG_ASSERT_MSG(c.mipLevel == 0 && c.arrayLayer == 0, "beginRenderPass: attachment mip / layer out of range");
        }
        a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp = toVk(c.load);
        a.storeOp = toVk(c.store);
        for (int i = 0; i < 4; ++i) {
            a.clearValue.color.float32[i] = c.clear[i];
        }
        colors.push_back(a);
        extent = {std::max(1u, t->desc.width >> c.mipLevel), std::max(1u, t->desc.height >> c.mipLevel)};
    }
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    const bool hasDepth = static_cast<bool>(desc.depth.texture);
    if (hasDepth) {
        if (Texture* t = dev_.textures().get(desc.depth.texture.id)) {
            depth.imageView = t->view;
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.loadOp = toVk(desc.depth.load);
            depth.storeOp = toVk(desc.depth.store);
            depth.clearValue.depthStencil = {desc.depth.clearDepth, 0};
            extent = {t->desc.width, t->desc.height};
        }
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = static_cast<uint32_t>(colors.size());
    ri.pColorAttachments = colors.data();
    ri.pDepthAttachment = hasDepth ? &depth : nullptr;
    dev_.cmdBeginRendering(cmd_, &ri);
    inRenderPass_ = true;
}

void VulkanCommandList::endRenderPass() {
    ENG_ASSERT_MSG(inRenderPass_, "endRenderPass without beginRenderPass");
    dev_.cmdEndRendering(cmd_);
    inRenderPass_ = false;
}

void VulkanCommandList::setPipeline(PipelineHandle pipeline) {
    Pipeline* p = dev_.pipelines().get(pipeline.id);
    if (!p) {
        return;
    }
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    pipeline_ = pipeline;
}

void VulkanCommandList::setBindGroup(uint32_t space, BindGroupHandle group) {
    // The descriptor set is written for the bound pipeline's layout: setPipeline comes first.
    BindGroup* g = dev_.bindGroups().get(group.id);
    const Pipeline* p = pipeline_ ? dev_.pipelines().tryGet(pipeline_.id) : nullptr;
    ENG_ASSERT_MSG(p != nullptr, "setBindGroup before setPipeline");
    if (!g || !p) {
        return;
    }
    ENG_ASSERT_MSG(g->space == space, "setBindGroup: the group was created for another space");
    const VkDescriptorSet set = dev_.descriptorSetFor(*g, *p);
    if (!set) {
        return; // logged; the draw then reaches the validation layer with the set missing (exit 4)
    }
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, space, 1, &set, 0, nullptr);
}

void VulkanCommandList::setVertexBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset) {
    Buffer* b = dev_.buffers().get(buffer.id);
    if (!b) {
        return;
    }
    const VkDeviceSize off = offset;
    vkCmdBindVertexBuffers(cmd_, slot, 1, &b->buffer, &off);
}

void VulkanCommandList::setIndexBuffer(BufferHandle buffer, IndexType type, uint64_t offset) {
    Buffer* b = dev_.buffers().get(buffer.id);
    if (!b) {
        return;
    }
    vkCmdBindIndexBuffer(cmd_, b->buffer, offset, type == IndexType::Uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void VulkanCommandList::setViewport(const Viewport& v) {
    // Negative height (VK_KHR_maintenance1, core 1.1): y' = y + height, height' = -height, so NDC +Y is up
    // and the framebuffer origin stays top-left, as in D3D.
    const VkViewport vp{v.x, v.y + v.height, v.width, -v.height, v.minDepth, v.maxDepth};
    vkCmdSetViewport(cmd_, 0, 1, &vp);
}

void VulkanCommandList::setScissor(const Rect& r) {
    const VkRect2D rect{{r.x, r.y}, {r.width, r.height}};
    vkCmdSetScissor(cmd_, 0, 1, &rect);
}

void VulkanCommandList::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
    // No pipeline check here on purpose: an invalid draw reaches the validation layer, which reports it
    // (exit 4) and skips the call (Review Focus 2).
    vkCmdDraw(cmd_, vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanCommandList::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t baseVertex,
                                    uint32_t firstInstance) {
    if (pipeline_) {
        if (const Pipeline* p = dev_.pipelines().tryGet(pipeline_.id)) {
            ENG_ASSERT_MSG(!p->vsHasMorphStorage || baseVertex == 0,
                           "morph meshes draw with base vertex 0 (SV_VertexID includes it on Vulkan)");
        }
    }
    vkCmdDrawIndexed(cmd_, indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
}

} // namespace eng::rhi::vk
