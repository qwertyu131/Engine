#pragma once
// Dear ImGui drawn only through engine/rhi (plan F2 Task 6): no imgui backend file (imgui_impl_dx11,
// imgui_impl_vulkan, imgui_impl_win32) is used, so the UI ports to every later backend unchanged.
//   - textures: ImGuiBackendFlags_RendererHasTextures (1.92 protocol: ImDrawData::Textures with
//     WantCreate / WantUpdates / WantDestroy); the font atlas is an RHI texture (RGBA8_UNORM), an update
//     re-creates the texture (atlas updates are rare);
//   - geometry: per frame host-visible vertex / index buffers created from the draw data and destroyed
//     after recording (deferred by the RHI until the GPU is done);
//   - shader shaders/ui/imgui.hlsl, alpha blending, scissor per command, no depth;
//   - input: platform::InputEvent -> ImGuiIO.
#include "rhi/Rhi.h"

#include <filesystem>
#include <map>
#include <memory>
#include <span>

struct ImGuiContext;
struct ImTextureData;

namespace eng::platform {
struct InputEvent;
}

namespace eng::ui {

class ImGuiRenderer {
public:
    // Creates the ImGui context (owned) and the pipeline for targets of `targetFormat`.
    static rhi::Result<std::unique_ptr<ImGuiRenderer>> create(rhi::IDevice& device, const std::filesystem::path& shaderDir,
                                                              rhi::Format targetFormat);
    ~ImGuiRenderer();
    ImGuiRenderer(const ImGuiRenderer&) = delete;
    ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;

    // Feeds input and starts an ImGui frame (ImGui::NewFrame); build the UI with ImGui:: calls afterwards.
    void newFrame(float deltaSeconds, uint32_t width, uint32_t height, float dpiScale,
                  std::span<const platform::InputEvent> events);
    // ImGui::Render + records the draw data into `target` (RenderTarget state; loads its content), size
    // width x height. Inside an RHI frame.
    rhi::Result<void> render(rhi::ICommandList& cmd, rhi::TextureHandle target, uint32_t width, uint32_t height);

private:
    ImGuiRenderer(rhi::IDevice& device, rhi::Format format) : dev_(device), format_(format) {}
    rhi::Result<void> init(const std::filesystem::path& shaderDir);
    rhi::Result<void> updateTexture(ImTextureData* tex);
    void destroyTexture(uint64_t id);

    struct UiTexture {
        rhi::TextureHandle texture;
        rhi::BindGroupHandle group;
    };

    rhi::IDevice& dev_;
    rhi::Format format_;
    ImGuiContext* ctx_ = nullptr;
    rhi::ShaderHandle vs_, ps_;
    rhi::PipelineHandle pipeline_;
    rhi::SamplerHandle sampler_;
    std::map<uint64_t, UiTexture> textures_; // ImTextureID = RHI texture handle id
};

} // namespace eng::ui
