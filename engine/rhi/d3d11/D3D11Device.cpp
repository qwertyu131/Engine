// D3D11 device: adapter / WARP selection, feature-level cap, debug layer + info queue, frames,
// submission, readback (plan F2 Task 2).
#include "D3D11Common.h"

#include "core/Log.h"

#include <windows.h>

#include <cstring>
#include <format>
#include <thread>

namespace eng::rhi {
Result<std::unique_ptr<IDevice>> createD3D11Device(const DeviceDesc& desc) {
    return d3d11::D3D11Device::create(desc);
}
} // namespace eng::rhi

namespace eng::rhi::d3d11 {

std::string hrString(long hr) {
    return std::format("HRESULT 0x{:08X}", static_cast<unsigned long>(hr));
}

std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) {
        return {};
    }
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

DXGI_FORMAT toDxgiFormat(Format f) {
    switch (f) {
    case Format::Unknown: return DXGI_FORMAT_UNKNOWN;
    case Format::RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case Format::BGRA8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case Format::BGRA8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case Format::RGBA16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case Format::RGBA32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case Format::D32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
    case Format::D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case Format::R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case Format::RG32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
    case Format::RGB32_FLOAT: return DXGI_FORMAT_R32G32B32_FLOAT;
    case Format::R32_UINT: return DXGI_FORMAT_R32_UINT;
    case Format::RG32_UINT: return DXGI_FORMAT_R32G32_UINT;
    case Format::RGB32_UINT: return DXGI_FORMAT_R32G32B32_UINT;
    case Format::RGBA32_UINT: return DXGI_FORMAT_R32G32B32A32_UINT;
    case Format::R32_SINT: return DXGI_FORMAT_R32_SINT;
    case Format::RGBA32_SINT: return DXGI_FORMAT_R32G32B32A32_SINT;
    case Format::RGBA16_UINT: return DXGI_FORMAT_R16G16B16A16_UINT;
    case Format::RGBA8_UINT: return DXGI_FORMAT_R8G8B8A8_UINT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

Error D3D11Device::hrError(const char* what, long hr) {
    drainMessages();
    if (device_) {
        const HRESULT removed = device_->GetDeviceRemovedReason();
        if (FAILED(removed)) {
            deviceLost_ = true;
            return Error{kExitDeviceLost, std::format("d3d11: {} failed ({}); device removed: {}", what, hrString(hr),
                                                      hrString(removed))};
        }
    }
    if (validationErrors_ > 0) {
        Error e = validationError();
        e.message = std::format("d3d11: {} failed ({}); {}", what, hrString(hr), e.message);
        return e;
    }
    return Error{kExitUsage, std::format("d3d11: {} failed ({})", what, hrString(hr))};
}

Result<std::unique_ptr<IDevice>> D3D11Device::create(const DeviceDesc& desc) {
    std::unique_ptr<D3D11Device> dev(new D3D11Device());
    if (auto r = dev->init(desc); !r) {
        return std::unexpected(r.error());
    }
    return std::unique_ptr<IDevice>(std::move(dev));
}

Result<void> D3D11Device::init(const DeviceDesc& desc) {
    validation_ = desc.validation;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory_)))) {
        return std::unexpected(Error{kExitNoBackend, "d3d11: CreateDXGIFactory1 failed"});
    }
    ComPtr<IDXGIAdapter1> adapter;
    D3D_DRIVER_TYPE driver = desc.warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    if (desc.adapterIndex >= 0) {
        if (desc.warp) {
            return std::unexpected(Error{kExitUsage, "d3d11: --adapter and --warp are exclusive"});
        }
        if (FAILED(factory_->EnumAdapters1(static_cast<UINT>(desc.adapterIndex), &adapter))) {
            return std::unexpected(Error{kExitUsage, std::format("d3d11: --adapter {} out of range", desc.adapterIndex)});
        }
        driver = D3D_DRIVER_TYPE_UNKNOWN;
    }

    const D3D_FEATURE_LEVEL all[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                     D3D_FEATURE_LEVEL_10_0};
    std::vector<D3D_FEATURE_LEVEL> levels;
    for (D3D_FEATURE_LEVEL l : all) {
        if (desc.d3d11FeatureLevelCap == 0 || static_cast<int>(l) <= desc.d3d11FeatureLevelCap) {
            levels.push_back(l);
        }
    }
    if (levels.empty()) {
        return std::unexpected(
            Error{kExitUsage, std::format("d3d11: feature level cap 0x{:x} is below 10_0", desc.d3d11FeatureLevelCap)});
    }
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | (validation_ ? D3D11_CREATE_DEVICE_DEBUG : 0u);
    ComPtr<ID3D11Device> base;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(adapter.Get(), driver, nullptr, flags, levels.data(), static_cast<UINT>(levels.size()),
                                   D3D11_SDK_VERSION, &base, &got, &ctx);
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
        return std::unexpected(Error{kExitValidation, "validation layer not available: D3D11 debug layer "
                                                      "(D3D11SDKLayers.dll, Windows optional feature 'Graphics Tools')"});
    }
    if (FAILED(hr)) {
        return std::unexpected(Error{kExitNoBackend, "d3d11: D3D11CreateDevice failed (" + hrString(hr) + ")"});
    }
    if (FAILED(base.As(&device_)) || FAILED(ctx.As(&context_))) {
        return std::unexpected(Error{kExitNoBackend, "d3d11: ID3D11Device1 not available (D3D11.1 runtime needed)"});
    }
    if (validation_) {
        if (FAILED(device_.As(&infoQueue_))) {
            return std::unexpected(Error{kExitValidation, "validation layer not available: ID3D11InfoQueue"});
        }
        infoQueue_->SetMessageCountLimit(4096);
        infoQueue_->ClearStoredMessages();
    }
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    if (FAILED(device_->CreateQuery(&qd, &eventQuery_))) {
        return std::unexpected(Error{kExitNoBackend, "d3d11: CreateQuery(EVENT) failed"});
    }

    caps_.backend = Backend::D3D11;
    caps_.tier = got >= D3D_FEATURE_LEVEL_11_0 ? 1 : 0;
    caps_.apiVersion = std::format("{}_{}", (static_cast<int>(got) >> 12) & 0xF, (static_cast<int>(got) >> 8) & 0xF);
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> used;
    if (SUCCEEDED(device_.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&used))) {
        DXGI_ADAPTER_DESC ad{};
        used->GetDesc(&ad);
        caps_.adapterName = narrow(ad.Description);
        caps_.vendorId = ad.VendorId;
    }
    caps_.software = desc.warp;
    // Transient uniforms bind ranges of one dynamic constant buffer (VS/PSSetConstantBuffers1) and append
    // to it with MAP_WRITE_NO_OVERWRITE (plan F2 Task 4); both are optional D3D11.1 features.
    D3D11_FEATURE_DATA_D3D11_OPTIONS opts{};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opts, sizeof(opts)))) {
        constantBufferOffsetting_ = opts.ConstantBufferOffsetting != FALSE;
        mapNoOverwriteOnDynamicCb_ = opts.MapNoOverwriteOnDynamicConstantBuffer != FALSE;
    }
    cmdList_ = std::make_unique<D3D11CommandList>(*this);
    drainMessages();
    if (validationErrors_ > 0) {
        return std::unexpected(validationError());
    }
    ENG_LOG_INFO("d3d11: {} (FL {}){}; ConstantBufferOffsetting={} MapNoOverwriteOnDynamicConstantBuffer={}",
                 caps_.adapterName, caps_.apiVersion, validation_ ? ", debug layer" : "", constantBufferOffsetting_,
                 mapNoOverwriteOnDynamicCb_);
    return {};
}

