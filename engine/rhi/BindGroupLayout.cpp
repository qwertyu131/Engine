// Bind group layouts derived from shader reflection + entry matching by name (plan F2 Task 3).
// Backend-independent: both backends call these, so the error messages are the same everywhere.
#include "Rhi.h"

#include <algorithm>
#include <format>

namespace eng::rhi {

const char* bindingKindName(BindingKind k) {
    switch (k) {
    case BindingKind::UniformBuffer: return "uniform buffer";
    case BindingKind::Texture: return "texture";
    case BindingKind::Sampler: return "sampler";
    case BindingKind::StorageBuffer: return "storage buffer";
    case BindingKind::StorageBufferRW: return "read-write storage buffer";
    }
    return "?";
}

namespace {

Result<BindingKind> kindOf(const ReflectedResource& r) {
    if (r.kind == "cbuffer") {
        return BindingKind::UniformBuffer;
    }
    if (r.kind == "texture") {
        return BindingKind::Texture;
    }
    if (r.kind == "sampler") {
        return BindingKind::Sampler;
    }
    if (r.kind == "storage_ro") {
        return BindingKind::StorageBuffer;
    }
    if (r.kind == "storage_rw") {
        return BindingKind::StorageBufferRW;
    }
    return std::unexpected(Error{kExitUsage, "resource " + r.name + " has unknown reflection kind '" + r.kind + "'"});
}

bool isBufferKind(BindingKind k) {
    return k == BindingKind::UniformBuffer || k == BindingKind::StorageBuffer || k == BindingKind::StorageBufferRW;
}

} // namespace

Result<std::vector<BindGroupLayout>> deriveBindGroupLayouts(const ShaderReflection& vs, const ShaderReflection& ps) {
    std::vector<BindGroupLayout> layouts;
    const ShaderReflection* stages[kGraphicsStages] = {&vs, &ps};
    for (uint32_t stage = 0; stage < kGraphicsStages; ++stage) {
        for (const ReflectedResource& r : stages[stage]->resources) {
            auto kind = kindOf(r);
            if (!kind) {
                return std::unexpected(kind.error());
            }
            if (r.arraySize == 0) {
                return std::unexpected(Error{kExitUsage, std::format("resource {} (space {}): unbounded arrays are not "
                                                                     "supported by the F2 RHI",
                                                                     r.name, r.space)});
            }
            auto lit = std::find_if(layouts.begin(), layouts.end(), [&](const auto& l) { return l.space == r.space; });
            if (lit == layouts.end()) {
                layouts.push_back({r.space, {}});
                lit = layouts.end() - 1;
            }
            auto eit = std::find_if(lit->entries.begin(), lit->entries.end(), [&](const auto& e) { return e.name == r.name; });
            if (eit == lit->entries.end()) {
                BindGroupLayoutEntry e;
                e.name = r.name;
                e.kind = *kind;
                e.index = r.index;
                e.arraySize = r.arraySize;
                e.sizeBytes = r.sizeBytes;
                lit->entries.push_back(std::move(e));
                eit = lit->entries.end() - 1;
            } else if (eit->kind != *kind || eit->index != r.index || eit->arraySize != r.arraySize) {
                return std::unexpected(Error{kExitUsage, std::format("resource {} (space {}) differs between the vertex "
                                                                     "and pixel shader reflection",
                                                                     r.name, r.space)});
            }
            eit->sizeBytes = std::max(eit->sizeBytes, r.sizeBytes);
            StageBinding& sb = eit->stages[stage];
            sb.used = true;
            sb.set = r.set;
            sb.binding = r.binding;
            sb.registerClass = r.registerClass;
            sb.registerSlot = r.registerSlot;
        }
    }
    for (BindGroupLayout& l : layouts) {
        std::sort(l.entries.begin(), l.entries.end(), [](const auto& a, const auto& b) { return a.index < b.index; });
    }
    std::sort(layouts.begin(), layouts.end(), [](const auto& a, const auto& b) { return a.space < b.space; });
    return layouts;
}

Result<std::vector<size_t>> matchBindGroupEntries(const BindGroupLayout& layout, const BindGroupDesc& desc) {
    const uint32_t space = desc.space;
    // Unknown names first: a typo then reports itself instead of the entry it should have filled.
    for (const BindGroupEntry& e : desc.entries) {
        const bool known = std::any_of(layout.entries.begin(), layout.entries.end(),
                                       [&](const BindGroupLayoutEntry& l) { return l.name == e.name; });
        if (!known) {
            return std::unexpected(Error{kExitUsage, std::format("bind group space {} has no resource {}", space, e.name)});
        }
    }
    std::vector<size_t> out;
    out.reserve(layout.entries.size());
    for (const BindGroupLayoutEntry& l : layout.entries) {
        const auto it = std::find_if(desc.entries.begin(), desc.entries.end(),
                                     [&](const BindGroupEntry& e) { return e.name == l.name; });
        if (it == desc.entries.end()) {
            return std::unexpected(Error{kExitUsage, std::format("bind group space {} missing {}", space, l.name)});
        }
        const bool ok = isBufferKind(l.kind)          ? std::holds_alternative<BufferBinding>(it->resource)
                        : l.kind == BindingKind::Texture ? std::holds_alternative<TextureHandle>(it->resource)
                                                         : std::holds_alternative<SamplerHandle>(it->resource);
        if (!ok) {
            return std::unexpected(Error{kExitUsage, std::format("bind group space {} entry {} must be a {}", space,
                                                                 l.name, bindingKindName(l.kind))});
        }
        if (std::count_if(desc.entries.begin(), desc.entries.end(), [&](const auto& e) { return e.name == l.name; }) > 1) {
            return std::unexpected(Error{kExitUsage, std::format("bind group space {} sets {} twice", space, l.name)});
        }
        out.push_back(static_cast<size_t>(it - desc.entries.begin()));
    }
    return out;
}

} // namespace eng::rhi
