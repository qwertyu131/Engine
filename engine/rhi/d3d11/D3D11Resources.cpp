// D3D11 resources: buffers, textures, shaders, graphics pipelines (plan F2 Task 2).
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
        bd.ByteWidth = (bd.ByteWidth + 15u) & ~15u;
    }
    if (any(desc.usage, BufferUsage::Storage)) {
        bd.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
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
    b.usage = desc.usage;
    if (HRESULT hr = device_->CreateBuffer(&bd, init.empty() ? nullptr : &sd, &b.buffer); FAILED(hr)) {
        return std::unexpected(hrError("CreateBuffer", hr));
    }
    if (!desc.debugName.empty()) {
        b.buffer->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(desc.debugName.size()), desc.debugName.data());
    }
    drainMessages();
    return BufferHandle{buffers_.add(std::move(b))};
}

Result<TextureHandle> D3D11Device::createTexture(const TextureDesc& desc, std::span<const SubresourceData> init) {
    if (!init.empty()) {
        return std::unexpected(Error{kExitUsage, "d3d11: texture initial data is implemented in plan F2 Task 4"});
    }
    if (desc.width == 0 || desc.height == 0 || desc.mipLevels == 0 || desc.arrayLayers == 0) {
        return std::unexpected(Error{kExitUsage, "d3d11: createTexture with a zero dimension"});
    }
    Texture t;
    t.desc = desc;
    t.format = toDxgiFormat(desc.format);
    if (t.format == DXGI_FORMAT_UNKNOWN) {
        return std::unexpected(Error{kExitUsage, "d3d11: createTexture with format Unknown"});
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = desc.width;
    td.Height = desc.height;
    td.MipLevels = desc.mipLevels;
    td.ArraySize = desc.arrayLayers;
    td.Format = t.format;
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
    if (HRESULT hr = device_->CreateTexture2D(&td, nullptr, &t.texture); FAILED(hr)) {
        return std::unexpected(hrError("CreateTexture2D", hr));
    }
    if (td.BindFlags & D3D11_BIND_RENDER_TARGET) {
        if (HRESULT hr = device_->CreateRenderTargetView(t.texture.Get(), nullptr, &t.rtv); FAILED(hr)) {
            return std::unexpected(hrError("CreateRenderTargetView", hr));
        }
    }
    if (td.BindFlags & D3D11_BIND_DEPTH_STENCIL) {
        if (HRESULT hr = device_->CreateDepthStencilView(t.texture.Get(), nullptr, &t.dsv); FAILED(hr)) {
            return std::unexpected(hrError("CreateDepthStencilView", hr));
        }
    }
    if (td.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
        if (HRESULT hr = device_->CreateShaderResourceView(t.texture.Get(), nullptr, &t.srv); FAILED(hr)) {
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
    // differently). Checked when the reflection records the path (plan F2 Task 4 adds it to shaderc).
    if (!vs->reflection.dxbcPath.empty() && !ps->reflection.dxbcPath.empty() &&
        vs->reflection.dxbcPath != ps->reflection.dxbcPath) {
        return std::unexpected(Error{kExitUsage, "VS and PS from different dxbc paths"});
    }
    for (const Shader* s : {vs, ps}) {
        if (!s->reflection.resources.empty()) {
            return std::unexpected(Error{kExitUsage, "d3d11: pipeline resources need bind groups (plan F2 Task 4): " +
                                                         s->entry + " uses '" + s->reflection.resources[0].name + "'"});
        }
    }

    Pipeline p;
    p.vs = vs->vs;
    p.ps = ps->ps;
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
