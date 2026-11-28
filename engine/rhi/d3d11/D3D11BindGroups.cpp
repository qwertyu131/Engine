// D3D11 samplers, bind groups and transient uniforms (plan F2 Task 4).
//
//   - Bind group: every entry resolves to (stage, register class, slot) from EACH stage's own reflection
//     (targetBinding.register; the slots differ between the VS and the PS of one pipeline, F1). setBindGroup
//     calls VS/PSSetConstantBuffers1 / ShaderResources / Samplers per stage; pixel-shader UAVs go through
//     OMSetRenderTargetsAndUnorderedAccessViews with UAVStartSlot = the PS reflection's renderTargets.
//   - Transient uniforms: one D3D11_USAGE_DYNAMIC constant buffer ring (4 MiB) + a CPU shadow. allocTransient
//     returns shadow memory; before each draw flushTransient() maps the ring (DISCARD on the first map of the
//     frame, MAP_WRITE_NO_OVERWRITE after it), copies the new bytes and unmaps (a buffer must not be mapped
//     while a draw uses it). Ranges are bound with *SetConstantBuffers1 (first / count in 16-byte constants,
//     multiples of 16, so allocations are 256-byte aligned and sized).
//   - setBindGroup (plan F3 Task 1) needs a bound pipeline whose layout for the space equals the group's (names,
//     kinds, per-stage register class / slot): D3D11 binds by slot, so a group made for another permutation would
//     silently feed the wrong registers. A missing pipeline or a mismatch is an RHI validation error (logged,
//     counted: exit 4) and the bind is skipped. A constant-buffer range never extends past the buffer: uniform
//     buffers are allocated in whole 256-byte blocks and the bound count is clamped to what is left of the buffer.
#include "D3D11Common.h"

#include "core/Log.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace eng::rhi::d3d11 {

namespace {

D3D11_TEXTURE_ADDRESS_MODE toD3D(AddressMode m) {
    switch (m) {
    case AddressMode::Repeat: return D3D11_TEXTURE_ADDRESS_WRAP;
    case AddressMode::ClampToEdge: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case AddressMode::MirroredRepeat: return D3D11_TEXTURE_ADDRESS_MIRROR;
    }
    return D3D11_TEXTURE_ADDRESS_WRAP;
}

constexpr uint64_t kConstantsAlign = 256; // 16 constants of 16 bytes (*SetConstantBuffers1 granularity)

std::string stageBindingText(const StageBinding& b) {
    return b.used ? std::format("{}{}", b.registerClass, b.registerSlot) : std::string("unused");
}

// Empty if `group` can be bound where the pipeline expects `pipe`; otherwise the first difference.
std::string layoutDifference(const BindGroupLayout& pipe, const BindGroupLayout& group) {
    if (pipe.entries.size() != group.entries.size()) {
        return std::format("{} resources in the group, {} in the pipeline", group.entries.size(), pipe.entries.size());
    }
    for (size_t i = 0; i < pipe.entries.size(); ++i) {
        const BindGroupLayoutEntry& a = pipe.entries[i];
        const BindGroupLayoutEntry& b = group.entries[i];
        if (a.name != b.name || a.kind != b.kind || a.index != b.index || a.arraySize != b.arraySize) {
            return std::format("group has {} ({}), pipeline has {} ({})", b.name, bindingKindName(b.kind), a.name,
                               bindingKindName(a.kind));
        }
        for (uint32_t stage = 0; stage < kGraphicsStages; ++stage) {
            const StageBinding& sa = a.stages[stage];
            const StageBinding& sb = b.stages[stage];
            if (sa.used != sb.used || (sa.used && (sa.registerClass != sb.registerClass ||
                                                    sa.registerSlot != sb.registerSlot))) {
                return std::format("{} {} stage: group {}, pipeline {}", a.name, stage == kStageVertex ? "vertex" : "pixel",
                                   stageBindingText(sb), stageBindingText(sa));
            }
        }
    }
    return {};
}

} // namespace

