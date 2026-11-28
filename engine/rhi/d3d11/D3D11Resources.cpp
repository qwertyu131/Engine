// D3D11 resources: buffers, textures, shaders, graphics pipelines (plan F2 Task 2); texture initial data,
// storage-buffer views, bind group layouts per stage, DXBC path check (plan F2 Task 4).
#include "D3D11Common.h"

#include <algorithm>
#include <cctype>
#include <format>

namespace eng::rhi::d3d11 {

Result<BufferHandle> D3D11Device::createBuffer(const BufferDesc& desc, std::span<const std::byte> init) {
    if (desc.size == 0 || desc.size > UINT32_MAX) {
        return std::unexpected(Error{kExitUsage, "d3d11: createBuffer size must be in [1, 4 GiB)"});
    }
    if (init.size() > desc.size) {
        return std::unexpected(Error{kExitUsage, "d3d11: createBuffer: initial data larger than the buffer"});
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = static_cast<UINT>(desc.size);
    bd.Usage = D3D11_USAGE_DEFAULT;
    if (any(desc.usage, BufferUsage::Vertex)) {
        bd.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
    }
    if (any(desc.usage, BufferUsage::Index)) {
        bd.BindFlags |= D3D11_BIND_INDEX_BUFFER;
    }
    if (any(desc.usage, BufferUsage::Uniform)) {
        bd.BindFlags |= D3D11_BIND_CONSTANT_BUFFER;
        // Whole 256-byte blocks (plan F3 Task 1): a *SetConstantBuffers1 range (first / count in multiples of
        // 16 constants) then always stays inside the allocation.
        bd.ByteWidth = (bd.ByteWidth + 255u) & ~255u;
    }
    const bool storage = any(desc.usage, BufferUsage::Storage);
    if (storage) {
        // Raw views: the cross DXBC path (SPIRV-Cross) declares storage buffers as (RW)ByteAddressBuffer.
        bd.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        bd.ByteWidth = (bd.ByteWidth + 3u) & ~3u;
        if (caps_.tier >= 1) {
            bd.BindFlags |= D3D11_BIND_UNORDERED_ACCESS; // pixel-shader UAVs need FL11
        }
    }
    std::vector<std::byte> padded;
    D3D11_SUBRESOURCE_DATA sd{};
    if (!init.empty()) {
        padded.assign(bd.ByteWidth, std::byte{0});
        std::copy(init.begin(), init.end(), padded.begin());
        sd.pSysMem = padded.data();
    }
    Buffer b;
    b.size = desc.size;
    b.allocBytes = bd.ByteWidth;
    b.usage = desc.usage;
    if (HRESULT hr = device_->CreateBuffer(&bd, init.empty() ? nullptr : &sd, &b.buffer); FAILED(hr)) {
        return std::unexpected(hrError("CreateBuffer", hr));
    }
    if (storage) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_TYPELESS;
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sv.BufferEx.NumElements = bd.ByteWidth / 4u;
        sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (HRESULT hr = device_->CreateShaderResourceView(b.buffer.Get(), &sv, &b.srv); FAILED(hr)) {
            return std::unexpected(hrError("CreateShaderResourceView (storage buffer)", hr));
        }
        if (bd.BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
            uv.Format = DXGI_FORMAT_R32_TYPELESS;
            uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uv.Buffer.NumElements = bd.ByteWidth / 4u;
            uv.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            if (HRESULT hr = device_->CreateUnorderedAccessView(b.buffer.Get(), &uv, &b.uav); FAILED(hr)) {
                return std::unexpected(hrError("CreateUnorderedAccessView (storage buffer)", hr));
            }
        }
    }
    if (!desc.debugName.empty()) {
        b.buffer->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(desc.debugName.size()), desc.debugName.data());
    }
    drainMessages();
    return BufferHandle{buffers_.add(std::move(b))};
}

