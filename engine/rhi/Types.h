#pragma once
// RHI value types (plan F2 Task 2; spec §5.3). Backend-independent: no Vulkan / D3D type appears here.
//
// Conventions fixed by the RHI (every backend implements them, apps never correct for a backend):
//   - clip space Z in [0, 1];
//   - framebuffer origin top-left, +Y of NDC points up (Vulkan: negative-height viewport,
//     VK_KHR_maintenance1 / core 1.1);
//   - texture UV (0, 0) = first row in memory;
//   - readback images are RGBA8, rows top-down.
#include "core/Result.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace eng::rhi {

enum class Backend : uint8_t { Vulkan, D3D11 }; // D3D12, Metal, GLES, WebGPU added later

const char* toString(Backend b); // "vulkan" | "d3d11"

enum class Format : uint16_t {
    Unknown,
    // Texture / render-target formats.
    RGBA8_UNORM,
    RGBA8_SRGB,
    BGRA8_UNORM,
    BGRA8_SRGB,
    RGBA16_FLOAT,
    RGBA32_FLOAT,
    D32_FLOAT,
    D24_UNORM_S8_UINT,
    // Vertex formats.
    R32_FLOAT,
    RG32_FLOAT,
    RGB32_FLOAT,
    R32_UINT,
    RG32_UINT,
    RGB32_UINT,
    RGBA32_UINT,
    R32_SINT,
    RGBA32_SINT,
    RGBA16_UINT,
    RGBA8_UINT,
    // Block-compressed texture formats (plan F3 Task 3): 4 x 4 texel blocks, sample-only (no render target, no
    // vertex use). BC1..BC7 = DXGI BC* / VK_FORMAT_BC*; ETC2 / ASTC have no DXGI format (Vulkan / mobile, F11).
    BC1_UNORM,
    BC1_SRGB,
    BC2_UNORM,
    BC2_SRGB,
    BC3_UNORM,
    BC3_SRGB,
    BC4_UNORM,
    BC5_UNORM,
    BC6H_UFLOAT,
    BC7_UNORM,
    BC7_SRGB,
    ETC2_RGBA8_UNORM,
    ETC2_RGBA8_SRGB,
    ASTC_4x4_UNORM,
    ASTC_4x4_SRGB,
};

const char* toString(Format f);
// Bytes per texel / vertex element, or per 4 x 4 block of a block-compressed format; 0 for Unknown.
uint32_t formatBytes(Format f);
std::span<const Format> allFormats(); // every Format except Unknown, in declaration order (DeviceCaps table)

// Block layout of a format: compressed formats 4 x 4 texels per `bytes`-byte block, every other format 1 x 1.
struct FormatBlock {
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t bytes = 0;
};
FormatBlock formatBlock(Format f);
inline bool isCompressed(Format f) {
    return formatBlock(f).width > 1;
}
bool isSrgbFormat(Format f); // the sRGB-encoded formats (RGBA8 / BGRA8 / BC1 / BC2 / BC3 / BC7 / ETC2 / ASTC _SRGB)
// Bytes of one row of texels / blocks of a w x h subresource, and the number of such rows.
inline uint32_t subresourceRowBytes(Format f, uint32_t w) {
    const FormatBlock b = formatBlock(f);
    return (w + b.width - 1) / b.width * b.bytes;
}
inline uint32_t subresourceRows(Format f, uint32_t h) {
    const FormatBlock b = formatBlock(f);
    return (h + b.height - 1) / b.height;
}

// --- errors ---------------------------------------------------------------------------------------

// exitCode per plan F2 Global Constraints: 2 = usage / IO error, 3 = backend not available,
// 4 = validation error, 5 = device lost / GPU error. The engine-wide type (core/Result.h, plan F3 Task 2).
// Using-declarations (not new aliases): eng::rhi::Result and eng::Result name the same template, so code in a namespace
// that sees both (e.g. `using namespace rhi;` inside namespace eng) is not ambiguous.
using ::eng::Error;
using ::eng::Result;

inline constexpr int kExitUsage = 2;
inline constexpr int kExitNoBackend = 3;
inline constexpr int kExitValidation = 4;
inline constexpr int kExitDeviceLost = 5;

// --- handles --------------------------------------------------------------------------------------
// 32-bit generational indices: low 20 bits = slot + 1 (0 = null handle), high 12 bits = generation.
// A stale handle (slot reused or destroyed) asserts in the backend.

enum class HandleType : uint8_t { None, Buffer, Texture, Sampler, Shader, Pipeline, BindGroup, Swapchain };

#define ENG_RHI_HANDLE(Name)                                                                                      \
    struct Name {                                                                                                 \
        uint32_t id = 0;                                                                                          \
        explicit operator bool() const { return id != 0; }                                                       \
        friend bool operator==(Name, Name) = default;                                                             \
    }