Result<SamplerHandle> D3D11Device::createSampler(const SamplerDesc& desc) {
    D3D11_SAMPLER_DESC sd{};
    // D3D11_FILTER encoding: min linear 0x10, mag linear 0x4, mip linear 0x1.
    sd.Filter = static_cast<D3D11_FILTER>((desc.minFilter == Filter::Linear ? 0x10 : 0) |
                                          (desc.magFilter == Filter::Linear ? 0x4 : 0) |
                                          (desc.mipFilter == Filter::Linear ? 0x1 : 0));
    sd.AddressU = toD3D(desc.addressU);
    sd.AddressV = toD3D(desc.addressV);
    sd.AddressW = toD3D(desc.addressW);
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0.0f;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    Sampler s;
    if (HRESULT hr = device_->CreateSamplerState(&sd, &s.sampler); FAILED(hr)) {
        return std::unexpected(hrError("CreateSamplerState", hr));
    }
    drainMessages();
    return SamplerHandle{samplers_.add(std::move(s))};
}

Result<BindGroupHandle> D3D11Device::createBindGroup(const BindGroupDesc& desc) {
    const Pipeline* p = pipelines_.tryGet(desc.pipeline.id);
    if (!p) {
        return std::unexpected(Error{kExitUsage, "d3d11: createBindGroup with an invalid pipeline handle"});
    }
    const auto lit = std::find_if(p->bindGroupLayouts.begin(), p->bindGroupLayouts.end(),
                                  [&](const BindGroupLayout& l) { return l.space == desc.space; });
    const BindGroupLayout empty{desc.space, {}};
    const BindGroupLayout& layout = lit != p->bindGroupLayouts.end() ? *lit : empty;
    auto match = matchBindGroupEntries(layout, desc);
    if (!match) {
        return std::unexpected(match.error());
    }
    if (layout.entries.empty()) {
        return std::unexpected(
            Error{kExitUsage, std::format("bind group space {}: the pipeline has no resources in this space", desc.space)});
    }
    BindGroup g;
    g.space = desc.space;
    g.layout = layout;
    for (size_t i = 0; i < layout.entries.size(); ++i) {
        const BindGroupLayoutEntry& e = layout.entries[i];
        const auto& res = desc.entries[(*match)[i]].resource;
        if (e.arraySize != 1) {
            return std::unexpected(Error{kExitUsage, std::format("bind group space {}: {} is an array (not supported "
                                                                 "by the F2 RHI)",
                                                                 desc.space, e.name)});
        }
        if (const auto* bb = std::get_if<BufferBinding>(&res)) {
            const Buffer* b = buffers_.tryGet(bb->buffer.id);
            const BufferUsage need = e.kind == BindingKind::UniformBuffer ? BufferUsage::Uniform : BufferUsage::Storage;
            if (!b || !any(b->usage, need)) {
                return std::unexpected(Error{kExitUsage, std::format("bind group space {} entry {}: invalid buffer or "
                                                                     "buffer without {} usage",
                                                                     desc.space, e.name, bindingKindName(e.kind))});
            }
            if (bb->offset >= b->size || (e.kind == BindingKind::UniformBuffer && bb->offset % kConstantsAlign != 0)) {
                return std::unexpected(Error{kExitUsage, std::format("bind group space {} entry {}: offset {} not "
                                                                     "aligned to {} or past the buffer",
                                                                     desc.space, e.name, bb->offset, kConstantsAlign)});
            }
            if (e.kind != BindingKind::UniformBuffer && bb->offset != 0) {
                return std::unexpected(Error{kExitUsage, std::format("bind group space {} entry {}: storage buffer "
                                                                     "offsets are not supported on D3D11",
                                                                     desc.space, e.name)});
            }
            if (e.kind == BindingKind::StorageBufferRW && !b->uav) {
                return std::unexpected(Error{kExitUsage, std::format("bind group space {} entry {}: no UAV for this "
                                                                     "buffer (needs FL11)",
                                                                     desc.space, e.name)});
            }
        } else if (const auto* th = std::get_if<TextureHandle>(&res)) {
            const Texture* t = textures_.tryGet(th->id);
            if (!t || !t->srv) {
                return std::unexpected(Error{kExitUsage, std::format("bind group space {} entry {}: invalid texture "
                                                                     "or texture without Sampled usage",
                                                                     desc.space, e.name)});
            }
        } else if (const auto* sh = std::get_if<SamplerHandle>(&res); !samplers_.tryGet(sh->id)) {
            return std::unexpected(
                Error{kExitUsage, std::format("bind group space {} entry {}: invalid sampler", desc.space, e.name)});
        }
        for (uint32_t stage = 0; stage < kGraphicsStages; ++stage) {
            const StageBinding& sb = e.stages[stage];
            if (!sb.used) {
                continue;
            }
            if (e.kind == BindingKind::StorageBufferRW && stage != kStageFragment) {
                return std::unexpected(Error{kExitUsage, std::format("bind group space {} entry {}: read-write storage "
                                                                     "outside the pixel shader is not supported on "
                                                                     "D3D11",
                                                                     desc.space, e.name)});
            }
            g.slots.push_back({stage, sb.registerClass, sb.registerSlot, e.sizeBytes, res});
        }
    }
    return BindGroupHandle{bindGroups_.add(std::move(g))};
}

