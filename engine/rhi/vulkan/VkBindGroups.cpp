// Vulkan samplers, pipeline layouts, bind groups, transient uniforms (plan F2 Task 3).
//
//   - Pipeline layout: one VkDescriptorSetLayout per space 0..max used space (set index = space, empty
//     layouts for unused spaces), bindings from each stage's reflection (spirv set / binding), stage
//     flags merged across the stages.
//   - Bind group: validated and resolved at createBindGroup; its VkDescriptorSet is allocated from the
//     current frame's descriptor pool and written when it is bound (cached for the rest of the frame).
//     The pools of a frame slot are reset when that slot is reused (after its fence), so a persistent
//     bind group never holds a set the GPU may still read.
//   - allocTransient: one host-visible, mapped uniform ring per frame in flight, reset with the frame.
#include "VkCommon.h"

#include "core/Log.h"

#include <algorithm>
#include <format>

namespace eng::rhi::vk {

namespace {

VkSamplerAddressMode toVk(AddressMode m) {
    switch (m) {
    case AddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case AddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case AddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

VkFilter toVk(Filter f) {
    return f == Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

// The spirv binding of a layout entry (the stages agree: one SPIR-V reflection source per shader).
const StageBinding* bindingOf(const BindGroupLayoutEntry& e) {
    for (const StageBinding& s : e.stages) {
        if (s.used) {
            return &s;
        }
    }
    return nullptr;
}

bool sameLayout(const BindGroupLayout& a, const BindGroupLayout& b) {
    if (a.space != b.space || a.entries.size() != b.entries.size()) {
        return false;
    }
    for (size_t i = 0; i < a.entries.size(); ++i) {
        const BindGroupLayoutEntry& x = a.entries[i];
        const BindGroupLayoutEntry& y = b.entries[i];
        if (x.name != y.name || x.kind != y.kind || x.arraySize != y.arraySize ||
            bindingOf(x)->binding != bindingOf(y)->binding) {
            return false;
        }
    }
    return true;
}

} // namespace

Result<SamplerHandle> VulkanDevice::createSampler(const SamplerDesc& desc) {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = toVk(desc.magFilter);
    ci.minFilter = toVk(desc.minFilter);
    ci.mipmapMode = desc.mipFilter == Filter::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ci.addressModeU = toVk(desc.addressU);
    ci.addressModeV = toVk(desc.addressV);
    ci.addressModeW = toVk(desc.addressW);
    ci.maxLod = VK_LOD_CLAMP_NONE;
    Sampler s;
    if (const VkResult r = vkCreateSampler(device_, &ci, nullptr, &s.sampler); r != VK_SUCCESS) {
        return std::unexpected(vkError("vkCreateSampler", r));
    }
    return SamplerHandle{samplers_.add(s)};
}

Result<void> VulkanDevice::createPipelineLayout(Pipeline& p) {
    const uint32_t setCount = p.bindGroupLayouts.empty() ? 0u : p.bindGroupLayouts.back().space + 1u;
    auto fail = [&](Error e) -> Result<void> {
        for (VkDescriptorSetLayout l : p.setLayouts) {
            vkDestroyDescriptorSetLayout(device_, l, nullptr);
        }
        p.setLayouts.clear();
        return std::unexpected(std::move(e));
    };
    for (uint32_t space = 0; space < setCount; ++space) {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        const auto it = std::find_if(p.bindGroupLayouts.begin(), p.bindGroupLayouts.end(),
                                     [&](const BindGroupLayout& l) { return l.space == space; });
        if (it != p.bindGroupLayouts.end()) {
            for (const BindGroupLayoutEntry& e : it->entries) {
                VkDescriptorSetLayoutBinding b{};
                VkShaderStageFlags stages = 0;
                for (uint32_t s = 0; s < kGraphicsStages; ++s) {
                    const StageBinding& sb = e.stages[s];
                    if (!sb.used) {
                        continue;
                    }
                    if (sb.set != space) {
                        return fail(Error{kExitUsage, std::format("vulkan: resource {} of space {} is in descriptor set "
                                                                  "{} (binding convention: set = space)",
                                                                  e.name, space, sb.set)});
                    }
                    if (stages != 0 && sb.binding != b.binding) {
                        return fail(Error{kExitUsage, std::format("vulkan: resource {} has different bindings in the "
                                                                  "vertex and fragment shader",
                                                                  e.name)});
                    }
                    b.binding = sb.binding;
                    stages |= s == kStageVertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
                }
                b.descriptorType = toVkDescriptorType(e.kind);
                b.descriptorCount = e.arraySize;
                b.stageFlags = stages;
                bindings.push_back(b);
            }
        }
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = static_cast<uint32_t>(bindings.size());
        ci.pBindings = bindings.data();
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        if (const VkResult r = vkCreateDescriptorSetLayout(device_, &ci, nullptr, &layout); r != VK_SUCCESS) {
            return fail(vkError("vkCreateDescriptorSetLayout", r));
        }
        p.setLayouts.push_back(layout);
    }
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = static_cast<uint32_t>(p.setLayouts.size());
    lci.pSetLayouts = p.setLayouts.data();
    if (const VkResult r = vkCreatePipelineLayout(device_, &lci, nullptr, &p.layout); r != VK_SUCCESS) {
        return fail(vkError("vkCreatePipelineLayout", r));
    }
    return {};
}

Result<BindGroupHandle> VulkanDevice::createBindGroup(const BindGroupDesc& desc) {
    const Pipeline* p = pipelines_.tryGet(desc.pipeline.id);
    if (!p) {
        return std::unexpected(Error{kExitUsage, "vulkan: createBindGroup with an invalid pipeline handle"});
    }
    const auto lit = std::find_if(p->bindGroupLayouts.begin(), p->bindGroupLayouts.end(),
                                  [&](const BindGroupLayout& l) { return l.space == desc.space; });
    const BindGroupLayout empty{desc.space, {}};
    const BindGroupLayout& layout = lit != p->bindGroupLayouts.end() ? *lit : empty;
    auto match = matchBindGroupEntries(layout, desc);
    if (!match) {
        return std::unexpected(match.error());
    }
    if (layout.entries.empty()) {
        return std::unexpected(
            Error{kExitUsage, std::format("bind group space {}: the pipeline has no resources in this space", desc.space)});
    }
    BindGroup g;
    g.space = desc.space;
    g.layout = layout;
    for (size_t i = 0; i < layout.entries.size(); ++i) {
        const BindGroupLayoutEntry& e = layout.entries[i];
        const auto& res = desc.entries[(*match)[i]].resource;
        if (e.arraySize != 1) {
            return std::unexpected(Error{kExitUsage, std::format("bind group space {}: {} is an array (not supported "
                                                                 "by the F2 RHI)",
                                                                 desc.space, e.name)});
        }
        if (const auto* bb = std::get_if<BufferBinding>(&res)) {
            const Buffer* b = buffers_.tryGet(bb->buffer.id);
            const BufferUsage need = e.kind == BindingKind::UniformBuffer ? BufferUsage::Uniform : BufferUsage::Storage;
            if (!b || !any(b->usage, need)) {
                return std::unexpected(Error{kExitUsage, std::format("bind group space {} entry {}: invalid buffer or "
                                                                     "buffer without {} usage",
                                                                     desc.space, e.name, bindingKindName(e.kind))});
            }
            if (bb->offset % minUniformOffsetAlignment_ != 0 || bb->offset >= b->size) {
                return std::unexpected(Error{kExitUsage, std::format("bind group space {} entry {}: offset {} not "
                                                                     "aligned to {} or past the buffer",
                                                                     desc.space, e.name, bb->offset,
                                                                     minUniformOffsetAlignment_)});
            }
        } else if (const auto* th = std::get_if<TextureHandle>(&res)) {
            const Texture* t = textures_.tryGet(th->id);
            if (!t || !any(t->desc.usage, TextureUsage::Sampled)) {
                return std::unexpected(Error{kExitUsage, std::format("bind group space {} entry {}: invalid texture "
                                                                     "or texture without Sampled usage",
                                                                     desc.space, e.name)});
            }
        } else if (const auto* sh = std::get_if<SamplerHandle>(&res); !samplers_.tryGet(sh->id)) {
            return std::unexpected(
                Error{kExitUsage, std::format("bind group space {} entry {}: invalid sampler", desc.space, e.name)});
        }
        g.resources.push_back(res);
    }
    return BindGroupHandle{bindGroups_.add(std::move(g))};
}

VkDescriptorPool VulkanDevice::newDescriptorPool() {
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1024},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1024},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512},
    };
    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets = 512;
    ci.poolSizeCount = static_cast<uint32_t>(std::size(sizes));
    ci.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateDescriptorPool(device_, &ci, nullptr, &pool); r != VK_SUCCESS) {
        ENG_LOG_ERROR("{}", vkError("vkCreateDescriptorPool", r).message);
        return VK_NULL_HANDLE;
    }
    return pool;
}

