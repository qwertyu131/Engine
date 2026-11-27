// d3d11_linkcheck: draws one VS + PS DXBC pair on a D3D11 debug device and reports every debug-layer
// message (plan F1 Task 5 spike tool, committed in plan F2 Task 1; the F1 scratch source was not kept,
// this is a rewrite).
//
//   d3d11_linkcheck <vs.dxbc> <ps.dxbc> [--hardware]
//
// WARP by default (deterministic, no GPU needed). The input layout comes from the VS input signature;
// every constant buffer, SRV and sampler both shaders declare is bound with a dummy resource of the
// declared kind, so the only messages left are about the pair itself (VS -> PS linkage: e.g.
// "ERROR id=343 ... Semantic 'TEXCOORD' is defined for mismatched hardware registers").
//
// Exit codes: 0 = no debug-layer message of severity WARNING or above, 1 = at least one (all printed as
// "<SEVERITY> id=<n> <text>"), 2 = usage / IO / device error.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

std::vector<char> readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

DXGI_FORMAT formatOf(const D3D11_SIGNATURE_PARAMETER_DESC& p) {
    int n = 0;
    for (BYTE m = p.Mask; m; m >>= 1) {
        n += m & 1;
    }
    static const DXGI_FORMAT f32[] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT,
                                      DXGI_FORMAT_R32G32B32A32_FLOAT};
    static const DXGI_FORMAT u32[] = {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32B32_UINT,
                                      DXGI_FORMAT_R32G32B32A32_UINT};
    static const DXGI_FORMAT s32[] = {DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32B32_SINT,
                                      DXGI_FORMAT_R32G32B32A32_SINT};
    n = n < 1 ? 1 : n;
    switch (p.ComponentType) {
    case D3D_REGISTER_COMPONENT_UINT32: return u32[n - 1];
    case D3D_REGISTER_COMPONENT_SINT32: return s32[n - 1];
    default: return f32[n - 1];
    }
}

struct Bindings {
    std::vector<std::pair<UINT, ComPtr<ID3D11Buffer>>> cbuffers;
    std::vector<std::pair<UINT, ComPtr<ID3D11ShaderResourceView>>> srvs;
    std::vector<std::pair<UINT, ComPtr<ID3D11SamplerState>>> samplers;
};

bool makeBindings(ID3D11Device* dev, ID3D11ShaderReflection* refl, Bindings& out) {
    D3D11_SHADER_DESC sd{};
    refl->GetDesc(&sd);
    for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC b{};
        refl->GetResourceBindingDesc(i, &b);
        if (b.Type == D3D_SIT_CBUFFER) {
            D3D11_SHADER_BUFFER_DESC cb{};
            refl->GetConstantBufferByName(b.Name)->GetDesc(&cb);
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = (cb.Size + 15u) & ~15u;
            d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            d.Usage = D3D11_USAGE_DEFAULT;
            ComPtr<ID3D11Buffer> buf;
            if (FAILED(dev->CreateBuffer(&d, nullptr, &buf))) {
                return false;
            }
            out.cbuffers.emplace_back(b.BindPoint, buf);
        } else if (b.Type == D3D_SIT_SAMPLER) {
            D3D11_SAMPLER_DESC d{};
            d.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
            d.MaxLOD = D3D11_FLOAT32_MAX;
            d.ComparisonFunc = D3D11_COMPARISON_NEVER;
            ComPtr<ID3D11SamplerState> s;
            if (FAILED(dev->CreateSamplerState(&d, &s))) {
                return false;
            }
            out.samplers.emplace_back(b.BindPoint, s);
        } else if (b.Type == D3D_SIT_TEXTURE && (b.Dimension == D3D_SRV_DIMENSION_TEXTURE2D ||
                                                 b.Dimension == D3D_SRV_DIMENSION_TEXTURECUBE)) {
            const bool cube = b.Dimension == D3D_SRV_DIMENSION_TEXTURECUBE;
            D3D11_TEXTURE2D_DESC d{};
            d.Width = d.Height = 1;
            d.MipLevels = 1;
            d.ArraySize = cube ? 6 : 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
            ComPtr<ID3D11Texture2D> tex;
            ComPtr<ID3D11ShaderResourceView> srv;
            if (FAILED(dev->CreateTexture2D(&d, nullptr, &tex)) ||
                FAILED(dev->CreateShaderResourceView(tex.Get(), nullptr, &srv))) {
                return false;
            }
            out.srvs.emplace_back(b.BindPoint, srv);
        } else if (b.Type == D3D_SIT_STRUCTURED) {
            D3D11_BUFFER_DESC d{};
            d.StructureByteStride = b.NumSamples; // structured: NumSamples = stride
            d.ByteWidth = d.StructureByteStride * 64u;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            ComPtr<ID3D11Buffer> buf;
            ComPtr<ID3D11ShaderResourceView> srv;
            if (FAILED(dev->CreateBuffer(&d, nullptr, &buf)) ||
                FAILED(dev->CreateShaderResourceView(buf.Get(), nullptr, &srv))) {
                return false;
            }
            out.srvs.emplace_back(b.BindPoint, srv);
        } else if (b.Type == D3D_SIT_BYTEADDRESS) {
            // SPIRV-Cross (dxbc cross path) emits StructuredBuffer as ByteAddressBuffer.
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = 64u * 1024u;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = DXGI_FORMAT_R32_TYPELESS;
            v.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            v.BufferEx.NumElements = d.ByteWidth / 4u;
            v.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            ComPtr<ID3D11Buffer> buf;
            ComPtr<ID3D11ShaderResourceView> srv;
            if (FAILED(dev->CreateBuffer(&d, nullptr, &buf)) || FAILED(dev->CreateShaderResourceView(buf.Get(), &v, &srv))) {
                return false;
            }
            out.srvs.emplace_back(b.BindPoint, srv);
        } else {
            std::fprintf(stderr, "d3d11_linkcheck: resource '%s' (type %d) is not supported by this tool\n", b.Name,
                         static_cast<int>(b.Type));
            return false;
        }
    }
    return true;
}