Result<TextureHandle> D3D11Device::createTexture(const TextureDesc& desc, std::span<const SubresourceData> init) {
    if (desc.width == 0 || desc.height == 0 || desc.mipLevels == 0 || desc.arrayLayers == 0) {
        return std::unexpected(Error{kExitUsage, "d3d11: createTexture with a zero dimension"});
    }
    Texture t;
    t.desc = desc;
    t.format = toDxgiFormat(desc.format);
    if (t.format == DXGI_FORMAT_UNKNOWN) {
        return std::unexpected(Error{kExitUsage, std::format("d3d11: createTexture: format {} has no DXGI format",
                                                             toString(desc.format))});
    }
    if (isCompressed(desc.format)) {
        // Block-compressed (plan F3 Task 3): sample-only, supported by this feature level, base level a multiple of
        // the 4 x 4 block (D3D11 requirement for BC textures).
        if (!caps_.format(desc.format).sample) {
            return std::unexpected(Error{kExitUsage, std::format("d3d11: createTexture: {} is not supported by this "
                                                                 "device",
                                                                 toString(desc.format))});
        }
        if (any(desc.usage, TextureUsage::RenderTarget | TextureUsage::DepthStencil | TextureUsage::Storage)) {
            return std::unexpected(Error{kExitUsage, std::format("d3d11: createTexture: {} can only be sampled",
                                                                 toString(desc.format))});
        }
        if (desc.width % 4 != 0 || desc.height % 4 != 0) {
            return std::unexpected(Error{kExitUsage, std::format("d3d11: createTexture: {} needs a base level whose "
                                                                 "size is a multiple of 4 ({}x{})",
                                                                 toString(desc.format), desc.width, desc.height)});
        }
    }
    // A sampled depth texture (plan F3 Task 7: the shadow atlas) is typeless, with a depth view for the attachment and
    // a float colour view for sampling (D3D11 cannot bind D32_FLOAT / D24S8 as a shader resource).
    const bool sampledDepth = (desc.format == Format::D32_FLOAT || desc.format == Format::D24_UNORM_S8_UINT) &&
                              any(desc.usage, TextureUsage::Sampled);
    if (sampledDepth && (desc.arrayLayers > 1 || desc.mipLevels > 1)) {
        return std::unexpected(Error{kExitUsage, "d3d11: createTexture: a sampled depth texture must be 2D with one "
                                                 "layer and one mip"});
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = desc.width;
    td.Height = desc.height;
    td.MipLevels = desc.mipLevels;
    td.ArraySize = desc.arrayLayers;
    td.Format = !sampledDepth ? t.format
                : desc.format == Format::D32_FLOAT ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.MiscFlags = desc.cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    if (any(desc.usage, TextureUsage::Sampled)) {
        td.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    }
    if (any(desc.usage, TextureUsage::RenderTarget)) {
        td.BindFlags |= D3D11_BIND_RENDER_TARGET;
    }
    if (any(desc.usage, TextureUsage::DepthStencil)) {
        td.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
    }
    if (any(desc.usage, TextureUsage::Storage)) {
        td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    }
    // Initial data: every subresource, in D3D subresource order (layer * mipLevels + mip) = SubresourceData order.
    std::vector<D3D11_SUBRESOURCE_DATA> sub;
    if (!init.empty()) {
        if (init.size() != size_t(desc.mipLevels) * desc.arrayLayers) {
            return std::unexpected(Error{kExitUsage, std::format("d3d11: createTexture: {} initial subresources, the "
                                                                 "texture has {} (mips x layers)",
                                                                 init.size(), desc.mipLevels * desc.arrayLayers)});
        }
        for (size_t i = 0; i < init.size(); ++i) {
            const uint32_t mip = static_cast<uint32_t>(i % desc.mipLevels);
            const uint32_t w = std::max(1u, desc.width >> mip), h = std::max(1u, desc.height >> mip);
            // Rows of texels, or of 4 x 4 blocks for a compressed format.
            const uint32_t rowBytes = subresourceRowBytes(desc.format, w), rows = subresourceRows(desc.format, h);
            const uint32_t pitch = init[i].rowPitch ? init[i].rowPitch : rowBytes;
            if (pitch < rowBytes || init[i].data.size() < size_t(pitch) * (rows - 1) + rowBytes) {
                return std::unexpected(Error{kExitUsage, std::format("d3d11: createTexture: initial data of "
                                                                     "subresource {} is too small ({} bytes for {}x{})",
                                                                     i, init[i].data.size(), w, h)});
            }
            sub.push_back({init[i].data.data(), pitch, 0});
        }
    }
    if (HRESULT hr = device_->CreateTexture2D(&td, sub.empty() ? nullptr : sub.data(), &t.texture); FAILED(hr)) {
        return std::unexpected(hrError("CreateTexture2D", hr));
    }
    if (td.BindFlags & D3D11_BIND_RENDER_TARGET) {
        if (HRESULT hr = device_->CreateRenderTargetView(t.texture.Get(), nullptr, &t.rtv); FAILED(hr)) {
            return std::unexpected(hrError("CreateRenderTargetView", hr));
        }
        if (desc.mipLevels > 1 || desc.arrayLayers > 1) {
            // ColorAttachment::{mipLevel, arrayLayer}: a 2D-array RTV of one slice and mip each.
            for (uint32_t layer = 0; layer < desc.arrayLayers; ++layer) {
                for (uint32_t mip = 0; mip < desc.mipLevels; ++mip) {
                    D3D11_RENDER_TARGET_VIEW_DESC rd{};
                    rd.Format = t.format;
                    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                    rd.Texture2DArray.MipSlice = mip;
                    rd.Texture2DArray.FirstArraySlice = layer;
                    rd.Texture2DArray.ArraySize = 1;
                    ComPtr<ID3D11RenderTargetView> v;
                    if (HRESULT hr = device_->CreateRenderTargetView(t.texture.Get(), &rd, &v); FAILED(hr)) {
                        return std::unexpected(hrError("CreateRenderTargetView (subresource)", hr));
                    }
                    t.subRtvs.push_back(std::move(v));
                }
            }
        }
    }
    if (td.BindFlags & D3D11_BIND_DEPTH_STENCIL) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = t.format;
        dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        if (HRESULT hr = device_->CreateDepthStencilView(t.texture.Get(), sampledDepth ? &dv : nullptr, &t.dsv);
            FAILED(hr)) {
            return std::unexpected(hrError("CreateDepthStencilView", hr));
        }
    }
    if (td.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = desc.format == Format::D32_FLOAT ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = desc.mipLevels;
        if (HRESULT hr = device_->CreateShaderResourceView(t.texture.Get(), sampledDepth ? &sv : nullptr, &t.srv);
            FAILED(hr)) {
            return std::unexpected(hrError("CreateShaderResourceView", hr));
        }
    }
    if (!desc.debugName.empty()) {
        t.texture->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(desc.debugName.size()),
                                  desc.debugName.data());
    }
    drainMessages();
    return TextureHandle{textures_.add(std::move(t))};
}

