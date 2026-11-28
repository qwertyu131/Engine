#pragma once
// D3D11 backend internals (plan F2 Task 2). Not part of the public RHI.
//   - the explicit RHI model is emulated: barriers are no-ops, the command list records straight into
//     the immediate context;
//   - every stage binds from its own reflection (D3D11 slots are per stage and permutation, F1);
//   - validation: D3D11_CREATE_DEVICE_DEBUG + ID3D11InfoQueue. The queue is drained after every call
//     that can produce messages; WARNING / ERROR / CORRUPTION are logged and counted.
#include "rhi/HandlePool.h"
#include "rhi/Rhi.h"

#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <memory>
#include <variant>
#include <utility>
#include <string>
#include <vector>

namespace eng::rhi::d3d11 {

using Microsoft::WRL::ComPtr;

struct Buffer {
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> srv;  // Storage: raw (ByteAddressBuffer, the SPIRV-Cross form)
    ComPtr<ID3D11UnorderedAccessView> uav; // Storage on FL11+: raw
    uint64_t size = 0;       // BufferDesc::size
    uint64_t allocBytes = 0; // D3D11_BUFFER_DESC::ByteWidth (uniform buffers: whole 256-byte blocks)
    BufferUsage usage = BufferUsage::None;
};

struct Sampler {
    ComPtr<ID3D11SamplerState> sampler;
};

// A bind group resolved to D3D11 slots: one entry per (stage, register class, slot) from each stage's own
// reflection (D3D11 slots are per stage and per permutation, F1).
struct BindGroup {
    struct Slot {
        uint32_t stage = kStageVertex;
        char registerClass = 'b';
        uint32_t slot = 0;
        uint32_t sizeBytes = 0; // cbuffer: reflected size
        std::variant<BufferBinding, TextureHandle, SamplerHandle> resource;
    };
    uint32_t space = 0;
    BindGroupLayout layout;
    std::vector<Slot> slots;
};

struct Texture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> srv;
    TextureDesc desc;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool swapchainImage = false; // a swapchain back buffer (plan F2 Task 6)
};

// DXGI flip-model swapchain (plan F2 Task 6, D3D11Swapchain.cpp). The buffers are UNORM (flip model
// rejects sRGB buffer formats); an sRGB SwapchainDesc::format gets an sRGB render-target view.
struct Swapchain {
    ComPtr<IDXGISwapChain1> swapchain;
    SwapchainDesc desc;         // width / height = the current buffer size (0 = minimised at creation)
    UINT flags = 0;             // DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING when supported (same on create / resize)
    bool tearing = false;
    TextureHandle backBuffer;   // null while the size is 0
};

struct Shader {
    ShaderStage stage = ShaderStage::Vertex;
    std::string entry;
    std::vector<uint8_t> code; // DXBC (the VS bytecode is needed for CreateInputLayout)
    ShaderReflection reflection;
    // Tier the shader model needs (plan F2 Task 7): SM 4.x = 0, SM 5.0 = 1. Above the device's tier the
    // blob is kept without a driver object (vs / ps null); createGraphicsPipeline rejects it.
    int requiredTier = 1;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
};

struct Pipeline {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11BlendState> blend;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    std::vector<uint32_t> strides; // per vertex buffer slot, from the pipeline's vertex layout
    std::vector<BindGroupLayout> bindGroupLayouts; // reflection-derived, per-stage registers
    uint32_t uavStartSlot = 0; // the PS reflection's renderTargets (F1: u slots start after the render targets)
    bool vsHasMorphStorage = false;
};

DXGI_FORMAT toDxgiFormat(Format f);
std::string hrString(long hr);
std::string narrow(const wchar_t* w);

class D3D11Device;

class D3D11CommandList final : public ICommandList {
public:
    explicit D3D11CommandList(D3D11Device& device) : dev_(device) {}
    void begin();

    void barrier(std::span<const TextureBarrier> barriers) override; // no-op (implicit API)
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
    static constexpr uint32_t kMaxVertexBuffers = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
    void flushVertexBuffers();