const char* severityName(D3D11_MESSAGE_SEVERITY s) {
    switch (s) {
    case D3D11_MESSAGE_SEVERITY_CORRUPTION: return "CORRUPTION";
    case D3D11_MESSAGE_SEVERITY_ERROR: return "ERROR";
    case D3D11_MESSAGE_SEVERITY_WARNING: return "WARNING";
    case D3D11_MESSAGE_SEVERITY_INFO: return "INFO";
    default: return "MESSAGE";
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: d3d11_linkcheck <vs.dxbc> <ps.dxbc> [--hardware]\n");
        return 2;
    }
    const bool hardware = argc > 3 && std::wstring(argv[3]) == L"--hardware";
    const std::vector<char> vs = readFile(argv[1]);
    const std::vector<char> ps = readFile(argv[2]);
    if (vs.empty() || ps.empty()) {
        std::fprintf(stderr, "d3d11_linkcheck: cannot read the shader files\n");
        return 2;
    }

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr,
                                   D3D11_CREATE_DEVICE_DEBUG, levels, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
        std::fprintf(stderr, "d3d11_linkcheck: D3D11CreateDevice (debug) failed 0x%08lx (Graphics Tools installed?)\n",
                     static_cast<unsigned long>(hr));
        return 2;
    }
    ComPtr<ID3D11InfoQueue> iq;
    if (FAILED(dev.As(&iq))) {
        std::fprintf(stderr, "d3d11_linkcheck: no ID3D11InfoQueue on the debug device\n");
        return 2;
    }

    ComPtr<ID3D11VertexShader> vso;
    ComPtr<ID3D11PixelShader> pso;
    ComPtr<ID3D11ShaderReflection> vrefl, prefl;
    if (FAILED(dev->CreateVertexShader(vs.data(), vs.size(), nullptr, &vso)) ||
        FAILED(dev->CreatePixelShader(ps.data(), ps.size(), nullptr, &pso)) ||
        FAILED(D3DReflect(vs.data(), vs.size(), IID_PPV_ARGS(&vrefl))) ||
        FAILED(D3DReflect(ps.data(), ps.size(), IID_PPV_ARGS(&prefl)))) {
        std::fprintf(stderr, "d3d11_linkcheck: shader creation / reflection failed\n");
        return 2;
    }

    // Input layout: one element per VS input-signature entry, all in slot 0, tightly packed.
    D3D11_SHADER_DESC vsd{};
    vrefl->GetDesc(&vsd);
    std::vector<D3D11_SIGNATURE_PARAMETER_DESC> params(vsd.InputParameters);
    std::vector<D3D11_INPUT_ELEMENT_DESC> layout;
    for (UINT i = 0; i < vsd.InputParameters; ++i) {
        vrefl->GetInputParameterDesc(i, &params[i]);
        if (params[i].SystemValueType != D3D_NAME_UNDEFINED) {
            continue; // SV_VertexID / SV_InstanceID: generated, no vertex data
        }
        layout.push_back({params[i].SemanticName, params[i].SemanticIndex, formatOf(params[i]), 0,
                          D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0});
    }
    ComPtr<ID3D11InputLayout> il;
    if (!layout.empty() &&
        FAILED(dev->CreateInputLayout(layout.data(), static_cast<UINT>(layout.size()), vs.data(), vs.size(), &il))) {
        std::fprintf(stderr, "d3d11_linkcheck: CreateInputLayout failed\n");
    }
    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth = 64u * 3u * 16u; // 3 vertices of up to 64 x float4
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    ComPtr<ID3D11Buffer> vb;
    dev->CreateBuffer(&vbd, nullptr, &vb);
    UINT stride = 0;
    for (const auto& e : layout) {
        stride += e.Format == DXGI_FORMAT_R32_FLOAT || e.Format == DXGI_FORMAT_R32_UINT || e.Format == DXGI_FORMAT_R32_SINT ? 4
                  : e.Format == DXGI_FORMAT_R32G32_FLOAT || e.Format == DXGI_FORMAT_R32G32_UINT || e.Format == DXGI_FORMAT_R32G32_SINT ? 8
                  : e.Format == DXGI_FORMAT_R32G32B32_FLOAT || e.Format == DXGI_FORMAT_R32G32B32_UINT || e.Format == DXGI_FORMAT_R32G32B32_SINT ? 12
                                                                                                                                                  : 16;
    }

    Bindings vb_, pb_;
    if (!makeBindings(dev.Get(), vrefl.Get(), vb_) || !makeBindings(dev.Get(), prefl.Get(), pb_)) {
        return 2;
    }

    // Render targets: one per PS output (SV_Target<n>).
    D3D11_SHADER_DESC psd{};
    prefl->GetDesc(&psd);
    std::vector<ComPtr<ID3D11RenderTargetView>> rtvs;
    std::vector<ID3D11RenderTargetView*> rawRtvs;
    for (UINT i = 0; i < psd.OutputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC o{};
        prefl->GetOutputParameterDesc(i, &o);
        if (o.SystemValueType != D3D_NAME_TARGET) {
            continue;
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 4;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> t;
        ComPtr<ID3D11RenderTargetView> rtv;
        dev->CreateTexture2D(&td, nullptr, &t);
        dev->CreateRenderTargetView(t.Get(), nullptr, &rtv);
        if (rawRtvs.size() <= o.SemanticIndex) {
            rawRtvs.resize(o.SemanticIndex + 1, nullptr);
            rtvs.resize(o.SemanticIndex + 1);
        }
        rtvs[o.SemanticIndex] = rtv;
        rawRtvs[o.SemanticIndex] = rtv.Get();
    }

    iq->ClearStoredMessages();
    ctx->IASetInputLayout(il.Get());
    ID3D11Buffer* vbs[] = {vb.Get()};
    const UINT offset = 0;
    ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vso.Get(), nullptr, 0);
    ctx->PSSetShader(pso.Get(), nullptr, 0);
    auto bind = [](ID3D11DeviceContext* c, const Bindings& b, bool vsStage) {
        for (const auto& [slot, buf] : b.cbuffers) {
            ID3D11Buffer* p = buf.Get();
            vsStage ? c->VSSetConstantBuffers(slot, 1, &p) : c->PSSetConstantBuffers(slot, 1, &p);
        }
        for (const auto& [slot, srv] : b.srvs) {
            ID3D11ShaderResourceView* p = srv.Get();
            vsStage ? c->VSSetShaderResources(slot, 1, &p) : c->PSSetShaderResources(slot, 1, &p);
        }
        for (const auto& [slot, s] : b.samplers) {
            ID3D11SamplerState* p = s.Get();
            vsStage ? c->VSSetSamplers(slot, 1, &p) : c->PSSetSamplers(slot, 1, &p);
        }
    };
    bind(ctx.Get(), vb_, true);
    bind(ctx.Get(), pb_, false);
    ctx->OMSetRenderTargets(static_cast<UINT>(rawRtvs.size()), rawRtvs.data(), nullptr);
    D3D11_VIEWPORT vp{0, 0, 4, 4, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->Draw(3, 0);
    ctx->Flush();

    int bad = 0;
    const UINT64 n = iq->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        iq->GetMessage(i, nullptr, &len);
        std::vector<char> buf(len);
        auto* m = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
        if (FAILED(iq->GetMessage(i, m, &len))) {
            continue;
        }
        if (m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
            ++bad;
        }
        std::printf("%s id=%d %.*s\n", severityName(m->Severity), static_cast<int>(m->ID),
                    static_cast<int>(m->DescriptionByteLength), m->pDescription);
    }
    std::printf("d3d11_linkcheck: %d message(s) of severity WARNING or above\n", bad);
    return bad ? 1 : 0;
}
