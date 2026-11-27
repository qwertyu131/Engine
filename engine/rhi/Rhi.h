#pragma once
// Backend-independent render hardware interface (plan F2 Task 2; spec §5.3).
//
// Explicit model (D3D12 / Vulkan / Metal style): command lists, pipeline state objects, explicit texture
// barriers. D3D11 emulates it (barriers are no-ops). One IDevice per backend instance; it is owned by the
// render thread (no internal locking).
//
// Validation (plan F2 Global Constraints): with DeviceDesc::validation (default on) the backend enables
// VK_LAYER_KHRONOS_validation with synchronization validation, or D3D11_CREATE_DEVICE_DEBUG +
// ID3D11InfoQueue. Every message of severity warning or above is logged through eng::log and counted
// (validationErrorCount()); submitAndWait() then fails with exit code 4. A missing validation layer is
// an error at createDevice (exit 4, "validation layer not available: <name>"), never a silent skip.
#include "DeviceCaps.h"
#include "Types.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace eng::rhi {

// --- bind group layouts (plan F2 Task 3) ----------------------------------------------------------
// Derived from the reflection of a pipeline's shaders: one layout per binding-convention space that a
// stage uses, resources merged across the stages by name. Each stage keeps its own target binding,
// because D3D11 slots are per stage and per permutation (F1); Vulkan uses set / binding.

enum class BindingKind : uint8_t { UniformBuffer, Texture, Sampler, StorageBuffer, StorageBufferRW };
const char* bindingKindName(BindingKind k); // "uniform buffer" | "texture" | "sampler" | "storage buffer" | ...

inline constexpr uint32_t kStageVertex = 0;
inline constexpr uint32_t kStageFragment = 1;
inline constexpr uint32_t kGraphicsStages = 2;

struct StageBinding {
    bool used = false;
    uint32_t set = 0, binding = 0;   // spirv targetBinding
    char registerClass = 0;          // dxbc targetBinding: 'b' | 't' | 's' | 'u'
    uint32_t registerSlot = 0;
};

struct BindGroupLayoutEntry {
    std::string name;
    BindingKind kind = BindingKind::UniformBuffer;
    uint32_t index = 0;     // register index within the space (binding convention)
    uint32_t arraySize = 1;
    uint32_t sizeBytes = 0; // cbuffer size (reflection), else 0
    StageBinding stages[kGraphicsStages]; // [kStageVertex], [kStageFragment]
};

struct BindGroupLayout {
    uint32_t space = 0;
    std::vector<BindGroupLayoutEntry> entries; // sorted by index
};

// Layouts of every space used by vs or ps, sorted by space. Error{2} if the stages disagree about a
// resource (same name, different kind / space / index), or a resource is an unbounded array.
Result<std::vector<BindGroupLayout>> deriveBindGroupLayouts(const ShaderReflection& vs, const ShaderReflection& ps);

// Matches desc.entries to layout.entries by name. Returns, per layout entry, the index of its entry in
// desc.entries. Errors (exit 2): "bind group space <S> missing <name>", "bind group space <S> has no
// resource <name>", "bind group space <S> entry <name> must be a <kind>".
Result<std::vector<size_t>> matchBindGroupEntries(const BindGroupLayout& layout, const BindGroupDesc& desc);

struct DeviceDesc {
    Backend backend = Backend::Vulkan;
    bool validation = true;
    bool warp = false;            // D3D11: WARP software rasterizer
    int adapterIndex = -1;        // -1 = default adapter (Vulkan: first discrete GPU, else the first device)
    int d3d11FeatureLevelCap = 0; // 0 = highest; 0xa000 = FL10_0 (plan F2 Task 7)
};

class ICommandList {
public:
    virtual ~ICommandList() = default;
    virtual void barrier(std::span<const TextureBarrier> barriers) = 0; // D3D11: no-op
    virtual void beginRenderPass(const RenderPassDesc& desc) = 0;       // color/depth attachments, load/store, clear
    virtual void endRenderPass() = 0;
    virtual void setPipeline(PipelineHandle pipeline) = 0;
    // After setPipeline: the group is bound for the bound pipeline's layout of `space` (must equal the group's).
    virtual void setBindGroup(uint32_t space, BindGroupHandle group) = 0; // plan F2 Task 3 (Vulkan), Task 4 (D3D11)
    virtual void setVertexBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset) = 0;
    virtual void setIndexBuffer(BufferHandle buffer, IndexType type, uint64_t offset) = 0;
    virtual void setViewport(const Viewport& viewport) = 0;
    virtual void setScissor(const Rect& rect) = 0;
    virtual void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) = 0;
    // Asserts baseVertex == 0 when the bound pipeline's VS reflects a morph storage buffer (Vulkan's
    // SV_VertexID includes the base vertex, D3D's does not; F1 Limitations).
    virtual void drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t baseVertex,
                             uint32_t firstInstance) = 0;
};

