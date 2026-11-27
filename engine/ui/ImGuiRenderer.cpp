// Dear ImGui through engine/rhi (plan F2 Task 6; see ImGuiRenderer.h).
#include "ui/ImGuiRenderer.h"

#include "core/Log.h"
#include "rhi/ShaderLibrary.h"

#if defined(_WIN32)
#include "platform/Window.h"
#endif

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace eng::ui {

namespace {

using namespace rhi;

bool isSrgb(Format f) {
    return f == Format::RGBA8_SRGB || f == Format::BGRA8_SRGB;
}

struct UiDrawCB {
    float scaleTranslate[4];
    uint32_t flags[4];
};

#if defined(_WIN32)
ImGuiKey toImGuiKey(platform::Key k) {
    using platform::Key;
    switch (k) {
    case Key::Tab: return ImGuiKey_Tab;
    case Key::Left: return ImGuiKey_LeftArrow;
    case Key::Right: return ImGuiKey_RightArrow;
    case Key::Up: return ImGuiKey_UpArrow;
    case Key::Down: return ImGuiKey_DownArrow;
    case Key::PageUp: return ImGuiKey_PageUp;
    case Key::PageDown: return ImGuiKey_PageDown;
    case Key::Home: return ImGuiKey_Home;
    case Key::End: return ImGuiKey_End;
    case Key::Insert: return ImGuiKey_Insert;
    case Key::Delete: return ImGuiKey_Delete;
    case Key::Backspace: return ImGuiKey_Backspace;
    case Key::Space: return ImGuiKey_Space;
    case Key::Enter: return ImGuiKey_Enter;
    case Key::Escape: return ImGuiKey_Escape;
    case Key::A: return ImGuiKey_A;
    case Key::C: return ImGuiKey_C;
    case Key::V: return ImGuiKey_V;
    case Key::X: return ImGuiKey_X;
    case Key::Y: return ImGuiKey_Y;
    case Key::Z: return ImGuiKey_Z;
    case Key::F12: return ImGuiKey_F12;
    case Key::Ctrl: return ImGuiKey_LeftCtrl;
    case Key::Shift: return ImGuiKey_LeftShift;
    case Key::Alt: return ImGuiKey_LeftAlt;
    default: return ImGuiKey_None;
    }
}
#endif

} // namespace

Result<std::unique_ptr<ImGuiRenderer>> ImGuiRenderer::create(IDevice& device, const std::filesystem::path& shaderDir,
                                                              Format targetFormat) {
    std::unique_ptr<ImGuiRenderer> r(new ImGuiRenderer(device, targetFormat));
    if (auto i = r->init(shaderDir); !i) {
        return std::unexpected(i.error());
    }
    return r;
}

Result<void> ImGuiRenderer::init(const std::filesystem::path& shaderDir) {
    IMGUI_CHECKVERSION();
    ctx_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "eng_rhi";
    io.BackendPlatformName = "eng_platform";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.IniFilename = nullptr; // deterministic layout (no imgui.ini)
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();

    const Backend backend = dev_.caps().backend;
    auto vsBlob = ShaderLibrary::load(shaderDir, "imgui", "VSMain", backend, "default");
    if (!vsBlob) {
        return std::unexpected(vsBlob.error());
    }
    auto psBlob = ShaderLibrary::load(shaderDir, "imgui", "PSMain", backend, "default");
    if (!psBlob) {
        return std::unexpected(psBlob.error());
    }
    auto vs = dev_.createShader(*vsBlob);
    if (!vs) {
        return std::unexpected(vs.error());
    }
    vs_ = *vs;
    auto ps = dev_.createShader(*psBlob);
    if (!ps) {
        return std::unexpected(ps.error());
    }
    ps_ = *ps;
    GraphicsPipelineDesc pd;
    pd.vs = vs_;
    pd.ps = ps_;
    pd.vertexLayout.buffers = {{sizeof(ImDrawVert), false}};
    pd.vertexLayout.attributes = {{"POSITION", Format::RG32_FLOAT, 0, offsetof(ImDrawVert, pos)},
                                  {"TEXCOORD0", Format::RG32_FLOAT, 0, offsetof(ImDrawVert, uv)},
                                  {"COLOR0", Format::RGBA8_UNORM, 0, offsetof(ImDrawVert, col)}};
    pd.raster.cull = CullMode::None;
    pd.colorFormats = {format_};
    pd.blend = {BlendState{true}};
    pd.debugName = "imgui";
    auto pipe = dev_.createGraphicsPipeline(pd);
    if (!pipe) {
        return std::unexpected(pipe.error());
    }
    pipeline_ = *pipe;
    SamplerDesc sd; // bilinear (ImGui's baked lines need it), clamp
    sd.mipFilter = Filter::Nearest;
    sd.addressU = sd.addressV = sd.addressW = AddressMode::ClampToEdge;
    auto smp = dev_.createSampler(sd);
    if (!smp) {
        return std::unexpected(smp.error());
    }
    sampler_ = *smp;
    return {};
}

ImGuiRenderer::~ImGuiRenderer() {
    if (ctx_) {
        ImGui::SetCurrentContext(ctx_);
        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
            if (tex->RefCount == 1 && tex->TexID != ImTextureID_Invalid) {
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }
    for (auto& [id, t] : textures_) {
        dev_.destroy(t.group);
        dev_.destroy(t.texture);
    }
    textures_.clear();
    for (Handle h : {Handle(sampler_), Handle(pipeline_), Handle(ps_), Handle(vs_)}) {
        if (h.id) {
            dev_.destroy(h);
        }
    }
    if (ctx_) {
        ImGui::DestroyContext(ctx_);
    }
}

void ImGuiRenderer::destroyTexture(uint64_t id) {
    auto it = textures_.find(id);
    if (it != textures_.end()) {
        dev_.destroy(it->second.group);
        dev_.destroy(it->second.texture); // deferred until the GPU is done with it
        textures_.erase(it);
    }
}

Result<void> ImGuiRenderer::updateTexture(ImTextureData* tex) {
    if (tex->Status == ImTextureStatus_WantDestroy) {
        if (tex->UnusedFrames > 0) {
            destroyTexture(static_cast<uint64_t>(tex->TexID));
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
        return {};
    }
    if (tex->Status != ImTextureStatus_WantCreate && tex->Status != ImTextureStatus_WantUpdates) {
        return {};
    }
    // Create, or re-create on update (atlas changes are rare): RGBA8 from RGBA32 / Alpha8 pixels.
    if (tex->Status == ImTextureStatus_WantUpdates) {
        destroyTexture(static_cast<uint64_t>(tex->TexID));
    }
    const auto w = static_cast<uint32_t>(tex->Width), h = static_cast<uint32_t>(tex->Height);
    std::vector<uint8_t> rgba(size_t(w) * h * 4u);
    const auto* src = static_cast<const uint8_t*>(tex->GetPixels());
    if (tex->Format == ImTextureFormat_RGBA32) {
        std::memcpy(rgba.data(), src, rgba.size());
    } else { // Alpha8: white + alpha
        for (size_t i = 0; i < size_t(w) * h; ++i) {
            rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = src[i];
        }
    }
    const SubresourceData init{std::as_bytes(std::span(rgba)), 0};
    TextureDesc td;
    td.width = w;
    td.height = h;
    td.format = Format::RGBA8_UNORM;
    td.usage = TextureUsage::Sampled;
    td.debugName = "imgui texture";
    auto t = dev_.createTexture(td, {&init, 1});
    if (!t) {
        return std::unexpected(t.error());
    }
    BindGroupDesc bg;
    bg.pipeline = pipeline_;
    bg.space = 2;
    bg.entries = {{"uiTexture", *t}, {"uiSampler", sampler_}};
    auto g = dev_.createBindGroup(bg);
    if (!g) {
        dev_.destroy(*t);
        return std::unexpected(g.error());
    }
    textures_[t->id] = UiTexture{*t, *g};
    tex->SetTexID(static_cast<ImTextureID>(t->id));
    tex->SetStatus(ImTextureStatus_OK);
    return {};
}

void ImGuiRenderer::newFrame(float deltaSeconds, uint32_t width, uint32_t height, float dpiScale,
                             std::span<const platform::InputEvent> events) {
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = deltaSeconds > 0.0f ? deltaSeconds : 1.0f / 60.0f;
    ImGui::GetStyle().FontScaleDpi = dpiScale;
#if defined(_WIN32)
    for (const platform::InputEvent& e : events) {
        switch (e.type) {
        case platform::InputEvent::Type::MouseMove: io.AddMousePosEvent(e.x, e.y); break;
        case platform::InputEvent::Type::MouseButton: io.AddMouseButtonEvent(e.button, e.down); break;
        case platform::InputEvent::Type::MouseWheel: io.AddMouseWheelEvent(e.x, e.y); break;
        case platform::InputEvent::Type::Char: io.AddInputCharacter(e.ch); break;
        case platform::InputEvent::Type::Focus: io.AddFocusEvent(e.down); break;
        case platform::InputEvent::Type::Key: {
            if (e.key == platform::Key::Ctrl) {
                io.AddKeyEvent(ImGuiMod_Ctrl, e.down);
            } else if (e.key == platform::Key::Shift) {
                io.AddKeyEvent(ImGuiMod_Shift, e.down);
            } else if (e.key == platform::Key::Alt) {
                io.AddKeyEvent(ImGuiMod_Alt, e.down);
            }
            if (const ImGuiKey k = toImGuiKey(e.key); k != ImGuiKey_None) {
                io.AddKeyEvent(k, e.down);
            }
            break;
        }
        }
    }
#else
    (void)events;
#endif
    ImGui::NewFrame();
}

Result<void> ImGuiRenderer::render(ICommandList& cmd, TextureHandle target, uint32_t width, uint32_t height) {
    ImGui::SetCurrentContext(ctx_);
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (dd->Textures) {
        for (ImTextureData* tex : *dd->Textures) {
            if (tex->Status != ImTextureStatus_OK) {
                if (auto r = updateTexture(tex); !r) {
                    return r;
                }
            }
        }
    }
    if (dd->TotalVtxCount == 0 || width == 0 || height == 0) {
        return {};
    }
    // Geometry of the whole frame in one vertex + one index buffer (host-visible, destroyed after recording).
    std::vector<ImDrawVert> vertices;
    std::vector<ImDrawIdx> indices;
    vertices.reserve(static_cast<size_t>(dd->TotalVtxCount));
    indices.reserve(static_cast<size_t>(dd->TotalIdxCount));
    for (const ImDrawList* list : dd->CmdLists) {
        vertices.insert(vertices.end(), list->VtxBuffer.begin(), list->VtxBuffer.end());
        indices.insert(indices.end(), list->IdxBuffer.begin(), list->IdxBuffer.end());
    }
    BufferDesc vbd;
    vbd.size = vertices.size() * sizeof(ImDrawVert);
    vbd.usage = BufferUsage::Vertex;
    vbd.hostVisible = true;
    vbd.debugName = "imgui vertices";
    auto vb = dev_.createBuffer(vbd, std::as_bytes(std::span(vertices)));
    if (!vb) {
        return std::unexpected(vb.error());
    }
    BufferDesc ibd;
    ibd.size = (indices.size() * sizeof(ImDrawIdx) + 3u) & ~uint64_t(3);
    ibd.usage = BufferUsage::Index;
    ibd.hostVisible = true;
    ibd.debugName = "imgui indices";
    indices.resize(ibd.size / sizeof(ImDrawIdx), 0);
    auto ib = dev_.createBuffer(ibd, std::as_bytes(std::span(indices)));
    if (!ib) {
        dev_.destroy(*vb);
        return std::unexpected(ib.error());
    }

    UiDrawCB cb{};
    const float l = dd->DisplayPos.x, t = dd->DisplayPos.y;
    const float r = l + dd->DisplaySize.x, b = t + dd->DisplaySize.y;
    cb.scaleTranslate[0] = 2.0f / (r - l);
    cb.scaleTranslate[1] = -2.0f / (b - t); // NDC +Y up, pixel y down
    cb.scaleTranslate[2] = -(r + l) / (r - l);
    cb.scaleTranslate[3] = (b + t) / (b - t);
    cb.flags[0] = isSrgb(format_) ? 1u : 0u;
    const BufferBinding cbBinding = dev_.allocTransient(sizeof(cb), 256);
    if (!cbBinding.buffer) {
        dev_.destroy(*vb);
        dev_.destroy(*ib);
        return std::unexpected(Error{kExitUsage, "imgui: allocTransient failed"});
    }
    std::memcpy(cbBinding.cpu, &cb, sizeof(cb));
    BindGroupDesc dg;
    dg.pipeline = pipeline_;
    dg.space = 3;
    dg.entries = {{"UiDraw", cbBinding}};
    auto drawGroup = dev_.createBindGroup(dg);
    if (!drawGroup) {
        dev_.destroy(*vb);
        dev_.destroy(*ib);
        return std::unexpected(drawGroup.error());
    }

    RenderPassDesc rp;
    ColorAttachment ca;
    ca.texture = target;
    ca.load = LoadOp::Load;
    ca.store = StoreOp::Store;
    rp.colors.push_back(ca);
    cmd.beginRenderPass(rp);
    cmd.setPipeline(pipeline_);
    cmd.setBindGroup(3, *drawGroup);
    cmd.setVertexBuffer(0, *vb, 0);
    cmd.setIndexBuffer(*ib, sizeof(ImDrawIdx) == 2 ? IndexType::Uint16 : IndexType::Uint32, 0);
    cmd.setViewport({0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f});
    uint64_t boundTexture = 0;
    uint32_t vtxBase = 0, idxBase = 0;
    for (const ImDrawList* list : dd->CmdLists) {
        for (const ImDrawCmd& c : list->CmdBuffer) {
            if (c.UserCallback) {
                continue; // no user callbacks in the viewer
            }
            const float x0 = std::clamp(c.ClipRect.x - l, 0.0f, static_cast<float>(width));
            const float y0 = std::clamp(c.ClipRect.y - t, 0.0f, static_cast<float>(height));
            const float x1 = std::clamp(c.ClipRect.z - l, 0.0f, static_cast<float>(width));
            const float y1 = std::clamp(c.ClipRect.w - t, 0.0f, static_cast<float>(height));
            if (x1 <= x0 || y1 <= y0) {
                continue;
            }
            const auto texId = static_cast<uint64_t>(c.GetTexID());
            auto it = textures_.find(texId);
            if (it == textures_.end()) {
                continue; // a texture we did not create (not used by the viewer)
            }
            if (texId != boundTexture) {
                cmd.setBindGroup(2, it->second.group);
                boundTexture = texId;
            }
            cmd.setScissor({static_cast<int32_t>(x0), static_cast<int32_t>(y0), static_cast<uint32_t>(x1 - x0),
                            static_cast<uint32_t>(y1 - y0)});
            cmd.drawIndexed(c.ElemCount, 1, idxBase + c.IdxOffset, static_cast<int32_t>(vtxBase + c.VtxOffset), 0);
        }
        vtxBase += static_cast<uint32_t>(list->VtxBuffer.Size);
        idxBase += static_cast<uint32_t>(list->IdxBuffer.Size);
    }
    cmd.endRenderPass();
    dev_.destroy(*drawGroup);
    dev_.destroy(*vb);
    dev_.destroy(*ib);
    return {};
}

} // namespace eng::ui
