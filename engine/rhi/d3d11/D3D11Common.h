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
#include <utility>
#include <string>
#include <vector>

namespace eng::rhi::d3d11 {

using Microsoft::WRL::ComPtr;

struct Buffer {
    ComPtr<ID3D11Buffer> buffer;
    uint64_t size = 0;
    BufferUsage usage = BufferUsage::None;
};

struct Texture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> srv;
    TextureDesc desc;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct Shader {
    ShaderStage stage = ShaderStage::Vertex;
    std::string entry;
    std::vector<uint8_t> code; // DXBC (the VS bytecode is needed for CreateInputLayout)
    ShaderReflection reflection;
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
    Result<SwapchainHandle> createSwapchain(const SwapchainDesc& desc) override;
    void destroy(Handle handle) override;
    ICommandList& beginFrame() override;
    Result<void> submitAndWait() override;
    Result<void> present(SwapchainHandle swapchain) override;
    Result<Image> readback(TextureHandle texture) override;
    uint32_t validationErrorCount() const override { return validationErrors_; }
    NativeHandles nativeHandles() const override;

    // --- internal (backend files) ---
    void drainMessages();         // D3D11Validation.cpp: info queue -> log + counter
    Error validationError() const; // exit 4 with the first message
    Error hrError(const char* what, long hr); // exit 5 on device removal, 4 with validation messages, else 2

    ID3D11DeviceContext1* context() const { return context_.Get(); }
    HandlePool<Buffer>& buffers() { return buffers_; }
    HandlePool<Texture>& textures() { return textures_; }
    HandlePool<Pipeline>& pipelines() { return pipelines_; }

private:
    D3D11Device() = default;
    Result<void> init(const DeviceDesc& desc);

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

    uint32_t validationErrors_ = 0;
    std::string firstMessage_;
    bool deviceLost_ = false;
};

} // namespace eng::rhi::d3d11