    D3D11Device& dev_;
    PipelineHandle pipeline_;
    bool inRenderPass_ = false;
    std::array<std::pair<BufferHandle, uint64_t>, kMaxVertexBuffers> vertexBuffers_{};
    bool vertexBuffersDirty_ = false;
};

class D3D11Device final : public IDevice {
public:
    static Result<std::unique_ptr<IDevice>> create(const DeviceDesc& desc);
    ~D3D11Device() override;

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
    uint32_t validationErrorCount() const override { return validationErrors_; }
    NativeHandles nativeHandles() const override;

    // --- internal (backend files) ---
    void drainMessages();         // D3D11Validation.cpp: info queue -> log + counter
    // An RHI-level validation error (D3D11Validation.cpp): logged and counted like a debug-layer ERROR, so
    // submitAndWait / present return exit 4 with it (plan F3 Task 1: setBindGroup layout check).
    void reportValidation(const std::string& message);
    Error validationError() const; // exit 4 with the first message
    Error hrError(const char* what, long hr); // exit 5 on device removal, 4 with validation messages, else 2

    ID3D11DeviceContext1* context() const { return context_.Get(); }
    HandlePool<Buffer>& buffers() { return buffers_; }
    HandlePool<Texture>& textures() { return textures_; }
    HandlePool<Pipeline>& pipelines() { return pipelines_; }
    HandlePool<BindGroup>& bindGroups() { return bindGroups_; }
    HandlePool<Sampler>& samplers() { return samplers_; }
    // Copies the transient allocations written since the last flush into the dynamic ring (Map
    // DISCARD on the first flush of a frame, NO_OVERWRITE after it) and unmaps it. Before every draw.
    void flushTransient();
    void resetTransient(); // beginFrame

private:
    D3D11Device() = default;
    Result<void> init(const DeviceDesc& desc);
    void queryCaps(D3D_FEATURE_LEVEL level); // plan F2 Task 7: limits, formats, features of caps_
    Result<void> wrapBackBuffer(Swapchain& sc); // D3D11Swapchain.cpp: back buffer 0 -> RHI texture

    DeviceCaps caps_;
    bool validation_ = false;
    ComPtr<IDXGIFactory1> factory_;
    ComPtr<ID3D11Device1> device_;
    ComPtr<ID3D11DeviceContext1> context_;
    ComPtr<ID3D11InfoQueue> infoQueue_;
    ComPtr<ID3D11Query> eventQuery_;
    std::unique_ptr<D3D11CommandList> cmdList_;
    bool recording_ = false;

    HandlePool<Buffer> buffers_;
    HandlePool<Texture> textures_;
    HandlePool<Shader> shaders_;
    HandlePool<Pipeline> pipelines_;
    HandlePool<Sampler> samplers_;
    HandlePool<BindGroup> bindGroups_;
    HandlePool<Swapchain> swapchains_;

    // Transient uniforms (plan F2 Task 4): D3D11_USAGE_DYNAMIC constant buffer ring + CPU shadow. allocTransient
    // hands out shadow memory; flushTransient() copies it into the ring before a draw.
    static constexpr uint64_t kTransientRingBytes = 4ull << 20;
    BufferHandle ring_;
    std::vector<std::byte> ringShadow_;
    uint64_t ringUsed_ = 0;     // bytes allocated this frame
    uint64_t ringFlushed_ = 0;  // bytes already copied into the ring this frame
    bool ringDiscarded_ = false; // the first Map of the frame (DISCARD) happened
    bool constantBufferOffsetting_ = false;    // D3D11_FEATURE_D3D11_OPTIONS
    bool mapNoOverwriteOnDynamicCb_ = false;   // D3D11_FEATURE_D3D11_OPTIONS

    uint32_t validationErrors_ = 0;
    std::string firstMessage_;
    bool deviceLost_ = false;
};

} // namespace eng::rhi::d3d11
