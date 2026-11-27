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

namespace eng::rhi {

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
    virtual void setBindGroup(uint32_t space, BindGroupHandle group) = 0; // plan F2 Task 3
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
    virtual Result<TextureHandle> createTexture(const TextureDesc& desc, std::span<const SubresourceData> init = {}) = 0;
    virtual Result<SamplerHandle> createSampler(const SamplerDesc& desc) = 0;
    virtual Result<ShaderHandle> createShader(const ShaderBlob& blob) = 0; // blob + parsed reflection
    virtual Result<PipelineHandle> createGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
    virtual Result<BindGroupHandle> createBindGroup(const BindGroupDesc& desc) = 0; // plan F2 Task 3
    virtual Result<SwapchainHandle> createSwapchain(const SwapchainDesc& desc) = 0; // plan F2 Task 6
    virtual void destroy(Handle handle) = 0; // deferred until the GPU is done with it
    virtual ICommandList& beginFrame() = 0;  // frames in flight = 2
    virtual Result<void> submitAndWait() = 0; // headless; exit 4 on validation errors, 5 on device loss
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