class IDevice {
public:
    virtual ~IDevice() = default;
    virtual const DeviceCaps& caps() const = 0;
    virtual Result<BufferHandle> createBuffer(const BufferDesc& desc, std::span<const std::byte> init = {}) = 0;
    // With init (every subresource, SubresourceData order) the texture is uploaded through a staging
    // buffer and is in ResourceState::ShaderRead afterwards; without it, in Undefined.
    virtual Result<TextureHandle> createTexture(const TextureDesc& desc, std::span<const SubresourceData> init = {}) = 0;
    virtual Result<SamplerHandle> createSampler(const SamplerDesc& desc) = 0;
    virtual Result<ShaderHandle> createShader(const ShaderBlob& blob) = 0; // blob + parsed reflection
    virtual Result<PipelineHandle> createGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
    // Resolves desc.entries against the pipeline's layout for desc.space (matchBindGroupEntries). The group
    // is persistent; the backend writes its descriptors when it is bound (Vulkan: per-frame pools).
    virtual Result<BindGroupHandle> createBindGroup(const BindGroupDesc& desc) = 0; // plan F2 Task 3
    // Per-frame ring of host-visible uniform memory (plan F2 Task 3). Only between beginFrame() and
    // submitAndWait() / present(): the range is valid for the frame that allocated it. The caller writes
    // `size` bytes to BufferBinding::cpu before its next command-list call (D3D11 unmaps the ring before
    // a draw). align is rounded up to 256 bytes. Returns a null binding (buffer 0) if the frame's ring
    // is full or no frame is open (logged).
    virtual BufferBinding allocTransient(uint64_t size, uint64_t align = 256) = 0;
    // Swapchains (plan F2 Task 6). createSwapchain with width or height 0 gives a swapchain without
    // images (minimised window); acquireNextImage then returns a null handle until a resize.
    virtual Result<SwapchainHandle> createSwapchain(const SwapchainDesc& desc) = 0; // plan F2 Task 6
    // Inside a frame (after beginFrame): the back buffer to draw into this frame (state Undefined on the first
    // acquire of an image; barrier it to RenderTarget, then to Present before present()). Returns the same
    // image until present(). A null handle = nothing to draw (zero size): still call present() to end the
    // frame. An out-of-date Vulkan swapchain is recreated here at its current size.
    virtual Result<TextureHandle> acquireNextImage(SwapchainHandle swapchain) = 0;
    // Between frames. width or height 0 does nothing (minimised window: rendering skips meanwhile).
    virtual Result<void> resizeSwapchain(SwapchainHandle swapchain, uint32_t width, uint32_t height) = 0;
    // Vulkan: FIFO when on; MAILBOX, else IMMEDIATE when off (recreates the swapchain). D3D11: present
    // interval 1 / 0 (+ DXGI_PRESENT_ALLOW_TEARING when supported).
    virtual Result<void> setSwapchainVsync(SwapchainHandle swapchain, bool vsync) = 0;
    virtual void destroy(Handle handle) = 0; // deferred until the GPU is done with it
    virtual ICommandList& beginFrame() = 0;  // frames in flight = 2
    virtual Result<void> submitAndWait() = 0; // headless; exit 4 on validation errors, 5 on device loss
    // Ends the frame: submits it (without waiting for the GPU; frames in flight = 2) and presents the
    // acquired image, if any. Exit 4 on validation errors, 5 on device loss.
    virtual Result<void> present(SwapchainHandle swapchain) = 0; // plan F2 Task 6
    virtual Result<Image> readback(TextureHandle texture) = 0; // RGBA8, rows top-down
    virtual uint32_t validationErrorCount() const = 0;
    virtual NativeHandles nativeHandles() const = 0; // spec §5.3 getNativeHandle
};

// Exit 3 if the backend is not compiled into this build or not available on this machine.
Result<std::unique_ptr<IDevice>> createDevice(const DeviceDesc& desc);

// Backends compiled into this build.
std::span<const Backend> compiledBackends();

} // namespace eng::rhi
