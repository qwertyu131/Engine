# F2 — RHI Core: Vulkan + D3D11 + Windowed Viewer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A backend-independent RHI with two real backends of different character (Vulkan explicit, D3D11 implicit) that renders a triangle, a textured cube and the F1 glTF PBR shader offscreen (headless → PNG) and in a window (`apps/viewer`). The images must match across backends, validation must be clean, and the T0 (D3D11 FL10_0) capability table must be measured.

**Architecture:**
- `engine/rhi` holds the interface (explicit model, spec §5.3) and a backend registry.
- `engine/rhi/vulkan` (volk + VMA) and `engine/rhi/d3d11` implement it.
  - D3D11 emulates barriers as no-ops.
  - D3D11 binds each stage from that stage's own reflection JSON, because D3D11 slots are per stage and per permutation (F1 Limitations).
- Shaders are loaded as loose `shaderc` outputs plus `.refl.json`. The `.shaderpack` is F5.
- `engine/platform` provides the Win32 window.
- `apps/headless` and `apps/viewer` drive built-in scenes through the RHI.
- ImGui is drawn through our own RHI, never through imgui's backend files, so it ports to every later backend.

**Tech Stack:** C++20; Vulkan SDK 1.4.341.1 headers + validation layers; volk and VulkanMemoryAllocator (FetchContent, pinned SHAs); D3D11 + DXGI 1.6 (Windows SDK 10.0.26100); Dear ImGui (FetchContent, pinned tag, core files only); stb_image (pinned, decode only); F1 `shaderc` for all shaders.

**Spec:** `docs/superpowers/specs/2026-11-27-cross-platform-gltf-renderer-design.md` (§5.2, §5.3, §5.4, §6.1, §6.2, §10, §11 F2, §14 K-D/K-E/K-G). Also `docs/spikes/f01-shader-pipeline.md` ("Limitations found per target", "Risks for F2+").

## Global Constraints

- All F0/F1 Global Constraints still apply:
  - C++20 for `engine`; pins in lock files or FetchContent SHAs.
  - Commit trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
  - No closed-source SDK in git.
  - Binding convention from F1: space0 frame, space1 view, space2 material, space3 draw; index unique per space.
- **Validation:** every test and gate run enables the backend's validation:
  - Vulkan: `VK_LAYER_KHRONOS_validation` with synchronization validation.
  - D3D11: `D3D11_CREATE_DEVICE_DEBUG` + `ID3D11InfoQueue`.

  Any message of severity warning or above (Vulkan) or WARNING/ERROR/CORRUPTION (D3D11) makes the run exit 4. A missing validation layer is a **failure** (exit 4, `validation layer not available: <name>`), never a skip. The F1 tool-skip pattern does not apply to validation.
- **Exit codes** for `headless` and `viewer`:
  - 0 = ok
  - 2 = usage / IO error
  - 3 = backend not available in this build or on this machine
  - 4 = validation error
  - 5 = device lost / GPU error
- **D3D11 rules from F1:**
  - Bind slots come from each stage's `targetBinding.register`.
  - `UAVStartSlot` = the pixel shader's `renderTargets`.
  - The VS and PS of one pipeline must come from the same DXBC path; the default is `cross`. Pipeline creation rejects a mixed pair with `VS and PS from different dxbc paths`.
  - A linkage message from the debug layer fails the run (Risk 1).
