#include "Reflection.h"

#include <spirv_cross.hpp>

#include <algorithm>
#include <set>

namespace eng::sc {

char registerClass(const std::string& kind) {
    if (kind == "cbuffer") {
        return 'b';
    }
    if (kind == "sampler") {
        return 's';
    }
    if (kind == "storage_rw") {
        return 'u';
    }
    return 't';
}

namespace {

std::string formatOf(const spirv_cross::SPIRType& t) {
    std::string base;
    switch (t.basetype) {
    case spirv_cross::SPIRType::Float: base = "float32"; break;
    case spirv_cross::SPIRType::Half: base = "float16"; break;
    case spirv_cross::SPIRType::Int: base = "sint32"; break;
    case spirv_cross::SPIRType::UInt: base = "uint32"; break;
    default: base = "unknown"; break;
    }
    return t.vecsize > 1 ? base + "x" + std::to_string(t.vecsize) : base;
}

uint32_t arraySizeOf(const spirv_cross::SPIRType& t) {
    uint32_t n = 1;
    for (uint32_t d : t.array) {
        if (d == 0) {
            return 0; // runtime-sized
        }
        n *= d;
    }
    return n;
}

} // namespace

std::expected<Reflection, std::string> reflectSpirv(const std::vector<uint32_t>& spirv, Stage stage,
                                                    const std::vector<uint32_t>* semantics) {
    try {
        spirv_cross::Compiler comp(spirv);
        const spirv_cross::ShaderResources res = comp.get_shader_resources();
        Reflection out;

        auto add = [&](const spirv_cross::Resource& r, const std::string& kind) {
            Resource x;
            x.spirvId = r.id;
            x.name = comp.get_name(r.id);
            if (x.name.empty()) {
                x.name = r.name;
            }
            x.kind = kind;
            x.space = comp.get_decoration(r.id, spv::DecorationDescriptorSet);
            x.index = comp.get_decoration(r.id, spv::DecorationBinding);
            x.arraySize = arraySizeOf(comp.get_type(r.type_id));
            if (kind == "cbuffer") {
                x.sizeBytes = static_cast<uint32_t>(comp.get_declared_struct_size(comp.get_type(r.base_type_id)));
            }
            out.resources.push_back(std::move(x));
        };
        for (const auto& r : res.uniform_buffers) {
            add(r, "cbuffer");
        }
        for (const auto& r : res.separate_images) {
            add(r, "texture");
        }
        for (const auto& r : res.separate_samplers) {
            add(r, "sampler");
        }
        for (const auto& r : res.storage_buffers) {
            const bool readOnly = comp.get_buffer_block_flags(r.id).get(spv::DecorationNonWritable);
            add(r, readOnly ? "storage_ro" : "storage_rw");
        }
        for (const auto& r : res.storage_images) {
            add(r, "storage_rw");
        }
        if (!res.sampled_images.empty()) {
            return std::unexpected("combined image samplers are not part of the HLSL binding convention ('" +
                                   res.sampled_images.front().name + "')");
        }
        if (!res.push_constant_buffers.empty()) {
            return std::unexpected("push constants are not supported by the binding convention");
        }
        std::sort(out.resources.begin(), out.resources.end(), [](const Resource& a, const Resource& b) {
            return std::tie(a.space, a.index, a.name) < std::tie(b.space, b.index, b.name);
        });

        if (stage == Stage::Vertex) {
            std::map<uint32_t, std::string> semanticByLocation;
            if (semantics) {
                spirv_cross::Compiler sem(*semantics);
                for (const auto& r : sem.get_shader_resources().stage_inputs) {
                    if (sem.has_decoration(r.id, spv::DecorationLocation)) {
                        semanticByLocation[sem.get_decoration(r.id, spv::DecorationLocation)] =
                            sem.get_decoration_string(r.id, spv::DecorationUserSemantic);
                    }
                }
            }
            for (const auto& r : res.stage_inputs) {
                if (!comp.has_decoration(r.id, spv::DecorationLocation)) {
                    continue;
                }
                VertexInput v;
                v.location = comp.get_decoration(r.id, spv::DecorationLocation);
                const auto it = semanticByLocation.find(v.location);
                v.semantic = it != semanticByLocation.end() ? it->second : std::string();
                v.format = formatOf(comp.get_type(r.type_id));
                out.vertexInputs.push_back(std::move(v));
            }
            std::sort(out.vertexInputs.begin(), out.vertexInputs.end(),
                      [](const VertexInput& a, const VertexInput& b) { return a.location < b.location; });
        }
        return out;
    } catch (const std::exception& e) {
        return std::unexpected(std::string("SPIR-V reflection failed: ") + e.what());
    }
}

std::map<RegisterKey, uint32_t> d3d11FlatRegisters(const Reflection& r, const std::vector<RegisterKey>& extra) {
    std::set<RegisterKey> used;
    for (const Resource& x : r.resources) {
        used.insert({registerClass(x.kind), x.space, x.index});
    }
    std::map<char, uint32_t> next;
    std::map<RegisterKey, uint32_t> flat;
    auto assign = [&](const RegisterKey& k) {
        if (!flat.contains(k)) {
            flat[k] = next[std::get<0>(k)]++;
        }
    };
    // std::set orders by (class, space, index): reflected registers first, then the extra ones.
    for (const RegisterKey& k : used) {
        assign(k);
    }
    for (const RegisterKey& k : std::set<RegisterKey>(extra.begin(), extra.end())) {
        assign(k);
    }
    return flat;
}

nlohmann::json toJson(const Reflection& r, Stage stage, const std::string& entry, Target target) {
    nlohmann::json j;
    j["stage"] = toString(stage);
    j["entry"] = entry;
    j["target"] = toString(target);
    j["resources"] = nlohmann::json::array();
    for (const Resource& x : r.resources) {
        nlohmann::json e{{"name", x.name},   {"kind", x.kind},           {"space", x.space},
                         {"index", x.index}, {"arraySize", x.arraySize}, {"targetBinding", x.targetBinding}};
        if (x.sizeBytes) {
            e["sizeBytes"] = *x.sizeBytes;
        }
        j["resources"].push_back(std::move(e));
    }
    j["vertexInputs"] = nlohmann::json::array();
    for (const VertexInput& v : r.vertexInputs) {
        j["vertexInputs"].push_back({{"location", v.location}, {"semantic", v.semantic}, {"format", v.format}});
    }
    j["pushConstants"] = nullptr;
    return j;
}

} // namespace eng::sc