ENG_RHI_HANDLE(BufferHandle);
ENG_RHI_HANDLE(TextureHandle);
ENG_RHI_HANDLE(SamplerHandle);
ENG_RHI_HANDLE(ShaderHandle);
ENG_RHI_HANDLE(PipelineHandle);
ENG_RHI_HANDLE(BindGroupHandle);
ENG_RHI_HANDLE(SwapchainHandle);
#undef ENG_RHI_HANDLE

// Any handle, for IDevice::destroy.
struct Handle {
    HandleType type = HandleType::None;
    uint32_t id = 0;
    Handle() = default;
    Handle(BufferHandle h) : type(HandleType::Buffer), id(h.id) {}
    Handle(TextureHandle h) : type(HandleType::Texture), id(h.id) {}
    Handle(SamplerHandle h) : type(HandleType::Sampler), id(h.id) {}
    Handle(ShaderHandle h) : type(HandleType::Shader), id(h.id) {}
    Handle(PipelineHandle h) : type(HandleType::Pipeline), id(h.id) {}
    Handle(BindGroupHandle h) : type(HandleType::BindGroup), id(h.id) {}
    Handle(SwapchainHandle h) : type(HandleType::Swapchain), id(h.id) {}
};

// --- resources ------------------------------------------------------------------------------------