- Morph meshes draw with base vertex 0 (F1 Limitations: Vulkan's `SV_VertexID` includes the base vertex). `CommandList::drawIndexed` asserts `baseVertex == 0` when the pipeline's VS reflection has a morph storage buffer.
- Processes: agents never kill a process. A viewer or headless run that does not exit within its timeout is reported as a hang, with the command line.
- Reviewers clone to a short path (for example `C:\tmp\f2g`) until Task 1's MAX_PATH fix lands. New or changed CI steps are `PENDING-CI` (billing lock, f00.t08).
- **K-D deviation (recorded, not silent):** F2 has no scene or asset, so the `include/engine/*.h` facade is not created yet. `apps/headless` and `apps/viewer` use `engine/rhi` directly. The facade starts in F3 with `Renderer` / `Scene` / `AssetHandle`, and both apps move to it there. Record this in the worklist f02 row.

## Review Focus

1. **Cancelling errors across backends.** A Y-flip, wrong winding, UV-origin or sRGB error made identically on both backends would pass a backend-vs-backend compare. Defences:
   - The test scenes are asymmetric (corner-coloured triangle, a cube with a texture that has distinct letters/colours per face).
   - Each case also checks absolute pixel probes at fixed coordinates against hand-computed values. (Task 5 test.)
2. **Validation hook really wired.** A deliberately invalid call must produce exit 4 on each backend:
   - Vulkan: a draw with no pipeline bound.
   - D3D11: binding a texture that is also a bound render target.

   Without this, a "validation clean" gate proves nothing. (Task 3 and Task 4 tests.)
3. **Window resize / minimise.** Resizing to 1×1, minimising (0×0) and restoring must not crash or produce a validation error. The swapchain is recreated and rendering skips while minimised. (Task 6 test via `--script resize`.)
4. **Device without the feature.** On FL10_0, a T1 pipeline (SM 5.0 DXBC) is rejected at creation with `pipeline needs TIER 1, device is TIER 0`, not a driver crash. Vulkan on a device without a required feature exits 3 with the feature named. (Task 7 test.)
5. **Non-ASCII / spaced paths** for `--out`, `--screenshot` and shader directories (the repo is under `C:\Users\Heros\Desktop`; F0/F1 precedent). (Task 5 test.)

---

### Task 1: F1/F0 carry-over cleanup

**Files:**
- Modify: `scripts/fetch_shader_tools.py`, `tests/unit/scripts/test_fetch_shader_tools.py`, `tools/shaderc/{Compiler.cpp,Reflection.cpp,Validate.cpp}`, `tests/unit/shaderc/{test_targets.cpp,test_reflection.cpp,test_pbr.cpp}`, `.github/workflows/ci.yml`, `tools/golden/Compare.{h,cpp}`, `tests/unit/golden/test_compare.cpp`, `docs/worklist.md`, `docs/superpowers/plans/2026-11-27-f01-shader-pipeline-spike.md` (interface text only)
- Create: `.gitattributes` (if absent: `* text=auto`, `*.png binary`, keep the existing `tests/shaders/msl/*.metal -text`), `tests/tools/d3d11_linkcheck/{CMakeLists.txt,main.cpp}` (the F1 scratch linkcheck, Windows-only, optional target)

**Interfaces:**
- `fetch_shader_tools.py --build-dir <dir>`. When the option is not given and `len(str(repo)) > 40`, build Dawn in `%LOCALAPPDATA%\eng-tint-<sha8>` and print that path. The built `tint.exe` is still copied to `third_party/tools/tint/`. When a step fails, `run_cmd` prints every output line matching `error` (case-insensitive), then the last 30 lines, plus the path of the full log.
- `shaderc` rules:
  - GLSL targets reject `index + max(arraySize,1) > 16` and unbounded arrays, with `binding range t14..t17 exceeds 16 per space for glsl-es320`.
  - A reflected resource with no declared `register()` is rejected: `resource <name> has no explicit register() (binding convention)`.
  - `validate()` for DXIL requires `dxil.dll` next to `dxc.exe`; otherwise it returns `tool not found: dxil.dll (...)`.
- golden `compare()` takes alpha: `CompareOptions::alpha = Ignore|Compare` (default `Compare` when either image has non-opaque alpha). The detail field uses repo-relative paths.

- [ ] **Step 1: Write the failing tests.**
  - fetch: long path → short build dir chosen; failing build → error lines printed (mocked `run_cmd`).
  - shaderc:
    - array range overflow fixture `shaders/test/glsl_array_overflow.hlsl` is rejected with the message above;
    - `shaders/test/no_register.hlsl` is rejected;
    - one negative test per validator: truncated SPIR-V, broken GLSL text, broken WGSL text and a zeroed DXBC digest all give `!validate()`;
    - a missing `dxil.dll` (hidden through `--tool`) gives an error.
  - golden:
    - two images that differ only in alpha give FAIL by default and PASS with `Ignore`;
    - FLIP cross-check: `tests/golden/fixtures/flip_upstream/{a,b}.png` with the mean FLIP printed by NVIDIA's reference implementation (run once by the implementer with `flip.py` from the pinned upstream commit, value stored in `expected.json`) must match ours within 1e-3.
- [ ] **Step 2: Run them, all FAIL.**
- [ ] **Step 3: Implement.**
  - CI (`ci.yml`):
    - add `workflow_dispatch` and `push` on `f*` branches;
    - the windows job installs the pinned Vulkan SDK 1.4.341.1 and runs `python scripts/fetch_shader_tools.py`, with `third_party/tools` cached on `hashFiles('third_party/tools/tools.lock.json')`;
    - the job then fails if `ctest` lists any Skipped `shaderc.*` test.
  - linkage doctest: skip inputs by the ISGN system-value field (non-zero and not POSITION), not by name.
  - Commit the linkcheck tool.
  - Fix plan F1's Task 2 interface text (the `validate()` return type, `dxbcPath`, Tint version = `.dawn-commit`).
  - Worklist: f01.t02 Evidence gets `a19ea2a`.
- [ ] **Step 4: Gate.**
  - From a clone at a deep path (≥ 100 characters): `python scripts/fetch_shader_tools.py` exits 0.
  - `ctest --preset win-msvc -C Debug` all pass with 0 skipped.
  - `python -m unittest discover tests` OK.
  - CI is PENDING-CI.
- [ ] **Step 5: Commit** `chore(f2): F1 carry-overs (MAX_PATH, shaderc checks, CI tools, golden alpha/FLIP check)`.

### Task 2: RHI interface + Vulkan and D3D11 triangle

Gets a triangle on **both** backends before textures and bind groups exist, so the interface does not overfit to one backend.

**Files:**
- Create:
  - `engine/rhi/{Rhi.h,Types.h,DeviceCaps.h,Registry.cpp,ShaderLibrary.{h,cpp}}`
  - `engine/rhi/vulkan/{VkDevice.cpp,VkCommandList.cpp,VkResources.cpp,VkValidation.cpp,...}`
  - `engine/rhi/d3d11/{D3D11Device.cpp,D3D11CommandList.cpp,D3D11Resources.cpp,D3D11Validation.cpp,...}`
  - `engine/rhi/CMakeLists.txt`
  - `tests/unit/rhi/{test_shader_library.cpp,test_triangle.cpp}`
- Modify:
  - `cmake/Dependencies.cmake` (volk, VMA pinned SHAs)
  - `engine/CMakeLists.txt`
  - `apps/headless/main.cpp` (backends `vulkan`, `d3d11`; scene `builtin:triangle`; `--warp` for D3D11; `--adapter <index>`)
  - `scripts/compile_shaders.py` (build step writes `build/<preset>/shaders/<shader>/<target>/<key>/...` consumed by `ShaderLibrary`; CMake target `shaders` runs it)

**Interfaces (pin these exact names; later tasks and F3+ use them):**

```cpp
namespace eng::rhi {
enum class Backend : uint8_t { Vulkan, D3D11 };          // D3D12, Metal, GLES, WebGPU added later
enum class Format : uint16_t { RGBA8_UNORM, RGBA8_SRGB, BGRA8_UNORM, BGRA8_SRGB, RGBA16_FLOAT,
                               RGBA32_FLOAT, D32_FLOAT, D24_UNORM_S8_UINT, /* + vertex formats */ };
struct DeviceDesc { Backend backend; bool validation = true; bool warp = false;
                    int adapterIndex = -1; int d3d11FeatureLevelCap = 0; /* 0 = highest; 0xa000 = FL10_0 */ };
struct Error { int exitCode; std::string message; };        // exitCode per Global Constraints
template <class T> using Result = std::expected<T, Error>;

class IDevice {                                           // one per backend instance; render-thread owned
public:
  virtual ~IDevice() = default;
  virtual const DeviceCaps& caps() const = 0;
  virtual Result<BufferHandle>   createBuffer(const BufferDesc&, std::span<const std::byte> init = {}) = 0;
  virtual Result<TextureHandle>  createTexture(const TextureDesc&, std::span<const SubresourceData> init = {}) = 0;
  virtual Result<SamplerHandle>  createSampler(const SamplerDesc&) = 0;
  virtual Result<ShaderHandle>   createShader(const ShaderBlob&) = 0;   // blob + parsed reflection
  virtual Result<PipelineHandle> createGraphicsPipeline(const GraphicsPipelineDesc&) = 0;
  virtual Result<BindGroupHandle> createBindGroup(const BindGroupDesc&) = 0;   // Task 3
  virtual Result<SwapchainHandle> createSwapchain(const SwapchainDesc&) = 0;   // Task 6
  virtual void destroy(Handle) = 0;                         // deferred until the GPU is done with it
  virtual ICommandList& beginFrame() = 0;                   // frames in flight = 2
  virtual Result<void> submitAndWait() = 0;                 // headless; returns exit 4/5 on error
  virtual Result<void> present(SwapchainHandle) = 0;        // Task 6
  virtual Result<Image> readback(TextureHandle) = 0;        // RGBA8, rows top-down
  virtual uint32_t validationErrorCount() const = 0;
  virtual NativeHandles nativeHandles() const = 0;          // spec §5.3 getNativeHandle
};
class ICommandList {
public:
  virtual void barrier(std::span<const TextureBarrier>) = 0;  // D3D11: no-op
  virtual void beginRenderPass(const RenderPassDesc&) = 0;    // color/depth attachments, load/store, clear
  virtual void endRenderPass() = 0;
  virtual void setPipeline(PipelineHandle) = 0;
  virtual void setBindGroup(uint32_t space, BindGroupHandle) = 0;   // Task 3
  virtual void setVertexBuffer(uint32_t slot, BufferHandle, uint64_t offset) = 0;
  virtual void setIndexBuffer(BufferHandle, IndexType, uint64_t offset) = 0;
  virtual void setViewport(const Viewport&) = 0;
  virtual void setScissor(const Rect&) = 0;
  virtual void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) = 0;
  virtual void drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t baseVertex, uint32_t firstInstance) = 0;
};
Result<std::unique_ptr<IDevice>> createDevice(const DeviceDesc&);   // exit 3 if backend absent
}
```

Notes on the interface:
- Handles are 32-bit generational indices (`struct BufferHandle { uint32_t id; }`, ...). A stale handle asserts.
- `ShaderLibrary::load(dir, shader, entry, Backend, key) -> Result<ShaderBlob>` reads the blob and the `.refl.json`.
- NDC conventions are fixed by the RHI, not per app:
  - clip space Z ∈ [0,1];
  - framebuffer origin top-left;
  - texture UV (0,0) = first row in memory.
  - Vulkan uses a negative-height viewport (`VK_KHR_maintenance1`, core 1.1).
- Vulkan minimum: 1.2 with `dynamicRendering` (1.3 core, or the `VK_KHR_dynamic_rendering` extension). There are no VkRenderPass objects. A missing feature gives exit 3 with the feature named.

- [ ] **Step 1: Write the failing tests.**
  - `test_shader_library`: loads `triangle.VSMain` for both backends. The reflection fields equal the F1 JSON. A missing file gives `Error{2, "shader not found: <path>"}`.
  - `test_triangle` (doctest), per backend available on the machine:
    - render `builtin:triangle` 64×64 offscreen and read it back;
    - the vertex colours are red at top-left, green at top-right, blue at bottom;
    - the pixel probes at the three vertex-near points are within ±2/255 of pure R/G/B;
    - a background probe is the clear colour (0.1, 0.1, 0.1);
    - `validationErrorCount() == 0`.
  - Negative validation tests:
    - Vulkan: `draw` without `setPipeline` gives exit 4 and a message containing `VUID`.
    - D3D11: draw with a mismatched input layout gives exit 4.
- [ ] **Step 2: Run, all FAIL** (no `engine/rhi`).
- [ ] **Step 3: Implement** the interface, both backends, the registry and `headless --backend vulkan|d3d11 --scene builtin:triangle`.
  - Each validation callback logs through `eng::log` and increments the error counter.
  - `submitAndWait` returns exit 4 when the counter is greater than 0.
- [ ] **Step 4: Run.** ctest is green. Both commands below exit 0 and write PNGs that are equal within FLIP 0.01:
  - `headless --backend vulkan --scene builtin:triangle --out build/f2/tri_vk.png`
  - `headless --backend d3d11 --scene builtin:triangle --out build/f2/tri_d3d11.png`
- [ ] **Step 5: Commit** `feat(rhi): interface + Vulkan and D3D11 triangle`.

### Task 3: Resources, bind groups, textured cube (Vulkan)

**Files:**
- Modify: `engine/rhi/Rhi.h` (`BindGroupLayout` derived from reflection), `engine/rhi/vulkan/*`
- Create: `tests/unit/rhi/{test_bind_group.cpp,test_cube.cpp}`, `shaders/test/textured.hlsl`, `tests/assets/f02/cube_faces.png` (committed, 256×256, 6 tiles with distinct colours and a letter per face, generated by `scripts/make_f02_assets.py`, which is committed)

**Interfaces:**
- `BindGroupDesc { PipelineHandle pipeline; uint32_t space; std::vector<BindGroupEntry> entries; }`
- `BindGroupEntry { std::string name; std::variant<BufferBinding, TextureHandle, SamplerHandle> resource; }`
- Entries are matched **by reflection name**, never by index, so the app never sees per-backend slots.
- Missing entry → `Error{2, "bind group space <S> missing <name>"}`. Unknown name → `Error{2, "bind group space <S> has no resource <name>"}`.
- Vulkan descriptor sets: one `VkDescriptorSetLayout` per space, merged across the pipeline's stages; descriptor pool per frame.
- Uniform buffers: `BufferUsage::Uniform` with a per-frame ring allocator `IDevice::allocTransient(size, align) -> BufferBinding` (256-byte alignment).
- Textures: mips are uploaded via a staging buffer (CPU-generated mips for now; GPU mip generation comes in F3). `Format::RGBA8_SRGB` is supported.
- Depth: `D32_FLOAT`.

- [ ] **Step 1: Write the failing tests.**
  - `test_bind_group`: missing / unknown entry messages as above.
  - `test_cube` (Vulkan), 128×128, fixed camera, the cube rotated 30°/20°:
    - probes at the three visible face centres equal the face colours of `cube_faces.png` within ±3/255 (sRGB decode + linear sampling, no lighting);
    - a depth-test probe shows the near face covering the far edge;
    - `validationErrorCount() == 0` with sync validation on.
- [ ] **Step 2: FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** ctest is green. `headless --backend vulkan --scene builtin:textured-cube` exits 0.
- [ ] **Step 5: Commit** `feat(rhi/vulkan): buffers, textures, bind groups, textured cube`.

### Task 4: Same on D3D11 (FL11_x)

**Files:** Modify `engine/rhi/d3d11/*`, `tests/unit/rhi/test_cube.cpp` (parametrised over the backends present).

**Interfaces:**
- D3D11 binding:
  - a bind group resolves each entry to `(stage, register class, slot)` from **each stage's** `targetBinding.register`;
  - `setBindGroup` calls `VS/PSSetConstantBuffers` / `ShaderResources` / `Samplers` per stage;
  - UAVs go through `OMSetRenderTargetsAndUnorderedAccessViews` with `UAVStartSlot = renderTargets`.
- Transient uniforms: `D3D11_USAGE_DYNAMIC` ring with `MAP_WRITE_NO_OVERWRITE`; constant-buffer offsets via `ID3D11DeviceContext1::VSSetConstantBuffers1`.
- Mixed DXBC paths: `createGraphicsPipeline` rejects a VS/PS pair whose reflection `dxbcPath` fields differ. If shaderc does not write `dxbcPath` into the reflection JSON yet, add it in this task.

- [ ] **Step 1: Write the failing tests.**
  - `test_cube` on D3D11 with the same probes.
  - New test: the cube image on D3D11 vs Vulkan, mean FLIP ≤ 0.01.
  - Negative validation test: a texture bound as SRV while it is the render target gives exit 4.
  - Mixed DXBC paths give the error above.
- [ ] **Step 2: FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** ctest is green on both backends. Also with `--warp`, so CI can run D3D11 later.
- [ ] **Step 5: Commit** `feat(rhi/d3d11): resources, per-stage binding, textured cube`.

### Task 5: Headless golden cases + PBR shader parity (spec §5.4, D3D11 vs Vulkan leg)

**Files:**
- Create:
  - `apps/headless/Scenes.{h,cpp}` (`builtin:triangle`, `builtin:textured-cube`, `builtin:pbr-spheres`)
  - `tests/golden/f02/cases.json`
  - `tests/golden/ref/f02/*.png`
- Modify: `scripts/run_golden.py` (case fields `backend`, `compareWith` = another case's output, `probes` = `[{x,y,rgba,tol}]`), `tools/golden/report.py` (show probes + backend pair), `docs/spikes/f01-shader-pipeline.md` + spec §5.4 (parity split note), `docs/worklist.md`

**Interfaces:**
- `builtin:pbr-spheres` is a 5×5 grid of UV spheres (metallic along X, roughness along Y), drawn with the F1 `pbr.hlsl`.
  - Permutations: Vulkan `spirv T2_SKIN0_MORPH0_UNLIT0`, D3D11 `dxbc T1_SKIN0_MORPH0_UNLIT0`. Their bodies are identical in F1 (F1 Task 4 review).
  - Plus one unlit sphere row.
  - IBL inputs are generated on the CPU at startup and are deterministic:
    - a 32×32 cubemap with a vertical sky/ground gradient and a sun spot;
    - its CPU-prefiltered mips and a 32×32 irradiance cube;
    - a 128×128 BRDF LUT (Sample-Viewer formula).

  This is not reference mode; reference mode is F3.
- References: `tests/golden/ref/f02/<case>.png` = the D3D11 output committed **after** the probes pass. Cases compare each backend against the reference **and** Vulkan directly against D3D11. Thresholds are set per case in `cases.json` with a reason, starting at mean FLIP 0.01 for triangle and cube and 0.03 for pbr-spheres.
- Parity split: the spec §5.4 runtime clause is done here for **D3D11 vs Vulkan**. The **D3D12 leg moves to the F7 gate**. Write this in spec §5.4, the spike doc and the worklist.

- [ ] **Step 1: Write the failing cases.**
  - `cases.json`: 3 scenes × 2 backends + 3 cross-backend compares.
  - Probes for `pbr-spheres`:
    - the rough dielectric sphere centre is darker than the smooth one in the sun direction;
    - the unlit row equals `baseColorFactor` exactly (±1/255);
    - the background equals the cubemap lookup.
  - Plus the Review Focus 5 case: `--out` under `build/f2/çıktı klasörü/`.
- [ ] **Step 2: FAIL** (scenes missing).
- [ ] **Step 3: Implement.** Generate the references and inspect them by eye in `report.html` before committing.
- [ ] **Step 4: Run.** `python scripts/run_golden.py --phase f02 --build-dir build/win-msvc --config Debug` exits 0. `tests/golden/out/f02/report.html` shows all cases PASS with the diff maps.
- [ ] **Step 5: Commit** `test(f2): headless golden cases, PBR D3D11/Vulkan parity`.

### Task 6: Window, swapchain, viewer with RHI-drawn ImGui

**Files:**
- Create:
  - `engine/platform/{Window.h,win32/Win32Window.cpp}`
  - `engine/rhi/vulkan/VkSwapchain.cpp`, `engine/rhi/d3d11/D3D11Swapchain.cpp`
  - `engine/ui/{ImGuiRenderer.h,ImGuiRenderer.cpp}`, `shaders/ui/imgui.hlsl`
  - `apps/viewer/{CMakeLists.txt,main.cpp,Panels.cpp}`
  - `tests/viewer/CMakeLists.txt` (ctest entries)
- Modify: `cmake/Dependencies.cmake` (imgui pinned tag)

**Interfaces:**
- `Window::create(WindowDesc{title, width, height}) -> Result<std::unique_ptr<Window>>`.
  - The Window provides `nativeHandle()`, `pollEvents()`, `size()`, `minimized()` and `shouldClose()`.
  - It owns DPI awareness (per-monitor v2).
- `SwapchainDesc { NativeWindow; uint32_t width, height; Format format = BGRA8_SRGB; bool vsync; uint32_t imageCount = 3; }`
  - Vulkan: FIFO when vsync is on; MAILBOX, else IMMEDIATE, when it is off.
  - D3D11: `DXGI_SWAP_EFFECT_FLIP_DISCARD` with `ALLOW_TEARING` when vsync is off.
  - A `resize(w, h)` with w or h = 0 does nothing; rendering skips while minimised.
- Viewer CLI:
  - `viewer --backend vulkan|d3d11 [--warp] [--scene builtin:...] [--cvar n=v]... [--frames N] [--screenshot <png>] [--script resize] [--size WxH]`.
  - `--frames N` renders N frames and exits 0.
  - `--screenshot` saves the last frame (swapchain back buffer readback).
  - `F12` saves to `Saved/Screenshots/<backend>-<yyyymmdd-hhmmss>.png`.
  - `--script resize` drives sizes 800×600 → 1×1 → minimise → restore → 1280×720 over 60 frames.
  - CVars: `r.VSync` (default 1), `r.Viewer.ShowUI`.
- ImGui panels:
  - DeviceCaps table (every field + backend + adapter name + tier);
  - FPS and frame-time graph (last 240 frames);
  - CVar list with editing at `CVarSource::Console`.
- Default scene: the cube rotating at 45°/s.

- [ ] **Step 1: Write the failing ctest entries.**
  - `viewer.frames.{vulkan,d3d11}`: `--frames 120 --screenshot ...` exits 0 within 60 s. The screenshot has non-background pixels at the cube centre.
  - `viewer.resize.{vulkan,d3d11}`: exit 0 and validation 0.
  - `viewer.ui.{vulkan,d3d11}`: with the UI on, a probe in the panel area differs from the UI-off screenshot.
- [ ] **Step 2: FAIL.**
- [ ] **Step 3: Implement.** ImGui is drawn only through `IDevice` / `ICommandList`, with the font atlas as an RHI texture. Do not use the `imgui_impl_dx11` or `imgui_impl_vulkan` sources.
- [ ] **Step 4: Run** ctest. Also run the viewer by hand on both backends and check that the cube rotates and the UI responds. Record the steady fps of both backends at 1280×720 with vsync off in the worklist.
- [ ] **Step 5: Commit** `feat(viewer): window, swapchains, RHI ImGui, rotating cube`.

### Task 7: T0 measurement — D3D11 FL10_0 + SM 4.0 shaders + DeviceCaps

**Files:**
- Modify:
  - `tools/shaderc/Compiler.cpp` (`dxbc` with `TIER=0` uses `vs_4_0`/`ps_4_0`; the shader model is recorded in the reflection as `"shaderModel":"4_0"`)
  - `shaders/pbr/permutations.json` (dxbc tiers `[0,1]`; update `excluded`)
  - `engine/rhi/d3d11/D3D11Device.cpp` (`d3d11FeatureLevelCap`; caps query)
  - `engine/rhi/DeviceCaps.h`, `engine/rhi/vulkan/VkDevice.cpp` (caps query)
  - spec §6.2 (measured column)
- Create:
  - `docs/caps/{d3d11-fl10_0-hw.json,d3d11-fl10_0-warp.json,d3d11-fl11_1-hw.json,vulkan-hw.json}`
  - `apps/headless` flag `--dump-caps <json>`
  - `tests/unit/rhi/test_caps.cpp`

**Interfaces:**
- `DeviceCaps` has these fields:
  - `tier`, `apiVersion`, `adapterName`, `vendorId`
  - `maxTexture2D`, `maxCubeSize`, `maxColorAttachments`, `maxVertexAttributes`
  - `maxUniformBufferBytes`, `maxBindingsPerStage{cbuffer,texture,sampler,storage}`
  - `computeShaders`, `storageBuffers`, `vertexStorageBuffers`
  - `formatSupport` (per `Format`: sample/render/blend/filter/depth)
  - `bc`, `etc2`, `astc`
  - `msaaSampleCounts`, `float16`, `waveOps`, `rayTracing`, `bindless`, `timestampQueries`
- D3D11 tier: FL10_x gives 0, FL11_x gives 1.
- `--dump-caps` writes these fields as JSON.

- [ ] **Step 1: Write the failing tests.**
  - FL10_0 device (hardware with `d3d11FeatureLevelCap=0xa000`, and WARP):
    - `caps().tier == 0`;
    - `computeShaders` reports the real `D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS` value;
    - creating the T1 PBR pipeline gives `pipeline needs TIER 1, device is TIER 0` (Review Focus 4);
    - the T0 triangle and the T0 PBR permutation (`dxbc T0_SKIN0_MORPH0_UNLIT0`, SM 4.0) render with validation 0.
  - The T0 `pbr-spheres` image vs FL11 is within the gated FLIP threshold of 0.05. Record the reason: T0 vs T1 shader bodies; this is a K-C regression gate, not a reference gate.
- [ ] **Step 2: FAIL.**
- [ ] **Step 3: Implement.** Dump caps for the four configurations.
  - Update spec §6.2 with a "measured F2" note per T0 row: D3D11 FL10_0 values; GLES 3.0 and WebGL2 stay "assumed" until F10/F11.
  - Record explicitly in the spec, the worklist and each caps JSON (`"note"`) that the FL10_0 values are API limits on an RTX 3070 Ti and on WARP, **not real FL10 hardware**.
- [ ] **Step 4: Run** ctest and `compile_shaders.py`. `failed: []`, and the dxbc set count is now 16.
- [ ] **Step 5: Commit** `feat(rhi): D3D11 FL10_0 (SM 4.0) + DeviceCaps measurement`.

### Task 8: Phase gate + bench + worklist

**Files:** Modify `docs/worklist.md` (f02.t01..t08 rows + f02 phase row) and `apps/headless` (`--bench` on the GPU backends).

- [ ] **Step 1: Run the bench.** `headless --backend vulkan|d3d11 --scene builtin:pbr-spheres --width 2560 --height 1440 --bench 300` in Release. Record `frame_ms_avg` and `frame_ms_p95` per backend in the worklist. This is the baseline for the K-E 10% regression rule.
- [ ] **Step 2: Run the full gate.** In a fresh clone at a short path:
  - `fetch_shader_tools.py` exits 0;
  - configure and build Debug + Release give 0 warnings in our targets;
  - `ctest` all pass with 0 skipped;
  - `unittest` OK;
  - `run_golden --phase f02` exits 0;
  - `viewer.*` ctest entries pass.
- [ ] **Step 3: Update the worklist rows.** Include the K-D deviation note, the parity split (D3D12 leg F7) and the measured-caps note. Open issues stay listed:
  - Metal PENDING-CI;
  - CI PENDING-CI;
  - MSL buffer-index vs vertex-buffer collision (F13);
  - direct-light BRDF untested until F3;
  - GLES 3.2 vertex SSBO check (F11).
- [ ] **Step 4: Commit and push** `docs(worklist): F2 gate evidence`. Push to `f02-rhi`; never push to `main`.
