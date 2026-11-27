// D3D11 swapchain (plan F2 Task 6): DXGI flip model (DXGI_SWAP_EFFECT_FLIP_DISCARD).
//   - buffers are UNORM (the flip model rejects sRGB buffer formats); an sRGB format gets an sRGB RTV;
//   - vsync on: Present(1, 0); off: Present(0, DXGI_PRESENT_ALLOW_TEARING) when the factory supports
//     tearing (DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING is then set on create and on every ResizeBuffers);
//   - ResizeBuffers runs after every reference to the back buffer is released (RHI texture + OM binding).
#include "D3D11Common.h"

#include "core/Assert.h"
#include "core/Log.h"

#include <format>

namespace eng::rhi::d3d11 {

namespace {

DXGI_FORMAT bufferFormat(Format f) {
    switch (f) {
    case Format::BGRA8_SRGB:
    case Format::BGRA8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case Format::RGBA8_SRGB:
    case Format::RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

} // namespace

Result<void> D3D11Device::wrapBackBuffer(Swapchain& sc) {
    Texture t;
    if (HRESULT hr = sc.swapchain->GetBuffer(0, IID_PPV_ARGS(&t.texture)); FAILED(hr)) {
        return std::unexpected(hrError("IDXGISwapChain::GetBuffer", hr));
    }
    t.desc.width = sc.desc.width;
    t.desc.height = sc.desc.height;
    t.desc.format = sc.desc.format;
    t.desc.usage = TextureUsage::RenderTarget | TextureUsage::CopySrc;
    t.desc.debugName = "swapchain back buffer";
    t.format = toDxgiFormat(sc.desc.format); // view format (sRGB for an sRGB swapchain)
    t.swapchainImage = true;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = t.format;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (HRESULT hr = device_->CreateRenderTargetView(t.texture.Get(), &rd, &t.rtv); FAILED(hr)) {
        return std::unexpected(hrError("CreateRenderTargetView (back buffer)", hr));
    }
    sc.backBuffer = TextureHandle{textures_.add(std::move(t))};
    drainMessages();
    return {};
}

Result<SwapchainHandle> D3D11Device::createSwapchain(const SwapchainDesc& desc) {
    if (!desc.nativeWindow) {
        return std::unexpected(Error{kExitUsage, "d3d11: createSwapchain without a native window"});
    }
    const DXGI_FORMAT fmt = bufferFormat(desc.format);
    if (fmt == DXGI_FORMAT_UNKNOWN) {
        return std::unexpected(Error{kExitUsage, std::string("d3d11: swapchain format ") + toString(desc.format) +
                                                     " not supported (RGBA8 / BGRA8 UNORM or SRGB)"});
    }
    // The factory that created the device (WARP and adapter selection included).
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(device_.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        return std::unexpected(Error{kExitNoBackend, "d3d11: IDXGIFactory2 not available (DXGI 1.2 needed)"});
    }
    Swapchain sc;
    sc.desc = desc;
    ComPtr<IDXGIFactory5> factory5;
    BOOL tearing = FALSE;
    if (SUCCEEDED(factory.As(&factory5)) &&
        SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) {
        sc.tearing = tearing != FALSE;
    }
    sc.flags = sc.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
    // A zero-size window (minimised) still gets a swapchain: DXGI takes the size from the window (8x8 for
    // an empty client area); the RHI reports no image until a resize to a non-zero size.
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = desc.width;
    sd.Height = desc.height;
    sd.Format = fmt;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = std::max(2u, desc.imageCount);
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = sc.flags;
    const HWND hwnd = static_cast<HWND>(desc.nativeWindow);
    if (HRESULT hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &sd, nullptr, nullptr, &sc.swapchain);
        FAILED(hr)) {
        return std::unexpected(hrError("CreateSwapChainForHwnd", hr));
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (desc.width > 0 && desc.height > 0) {
        if (auto r = wrapBackBuffer(sc); !r) {
            return std::unexpected(r.error());
        }
    } else {
        sc.desc.width = sc.desc.height = 0;
    }
    ENG_LOG_INFO("d3d11: swapchain {}x{}, {} buffers, {}, FLIP_DISCARD, tearing {}", sc.desc.width, sc.desc.height,
                 sd.BufferCount, toString(desc.format), sc.tearing ? "supported" : "not supported");
    drainMessages();
    if (validationErrors_ > 0) {
        return std::unexpected(validationError());
    }
    return SwapchainHandle{swapchains_.add(std::move(sc))};
}

Result<TextureHandle> D3D11Device::acquireNextImage(SwapchainHandle handle) {
    ENG_ASSERT_MSG(recording_, "acquireNextImage outside beginFrame / present");
    Swapchain* sc = swapchains_.get(handle.id);
    if (!sc) {
        return std::unexpected(Error{kExitUsage, "d3d11: acquireNextImage with an invalid swapchain"});
    }
    return sc->backBuffer; // FLIP_DISCARD: buffer 0 is always the one to draw into; null while size 0
}

Result<void> D3D11Device::resizeSwapchain(SwapchainHandle handle, uint32_t width, uint32_t height) {
    ENG_ASSERT_MSG(!recording_, "resizeSwapchain inside a frame");
    Swapchain* sc = swapchains_.get(handle.id);
    if (!sc) {
        return std::unexpected(Error{kExitUsage, "d3d11: resizeSwapchain with an invalid swapchain"});
    }
    if (width == 0 || height == 0) {
        return {}; // minimised: nothing to do, rendering skips
    }
    if (sc->backBuffer && sc->desc.width == width && sc->desc.height == height) {
        return {};
    }
    // Every reference to the buffers goes first: the OM binding and our RTV / texture.
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    if (sc->backBuffer) {
        textures_.remove(sc->backBuffer.id);
        sc->backBuffer = {};
    }
    context_->Flush();
    if (HRESULT hr = sc->swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, sc->flags); FAILED(hr)) {
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            deviceLost_ = true;
            return std::unexpected(Error{kExitDeviceLost, "d3d11: ResizeBuffers: device removed (" + hrString(hr) + ")"});
        }
        return std::unexpected(hrError("ResizeBuffers", hr));
    }
    sc->desc.width = width;
    sc->desc.height = height;
    if (auto r = wrapBackBuffer(*sc); !r) {
        return r;
    }
    drainMessages();
    if (validationErrors_ > 0) {
        return std::unexpected(validationError());
    }
    return {};
}

Result<void> D3D11Device::setSwapchainVsync(SwapchainHandle handle, bool vsync) {
    Swapchain* sc = swapchains_.get(handle.id);
    if (!sc) {
        return std::unexpected(Error{kExitUsage, "d3d11: setSwapchainVsync with an invalid swapchain"});
    }
    sc->desc.vsync = vsync; // applied by the next Present
    return {};
}

Result<void> D3D11Device::present(SwapchainHandle handle) {
    if (deviceLost_) {
        return std::unexpected(Error{kExitDeviceLost, "d3d11: device removed"});
    }
    if (!recording_) {
        return std::unexpected(Error{kExitUsage, "d3d11: present without beginFrame"});
    }
    Swapchain* sc = swapchains_.get(handle.id);
    if (!sc) {
        return std::unexpected(Error{kExitUsage, "d3d11: present with an invalid swapchain"});
    }
    recording_ = false;
    flushTransient();
    if (sc->backBuffer) {
        const UINT interval = sc->desc.vsync ? 1u : 0u;
        const UINT flags = (!sc->desc.vsync && sc->tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
        const HRESULT hr = sc->swapchain->Present(interval, flags);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            deviceLost_ = true;
            return std::unexpected(Error{kExitDeviceLost, "d3d11: Present: device removed (" +
                                                              hrString(device_->GetDeviceRemovedReason()) + ")"});
        }
        if (FAILED(hr)) {
            return std::unexpected(hrError("Present", hr));
        }
    } else {
        context_->Flush();
    }
    drainMessages();
    if (validationErrors_ > 0) {
        return std::unexpected(validationError());
    }
    return {};
}

} // namespace eng::rhi::d3d11