enum class BufferUsage : uint32_t {
    None = 0,
    Vertex = 1u << 0,
    Index = 1u << 1,
    Uniform = 1u << 2,
    Storage = 1u << 3,
    CopySrc = 1u << 4,
    CopyDst = 1u << 5,
};
constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr bool any(BufferUsage a, BufferUsage b) {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

struct BufferDesc {
    uint64_t size = 0;
    BufferUsage usage = BufferUsage::None;
    bool hostVisible = false; // CPU-writable (mapped); otherwise device-local, initial data via staging
    std::string debugName;
};

enum class TextureUsage : uint32_t {
    None = 0,
    Sampled = 1u << 0,
    RenderTarget = 1u << 1,
    DepthStencil = 1u << 2,
    Storage = 1u << 3,
    CopySrc = 1u << 4, // readback()
    CopyDst = 1u << 5, // initial data upload
};
constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) {
    return static_cast<TextureUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr bool any(TextureUsage a, TextureUsage b) {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

struct TextureDesc {
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1; // 6 + cube = cubemap
    bool cube = false;
    Format format = Format::RGBA8_UNORM;
    TextureUsage usage = TextureUsage::Sampled;
    std::string debugName;
};

// Initial data of one subresource. createTexture takes them in D3D order: index = layer * mipLevels + mip
// (all mips of layer 0, then layer 1, ...); every subresource must be given.
struct SubresourceData {
    std::span<const std::byte> data;
    uint32_t rowPitch = 0; // bytes per row; 0 = tightly packed
};

enum class Filter : uint8_t { Nearest, Linear };
enum class AddressMode : uint8_t { Repeat, ClampToEdge, MirroredRepeat };

struct SamplerDesc {
    Filter minFilter = Filter::Linear;
    Filter magFilter = Filter::Linear;
    Filter mipFilter = Filter::Linear;
    AddressMode addressU = AddressMode::Repeat;
    AddressMode addressV = AddressMode::Repeat;
    AddressMode addressW = AddressMode::Repeat;
};

// --- shaders --------------------------------------------------------------------------------------

enum class ShaderStage : uint8_t { Vertex, Fragment, Compute };

// One resource of the shaderc reflection JSON (plan F1 Task 3 schema).
struct ReflectedResource {
    std::string name;
    std::string kind; // cbuffer | texture | sampler | storage_ro | storage_rw
    uint32_t space = 0;
    uint32_t index = 0;
    uint32_t arraySize = 1; // 0 = unbounded
    uint32_t sizeBytes = 0; // cbuffer only, else 0
    // Target binding: Vulkan (spirv) set/binding; D3D11 (dxbc) register class + slot, e.g. "b1" -> 'b', 1.
    uint32_t set = 0;
    uint32_t binding = 0;
    char registerClass = 0;
    uint32_t registerSlot = 0;
};

struct ReflectedVertexInput {
    uint32_t location = 0;
    std::string semantic; // HLSL semantic with index, e.g. POSITION, TEXCOORD0
    std::string format;   // float32x3, ...
};

struct ShaderReflection {
    ShaderStage stage = ShaderStage::Vertex;
    std::string entry;
    std::string target; // spirv | dxbc
    std::string dxbcPath; // dxbc: "cross" | "direct" (shaderc records it since plan F2 Task 4), else empty
    std::string shaderModel; // dxbc: "4_0" (T0, D3D11 FL10_x) | "5_0" (T1) (plan F2 Task 7), else empty
    uint32_t renderTargets = 0; // pixel shader: highest SV_Target + 1 (D3D11 UAVStartSlot), else 0
    std::vector<ReflectedResource> resources;
    std::vector<ReflectedVertexInput> vertexInputs;
};

// A compiled shader as ShaderLibrary loads it: blob + parsed reflection.
struct ShaderBlob {
    Backend backend = Backend::Vulkan;
    ShaderStage stage = ShaderStage::Vertex;
    std::string entry;
    std::vector<uint8_t> code; // SPIR-V words (little endian) or a DXBC container
    ShaderReflection reflection;
};

// --- pipelines ------------------------------------------------------------------------------------

enum class PrimitiveTopology : uint8_t { TriangleList, TriangleStrip, LineList, PointList };
enum class CullMode : uint8_t { None, Front, Back };
enum class FrontFace : uint8_t { Clockwise, CounterClockwise }; // in framebuffer space (origin top-left)
enum class CompareOp : uint8_t { Never, Less, LessEqual, Equal, Greater, GreaterEqual, Always };
enum class IndexType : uint8_t { Uint16, Uint32 };

struct VertexAttribute {
    std::string semantic; // matched against the VS reflection's vertexInputs[].semantic (e.g. COLOR0)
    Format format = Format::RGBA32_FLOAT;
    uint32_t buffer = 0; // vertex buffer slot
    uint32_t offset = 0;
};

struct VertexBufferLayout {
    uint32_t stride = 0;
    bool perInstance = false;
};

struct VertexLayout {
    std::vector<VertexBufferLayout> buffers;
    std::vector<VertexAttribute> attributes;
};

struct RasterState {
    CullMode cull = CullMode::None;
    FrontFace frontFace = FrontFace::Clockwise;
};

struct DepthState {
    bool test = false;
    bool write = false;
    CompareOp compare = CompareOp::Less;
};

struct BlendState {
    bool enable = false; // standard alpha blending (src alpha, one minus src alpha) when on
};

struct GraphicsPipelineDesc {
    ShaderHandle vs;
    ShaderHandle ps;
    VertexLayout vertexLayout;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    RasterState raster;
    DepthState depth;
    std::vector<Format> colorFormats;
    std::vector<BlendState> blend; // per colour target; missing entries = blending off
    Format depthFormat = Format::Unknown;
    std::string debugName;
};

// --- bind groups (plan F2 Task 3) -----------------------------------------------------------------

// A buffer range for a bind group entry (cbuffer or storage buffer). size 0 = from offset to the end.
// `cpu` is set only by IDevice::allocTransient: the write pointer of the transient range.
struct BufferBinding {
    BufferHandle buffer;
    uint64_t offset = 0;
    uint64_t size = 0;
    std::byte* cpu = nullptr;
};

// One resource of a bind group, matched by the reflection NAME of the resource (cbuffer block name,
// texture / sampler / storage buffer variable name), never by index or backend slot.
struct BindGroupEntry {
    std::string name;
    std::variant<BufferBinding, TextureHandle, SamplerHandle> resource;
};

// All resources of one binding-convention space (0 frame, 1 view, 2 material, 3 draw) of `pipeline`'s
// shaders. The layout comes from the pipeline (its shaders' reflection); the group can be bound with
// any pipeline whose layout for this space is the same.
struct BindGroupDesc {
    PipelineHandle pipeline;
    uint32_t space = 0;
    std::vector<BindGroupEntry> entries;
};

// Swapchains (plan F2 Task 6).
struct SwapchainDesc {
    void* nativeWindow = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    Format format = Format::BGRA8_SRGB;
    bool vsync = true;
    uint32_t imageCount = 3;
};

// --- command recording ----------------------------------------------------------------------------

enum class ResourceState : uint8_t { Undefined, RenderTarget, DepthWrite, ShaderRead, CopySrc, CopyDst, Present };

struct TextureBarrier {
    TextureHandle texture;
    ResourceState before = ResourceState::Undefined;
    ResourceState after = ResourceState::Undefined;
};

enum class LoadOp : uint8_t { Load, Clear, DontCare };
enum class StoreOp : uint8_t { Store, DontCare };

struct ColorAttachment {
    TextureHandle texture;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

struct DepthAttachment {
    TextureHandle texture;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::DontCare;
    float clearDepth = 1.0f;
};

struct RenderPassDesc {
    std::vector<ColorAttachment> colors;
    DepthAttachment depth; // texture null = no depth attachment
};

struct Viewport {
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f; // framebuffer pixels, origin top-left
    float minDepth = 0.0f, maxDepth = 1.0f;
};

struct Rect {
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
};

// --- misc -----------------------------------------------------------------------------------------

// RGBA8, rows top-down, tightly packed (width * 4 bytes per row).
struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba8;
};

// Native objects for upscalers / third-party integrations (spec §5.3 getNativeHandle).
struct NativeHandles {
    Backend backend = Backend::Vulkan;
    // Vulkan: VkInstance, VkPhysicalDevice, VkDevice, VkQueue, queue family index.
    void* vkInstance = nullptr;
    void* vkPhysicalDevice = nullptr;
    void* vkDevice = nullptr;
    void* vkQueue = nullptr;
    uint32_t vkQueueFamily = 0;
    // D3D11: ID3D11Device*, ID3D11DeviceContext* (immediate).
    void* d3d11Device = nullptr;
    void* d3d11Context = nullptr;
};

} // namespace eng::rhi
