// eng::Renderer facade (plan F3 Task 4, spec K-D). See include/engine/Renderer.h and docs/facade-changelog.md.
#include <engine/Renderer.h>

#include "Passes.h"
#include "asset/GltfLoader.h"
#include "builtin/Scenes.h"
#include "core/Log.h"
#include "scene/Scene.h"
#if defined(_WIN32)
#include "ui/ImGuiRenderer.h"
#include <imgui.h>
#endif

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <map>
#include <optional>

namespace eng {

// RHI names used below (using-declarations, not a using-directive: eng::Image and rhi::Image both exist).
using rhi::Backend;
using rhi::ColorAttachment;
using rhi::DeviceCaps;
using rhi::DeviceDesc;
using rhi::Format;
using rhi::ICommandList;
using rhi::IDevice;
using rhi::RenderPassDesc;
using rhi::ResourceState;
using rhi::SwapchainDesc;
using rhi::SwapchainHandle;
using rhi::TextureBarrier;
using rhi::TextureDesc;
using rhi::TextureHandle;
using rhi::TextureUsage;
using rhi::createDevice;
using rhi::isSrgbFormat;

namespace {

float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

std::string u8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// "r,g,b" with each component in [0, 1]; nullopt otherwise.
std::optional<std::array<float, 3>> parseColor(std::string_view s) {
    std::array<float, 3> c{};
    size_t pos = 0;
    for (int k = 0; k < 3; ++k) {
        const size_t end = k < 2 ? s.find(',', pos) : s.size();
        if (end == std::string_view::npos) {
            return std::nullopt;
        }
        std::string_view part = s.substr(pos, end - pos);
        while (!part.empty() && part.front() == ' ') {
            part.remove_prefix(1);
        }
        while (!part.empty() && part.back() == ' ') {
            part.remove_suffix(1);
        }
        const auto [p, ec] = std::from_chars(part.data(), part.data() + part.size(), c[k]);
        if (ec != std::errc() || p != part.data() + part.size() || !(c[k] >= 0.0f && c[k] <= 1.0f)) {
            return std::nullopt;
        }
        pos = end + 1;
    }
    return c;
}

} // namespace

struct Renderer::Impl {
    RendererDesc desc;
    std::unique_ptr<IDevice> dev;
    Settings settings;
    CVar* background = nullptr;
    CVar* backgroundColor = nullptr;
    uint32_t width = 1, height = 1;
    // Offscreen target.
    TextureHandle target;
    Format targetFormat = Format::RGBA8_UNORM;
    ResourceState targetState = ResourceState::Undefined;
    bool rendered = false;
    // Window.
    SwapchainHandle swapchain;
    Format swapchainFormat = Format::BGRA8_SRGB;
    bool captureNext = false;
    std::optional<Image> captured;
#if defined(_WIN32)
    std::unique_ptr<ui::ImGuiRenderer> ui;
#endif
    bool uiOpen = false;

    struct Asset {
        AssetInfo info;
        std::unique_ptr<asset::ModelData> model; // null for a built-in scene
        std::string builtin;
    };
    struct SceneEntry {
        uint32_t asset = 0;
        Format format = Format::RGBA8_UNORM;
        std::unique_ptr<scene::SceneInstance> instance;
        std::unique_ptr<renderer::ModelRenderer> model;
        std::unique_ptr<scenes::Scene> builtin;
    };
    std::map<uint32_t, Asset> assets;
    std::map<uint32_t, SceneEntry> scenes;
    uint32_t nextAsset = 1, nextScene = 1, active = 0;
    Camera camera;
    float time = 0.0f;

    bool windowed() const { return desc.window.handle != nullptr; }

    ~Impl() {
        if (dev) {
            (void)dev->submitAndWait();
        }
        scenes.clear();
#if defined(_WIN32)
        ui.reset();
#endif
        if (dev) {
            if (target) {
                dev->destroy(target);
            }
            if (swapchain) {
                dev->destroy(swapchain);
            }
        }
    }