Result<ShaderHandle> D3D11Device::createShader(const ShaderBlob& blob) {
    if (blob.backend != Backend::D3D11) {
        return std::unexpected(Error{kExitUsage, "d3d11: createShader with a " + std::string(toString(blob.backend)) +
                                                     " blob (" + blob.entry + ")"});
    }
    Shader s;
    s.stage = blob.stage;
    s.entry = blob.entry;
    s.code = blob.code;
    s.reflection = blob.reflection;
    // Plan F2 Task 7 (Review Focus 4): the shader model decides the tier. An SM 5.0 blob on a T0 (FL10_x)
    // device is not handed to the driver (CreateVertexShader would fail with a debug-layer error); the
    // pipeline that uses it is rejected with the tier named.
    if (s.reflection.shaderModel == "4_0" || s.reflection.shaderModel == "4_1") {
        s.requiredTier = 0;
    } else if (s.reflection.shaderModel == "5_0") {
        s.requiredTier = 1;
    } else if (s.reflection.shaderModel.empty()) {
        return std::unexpected(Error{kExitUsage, "d3d11: " + blob.entry +
                                                     ": reflection has no shaderModel (rebuild the shaders with this shaderc)"});
    } else {
        return std::unexpected(Error{kExitUsage, "d3d11: " + blob.entry + ": unsupported shaderModel " +
                                                     s.reflection.shaderModel});
    }
    if (s.requiredTier > caps_.tier) {
        return ShaderHandle{shaders_.add(std::move(s))};
    }
    HRESULT hr = E_INVALIDARG;
    if (blob.stage == ShaderStage::Vertex) {
        hr = device_->CreateVertexShader(s.code.data(), s.code.size(), nullptr, &s.vs);
    } else if (blob.stage == ShaderStage::Fragment) {
        hr = device_->CreatePixelShader(s.code.data(), s.code.size(), nullptr, &s.ps);
    } else {
        return std::unexpected(Error{kExitUsage, "d3d11: compute shaders are not part of F2"});
    }
    if (FAILED(hr)) {
        return std::unexpected(hrError(blob.stage == ShaderStage::Vertex ? "CreateVertexShader" : "CreatePixelShader", hr));
    }
    drainMessages();
    return ShaderHandle{shaders_.add(std::move(s))};
}

