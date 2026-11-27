// D3D11 command recording (plan F2 Task 2): straight into the immediate context. Barriers are no-ops
// (the runtime tracks hazards); the debug-layer queue is drained after every draw.
#include "D3D11Common.h"

#include "core/Assert.h"

#include <vector>

namespace eng::rhi::d3d11 {

void D3D11CommandList::begin() {
    pipeline_ = {};
    vertexBuffers_ = {};
    vertexBuffersDirty_ = false;
    inRenderPass_ = false;
    dev_.context()->ClearState();
}

void D3D11CommandList::barrier(std::span<const TextureBarrier>) {}

void D3D11CommandList::beginRenderPass(const RenderPassDesc& desc) {
    ENG_ASSERT_MSG(!inRenderPass_, "beginRenderPass inside a render pass");
    ID3D11DeviceContext1* ctx = dev_.context();
    std::vector<ID3D11RenderTargetView*> rtvs;
    for (const ColorAttachment& c : desc.colors) {
        Texture* t = dev_.textures().get(c.texture.id);
        if (!t || !t->rtv) {
            continue;
        }
        rtvs.push_back(t->rtv.Get());
        if (c.load == LoadOp::Clear) {
            ctx->ClearRenderTargetView(t->rtv.Get(), c.clear);
        } else if (c.load == LoadOp::DontCare) {
            ctx->DiscardView(t->rtv.Get());
        }
    }
    ID3D11DepthStencilView* dsv = nullptr;
    if (desc.depth.texture) {
        if (Texture* t = dev_.textures().get(desc.depth.texture.id); t && t->dsv) {
            dsv = t->dsv.Get();
            if (desc.depth.load == LoadOp::Clear) {
                ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, desc.depth.clearDepth, 0);
            } else if (desc.depth.load == LoadOp::DontCare) {
                ctx->DiscardView(dsv);
            }
        }
    }
    ctx->OMSetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.data(), dsv);
    inRenderPass_ = true;
    dev_.drainMessages();
}

void D3D11CommandList::endRenderPass() {
    ENG_ASSERT_MSG(inRenderPass_, "endRenderPass without beginRenderPass");
    dev_.context()->OMSetRenderTargets(0, nullptr, nullptr);
    inRenderPass_ = false;
}

void D3D11CommandList::setPipeline(PipelineHandle pipeline) {
    Pipeline* p = dev_.pipelines().get(pipeline.id);
    if (!p) {
        return;
    }
    ID3D11DeviceContext1* ctx = dev_.context();
    ctx->VSSetShader(p->vs.Get(), nullptr, 0);
    ctx->PSSetShader(p->ps.Get(), nullptr, 0);
    ctx->IASetInputLayout(p->inputLayout.Get());
    ctx->IASetPrimitiveTopology(p->topology);
    ctx->RSSetState(p->raster.Get());
    ctx->OMSetDepthStencilState(p->depth.Get(), 0);
    const float factor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(p->blend.Get(), factor, 0xFFFFFFFFu);
    pipeline_ = pipeline;
    vertexBuffersDirty_ = true; // strides come from the pipeline
}

void D3D11CommandList::setBindGroup(uint32_t, BindGroupHandle) {
    ENG_ASSERT_MSG(false, "setBindGroup is implemented in plan F2 Task 4");
}

void D3D11CommandList::setVertexBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset) {
    // D3D11 takes the stride at bind time; the RHI keeps it in the pipeline's vertex layout, so vertex
    // buffers are applied at draw time with the strides of the pipeline bound then.
    if (slot >= kMaxVertexBuffers || !dev_.buffers().get(buffer.id)) {
        return;
    }
    vertexBuffers_[slot] = {buffer, offset};
    vertexBuffersDirty_ = true;
}

void D3D11CommandList::flushVertexBuffers() {
    if (!vertexBuffersDirty_) {
        return;
    }
    vertexBuffersDirty_ = false;
    const Pipeline* p = pipeline_ ? dev_.pipelines().tryGet(pipeline_.id) : nullptr;
    for (uint32_t slot = 0; slot < kMaxVertexBuffers; ++slot) {
        const auto& [handle, offset] = vertexBuffers_[slot];
        if (!handle) {
            continue;
        }
        Buffer* b = dev_.buffers().tryGet(handle.id);
        if (!b) {
            continue;
        }
        ID3D11Buffer* buf = b->buffer.Get();
        const UINT stride = p && slot < p->strides.size() ? p->strides[slot] : 0u;
        const UINT off = static_cast<UINT>(offset);
        dev_.context()->IASetVertexBuffers(slot, 1, &buf, &stride, &off);
    }
}

void D3D11CommandList::setIndexBuffer(BufferHandle buffer, IndexType type, uint64_t offset) {
    Buffer* b = dev_.buffers().get(buffer.id);
    if (!b) {
        return;
    }
    dev_.context()->IASetIndexBuffer(b->buffer.Get(), type == IndexType::Uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT,
                                     static_cast<UINT>(offset));
}

void D3D11CommandList::setViewport(const Viewport& v) {
    const D3D11_VIEWPORT vp{v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth};
    dev_.context()->RSSetViewports(1, &vp);
}

void D3D11CommandList::setScissor(const Rect& r) {
    const D3D11_RECT rect{r.x, r.y, r.x + static_cast<LONG>(r.width), r.y + static_cast<LONG>(r.height)};
    dev_.context()->RSSetScissorRects(1, &rect);
}

void D3D11CommandList::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
    flushVertexBuffers();
    if (instanceCount == 1 && firstInstance == 0) {
        dev_.context()->Draw(vertexCount, firstVertex);
    } else {
        dev_.context()->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
    }
    dev_.drainMessages();
}

void D3D11CommandList::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t baseVertex,
                                   uint32_t firstInstance) {
    if (pipeline_) {
        if (const Pipeline* p = dev_.pipelines().tryGet(pipeline_.id)) {
            ENG_ASSERT_MSG(!p->vsHasMorphStorage || baseVertex == 0,
                           "morph meshes draw with base vertex 0 (SV_VertexID includes it on Vulkan)");
        }
    }
    flushVertexBuffers();
    dev_.context()->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
    dev_.drainMessages();
}

} // namespace eng::rhi::d3d11
