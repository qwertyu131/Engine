// Vulkan resources: buffers, textures, samplers, shaders, graphics pipelines (plan F2 Task 2), bind groups,
// staging uploads, transient uniforms (plan F2 Task 3).
#include "VkCommon.h"

#include "core/Log.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace eng::rhi::vk {

VkFormat toVkFormat(Format f) {
    switch (f) {
    case Format::Unknown: return VK_FORMAT_UNDEFINED;
    case Format::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::BGRA8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::BGRA8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case Format::RGBA16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::RGBA32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::D32_FLOAT: return VK_FORMAT_D32_SFLOAT;
    case Format::D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
    case Format::R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case Format::RG32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case Format::RGB32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
    case Format::R32_UINT: return VK_FORMAT_R32_UINT;
    case Format::RG32_UINT: return VK_FORMAT_R32G32_UINT;
    case Format::RGB32_UINT: return VK_FORMAT_R32G32B32_UINT;
    case Format::RGBA32_UINT: return VK_FORMAT_R32G32B32A32_UINT;
    case Format::R32_SINT: return VK_FORMAT_R32_SINT;
    case Format::RGBA32_SINT: return VK_FORMAT_R32G32B32A32_SINT;
    case Format::RGBA16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case Format::RGBA8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}

bool isDepthFormat(Format f) {
    return f == Format::D32_FLOAT || f == Format::D24_UNORM_S8_UINT;
}

VkDescriptorType toVkDescriptorType(BindingKind k) {
    switch (k) {
    case BindingKind::UniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case BindingKind::Texture: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case BindingKind::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
    case BindingKind::StorageBuffer:
    case BindingKind::StorageBufferRW: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

StateInfo stateInfo(ResourceState s, bool depth) {
    switch (s) {
    case ResourceState::Undefined: return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0};
    case ResourceState::RenderTarget:
        return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    case ResourceState::DepthWrite:
        return {VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    case ResourceState::ShaderRead:
        return {depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
    case ResourceState::CopySrc:
        return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    case ResourceState::CopyDst:
        return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
    case ResourceState::Present: return {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0};
    }
    return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0};
}

Result<BufferHandle> VulkanDevice::createBuffer(const BufferDesc& desc, std::span<const std::byte> init) {
    if (desc.size == 0) {
        return std::unexpected(Error{kExitUsage, "vulkan: createBuffer with size 0"});
    }
    if (init.size() > desc.size) {
        return std::unexpected(Error{kExitUsage, "vulkan: createBuffer: initial data larger than the buffer"});
    }
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = desc.size;
    if (any(desc.usage, BufferUsage::Vertex)) {
        bci.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if (any(desc.usage, BufferUsage::Index)) {
        bci.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (any(desc.usage, BufferUsage::Uniform)) {
        bci.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if (any(desc.usage, BufferUsage::Storage)) {
        bci.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if (any(desc.usage, BufferUsage::CopySrc)) {
        bci.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if (any(desc.usage, BufferUsage::CopyDst)) {
        bci.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    // hostVisible: mapped, initial data = memcpy. Otherwise device-local; initial data goes through a
    // staging buffer (plan F2 Task 3).
    VmaAllocationCreateInfo aci{};
    if (desc.hostVisible) {
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    } else {
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (!init.empty()) {
            bci.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        }
    }
    Buffer b;
    b.size = desc.size;
    b.usage = desc.usage;
    VmaAllocationInfo info{};
    if (const VkResult r = vmaCreateBuffer(allocator_, &bci, &aci, &b.buffer, &b.allocation, &info); r != VK_SUCCESS) {
        return std::unexpected(vkError("vmaCreateBuffer", r));
    }
    b.mapped = desc.hostVisible ? info.pMappedData : nullptr;
    if (!init.empty()) {
        if (b.mapped) {
            std::memcpy(b.mapped, init.data(), init.size());
            vmaFlushAllocation(allocator_, b.allocation, 0, init.size());
        } else if (auto r = uploadBuffer(b, init); !r) {
            vmaDestroyBuffer(allocator_, b.buffer, b.allocation);
            return std::unexpected(r.error());
        }
    }
    if (!desc.debugName.empty() && vkSetDebugUtilsObjectNameEXT) {
        VkDebugUtilsObjectNameInfoEXT ni{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
        ni.objectType = VK_OBJECT_TYPE_BUFFER;
        ni.objectHandle = reinterpret_cast<uint64_t>(b.buffer);
        ni.pObjectName = desc.debugName.c_str();
        vkSetDebugUtilsObjectNameEXT(device_, &ni);
    }
    return BufferHandle{buffers_.add(b)};
}

Result<TextureHandle> VulkanDevice::createTexture(const TextureDesc& desc, std::span<const SubresourceData> init) {
    if (desc.width == 0 || desc.height == 0 || desc.mipLevels == 0 || desc.arrayLayers == 0) {
        return std::unexpected(Error{kExitUsage, "vulkan: createTexture with a zero dimension"});
    }
    Texture t;
    t.desc = desc;
    t.format = toVkFormat(desc.format);
    if (t.format == VK_FORMAT_UNDEFINED) {
        return std::unexpected(Error{kExitUsage, "vulkan: createTexture with format Unknown"});
    }
    const bool depth = isDepthFormat(desc.format);
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = t.format;
    ici.extent = {desc.width, desc.height, 1};
    ici.mipLevels = desc.mipLevels;
    ici.arrayLayers = desc.arrayLayers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ici.flags = desc.cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    if (any(desc.usage, TextureUsage::Sampled)) {
        ici.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (any(desc.usage, TextureUsage::RenderTarget)) {
        ici.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (any(desc.usage, TextureUsage::DepthStencil)) {
        ici.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (any(desc.usage, TextureUsage::Storage)) {
        ici.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (any(desc.usage, TextureUsage::CopySrc)) {
        ici.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (any(desc.usage, TextureUsage::CopyDst) || !init.empty()) {
        ici.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (!init.empty()) {
        if (init.size() != size_t(desc.mipLevels) * desc.arrayLayers) {
            return std::unexpected(Error{kExitUsage, std::format("vulkan: createTexture: {} initial subresources, the "
                                                                 "texture has {} (mips x layers)",
                                                                 init.size(), desc.mipLevels * desc.arrayLayers)});
        }
        if (depth) {
            return std::unexpected(Error{kExitUsage, "vulkan: createTexture: initial data for a depth format"});
        }
    }
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (const VkResult r = vmaCreateImage(allocator_, &ici, &aci, &t.image, &t.allocation, nullptr); r != VK_SUCCESS) {
        return std::unexpected(vkError("vmaCreateImage", r));
    }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = t.image;
    vci.viewType = desc.cube ? VK_IMAGE_VIEW_TYPE_CUBE
                             : (desc.arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
    vci.format = t.format;
    vci.subresourceRange = {static_cast<VkImageAspectFlags>(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
                            0, desc.mipLevels, 0, desc.arrayLayers};
    if (const VkResult r = vkCreateImageView(device_, &vci, nullptr, &t.view); r != VK_SUCCESS) {
        vmaDestroyImage(allocator_, t.image, t.allocation);
        return std::unexpected(vkError("vkCreateImageView", r));
    }
    if (!desc.debugName.empty() && vkSetDebugUtilsObjectNameEXT) {
        VkDebugUtilsObjectNameInfoEXT ni{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
        ni.objectType = VK_OBJECT_TYPE_IMAGE;
        ni.objectHandle = reinterpret_cast<uint64_t>(t.image);
        ni.pObjectName = desc.debugName.c_str();
        vkSetDebugUtilsObjectNameEXT(device_, &ni);
    }
    if (!init.empty()) {
        if (auto r = uploadTexture(t, init); !r) { // VkUpload.cpp; leaves the texture in ShaderRead
            vkDestroyImageView(device_, t.view, nullptr);
            vmaDestroyImage(allocator_, t.image, t.allocation);
            return std::unexpected(r.error());
        }
    }
    return TextureHandle{textures_.add(t)};
}

Result<ShaderHandle> VulkanDevice::createShader(const ShaderBlob& blob) {
    if (blob.backend != Backend::Vulkan) {
        return std::unexpected(Error{kExitUsage, "vulkan: createShader with a " + std::string(toString(blob.backend)) +
                                                     " blob (" + blob.entry + ")"});
    }
    if (blob.code.size() < 20 || blob.code.size() % 4 != 0) {
        return std::unexpected(Error{kExitUsage, "vulkan: createShader: not a SPIR-V module (" + blob.entry + ")"});
    }
    std::vector<uint32_t> words(blob.code.size() / 4);
    std::memcpy(words.data(), blob.code.data(), blob.code.size());
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = blob.code.size();
    ci.pCode = words.data();
    Shader s;
    s.stage = blob.stage;
    s.entry = blob.entry;
    s.reflection = blob.reflection;
    if (const VkResult r = vkCreateShaderModule(device_, &ci, nullptr, &s.module); r != VK_SUCCESS) {
        return std::unexpected(vkError("vkCreateShaderModule", r));
    }
    return ShaderHandle{shaders_.add(std::move(s))};
}

namespace {

VkCompareOp toVk(CompareOp c) {
    switch (c) {
    case CompareOp::Never: return VK_COMPARE_OP_NEVER;
    case CompareOp::Less: return VK_COMPARE_OP_LESS;
    case CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
    case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
    case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

VkPrimitiveTopology toVk(PrimitiveTopology t) {
    switch (t) {
    case PrimitiveTopology::TriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PrimitiveTopology::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PrimitiveTopology::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

bool isMorphStorage(const ReflectedResource& r) {
    return (r.kind == "storage_ro" || r.kind == "storage_rw") && r.name.rfind("morph", 0) == 0;
}

} // namespace

Result<PipelineHandle> VulkanDevice::createGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    Shader* vs = shaders_.tryGet(desc.vs.id);
    Shader* ps = shaders_.tryGet(desc.ps.id);
    if (!vs || !ps || vs->stage != ShaderStage::Vertex || ps->stage != ShaderStage::Fragment) {
        return std::unexpected(Error{kExitUsage, "vulkan: createGraphicsPipeline needs a vertex and a fragment shader"});
    }
    // Bind group layouts from the reflection of both stages (plan F2 Task 3).
    auto bgl = deriveBindGroupLayouts(vs->reflection, ps->reflection);
    if (!bgl) {
        return std::unexpected(Error{bgl.error().exitCode, "vulkan: " + bgl.error().message});
    }

    // Vertex input: every VS input (reflection, by HLSL semantic) must be fed by an attribute.
    std::vector<VkVertexInputBindingDescription> bindings;
    for (uint32_t i = 0; i < desc.vertexLayout.buffers.size(); ++i) {
        const VertexBufferLayout& b = desc.vertexLayout.buffers[i];
        bindings.push_back({i, b.stride, b.perInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
    }
    std::vector<VkVertexInputAttributeDescription> attrs;
    for (const ReflectedVertexInput& in : vs->reflection.vertexInputs) {
        const auto it = std::find_if(desc.vertexLayout.attributes.begin(), desc.vertexLayout.attributes.end(),
                                     [&](const VertexAttribute& a) { return a.semantic == in.semantic; });
        if (it == desc.vertexLayout.attributes.end()) {
            return std::unexpected(
                Error{kExitUsage, "vulkan: vertex layout has no attribute for VS input " + in.semantic});
        }
        if (it->buffer >= bindings.size()) {
            return std::unexpected(Error{kExitUsage, "vulkan: attribute " + it->semantic + " uses vertex buffer " +
                                                         std::to_string(it->buffer) + ", layout has " +
                                                         std::to_string(bindings.size())});
        }
        attrs.push_back({in.location, it->buffer, toVkFormat(it->format), it->offset});
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
    vi.pVertexBindingDescriptions = bindings.data();
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs->module;
    stages[0].pName = vs->entry.c_str();
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps->module;
    stages[1].pName = ps->entry.c_str();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = toVk(desc.topology);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = desc.raster.cull == CullMode::None    ? VK_CULL_MODE_NONE
                  : desc.raster.cull == CullMode::Front ? VK_CULL_MODE_FRONT_BIT
                                                        : VK_CULL_MODE_BACK_BIT;
    // Framebuffer space (origin top-left): with the negative-height viewport a triangle that is
    // clockwise on screen is VK_FRONT_FACE_CLOCKWISE, as D3D's FrontCounterClockwise = FALSE.
    rs.frontFace = desc.raster.frontFace == FrontFace::Clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = desc.depth.test;
    ds.depthWriteEnable = desc.depth.write;
    ds.depthCompareOp = toVk(desc.depth.compare);
    std::vector<VkPipelineColorBlendAttachmentState> blend(desc.colorFormats.size());
    for (size_t i = 0; i < blend.size(); ++i) {
        VkPipelineColorBlendAttachmentState& b = blend[i];
        b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
        if (i < desc.blend.size() && desc.blend[i].enable) {
            b.blendEnable = VK_TRUE;
            b.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            b.colorBlendOp = VK_BLEND_OP_ADD;
            b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            b.alphaBlendOp = VK_BLEND_OP_ADD;
        }
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = static_cast<uint32_t>(blend.size());
    cb.pAttachments = blend.data();
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    std::vector<VkFormat> colorFormats;
    for (Format f : desc.colorFormats) {
        colorFormats.push_back(toVkFormat(f));
    }
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = static_cast<uint32_t>(colorFormats.size());
    ri.pColorAttachmentFormats = colorFormats.data();
    ri.depthAttachmentFormat = toVkFormat(desc.depthFormat);

    Pipeline p;
    p.bindGroupLayouts = std::move(*bgl);
    if (auto l = createPipelineLayout(p); !l) { // VkBindGroups.cpp
        return std::unexpected(l.error());
    }
    VkGraphicsPipelineCreateInfo gci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gci.pNext = &ri;
    gci.stageCount = 2;
    gci.pStages = stages;
    gci.pVertexInputState = &vi;
    gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vp;
    gci.pRasterizationState = &rs;
    gci.pMultisampleState = &ms;
    gci.pDepthStencilState = &ds;
    gci.pColorBlendState = &cb;
    gci.pDynamicState = &dy;
    gci.layout = p.layout;
    if (const VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gci, nullptr, &p.pipeline);
        r != VK_SUCCESS) {
        vkDestroyPipelineLayout(device_, p.layout, nullptr);
        for (VkDescriptorSetLayout l : p.setLayouts) {
            vkDestroyDescriptorSetLayout(device_, l, nullptr);
        }
        return std::unexpected(vkError("vkCreateGraphicsPipelines", r));
    }
    p.vsHasMorphStorage = std::any_of(vs->reflection.resources.begin(), vs->reflection.resources.end(), isMorphStorage);
    return PipelineHandle{pipelines_.add(p)};
}

} // namespace eng::rhi::vk