D3D11Device::~D3D11Device() {
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
    bindGroups_.clear([](BindGroup&) {});
    samplers_.clear([](Sampler&) {});
    pipelines_.clear([](Pipeline&) {});
    shaders_.clear([](Shader&) {});
    textures_.clear([](Texture&) {});
    buffers_.clear([](Buffer&) {});
}

NativeHandles D3D11Device::nativeHandles() const {
    NativeHandles h;
    h.backend = Backend::D3D11;
    h.d3d11Device = device_.Get();
    h.d3d11Context = context_.Get();
    return h;
}

ICommandList& D3D11Device::beginFrame() {
    ENG_ASSERT_MSG(!recording_, "beginFrame while a frame is open");
    recording_ = true;
    resetTransient();
    cmdList_->begin();
    return *cmdList_;
}

Result<void> D3D11Device::submitAndWait() {
    if (deviceLost_) {
        return std::unexpected(Error{kExitDeviceLost, "d3d11: device removed"});
    }
    recording_ = false;
    flushTransient();
    context_->End(eventQuery_.Get());
    context_->Flush();
    BOOL done = FALSE;
    for (;;) {
        const HRESULT hr = context_->GetData(eventQuery_.Get(), &done, sizeof(done), 0);
        if (hr == S_OK && done) {
            break;
        }
        if (FAILED(hr)) {
            return std::unexpected(hrError("GetData(EVENT)", hr));
        }
        std::this_thread::yield();
    }
    const HRESULT removed = device_->GetDeviceRemovedReason();
    if (FAILED(removed)) {
        deviceLost_ = true;
        return std::unexpected(Error{kExitDeviceLost, "d3d11: device removed (" + hrString(removed) + ")"});
    }
    drainMessages();
    if (validationErrors_ > 0) {
        return std::unexpected(validationError());
    }
    return {};
}

