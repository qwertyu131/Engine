#pragma once
// Vulkan backend internals (plan F2 Task 2). Not part of the public RHI.
//   - volk (instance / device function loading) + VMA (memory);
//   - Vulkan 1.2+ with dynamic rendering (1.3 core or VK_KHR_dynamic_rendering), no VkRenderPass;
//   - negative-height viewport: NDC +Y up, framebuffer origin top-left (RHI convention);
//   - validation: VK_LAYER_KHRONOS_validation + synchronization validation (VK_EXT_layer_settings),
//     messages of severity warning or above are logged and counted (VkValidation.cpp).
#include "rhi/HandlePool.h"
#include "rhi/Rhi.h"

#include <volk.h>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <vk_mem_alloc.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

namespace eng::rhi::vk {

inline constexpr uint32_t kFramesInFlight = 2;

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    uint64_t size = 0;
    BufferUsage usage = BufferUsage::None;
    void* mapped = nullptr;
};

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkImageView view = VK_NULL_HANDLE; // full-resource view (render target / sampling)
    TextureDesc desc;
    VkFormat format = VK_FORMAT_UNDEFINED;
    ResourceState state = ResourceState::Undefined; // last state recorded by barrier() (record order)
    bool swapchainImage = false; // image owned by a VkSwapchainKHR: only the view is ours (plan F2 Task 6)
};

// A VkSwapchainKHR + its images registered as textures (plan F2 Task 6, VkSwapchain.cpp).
struct Swapchain {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    SwapchainDesc desc;                         // width / height = the current extent (0 = no images)
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    std::vector<TextureHandle> images;
    std::vector<VkSemaphore> renderFinished;    // per image (signalled by the frame's submit, waited by present)
    VkSemaphore acquired[kFramesInFlight] = {}; // per frame slot (signalled by vkAcquireNextImageKHR)
    int32_t acquiredIndex = -1;                 // image acquired and not yet presented
    VkSemaphore pendingWait = VK_NULL_HANDLE;   // acquire semaphore the next submit must wait on
    bool outOfDate = false;
};

struct Shader {
    VkShaderModule module = VK_NULL_HANDLE;
    ShaderStage stage = ShaderStage::Vertex;
    std::string entry;
    ShaderReflection reflection;
};

struct Pipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    // One VkDescriptorSetLayout per space 0..max used space (empty layouts for the gaps); index = space.
    std::vector<VkDescriptorSetLayout> setLayouts;
    std::vector<BindGroupLayout> bindGroupLayouts; // reflection-derived, sorted by space
    bool vsHasMorphStorage = false; // drawIndexed asserts baseVertex == 0 (F1 Limitations)
};

struct Sampler {
    VkSampler sampler = VK_NULL_HANDLE;
};

// A persistent bind group: the resolved resources. Its VkDescriptorSet is allocated and written from
// the current frame's descriptor pool when it is bound (cached for the rest of that frame).
struct BindGroup {
    uint32_t space = 0;
    BindGroupLayout layout;
    std::vector<std::variant<BufferBinding, TextureHandle, SamplerHandle>> resources; // per layout entry
    uint64_t cachedEpoch = 0;
    VkDescriptorSetLayout cachedSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet cachedSet = VK_NULL_HANDLE;
};

// Layout / stage / access of a ResourceState (explicit barriers, sync validation clean).
struct StateInfo {
    VkImageLayout layout;
    VkPipelineStageFlags stage;
    VkAccessFlags access;
};
StateInfo stateInfo(ResourceState s, bool depth);

// Messenger create info that routes to VulkanDevice::onValidationMessage (VkValidation.cpp).
VkDebugUtilsMessengerCreateInfoEXT messengerCreateInfo(void* device);

VkFormat toVkFormat(Format f);
bool isDepthFormat(Format f);
VkDescriptorType toVkDescriptorType(BindingKind k);
std::string vkResultString(VkResult r);

class VulkanDevice;

class VulkanCommandList final : public ICommandList {
public:
    explicit VulkanCommandList(VulkanDevice& device) : dev_(device) {}
    void begin(VkCommandBuffer cmd);

    void barrier(std::span<const TextureBarrier> barriers) override;
    void beginRenderPass(const RenderPassDesc& desc) override;
    void endRenderPass() override;
    void setPipeline(PipelineHandle pipeline) override;
    void setBindGroup(uint32_t space, BindGroupHandle group) override;
    void setVertexBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset) override;
    void setIndexBuffer(BufferHandle buffer, IndexType type, uint64_t offset) override;
    void setViewport(const Viewport& viewport) override;
    void setScissor(const Rect& rect) override;
    void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override;
    void drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t baseVertex,
                     uint32_t firstInstance) override;

private:
    VulkanDevice& dev_;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    PipelineHandle pipeline_;
    bool inRenderPass_ = false;
};

class VulkanDevice final : public IDevice {
public:
    static Result<std::unique_ptr<IDevice>> create(const DeviceDesc& desc);
    ~VulkanDevice() override;