void D3D11CommandList::setBindGroup(uint32_t space, BindGroupHandle group) {
    BindGroup* g = dev_.bindGroups().get(group.id);
    if (!g) {
        return;
    }
    ENG_ASSERT_MSG(g->space == space, "setBindGroup: the group was created for another space");
    const Pipeline* p = pipeline_ ? dev_.pipelines().tryGet(pipeline_.id) : nullptr;
    if (!p) {
        dev_.reportValidation(std::format("bind group space {} bound without a pipeline (setPipeline first); bind "
                                          "skipped",
                                          space));
        return;
    }
    const auto lit = std::find_if(p->bindGroupLayouts.begin(), p->bindGroupLayouts.end(),
                                  [&](const BindGroupLayout& l) { return l.space == space; });
    const std::string diff = lit == p->bindGroupLayouts.end() ? std::string("the pipeline has no resources in this space")
                                                              : layoutDifference(*lit, g->layout);
    if (!diff.empty()) {
        dev_.reportValidation(
            std::format("bind group space {} layout does not match pipeline ({}); bind skipped", space, diff));
        return;
    }
    ID3D11DeviceContext1* ctx = dev_.context();
    std::vector<std::pair<uint32_t, ID3D11UnorderedAccessView*>> uavs;
    for (const BindGroup::Slot& s : g->slots) {
        const bool vs = s.stage == kStageVertex;
        if (const auto* bb = std::get_if<BufferBinding>(&s.resource)) {
            Buffer* b = dev_.buffers().tryGet(bb->buffer.id);
            if (!b) {
                ENG_LOG_ERROR("d3d11: setBindGroup space {}: a buffer was destroyed", space);
                continue;
            }
            if (s.registerClass == 'b') {
                ID3D11Buffer* buf = b->buffer.Get();
                if (bb->offset == 0 && bb->size == 0) {
                    vs ? ctx->VSSetConstantBuffers1(s.slot, 1, &buf, nullptr, nullptr)
                       : ctx->PSSetConstantBuffers1(s.slot, 1, &buf, nullptr, nullptr);
                } else {
                    // First / count in 16-byte constants, both multiples of 16; the count covers the reflected size
                    // and is clamped to the rest of the buffer (allocBytes and offset are multiples of 256).
                    const uint64_t bytes = std::max<uint64_t>(bb->size, s.sizeBytes);
                    const UINT first = static_cast<UINT>(bb->offset / 16u);
                    const uint64_t left = b->allocBytes > bb->offset ? (b->allocBytes - bb->offset) / 16u : 0u;
                    const UINT num = static_cast<UINT>(
                        std::min<uint64_t>((bytes + kConstantsAlign - 1) / kConstantsAlign * 16u, left));
                    vs ? ctx->VSSetConstantBuffers1(s.slot, 1, &buf, &first, &num)
                       : ctx->PSSetConstantBuffers1(s.slot, 1, &buf, &first, &num);
                }
            } else if (s.registerClass == 't') {
                ID3D11ShaderResourceView* srv = b->srv.Get();
                vs ? ctx->VSSetShaderResources(s.slot, 1, &srv) : ctx->PSSetShaderResources(s.slot, 1, &srv);
            } else if (s.registerClass == 'u') {
                uavs.emplace_back(s.slot, b->uav.Get());
            }
        } else if (const auto* th = std::get_if<TextureHandle>(&s.resource)) {
            Texture* t = dev_.textures().tryGet(th->id);
            ID3D11ShaderResourceView* srv = t ? t->srv.Get() : nullptr;
            vs ? ctx->VSSetShaderResources(s.slot, 1, &srv) : ctx->PSSetShaderResources(s.slot, 1, &srv);
        } else {
            Sampler* sm = dev_.samplers().tryGet(std::get<SamplerHandle>(s.resource).id);
            ID3D11SamplerState* st = sm ? sm->sampler.Get() : nullptr;
            vs ? ctx->VSSetSamplers(s.slot, 1, &st) : ctx->PSSetSamplers(s.slot, 1, &st);
        }
    }
    if (!uavs.empty()) {
        // Pixel-shader UAV slots start after the render targets (F1: register u<n> already counts them).
        const UINT start = p ? p->uavStartSlot : 0u;
        UINT end = start;
        for (const auto& [slot, view] : uavs) {
            end = std::max<UINT>(end, slot + 1u);
        }
        std::vector<ID3D11UnorderedAccessView*> views(end - start, nullptr);
        for (const auto& [slot, view] : uavs) {
            ENG_ASSERT_MSG(slot >= start, "UAV register below UAVStartSlot");
            if (slot >= start) {
                views[slot - start] = view;
            }
        }
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr,
                                                       start, static_cast<UINT>(views.size()), views.data(), nullptr);
    }
    dev_.drainMessages();
}

