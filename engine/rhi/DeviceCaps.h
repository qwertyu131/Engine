#pragma once
// What a device really supports, queried from the backend (spec §5.3, §6.1, §6.2). Plan F2 Task 2 filled
// the identity fields; Task 7 adds the capability table (limits, formats, features), `headless --dump-caps`
// and the viewer's DeviceCaps panel, which both list the fields through forEachCapsField().
//
// Meaning of the feature fields: what the device / feature level SUPPORTS (as reported by the API), not
// what the RHI enables. In F2 the Vulkan backend enables only dynamicRendering; the D3D11 backend gets a
// feature level's whole feature set implicitly.
#include "Types.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace eng::rhi {

// Per-format support, optimal tiling / 2D texture:
//   sample = shader load / sample (D3D11 SHADER_LOAD; Vulkan SAMPLED_IMAGE), filter = linear filtering
//   (D3D11 SHADER_SAMPLE; Vulkan SAMPLED_IMAGE_FILTER_LINEAR), render = colour attachment, blend = colour
//   attachment with blending, depth = depth / stencil attachment, vertex = vertex buffer element.
struct FormatCaps {
    bool sample = false;
    bool filter = false;
    bool render = false;
    bool blend = false;
    bool depth = false;
    bool vertex = false;
    friend bool operator==(const FormatCaps&, const FormatCaps&) = default;
};

// Resource bindings one shader stage can see. storage = read-write storage (D3D11: UAV slots of the
// pixel / compute stage, 0 below FL11; Vulkan: maxPerStageDescriptorStorageBuffers).
struct BindingLimits {
    uint32_t cbuffer = 0;
    uint32_t texture = 0;
    uint32_t sampler = 0;
    uint32_t storage = 0;
};

struct DeviceCaps {
    Backend backend = Backend::Vulkan;
    int tier = 0;            // spec §6.1: 0 = T0, 1 = T1, 2 = T2
    std::string apiVersion;  // "1.4.329" (Vulkan device API) / "11_1" (D3D11 feature level)
    std::string adapterName; // UTF-8
    uint32_t vendorId = 0;
    bool software = false;   // WARP / CPU device

    // Limits.
    uint32_t maxTexture2D = 0;        // width / height of a 2D texture
    uint32_t maxCubeSize = 0;         // width / height of a cube face
    uint32_t maxColorAttachments = 0; // simultaneous render targets
    uint32_t maxVertexAttributes = 0; // vertex input elements
    uint64_t maxUniformBufferBytes = 0; // one uniform / constant buffer binding
    BindingLimits maxBindingsPerStage;

    // Shader features.
    bool computeShaders = false;       // D3D11 FL10_x: D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS (CS 4.x)
    bool storageBuffers = false;       // structured / storage buffers in the graphics stages
    bool vertexStorageBuffers = false; // read-only storage buffers in the vertex stage (T1/T2 skin + morph)

    // Formats: one entry per Format (allFormats() order).
    std::vector<std::pair<Format, FormatCaps>> formatSupport;
    const FormatCaps& format(Format f) const; // all-false entry for a format not in the table

    // Compressed texture families (sampling): bc = BC1..BC5 (DXT1/3/5, RGTC), bc6h7 = BC6H + BC7 (FL11+),
    // etc2 = ETC2 / EAC, astc = ASTC LDR.
    bool bc = false;
    bool bc6h7 = false;
    bool etc2 = false;
    bool astc = false;

    uint32_t msaaSampleCounts = 0; // bit n set = 2^n samples supported for RGBA8_UNORM colour + D32_FLOAT depth
    bool float16 = false;          // 16-bit float shader arithmetic (D3D11: min-precision 16-bit in VS and PS)
    bool waveOps = false;          // subgroup / wave ops (basic + ballot + arithmetic) in fragment and compute
    bool rayTracing = false;       // ray tracing pipeline or ray query
    bool bindless = false;         // descriptor indexing (runtime arrays, non-uniform indexing, partially bound)
    bool timestampQueries = false; // GPU timestamps on the graphics queue
};

// Value of one field for forEachCapsField: bool, number, string or a format entry.
using CapsValue = std::variant<bool, int64_t, std::string, FormatCaps>;

// Every DeviceCaps field in declaration order, as (name, value). Nested fields are dotted:
// "maxBindingsPerStage.cbuffer", "formatSupport.RGBA8_UNORM". The single list behind toJson() and the
// viewer's DeviceCaps panel (they cannot drift apart).
void forEachCapsField(const DeviceCaps& caps, const std::function<void(std::string_view, const CapsValue&)>& fn);

// "sample filter render blend depth vertex" subset, or "-" when nothing is supported.
std::string toString(const FormatCaps& f);

// JSON object of every field (dotted names become nested objects, a FormatCaps an object of bools), plus
// "note" when not empty. Two-space indented, trailing newline.
std::string capsToJson(const DeviceCaps& caps, std::string_view note);

} // namespace eng::rhi