    const DeviceCaps& caps() const override { return caps_; }
    Result<BufferHandle> createBuffer(const BufferDesc& desc, std::span<const std::byte> init) override;
    Result<TextureHandle> createTexture(const TextureDesc& desc, std::span<const SubresourceData> init) override;
    Result<SamplerHandle> createSampler(const SamplerDesc& desc) override;
    Result<ShaderHandle> createShader(const ShaderBlob& blob) override;
    Result<PipelineHandle> createGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
    Result<BindGroupHandle> createBindGroup(const BindGroupDesc& desc) override;
    BufferBinding allocTransient(uint64_t size, uint64_t align) override;
    Result<SwapchainHandle> createSwapchain(const SwapchainDesc& desc) override;
    Result<TextureHandle> acquireNextImage(SwapchainHandle swapchain) override;
    Result<void> resizeSwapchain(SwapchainHandle swapchain, uint32_t width, uint32_t height) override;
    Result<void> setSwapchainVsync(SwapchainHandle swapchain, bool vsync) override;
    void destroy(Handle handle) override;
    ICommandList& beginFrame() override;
    Result<void> submitAndWait() override;
    Result<void> present(SwapchainHandle swapchain) override;
    Result<Image> readback(TextureHandle texture) override;
    uint32_t validationErrorCount() const override { return validationErrors_.load(); }
    NativeHandles nativeHandles() const override;

    // --- internal (backend files) ---
    void onValidationMessage(bool error, const char* message); // VkValidation.cpp
    Error validationError() const;                              // exit 4 with the first message
    Error vkError(const char* what, VkResult r);                // exit 5 on device loss, else 4 / 2

    VkDevice device() const { return device_; }
    HandlePool<Buffer>& buffers() { return buffers_; }
    HandlePool<Texture>& textures() { return textures_; }
    HandlePool<Pipeline>& pipelines() { return pipelines_; }
    HandlePool<BindGroup>& bindGroups() { return bindGroups_; }
    // The group's descriptor set for `pipeline`'s layout in the open frame (VkResources.cpp);
    // VK_NULL_HANDLE (logged) if the layouts differ or a resource is gone.
    VkDescriptorSet descriptorSetFor(BindGroup& group, const Pipeline& pipeline);
    PFN_vkCmdBeginRendering cmdBeginRendering = nullptr;
    PFN_vkCmdEndRendering cmdEndRendering = nullptr;

private:
    VulkanDevice() = default;
    Result<void> init(const DeviceDesc& desc);
    Result<void> createInstance(const DeviceDesc& desc);
    Result<void> pickPhysicalDevice(const DeviceDesc& desc);
    void queryCaps(const VkPhysicalDeviceProperties& props); // plan F2 Task 7: limits, formats, features of caps_
    Result<void> createLogicalDevice();
    void runDeferred(bool all);
    Result<void> waitFrame(uint32_t index);
    // Records `record` into a one-shot command buffer, submits it and waits (staging uploads).
    Result<void> immediateSubmit(const std::function<void(VkCommandBuffer)>& record);
    Result<void> uploadBuffer(const Buffer& dst, std::span<const std::byte> data);
    Result<void> uploadTexture(Texture& dst, std::span<const SubresourceData> init);
    Result<void> createPipelineLayout(Pipeline& p); // set layouts per space + VkPipelineLayout into p
    // VkSwapchain.cpp
    Result<void> buildSwapchain(Swapchain& sc, uint32_t width, uint32_t height); // (re)creates images
    void releaseSwapchainImages(Swapchain& sc);
    void destroySwapchain(Swapchain& sc);
    // Ends the open frame's command buffer and submits it, waiting on every pending acquire semaphore and
    // signalling `signal` (VK_NULL_HANDLE = none). Advances the frame slot.
    Result<void> submitFrame(VkSemaphore signal);

    static constexpr uint64_t kTransientRingBytes = 4ull << 20; // per frame in flight

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        uint64_t serial = 0; // submission serial of the last submit from this frame; 0 = none pending
        std::vector<VkDescriptorPool> descriptorPools; // reset at beginFrame; [0] is filled first
        size_t descriptorPoolInUse = 0;
        BufferHandle ring;      // transient uniform ring (allocTransient), host-visible + mapped
        uint64_t ringUsed = 0;
    };
    VkDescriptorPool newDescriptorPool();

    DeviceCaps caps_;
    bool validation_ = false;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    uint32_t apiVersion_ = 0;
    bool khrDynamicRendering_ = false;
    bool surfaceExtensions_ = false;   // VK_KHR_surface + VK_KHR_win32_surface enabled on the instance
    bool swapchainExtension_ = false;  // VK_KHR_swapchain enabled on the device
    bool maintenance5_ = false;        // VK_KHR_maintenance5 enabled: point lists without a PointSize write
    VmaAllocator allocator_ = nullptr;

    Frame frames_[kFramesInFlight];
    uint32_t frameIndex_ = 0;
    bool recording_ = false;
    uint64_t nextSerial_ = 1;      // serial of the next submission
    uint64_t completedSerial_ = 0; // every submission <= this has finished on the GPU
    std::unique_ptr<VulkanCommandList> cmdList_;

    // Readback: own pool / buffer / fence (outside the frames).
    VkCommandPool transferPool_ = VK_NULL_HANDLE;
    VkFence transferFence_ = VK_NULL_HANDLE;

    HandlePool<Buffer> buffers_;
    HandlePool<Texture> textures_;
    HandlePool<Shader> shaders_;
    HandlePool<Pipeline> pipelines_;
    HandlePool<Sampler> samplers_;
    HandlePool<BindGroup> bindGroups_;
    HandlePool<Swapchain> swapchains_;
    uint64_t frameEpoch_ = 0; // incremented by every beginFrame (bind group descriptor-set cache key)
    VkDeviceSize minUniformOffsetAlignment_ = 256;
    std::vector<std::pair<uint64_t, std::function<void()>>> deferred_; // (serial, destroy)

    std::atomic<uint32_t> validationErrors_{0};
    mutable std::mutex messageMutex_;
    std::string firstMessage_;
    bool deviceLost_ = false;
};

} // namespace eng::rhi::vk