// --- transient uniforms -----------------------------------------------------------------------------

void D3D11Device::resetTransient() {
    ringUsed_ = 0;
    ringFlushed_ = 0;
    ringDiscarded_ = false;
}

BufferBinding D3D11Device::allocTransient(uint64_t size, uint64_t align) {
    if (!recording_) {
        ENG_LOG_ERROR("d3d11: allocTransient outside beginFrame / submit");
        return {};
    }
    if (!constantBufferOffsetting_) {
        ENG_LOG_ERROR("d3d11: allocTransient needs D3D11_FEATURE_D3D11_OPTIONS.ConstantBufferOffsetting");
        return {};
    }
    if (!ring_) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>(kTransientRingBytes);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Buffer b;
        b.size = kTransientRingBytes;
        b.allocBytes = kTransientRingBytes;
        b.usage = BufferUsage::Uniform;
        if (HRESULT hr = device_->CreateBuffer(&bd, nullptr, &b.buffer); FAILED(hr)) {
            ENG_LOG_ERROR("{}", hrError("CreateBuffer (transient ring)", hr).message);
            return {};
        }
        constexpr char kName[] = "transient ring";
        b.buffer->SetPrivateData(WKPDID_D3DDebugObjectName, sizeof(kName) - 1, kName);
        ring_ = BufferHandle{buffers_.add(std::move(b))};
        ringShadow_.assign(kTransientRingBytes, std::byte{0});
    }
    const uint64_t a = std::max<uint64_t>(align, kConstantsAlign);
    const uint64_t offset = (ringUsed_ + a - 1) / a * a;
    const uint64_t padded = (size + kConstantsAlign - 1) / kConstantsAlign * kConstantsAlign;
    if (size == 0 || offset + padded > kTransientRingBytes) {
        ENG_LOG_ERROR("d3d11: allocTransient({}) does not fit the {}-byte frame ring", size, kTransientRingBytes);
        return {};
    }
    ringUsed_ = offset + padded; // whole 256-byte blocks: the bound constant range never leaves the allocation
    return BufferBinding{ring_, offset, size, ringShadow_.data() + offset};
}

void D3D11Device::flushTransient() {
    if (ringUsed_ == ringFlushed_ || !ring_) {
        return;
    }
    Buffer* ring = buffers_.tryGet(ring_.id);
    if (!ring) {
        return;
    }
    // DISCARD on the first map of a frame (earlier frames may still read the old contents: the runtime
    // renames), NO_OVERWRITE afterwards (only bytes no submitted draw has used yet are written). Without
    // MapNoOverwriteOnDynamicConstantBuffer every flush discards and rewrites the whole frame's range.
    const bool discard = !ringDiscarded_ || !mapNoOverwriteOnDynamicCb_;
    const uint64_t from = discard ? 0 : ringFlushed_;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (HRESULT hr = context_->Map(ring->buffer.Get(), 0, discard ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE,
                                   0, &m);
        FAILED(hr)) {
        ENG_LOG_ERROR("{}", hrError("Map (transient ring)", hr).message);
        return;
    }
    std::memcpy(static_cast<std::byte*>(m.pData) + from, ringShadow_.data() + from, ringUsed_ - from);
    context_->Unmap(ring->buffer.Get(), 0);
    ringDiscarded_ = true;
    ringFlushed_ = ringUsed_;
}

} // namespace eng::rhi::d3d11
