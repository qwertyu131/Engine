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
// FXC path sees them); they get the slots after the reflected ones.
using RegisterKey = std::tuple<char, uint32_t, uint32_t>; // class, space, index
std::map<RegisterKey, uint32_t> d3d11FlatRegisters(const Reflection& r, const std::vector<RegisterKey>& extra = {});

nlohmann::json toJson(const Reflection& r, Stage stage, const std::string& entry, Target target);

} // namespace eng::sc
