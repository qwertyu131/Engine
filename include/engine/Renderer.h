#pragma once
// eng::Renderer, the library facade (spec K-D, plan F3 Task 4). Hosts (apps/headless, apps/viewer, later other
// applications) use only include/engine/*.h. Frozen from F7; every change before then is logged in
// docs/facade-changelog.md.
//
// Threading: one render thread owns the Renderer (no internal locking). loadAsset is synchronous in F3 (a job system
// comes in F5).
//
// Targets:
//   offscreen (RendererDesc::window null): renderFrame() renders into an RGBA8 target of the current size, submits
//     and waits; readback() returns that frame (RGBA8, rows top-down). glTF scenes and builtin:triangle /
//     builtin:pbr-spheres use RGBA8_UNORM with the shader's gamma 2.2 output encode (as Sample-Viewer),
//     builtin:textured-cube RGBA8_SRGB (the F2 golden cases, unchanged);
//   window: renderFrame() draws into the swapchain (BGRA8_SRGB) and presents; requestCapture() makes the next
//     renderFrame() read its back buffer before presenting, readback() then returns it.
// Scenes: instantiate() makes the new scene the active one; renderFrame() draws the active scene with the camera of
// setCamera() (built-in scenes keep their own fixed cameras), then the debug UI when one is open (beginUi()).
//
// Errors are eng::Error with the exit codes of the F2 Global Constraints: 2 usage / IO / malformed asset, 3 backend not
// available, 4 validation error, 5 device lost / GPU error.
#include <engine/AssetHandle.h>
#include <engine/Camera.h>
#include <engine/Result.h>
#include <engine/Scene.h>
#include <engine/Settings.h>
#include <engine/Window.h>
#include <engine/rhi/DeviceCaps.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace eng {

// A host window handle (HWND on Windows). Null = offscreen.
struct NativeWindow {
    void* handle = nullptr;
};

// RGBA8, rows top-down, tightly packed.
struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba8;
};

struct RendererDesc {
    rhi::Backend backend = rhi::Backend::Vulkan;
    bool validation = true;       // validation layers / debug layer (a missing layer is exit 4)
    bool warp = false;            // D3D11 WARP
    int adapterIndex = -1;        // -1 = the backend's default adapter
    int d3d11FeatureLevelCap = 0; // 0 = highest; 0xa000 = FL10_0 (T0)
    NativeWindow window;          // null = offscreen
    uint32_t width = 256, height = 256;
    bool vsync = true;            // window only
    bool debugUi = false;         // window only: Dear ImGui overlay (beginUi)
    std::filesystem::path shaderDir; // the shader library of the `shaders` build step
    std::filesystem::path assetDir;  // tests/assets (built-in scenes' textures)
};

class Renderer {
public:
    static Result<std::unique_ptr<Renderer>> create(const RendererDesc& desc);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // A .gltf / .glb file, or "builtin:<name>" (builtinScenes()). Sync in F3; async job system in F5.
    Result<AssetHandle> loadAsset(const std::filesystem::path& path);
    // -1 = gltf.scene ?? 0. The new scene becomes the active one.
    Result<SceneHandle> instantiate(AssetHandle asset, int sceneIndex = -1);
    void setActiveScene(SceneHandle scene);
    // Sample-Viewer resetView fit camera of the scene (rest pose), for `aspect` = width / height.
    Camera fitCamera(SceneHandle scene, float aspect) const;
    void setCamera(const Camera& camera);
    // Built-in scenes only (the viewer's spinning cube): animation time in seconds; glTF scenes stay at rest (F4).
    void setTime(float seconds);
    Result<void> renderFrame(); // to the window or the offscreen target
    Result<Image> readback();   // the last frame (offscreen), or the captured one (window, requestCapture)
    Settings& settings();       // cvar access (CVarSource::Code / Console)
    void resize(uint32_t width, uint32_t height);
    const rhi::DeviceCaps& caps() const;

    // --- additions before the F7 freeze (docs/facade-changelog.md) ---------------------------------------------
    static std::span<const rhi::Backend> compiledBackends();
    static std::span<const std::string_view> builtinScenes();
    const AssetInfo* assetInfo(AssetHandle asset) const; // null for an unknown handle
    SceneInfo sceneInfo(SceneHandle scene) const;
    uint32_t validationErrorCount() const;
    void requestCapture();                       // window: read the next frame's back buffer (readback())
    Result<void> setVsync(bool vsync);           // window
    // Window + debugUi: starts a Dear ImGui frame (ImGui:: calls follow until renderFrame(), which draws it).
    void beginUi(float deltaSeconds, float dpiScale, std::span<const platform::InputEvent> events);
    // Ends rendering: releases every GPU object (scenes, debug UI, target / swapchain) and waits for the GPU, so
    // validation messages of work in flight and of the teardown are counted (validationErrorCount) before the host
    // picks its exit code. Only validationErrorCount / caps / assetInfo are meaningful afterwards.
    Result<void> finish();

private:
    struct Impl;
    explicit Renderer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace eng