VkDescriptorSet VulkanDevice::descriptorSetFor(BindGroup& g, const Pipeline& p) {
    ENG_ASSERT_MSG(recording_, "setBindGroup outside beginFrame / submit");
    if (g.space >= p.setLayouts.size()) {
        ENG_LOG_ERROR("vulkan: setBindGroup space {}: the bound pipeline has no resources in this space", g.space);
        return VK_NULL_HANDLE;
    }
    const auto lit = std::find_if(p.bindGroupLayouts.begin(), p.bindGroupLayouts.end(),
                                  [&](const BindGroupLayout& l) { return l.space == g.space; });
    if (lit == p.bindGroupLayouts.end() || !sameLayout(*lit, g.layout)) {
        ENG_LOG_ERROR("vulkan: setBindGroup space {}: the bind group's layout differs from the bound pipeline's",
                      g.space);
        return VK_NULL_HANDLE;
    }
    const VkDescriptorSetLayout setLayout = p.setLayouts[g.space];
    if (g.cachedEpoch == frameEpoch_ && g.cachedSetLayout == setLayout && g.cachedSet) {
        return g.cachedSet;
    }

    Frame& f = frames_[frameIndex_];
    VkDescriptorSet set = VK_NULL_HANDLE;
    for (;;) {
        if (f.descriptorPoolInUse == f.descriptorPools.size()) {
            const VkDescriptorPool pool = newDescriptorPool();
            if (!pool) {
                return VK_NULL_HANDLE;
            }
            f.descriptorPools.push_back(pool);
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = f.descriptorPools[f.descriptorPoolInUse];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &setLayout;
        const VkResult r = vkAllocateDescriptorSets(device_, &ai, &set);
        if (r == VK_SUCCESS) {
            break;
        }
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
            ENG_LOG_ERROR("{}", vkError("vkAllocateDescriptorSets", r).message);
            return VK_NULL_HANDLE;
        }
        ++f.descriptorPoolInUse; // this pool is full: next (or a new) one
    }

    std::vector<VkWriteDescriptorSet> writes(g.layout.entries.size());
    std::vector<VkDescriptorBufferInfo> bufferInfos(g.layout.entries.size());
    std::vector<VkDescriptorImageInfo> imageInfos(g.layout.entries.size());
    for (size_t i = 0; i < g.layout.entries.size(); ++i) {
        const BindGroupLayoutEntry& e = g.layout.entries[i];
        VkWriteDescriptorSet& w = writes[i];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = bindingOf(e)->binding;
        w.descriptorCount = 1;
        w.descriptorType = toVkDescriptorType(e.kind);
        const auto& res = g.resources[i];
        if (const auto* bb = std::get_if<BufferBinding>(&res)) {
            const Buffer* b = buffers_.tryGet(bb->buffer.id);
            if (!b) {
                ENG_LOG_ERROR("vulkan: setBindGroup space {}: buffer of {} was destroyed", g.space, e.name);
                return VK_NULL_HANDLE;
            }
            bufferInfos[i] = {b->buffer, bb->offset, bb->size ? bb->size : b->size - bb->offset};
            w.pBufferInfo = &bufferInfos[i];
        } else if (const auto* th = std::get_if<TextureHandle>(&res)) {
            const Texture* t = textures_.tryGet(th->id);
            if (!t) {
                ENG_LOG_ERROR("vulkan: setBindGroup space {}: texture {} was destroyed", g.space, e.name);
                return VK_NULL_HANDLE;
            }
            imageInfos[i].imageView = t->view;
            imageInfos[i].imageLayout = stateInfo(ResourceState::ShaderRead, isDepthFormat(t->desc.format)).layout;
            w.pImageInfo = &imageInfos[i];
        } else {
            const Sampler* s = samplers_.tryGet(std::get<SamplerHandle>(res).id);
            if (!s) {
                ENG_LOG_ERROR("vulkan: setBindGroup space {}: sampler {} was destroyed", g.space, e.name);
                return VK_NULL_HANDLE;
            }
            imageInfos[i].sampler = s->sampler;
            w.pImageInfo = &imageInfos[i];
        }
    }
    vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    g.cachedEpoch = frameEpoch_;
    g.cachedSetLayout = setLayout;
    g.cachedSet = set;
    return set;
}