    // Colour format a scene renders into: the swapchain's, or per scene kind offscreen (builtin:textured-cube RGBA8_SRGB,
    // as its F2 golden; every other scene RGBA8_UNORM + the shader's gamma 2.2 encode, as Sample-Viewer).
    Format formatFor(const Asset& a) const {
        if (windowed()) {
            return swapchainFormat;
        }
        return a.builtin == scenes::kTexturedCube ? Format::RGBA8_SRGB : Format::RGBA8_UNORM;
    }

    Result<void> ensureTarget(Format f) {
        if (windowed()) {
            return {};
        }
        if (target && targetFormat == f) {
            return {};
        }
        if (target) {
            (void)dev->submitAndWait();
            dev->destroy(target);
            target = {};
        }
        TextureDesc td;
        td.width = width;
        td.height = height;
        td.format = f;
        td.usage = TextureUsage::RenderTarget | TextureUsage::CopySrc;
        td.debugName = "renderer offscreen target";
        auto t = dev->createTexture(td);
        if (!t) {
            return std::unexpected(t.error());
        }
        target = *t;
        targetFormat = f;
        targetState = ResourceState::Undefined;
        return {};
    }

    void clearValue(Format f, float out[4]) const {
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        if (background->getString() == "Color") {
            auto c = parseColor(backgroundColor->getString());
            if (!c) {
                c = std::array<float, 3>{0.0f, 0.0f, 0.0f};
            }
            for (int k = 0; k < 3; ++k) {
                // r.Background.Color is a display (sRGB-encoded) colour: an sRGB target stores it after the hardware
                // encode of the linear value; an UNORM target stores it as is (the shader encodes gamma itself).
                out[k] = isSrgbFormat(f) ? srgbToLinear((*c)[k]) : (*c)[k];
            }
        }
    }