namespace {

// HLSL semantic with index ("TEXCOORD0") -> name + index, as D3D11 input elements want them.
std::pair<std::string, UINT> splitSemantic(const std::string& s) {
    size_t i = s.size();
    while (i > 0 && std::isdigit(static_cast<unsigned char>(s[i - 1]))) {
        --i;
    }
    const UINT index = i < s.size() ? static_cast<UINT>(std::stoul(s.substr(i))) : 0u;
    return {s.substr(0, i), index};
}

D3D11_COMPARISON_FUNC toD3D(CompareOp c) {
    switch (c) {
    case CompareOp::Never: return D3D11_COMPARISON_NEVER;
    case CompareOp::Less: return D3D11_COMPARISON_LESS;
    case CompareOp::LessEqual: return D3D11_COMPARISON_LESS_EQUAL;
    case CompareOp::Equal: return D3D11_COMPARISON_EQUAL;
    case CompareOp::Greater: return D3D11_COMPARISON_GREATER;
    case CompareOp::GreaterEqual: return D3D11_COMPARISON_GREATER_EQUAL;
    case CompareOp::Always: return D3D11_COMPARISON_ALWAYS;
    }
    return D3D11_COMPARISON_ALWAYS;
}

D3D11_PRIMITIVE_TOPOLOGY toD3D(PrimitiveTopology t) {
    switch (t) {
    case PrimitiveTopology::TriangleList: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case PrimitiveTopology::TriangleStrip: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case PrimitiveTopology::LineList: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case PrimitiveTopology::PointList: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    }
    return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

bool isMorphStorage(const ReflectedResource& r) {
    return (r.kind == "storage_ro" || r.kind == "storage_rw") && r.name.rfind("morph", 0) == 0;
}

} // namespace

Result<PipelineHandle> D3D11Device::createGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    Shader* vs = shaders_.tryGet(desc.vs.id);
    Shader* ps = shaders_.tryGet(desc.ps.id);
    if (!vs || !ps || vs->stage != ShaderStage::Vertex || ps->stage != ShaderStage::Fragment) {
        return std::unexpected(Error{kExitUsage, "d3d11: createGraphicsPipeline needs a vertex and a pixel shader"});
    }
    // F1 Risk 1: VS and PS of one pipeline must come from the same DXBC path (their signatures are packed
    // differently). shaderc records the path in the reflection (plan F2 Task 4); a blob without it is
    // rejected, so the check can never be skipped silently.
    for (const Shader* s : {vs, ps}) {
        if (s->reflection.dxbcPath.empty()) {
            return std::unexpected(Error{kExitUsage, "d3d11: " + s->entry +
                                                         ": reflection has no dxbcPath (rebuild the shaders with this shaderc)"});
        }
    }
    if (vs->reflection.dxbcPath != ps->reflection.dxbcPath) {
        return std::unexpected(Error{kExitUsage, "VS and PS from different dxbc paths"});
    }
    // Review Focus 4: a T1 pipeline (SM 5.0) on a T0 device (FL10_x) is rejected here, not by the driver.
    if (const int need = std::max(vs->requiredTier, ps->requiredTier); need > caps_.tier) {
        return std::unexpected(Error{kExitNoBackend, "pipeline needs TIER " + std::to_string(need) + ", device is TIER " +
                                                         std::to_string(caps_.tier)});
    }
    // Bind group layouts with each stage's own registers (plan F2 Task 4).
    auto bgl = deriveBindGroupLayouts(vs->reflection, ps->reflection);
    if (!bgl) {
        return std::unexpected(Error{bgl.error().exitCode, "d3d11: " + bgl.error().message});
    }