void D3D11Device::destroy(Handle h) {
    // D3D11 keeps a released object alive until the GPU is done with it: release now.
    switch (h.type) {
    case HandleType::Buffer: buffers_.remove(h.id); break;
    case HandleType::Texture: textures_.remove(h.id); break;
    case HandleType::Shader: shaders_.remove(h.id); break;
    case HandleType::Pipeline: pipelines_.remove(h.id); break;
    case HandleType::Sampler: samplers_.remove(h.id); break;
    case HandleType::BindGroup: bindGroups_.remove(h.id); break;
    default: ENG_ASSERT_MSG(false, "destroy: handle type not supported by the D3D11 backend yet"); break;
    }
}

Result<SwapchainHandle> D3D11Device::createSwapchain(const SwapchainDesc&) {
    return std::unexpected(Error{kExitUsage, "d3d11: createSwapchain is implemented in plan F2 Task 6"});
}
Result<void> D3D11Device::present(SwapchainHandle) {
    return std::unexpected(Error{kExitUsage, "d3d11: present is implemented in plan F2 Task 6"});
}

Result<Image> D3D11Device::readback(TextureHandle handle) {
    Texture* t = textures_.get(handle.id);
    if (!t) {
        return std::unexpected(Error{kExitUsage, "d3d11: readback of an invalid texture handle"});
    }
    const Format fmt = t->desc.format;
    if (fmt != Format::RGBA8_UNORM && fmt != Format::RGBA8_SRGB && fmt != Format::BGRA8_UNORM &&
        fmt != Format::BGRA8_SRGB) {
        return std::unexpected(Error{kExitUsage, std::string("d3d11: readback of ") + toString(fmt) + " not supported"});
    }
    D3D11_TEXTURE2D_DESC sd{};
    t->texture->GetDesc(&sd);
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (HRESULT hr = device_->CreateTexture2D(&sd, nullptr, &staging); FAILED(hr)) {
        return std::unexpected(hrError("CreateTexture2D (readback staging)", hr));
    }
    context_->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, t->texture.Get(), 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (HRESULT hr = context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m); FAILED(hr)) {
        return std::unexpected(hrError("Map (readback)", hr));
    }
    Image img;
    img.width = t->desc.width;
    img.height = t->desc.height;
    img.rgba8.resize(size_t(img.width) * img.height * 4u);
    for (uint32_t y = 0; y < img.height; ++y) {
        std::memcpy(img.rgba8.data() + size_t(y) * img.width * 4u, static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch,
                    size_t(img.width) * 4u);
    }
    context_->Unmap(staging.Get(), 0);
    if (fmt == Format::BGRA8_UNORM || fmt == Format::BGRA8_SRGB) {
        for (size_t i = 0; i < img.rgba8.size(); i += 4) {
            std::swap(img.rgba8[i], img.rgba8[i + 2]);
        }
    }
    drainMessages();
    if (validationErrors_ > 0) {
        return std::unexpected(validationError());
    }
    return img;
}

} // namespace eng::rhi::d3d11
