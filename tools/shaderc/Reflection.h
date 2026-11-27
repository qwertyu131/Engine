#pragma once
// Reflection of a compiled shader, one JSON schema for all targets (plan F1 Task 2/3):
// {"stage","entry","target","resources":[{"name","kind","space","index","arraySize","sizeBytes"?,
//   "targetBinding":{...}}],"vertexInputs":[{"location","semantic","format"}],"pushConstants":null}
#include "Targets.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace eng::sc {

struct Resource {
    std::string name;
    std::string kind; // cbuffer | texture | sampler | storage_ro | storage_rw
    uint32_t space = 0;
    uint32_t index = 0;
    uint32_t arraySize = 1; // 0 = unbounded
    std::optional<uint32_t> sizeBytes;
    uint32_t spirvId = 0; // variable id in the SPIR-V module it was reflected from
    bool image = false;   // storage_rw: RWTexture (storage image) rather than a RWStructuredBuffer
    nlohmann::json targetBinding = nlohmann::json::object();
};

struct VertexInput {
    uint32_t location = 0;
    std::string semantic; // HLSL semantic, e.g. POSITION, TEXCOORD0
    std::string format;   // float32x3, sint32x4, ...
};

struct Reflection {
    std::vector<Resource> resources; // sorted by (space, index, name)
    std::vector<VertexInput> vertexInputs;
    uint32_t renderTargets = 0; // fragment stage: highest SV_Target location + 1
};

// D3D register class of a resource kind: b (cbuffer), t (texture, storage_ro), s (sampler), u (storage_rw).
char registerClass(const std::string& kind);

// Reflects a DXC SPIR-V module (set = HLSL space, binding = HLSL register index).
// `semantics` (optional): the same entry compiled with -fspv-reflect; provides the HLSL semantics of
// the vertex inputs.
std::expected<Reflection, std::string> reflectSpirv(const std::vector<uint32_t>& spirv, Stage stage,
                                                    const std::vector<uint32_t>* semantics);

// D3D11 (SM5 has no register spaces): per register class, the reflected resources sorted by
// (space, index) get slots 0..n-1. `extra` are further declared registers (unused ones; only the direct
// FXC path sees them); they get the slots after the reflected ones. Pixel-shader UAVs share the slot
// range of the render targets (D3D11 OMSetRenderTargetsAndUnorderedAccessViews), so `u` slots of a
// fragment shader start at r.renderTargets.
using RegisterKey = std::tuple<char, uint32_t, uint32_t>; // class, space, index
std::map<RegisterKey, uint32_t> d3d11FlatRegisters(const Reflection& r, const std::vector<RegisterKey>& extra = {});

// Every register(<c><i>[, space<S>]) declared in preprocessed HLSL, in source order (duplicates kept).
std::vector<RegisterKey> declaredRegisters(const std::string& preprocessedHlsl);

// Binding convention (plan F1 Global Constraints): within one space the index is unique across
// b/t/s/u, because Vulkan/WGSL bind (set = space, binding = index) and GLSL ES flattens space*16+index.
// Checks the declared registers (all of them, also the ones this entry does not use) plus the ranges of
// reflected resource arrays. Error: "binding collision in space<S>: <c><i> and <c><j>".
std::expected<void, std::string> checkBindingConvention(const Reflection& r, const std::vector<RegisterKey>& declared);

// Binding convention: every resource the entry uses is declared with an explicit register(). DXC would
// otherwise assign one silently (and a plain cbuffer / loose global gets b0 in space0). Checked on the
// DXC-preprocessed source: the reflected name must be declared as `<name>[...] : register(`. Error:
// "resource <name> has no explicit register() (binding convention)".
std::expected<void, std::string> checkExplicitRegisters(const Reflection& r, const std::string& preprocessedHlsl);

nlohmann::json toJson(const Reflection& r, Stage stage, const std::string& entry, Target target);

} // namespace eng::sc