    Result<void> recordScene(ICommandList& cmd, TextureHandle color, Format f) {
        auto it = scenes.find(active);
        if (it == scenes.end()) {
            // No scene: the background colour only.
            RenderPassDesc rp;
            ColorAttachment ca;
            ca.texture = color;
            clearValue(f, ca.clear);
            rp.colors.push_back(ca);
            cmd.beginRenderPass(rp);
            cmd.endRenderPass();
            return {};
        }
        SceneEntry& s = it->second;
        if (s.builtin) {
            s.builtin->setTime(time);
            return s.builtin->record(cmd, color);
        }
        renderer::FrameParams fp;
        const scene::Vec3 eye{camera.position[0], camera.position[1], camera.position[2]};
        fp.view = scene::lookAt(eye, {camera.target[0], camera.target[1], camera.target[2]},
                                {camera.up[0], camera.up[1], camera.up[2]});
        fp.aspect = float(width) / float(height);
        fp.yfov = camera.yfov;
        fp.proj = scene::perspectiveRhZo(camera.yfov, fp.aspect, camera.znear, camera.zfar);
        fp.cameraPosition = eye;
        fp.gammaEncode = !isSrgbFormat(f);
        fp.environmentBackground = background->getString() != "Color";
        clearValue(f, fp.clear);
        return s.model->record(cmd, color, fp);
    }
};

Renderer::Renderer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Renderer::~Renderer() = default;

Result<std::unique_ptr<Renderer>> Renderer::create(const RendererDesc& desc) {
    auto impl = std::make_unique<Impl>();
    impl->desc = desc;
    impl->width = std::max(1u, desc.width);
    impl->height = std::max(1u, desc.height);
    CVarRegistry& reg = CVarRegistry::get();
    // r.Background / r.Background.Color (the Task 5 cvar names; Task 4 needs a known background for its probes).
    impl->background = reg.find("r.Background");
    if (!impl->background) {
        impl->background = reg.registerString("r.Background", "Environment",
                                              "glTF scenes: Environment (skybox of the IBL environment) or Color "
                                              "(r.Background.Color)");
    }
    impl->backgroundColor = reg.find("r.Background.Color");
    if (!impl->backgroundColor) {
        impl->backgroundColor = reg.registerString("r.Background.Color", "0,0,0",
                                                   "Background colour with r.Background=Color: r,g,b display values in "
                                                   "[0, 1] (sRGB-encoded)");
    }
    (void)asset::loadOptionsFromCVars(); // registers r.Asset.* so hosts can set them before loading
    DeviceDesc dd;
    dd.backend = desc.backend;
    dd.validation = desc.validation;
    dd.warp = desc.warp;
    dd.adapterIndex = desc.adapterIndex;
    dd.d3d11FeatureLevelCap = desc.d3d11FeatureLevelCap;
    auto dev = createDevice(dd);
    if (!dev) {
        return std::unexpected(dev.error());
    }
    impl->dev = std::move(*dev);
    if (impl->windowed()) {
        SwapchainDesc sd;
        sd.nativeWindow = desc.window.handle;
        sd.width = desc.width;
        sd.height = desc.height;
        sd.format = impl->swapchainFormat;
        sd.vsync = desc.vsync;
        auto sc = impl->dev->createSwapchain(sd);
        if (!sc) {
            return std::unexpected(sc.error());
        }
        impl->swapchain = *sc;
#if defined(_WIN32)
        if (desc.debugUi) {
            auto ui = ui::ImGuiRenderer::create(*impl->dev, desc.shaderDir, impl->swapchainFormat);
            if (!ui) {
                return std::unexpected(ui.error());
            }
            impl->ui = std::move(*ui);
        }
#endif
    }
    return std::unique_ptr<Renderer>(new Renderer(std::move(impl)));
}

std::span<const Backend> Renderer::compiledBackends() {
    return rhi::compiledBackends();
}

std::span<const std::string_view> Renderer::builtinScenes() {
    return scenes::sceneNames();
}

Result<AssetHandle> Renderer::loadAsset(const std::filesystem::path& path) {
    Impl& m = *impl_;
    Impl::Asset a;
    const std::string src = u8(path);
    a.info.source = src;
    if (src.rfind("builtin:", 0) == 0) {
        const auto names = scenes::sceneNames();
        if (std::find(names.begin(), names.end(), std::string_view(src)) == names.end()) {
            return std::unexpected(Error{2, "unknown scene " + src});
        }
        a.builtin = src;
        a.info.builtin = true;
        a.info.sceneCount = 1;
    } else {
        std::vector<asset::Diagnostic> diags;
        auto model = asset::loadGltf(path, asset::loadOptionsFromCVars(), &diags);
        for (const asset::Diagnostic& d : diags) {
            a.info.messages.push_back((d.level == asset::Diagnostic::Warning ? "[warning] " : "[info] ") + d.message);
            if (d.level == asset::Diagnostic::Warning) {
                ENG_LOG_WARN("{}: {}", src, d.message);
            }
        }
        if (!model) {
            return std::unexpected(model.error());
        }
        a.info.sceneCount = static_cast<uint32_t>(model->scenes.size());
        a.info.defaultScene = model->defaultScene;
        a.info.extensionsUsed = model->extensionsUsed;
        a.model = std::make_unique<asset::ModelData>(std::move(*model));
    }
    const uint32_t id = m.nextAsset++;
    m.assets[id] = std::move(a);
    return AssetHandle{id};
}

Result<SceneHandle> Renderer::instantiate(AssetHandle handle, int sceneIndex) {
    Impl& m = *impl_;
    auto it = m.assets.find(handle.id);
    if (it == m.assets.end()) {
        return std::unexpected(Error{2, std::format("instantiate: unknown asset {}", handle.id)});
    }
    Impl::Asset& a = it->second;
    Impl::SceneEntry s;
    s.asset = handle.id;
    s.format = m.formatFor(a);
    if (!a.builtin.empty()) {
        scenes::SceneDesc sd;
        sd.shaderDir = m.desc.shaderDir;
        sd.assetDir = m.desc.assetDir;
        sd.colorFormat = s.format;
        sd.width = m.width;
        sd.height = m.height;
        auto b = scenes::createScene(*m.dev, a.builtin, sd);
        if (!b) {
            return std::unexpected(b.error());
        }
        s.builtin = std::move(*b);
    } else {
        auto inst = scene::instantiate(*a.model, sceneIndex);
        if (!inst) {
            return std::unexpected(inst.error());
        }
        s.instance = std::make_unique<scene::SceneInstance>(std::move(*inst));
        // IBL and pipelines are made here, on the first glTF scene, not at Renderer::create (a run with only
        // built-in scenes does the same GPU work as before the facade).
        std::vector<asset::Diagnostic> diags;
        auto mr = renderer::ModelRenderer::create(*m.dev, m.desc.shaderDir, s.format, *a.model, *s.instance, diags);
        for (const asset::Diagnostic& d : diags) {
            a.info.messages.push_back((d.level == asset::Diagnostic::Warning ? "[warning] " : "[info] ") + d.message);
            if (d.level == asset::Diagnostic::Warning) {
                ENG_LOG_WARN("{}: {}", a.info.source, d.message);
            }
        }
        if (!mr) {
            return std::unexpected(mr.error());
        }
        s.model = std::move(*mr);
        if (auto r = s.model->resize(m.width, m.height); !r) {
            return std::unexpected(r.error());
        }
    }
    const uint32_t id = m.nextScene++;
    m.scenes[id] = std::move(s);
    m.active = id;
    return SceneHandle{id};
}

void Renderer::setActiveScene(SceneHandle scene) {
    if (impl_->scenes.count(scene.id)) {
        impl_->active = scene.id;
    }
}

Camera Renderer::fitCamera(SceneHandle handle, float aspect) const {
    Camera c;
    auto it = impl_->scenes.find(handle.id);
    if (it == impl_->scenes.end() || !it->second.instance) {
        return c;
    }
    const scene::FitCamera f = scene::fitCamera(*it->second.instance, aspect);
    c.position = f.position;
    c.target = f.target;
    c.up = f.up;
    c.yfov = f.yfov;
    c.znear = f.znear;
    c.zfar = f.zfar;
    return c;
}

void Renderer::setCamera(const Camera& camera) {
    impl_->camera = camera;
}

void Renderer::setTime(float seconds) {
    impl_->time = seconds;
}

Result<void> Renderer::renderFrame() {
    Impl& m = *impl_;
    Format f = m.windowed() ? m.swapchainFormat : Format::RGBA8_UNORM;
    if (auto it = m.scenes.find(m.active); it != m.scenes.end()) {
        f = it->second.format;
    }
    if (!m.windowed()) {
        if (auto r = m.ensureTarget(f); !r) {
            return r;
        }
        ICommandList& cmd = m.dev->beginFrame();
        const TextureBarrier toRt{m.target, m.targetState, ResourceState::RenderTarget};
        cmd.barrier({&toRt, 1});
        Result<void> rec = m.recordScene(cmd, m.target, f);
        const TextureBarrier toCopy{m.target, ResourceState::RenderTarget, ResourceState::CopySrc};
        cmd.barrier({&toCopy, 1});
        m.targetState = ResourceState::CopySrc;
        Result<void> sub = m.dev->submitAndWait();
        if (!rec) {
            return rec;
        }
        if (!sub) {
            return sub;
        }
        m.rendered = true;
        return {};
    }
    // Window.
    ICommandList& cmd = m.dev->beginFrame();
    auto bb = m.dev->acquireNextImage(m.swapchain);
    if (!bb) {
        (void)m.dev->present(m.swapchain);
        return std::unexpected(bb.error());
    }
    if (!*bb) { // nothing to draw (zero size / swapchain being rebuilt)
#if defined(_WIN32)
        if (m.uiOpen) {
            ImGui::EndFrame();
            m.uiOpen = false;
        }
#endif
        return m.dev->present(m.swapchain);
    }
    const TextureBarrier toTarget{*bb, ResourceState::Undefined, ResourceState::RenderTarget};
    cmd.barrier({&toTarget, 1});
    Result<void> rec = m.recordScene(cmd, *bb, f);
#if defined(_WIN32)
    if (m.uiOpen) {
        if (rec) {
            rec = m.ui->render(cmd, *bb, m.width, m.height);
        } else {
            ImGui::EndFrame();
        }
        m.uiOpen = false;
    }
#endif
    Result<void> pres;
    if (rec && m.captureNext) {
        m.captureNext = false;
        const TextureBarrier toCopy{*bb, ResourceState::RenderTarget, ResourceState::CopySrc};
        cmd.barrier({&toCopy, 1});
        pres = m.dev->submitAndWait();
        if (pres) {
            auto img = m.dev->readback(*bb);
            if (!img) {
                pres = std::unexpected(img.error());
            } else {
                m.captured = Image{img->width, img->height, std::move(img->rgba8)};
            }
        }
        ICommandList& cmd2 = m.dev->beginFrame();
        (void)m.dev->acquireNextImage(m.swapchain); // the same, still acquired image
        const TextureBarrier toPresent{*bb, ResourceState::CopySrc, ResourceState::Present};
        cmd2.barrier({&toPresent, 1});
        const Result<void> p2 = m.dev->present(m.swapchain);
        if (pres) {
            pres = p2;
        }
    } else {
        const TextureBarrier toPresent{*bb, ResourceState::RenderTarget, ResourceState::Present};
        cmd.barrier({&toPresent, 1});
        pres = m.dev->present(m.swapchain);
    }
    if (!rec) {
        return rec;
    }
    return pres;
}

Result<Image> Renderer::readback() {
    Impl& m = *impl_;
    if (m.windowed()) {
        if (!m.captured) {
            return std::unexpected(Error{2, "readback: no captured frame (requestCapture before renderFrame)"});
        }
        Image out = std::move(*m.captured);
        m.captured.reset();
        return out;
    }
    if (!m.rendered || !m.target) {
        return std::unexpected(Error{2, "readback: no frame rendered"});
    }
    auto img = m.dev->readback(m.target);
    if (!img) {
        return std::unexpected(img.error());
    }
    return Image{img->width, img->height, std::move(img->rgba8)};
}

Settings& Renderer::settings() {
    return impl_->settings;
}

void Renderer::resize(uint32_t width, uint32_t height) {
    Impl& m = *impl_;
    width = std::max(1u, width);
    height = std::max(1u, height);
    if (width == m.width && height == m.height) {
        return;
    }
    m.width = width;
    m.height = height;
    if (m.windowed()) {
        if (auto r = m.dev->resizeSwapchain(m.swapchain, width, height); !r) {
            ENG_LOG_ERROR("resize: {}", r.error().message);
        }
    } else if (m.target) {
        (void)m.dev->submitAndWait();
        m.dev->destroy(m.target);
        m.target = {};
        m.rendered = false;
    }
    for (auto& [id, s] : m.scenes) {
        Result<void> r = s.builtin ? s.builtin->resize(width, height) : s.model->resize(width, height);
        if (!r) {
            ENG_LOG_ERROR("resize: {}", r.error().message);
        }
    }
}

const DeviceCaps& Renderer::caps() const {
    return impl_->dev->caps();
}

const AssetInfo* Renderer::assetInfo(AssetHandle asset) const {
    auto it = impl_->assets.find(asset.id);
    return it == impl_->assets.end() ? nullptr : &it->second.info;
}

SceneInfo Renderer::sceneInfo(SceneHandle handle) const {
    SceneInfo info;
    auto it = impl_->scenes.find(handle.id);
    if (it == impl_->scenes.end() || !it->second.instance) {
        return info;
    }
    const scene::SceneInstance& s = *it->second.instance;
    const asset::ModelData& model = *impl_->assets.at(it->second.asset).model;
    for (const scene::DrawNode& d : s.draws) {
        info.drawCount += static_cast<uint32_t>(model.meshes[d.mesh].primitives.size());
    }
    info.extentsValid = s.extentsValid;
    info.extentMin = s.extentMin;
    info.extentMax = s.extentMax;
    return info;
}

uint32_t Renderer::validationErrorCount() const {
    return impl_->dev->validationErrorCount();
}

void Renderer::requestCapture() {
    impl_->captureNext = true;
}

Result<void> Renderer::setVsync(bool vsync) {
    if (!impl_->windowed()) {
        return {};
    }
    return impl_->dev->setSwapchainVsync(impl_->swapchain, vsync);
}

void Renderer::beginUi(float deltaSeconds, float dpiScale, std::span<const platform::InputEvent> events) {
#if defined(_WIN32)
    Impl& m = *impl_;
    if (!m.ui) {
        return;
    }
    if (m.uiOpen) {
        ImGui::EndFrame();
    }
    m.ui->newFrame(deltaSeconds, m.width, m.height, dpiScale, events);
    m.uiOpen = true;
#else
    (void)deltaSeconds;
    (void)dpiScale;
    (void)events;
#endif
}

Result<void> Renderer::finish() {
    return impl_->dev->submitAndWait();
}

} // namespace eng