    Pipeline p;
    p.vs = vs->vs;
    p.ps = ps->ps;
    p.bindGroupLayouts = std::move(*bgl);
    p.uavStartSlot = ps->reflection.renderTargets;
    p.topology = toD3D(desc.topology);
    for (const VertexBufferLayout& b : desc.vertexLayout.buffers) {
        p.strides.push_back(b.stride);
    }
    // Input layout: one element per VS input (reflection order), fed by the attribute with that semantic.
    std::vector<std::pair<std::string, UINT>> names;
    names.reserve(vs->reflection.vertexInputs.size());
    std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
    for (const ReflectedVertexInput& in : vs->reflection.vertexInputs) {
        const auto it = std::find_if(desc.vertexLayout.attributes.begin(), desc.vertexLayout.attributes.end(),
                                     [&](const VertexAttribute& a) { return a.semantic == in.semantic; });
        if (it == desc.vertexLayout.attributes.end()) {
            return std::unexpected(Error{kExitUsage, "d3d11: vertex layout has no attribute for VS input " + in.semantic});
        }
        if (it->buffer >= p.strides.size()) {
            return std::unexpected(Error{kExitUsage, "d3d11: attribute " + it->semantic + " uses vertex buffer " +
                                                         std::to_string(it->buffer) + ", layout has " +
                                                         std::to_string(p.strides.size())});
        }
        names.push_back(splitSemantic(in.semantic));
        const bool instanced = desc.vertexLayout.buffers[it->buffer].perInstance;
        elements.push_back({nullptr, names.back().second, toDxgiFormat(it->format), it->buffer, it->offset,
                            instanced ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA, instanced ? 1u : 0u});
    }
    for (size_t i = 0; i < elements.size(); ++i) {
        elements[i].SemanticName = names[i].first.c_str();
    }
    if (!elements.empty()) {
        if (HRESULT hr = device_->CreateInputLayout(elements.data(), static_cast<UINT>(elements.size()), vs->code.data(),
                                                    vs->code.size(), &p.inputLayout);
            FAILED(hr)) {
            return std::unexpected(hrError("CreateInputLayout", hr));
        }
    }

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = desc.raster.cull == CullMode::None    ? D3D11_CULL_NONE
                  : desc.raster.cull == CullMode::Front ? D3D11_CULL_FRONT
                                                        : D3D11_CULL_BACK;
    rd.FrontCounterClockwise = desc.raster.frontFace == FrontFace::CounterClockwise;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    if (HRESULT hr = device_->CreateRasterizerState(&rd, &p.raster); FAILED(hr)) {
        return std::unexpected(hrError("CreateRasterizerState", hr));
    }
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = desc.depth.test;
    dd.DepthWriteMask = desc.depth.write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = toD3D(desc.depth.compare);
    if (HRESULT hr = device_->CreateDepthStencilState(&dd, &p.depth); FAILED(hr)) {
        return std::unexpected(hrError("CreateDepthStencilState", hr));
    }
    D3D11_BLEND_DESC bd{};
    bd.IndependentBlendEnable = TRUE;
    for (UINT i = 0; i < 8; ++i) {
        D3D11_RENDER_TARGET_BLEND_DESC& t = bd.RenderTarget[i];
        t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        t.SrcBlend = t.SrcBlendAlpha = D3D11_BLEND_ONE;
        t.DestBlend = t.DestBlendAlpha = D3D11_BLEND_ZERO;
        t.BlendOp = t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        if (i < desc.blend.size() && desc.blend[i].enable) {
            t.BlendEnable = TRUE;
            t.SrcBlend = D3D11_BLEND_SRC_ALPHA;
            t.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            t.SrcBlendAlpha = D3D11_BLEND_ONE;
            t.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        }
    }
    if (HRESULT hr = device_->CreateBlendState(&bd, &p.blend); FAILED(hr)) {
        return std::unexpected(hrError("CreateBlendState", hr));
    }
    p.vsHasMorphStorage = std::any_of(vs->reflection.resources.begin(), vs->reflection.resources.end(), isMorphStorage);
    drainMessages();
    return PipelineHandle{pipelines_.add(std::move(p))};
}

} // namespace eng::rhi::d3d11