BufferBinding VulkanDevice::allocTransient(uint64_t size, uint64_t align) {
    if (!recording_) {
        ENG_LOG_ERROR("vulkan: allocTransient outside beginFrame / submit");
        return {};
    }
    Frame& f = frames_[frameIndex_];
    if (!f.ring) {
        BufferDesc bd;
        bd.size = kTransientRingBytes;
        bd.usage = BufferUsage::Uniform;
        bd.hostVisible = true;
        bd.debugName = std::format("transient ring {}", frameIndex_);
        auto ring = createBuffer(bd, {});
        if (!ring) {
            ENG_LOG_ERROR("vulkan: allocTransient: {}", ring.error().message);
            return {};
        }
        f.ring = *ring;
    }
    Buffer* ring = buffers_.get(f.ring.id);
    const uint64_t a = std::max<uint64_t>({align, 256u, minUniformOffsetAlignment_});
    const uint64_t offset = (f.ringUsed + a - 1) / a * a;
    if (size == 0 || offset + size > ring->size) {
        ENG_LOG_ERROR("vulkan: allocTransient({}) does not fit the {}-byte frame ring", size, ring->size);
        return {};
    }
    f.ringUsed = offset + size;
    return BufferBinding{f.ring, offset, size, static_cast<std::byte*>(ring->mapped) + offset};
}

} // namespace eng::rhi::vk
