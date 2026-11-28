# F3 — glTF Loader + Core PBR Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Load every core glTF 2.0 feature (spec §7.1) and the F3 decoders: Draco, meshopt, mesh quantization, KTX2/Basis and WebP. Render with metal-roughness PBR, image-based lighting and punctual lights + shadows on Vulkan and D3D11. Reference mode (K-B) must match Khronos glTF-Sample-Viewer images on a defined core subset of glTF-Sample-Assets. The library facade (K-D) is introduced, and `apps/viewer` shows a real `.glb`.

**Architecture:**
- `engine/asset` parses with fastgltf into a CPU `ModelData`: decoded vertex streams, images and materials, with no GPU types.
- `engine/scene` holds the node hierarchy, world transforms, cameras, lights and skins in rest pose. Animation playback is F4.
- `engine/renderer` owns GPU upload, the material bind groups (space2) and the draw bind groups (space3). It sorts draw lists (opaque / mask / blend back-to-front) and runs the passes: shadow (normal mode only), opaque, skybox, transparent, then tonemap.
- The IBL is a GPU prefilter where compute exists (T1/T2) and a CPU prefilter at T0.
- The public facade is `include/engine/{Renderer,Scene,AssetHandle,Settings,Camera}.h`. `apps/headless` and `apps/viewer` use only the facade from Task 4 on (K-D; ends the F2 deviation).

**Tech Stack:** C++20 (`engine`), C++23 where `std::expected` is used (as F2 `engine_rhi`).
Pinned FetchContent:
- fastgltf (+ simdjson as its dependency)
- draco (decoder only)
- meshoptimizer
- KTX-Software `libktx` read + Basis transcoder
- libwebp (decode only)
- MikkTSpace (`mikktspace.c`)
- stb_image (PNG/JPEG/HDR, already pinned)

Also the F2 RHI and F1 `shaderc`, and `tools/refgen` (Firefox only) for new references.

**Spec:** `docs/superpowers/specs/2026-11-27-cross-platform-gltf-renderer-design.md`:
- §7.1, §7.2 (F3 extensions), §7.3
- §6, §6.2, §8 (cvars), §10, §11 F3
- §14 K-B, K-C, K-D, K-E, K-F, K-G

Also `tools/refgen/README.md` and `tests/golden/ref/sample-viewer/settings.json`.

## Global Constraints

- All F0–F2 Global Constraints apply:
  - validation on, any message gives exit 4, a missing layer is a failure;
  - exit codes 0/2/3/4/5;
  - no process kills;
  - reviewers clone from origin at a short path;
  - CI = PENDING-CI;
  - no browser download, Firefox only for refgen;
  - commit trailer.
- **Reference mode** (`r.ReferenceMode=1`) reproduces `tests/golden/ref/sample-viewer/settings.json` by value:
  - `Cannon_Exterior.hdr` (sha256 pinned there), environment rotation 90°, blurred environment background;
  - Khronos PBR Neutral tonemap, exposure 1.0, IBL intensity 1.0;
  - punctual lights off unless a case says `usePunctual`;
  - fit camera as `UserCamera.resetView` (yfov 45°, rest pose, after `applyTransformHierarchy`);
  - scene `gltf.scene ?? 0`;
  - 512×512 output;
  - no shadows, TAA, bloom or MSAA.

  The BRDF, IBL filtering (sample counts, LUT, mip mapping of roughness) and tonemap follow glTF-Sample-Renderer v1.1.0 (`d59d1d6`) source by value. Cite the file in a comment where each formula is ported.
- **Normal mode** (`r.ReferenceMode=0`, the default) adds shadows and punctual lights. It is tested against our own D3D11 goldens (K-C).
- Per-asset FLIP thresholds live in `tests/golden/f03/cases.json`. Each has a `thresholdReason`. A default threshold is not allowed for a case that fails it.
- Loader behaviour per spec §7.2:
  - an unknown extension in `extensionsRequired` gives exit 2 `unsupported required extension <name>`;
  - one in `extensionsUsed` gives a warning and is ignored;
  - a known later-phase extension (for example `KHR_materials_clearcoat`) in `extensionsRequired` gives exit 2 `<name> is supported from F<n>`;
  - in `extensionsUsed` it is parsed and ignored with an info log.
- Unbounded inputs are capped by cvars, and exceeding a cap gives a warning, never a crash (§7.1 "sınırsız-boyut uyarıları"):
  - `r.Asset.MaxTextureSize` (default 8192; larger images are downscaled with a warning);
  - `r.Asset.MaxVertices` (16M);
  - `r.Asset.MaxFileBytes` (2 GiB).
- Skins and morph targets render at rest pose with skinning applied (joint matrices from node world transforms at t = 0, morph weights from `mesh.weights`). Playback is F4. Animated assets in the reference set are sampled at t = 0, as refgen does.

## Review Focus

1. **Malformed / hostile files.** These must give exit 2 with a message, never a crash, hang or out-of-bounds read:
   - truncated `.glb`;
   - accessor out of buffer range;
   - index ≥ vertex count;
   - a data-URI with bad base64;
   - an image that fails to decode;
   - a 2^31 accessor count.

   The Task 2 fuzz-style test uses fixtures under `tests/assets/f03/bad/`. Every fixture runs under an ASan build (`ENG_ASAN=ON`, MSVC `/fsanitize=address`).
2. **Non-ASCII / spaced paths** for `.gltf` files with external `.bin` / images in the same and in sibling directories, including URI-encoded names (`%20`, UTF-8). (Task 2 test.)
3. **sRGB vs linear textures.** baseColor and emissive are sRGB; metallicRoughness, normal and occlusion are linear. This also holds for KTX2 (`KHR_DF` transfer function) and for transcoded BC/RGBA formats. A swapped case must fail a probe. (Task 3 test with `TextureLinearInterpolationTest` + a crafted swap mutation.)
4. **Missing attributes.**
   - No normals → flat normals.
   - No tangents with a normal map → MikkTSpace.
   - No UVs with a texture → warning, sampled at (0,0).
   - No material → the default material.

   Each case must render, not error. (Task 2/4 tests.)
5. **Camera inside or at zero size.** The fit camera on a degenerate bounding box (single point, `Cameras` asset, empty scene) gives a finite camera and a background-only image, not NaNs. (Task 5 test.)

---

### Task 1: F2 carry-overs

**Files:** `engine/rhi/d3d11/D3D11BindGroups.cpp`, `engine/rhi/vulkan/VkDevice.cpp`, `apps/viewer/{main.cpp,Panels.cpp}`, `tests/viewer/run_viewer.py`, `tests/golden/CMakeLists.txt`, `tests/unit/CMakeLists.txt`, `cmake/Dependencies.cmake`, `apps/headless/CubeScene.cpp`, `tests/unit/rhi/*`, spec §6.2, `scripts/make_f02_assets.py`

**Interfaces:**
- D3D11 `setBindGroup`:
  - requires a bound pipeline;
  - compares the group layout (names, kinds, per-stage register class/slot) with the pipeline's layout for that space;
  - on mismatch it logs `bind group space <S> layout does not match pipeline`, counts a validation error (exit 4) and skips the bind.
- CB binding count is clamped to the buffer size; uniform buffers are padded to 256 bytes at creation.
- CMake option `ENG_REQUIRE_RHI_BACKENDS` (default `vulkan;d3d11` on Windows). Configure fails when a listed backend is not built.
- The headless/viewer skip regexes match only the backend-absent text `backend not available:`.
- Vulkan `computeShaders` = the chosen queue family has `VK_QUEUE_COMPUTE_BIT`.

- [ ] **Step 1: Write the failing tests.**
  - D3D11: a group made for pipeline A bound with pipeline B (different slots) gives exit 4.
  - A <256-byte uniform buffer bound with size 0 gives no debug-layer message.
  - Culled-cube case: `CullMode::Back` with a probe on both backends.
  - `viewer.frames` screenshot under `çıktı klasörü/ekran görüntüsü.png`.
  - The `dump_caps` JSON is parsed: `tier`, `apiVersion` and `note` are present, and the field names equal `forEachCapsField`.
  - An exit-3 tier rejection on a present backend is reported Failed, not Skipped.
- [ ] **Step 2: FAIL.**
- [ ] **Step 3: Implement.** Also:
  - the minimised viewer waits with `MsgWaitForMultipleObjects` (16 ms);
  - the device panel value column stretches, with a tooltip for long names;
  - spec §6.2 storage-buffer row says "D3D11 feature-level rule, not a device query";
  - fix the `make_f02_assets.py` references.
- [ ] **Step 4: Run** ctest (0 skipped) and unittest.
- [ ] **Step 5: Commit** `chore(f3): F2 carry-overs`.

### Task 2: glTF loader core (CPU, `engine/asset`)

**Files:**
- Create:
  - `engine/asset/{ModelData.h,GltfLoader.h,GltfLoader.cpp,Accessors.cpp,Primitives.cpp,Tangents.cpp,Images.cpp,Extensions.cpp,CMakeLists.txt}`
  - `tests/unit/asset/{test_loader.cpp,test_bad_files.cpp,test_conformance.cpp}`
  - `tests/assets/f03/{bad/*,paths/*}`
- Modify: `cmake/Dependencies.cmake` (fastgltf, simdjson, mikktspace)

**Interfaces:**
```cpp
namespace eng::asset {
struct LoadOptions { uint32_t maxTextureSize = 8192; uint64_t maxVertices = 16u << 20; uint64_t maxFileBytes = 2ull << 30;
                     bool generateTangents = true; };
struct Diagnostic { enum Level { Info, Warning } level; std::string message; };
Result<ModelData> loadGltf(const std::filesystem::path&, const LoadOptions&, std::vector<Diagnostic>* diags);
}
```

`ModelData` is plain data:
- `scenes`, `defaultScene`;
- `nodes` (parent/children, TRS or matrix, mesh/camera/skin/light index);
- `meshes` → `primitives`. A primitive has:
  - topology: `PointList|LineList|LineStrip|TriangleList|TriangleStrip`. LINE_LOOP and TRIANGLE_FAN are converted to an index list here;
  - streams: decoded `float32` position/normal/tangent and `float32x2` UV0..UV3, `float32x4` color0, `uint16x4` joints0/1, `float32x4` weights0/1;
  - `uint32` indices;
  - morph targets (position/normal/tangent deltas);
  - material index;
  - `bounds`.
- `materials`: core metal-roughness fields + `alphaMode` / `alphaCutoff` / `doubleSided` + texture refs (image, sampler, texCoord) + a raw `extensions` JSON per material for F6.
- `images` (decoded or compressed payload, `colorSpace` sRGB/linear from usage);
- `samplers`;
- `cameras` (perspective/ortho);
- `skins`;
- `animations` (parsed, kept for F4);
- `lights` (KHR_lights_punctual);
- `extensionsUsed`.

Normalized integer and sparse accessors are decoded to float on load. A primitive with no indices gets a generated 0..n-1 list.

- [ ] **Step 1: Write the failing tests.**
  - `test_loader`, on Sample-Assets:
    - `Box`, `BoxTextured` (.glb and .gltf, embedded and data-URI variants);
    - `SimpleSparseAccessor`: decoded position equals the hand values in the file;
    - `MeshPrimitiveModes`: all 7 modes, with fan and loop converted to the expected index count;
    - `MultiUVTest`, `VertexColorTest`;
    - `NormalTangentTest` with tangents stripped: MikkTSpace output equals the file's tangents within 1e-3 on ≥ 99% of vertices;
    - `Cameras`, `SimpleSkin`, `SimpleMorph` (parsed counts).
  - `test_bad_files` (Review Focus 1): each fixture gives exit 2 with its message substring.
  - Paths (Review Focus 2) pass.
  - `test_conformance` (§7.3.1):
    - every model in `tests/assets/glTF-Sample-Assets/Models` whose variants are `glTF`, `glTF-Binary` or `glTF-Embedded` loads with 0 errors;
    - one that uses an extension planned for F4–F16 loads with an info log;
    - glTF-Validator (pinned, from `fetch_assets`) runs on each, and a validator error = our loader error, case by case;
    - the list and results are written to `build/f03/conformance.json`.
- [ ] **Step 2: FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the tests, also in the ASan build for `test_bad_files`.
- [ ] **Step 5: Commit** `feat(asset): glTF 2.0 core loader`.

### Task 3: Decoders — Draco, meshopt, quantization, KTX2/Basis, WebP

**Files:**
- Create: `engine/asset/{Draco.cpp,Meshopt.cpp,Ktx2.cpp,Webp.cpp}`, `tests/unit/asset/test_decoders.cpp`
- Modify: `cmake/Dependencies.cmake`, `engine/asset/Extensions.cpp`, `engine/rhi` (BC1–BC7 / ETC2 / ASTC formats added to `Format` + upload paths on both backends)

**Interfaces:**
- `KHR_draco_mesh_compression`, `EXT_meshopt_compression` (and `KHR_meshopt_compression` with the same decoder) and `KHR_mesh_quantization` decode into the same `ModelData` streams.
- KTX2 (`KHR_texture_basisu`): transcode target chosen from `DeviceCaps`:
  - `BC7` if supported, else `BC1/BC3`, else `RGBA8`, for ETC1S and UASTC alike;
  - the transfer function is taken from the KTX2 DFD.

  The image keeps the compressed payload in `ModelData`, with `gpuFormat` chosen at upload. Desktop ETC2/ASTC paths exist for F11.
- `EXT_texture_webp` decodes to RGBA8.

- [ ] **Step 1: Write the failing tests.** For each Sample-Assets variant directory `glTF-Draco`, `glTF-Meshopt`, `glTF-Quantized` and `glTF-KTX-BasisU` (the implementer enumerates the existing ones at the pinned commit and lists them in the test):
  - the decoded vertex count equals that of the plain `glTF` variant;
  - the positions' bounds match within the quantization step.
  - KTX2 → BC7 then CPU-decoded vs the PNG source: mean abs error ≤ 8/255.
  - Review Focus 3:
    - an sRGB KTX2 is uploaded as `*_SRGB`;
    - the swapped-colour-space mutation fails the `TextureLinearInterpolationTest` probe.
- [ ] **Step 2: FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.**
- [ ] **Step 5: Commit** `feat(asset): Draco, meshopt, quantization, KTX2/Basis, WebP`.

### Task 4: Scene, renderer core, facade

**Files:**
- Create:
  - `engine/scene/{Scene.h,Scene.cpp,FitCamera.cpp}`
  - `engine/renderer/{Renderer.cpp,GpuModel.cpp,DrawList.cpp,Passes.cpp,CMakeLists.txt}`
  - `include/engine/{Renderer.h,Scene.h,AssetHandle.h,Settings.h,Camera.h}`
  - `tests/unit/renderer/test_facade.cpp`
- Modify: `apps/headless/*` and `apps/viewer/*` (facade only; built-in F2 scenes stay as `builtin:*` scenes behind the facade), `shaders/pbr/*` (multi-UV `texCoord` already in the cbuffer; vertex colour; skin/morph at rest pose)

**Interfaces (facade, frozen from F7; changes before then are logged in `docs/facade-changelog.md`):**
```cpp
namespace eng {
struct RendererDesc { rhi::Backend backend; bool validation; bool warp; int adapterIndex; int d3d11FeatureLevelCap;
                      NativeWindow window /* optional, null = offscreen */; uint32_t width, height; };
class Renderer {
public:
  static Result<std::unique_ptr<Renderer>> create(const RendererDesc&);
  Result<AssetHandle> loadAsset(const std::filesystem::path&);          // sync in F3; async job system in F5
  Result<SceneHandle> instantiate(AssetHandle, int sceneIndex = -1);    // -1 = gltf.scene ?? 0
  Camera fitCamera(SceneHandle, float aspect) const;                    // Sample-Viewer resetView
  void setCamera(const Camera&);
  Result<void> renderFrame();                                           // to window or offscreen target
  Result<Image> readback();                                             // last frame, RGBA8
  Settings& settings();                                                 // cvar access (CVarSource::Code / Console)
  void resize(uint32_t w, uint32_t h);
  const rhi::DeviceCaps& caps() const;
};
}
```

- Draw order: opaque + mask front-to-back, then the skybox, then blend back-to-front by node-centre distance (as Sample-Viewer).
- `doubleSided=false` gives back-face cull. Negative-determinant node transforms flip the front face.
- Mask uses `alphaCutoff`.
- One material bind group per material, one draw bind group per node-primitive (world matrix, normal matrix, joint matrices).

- [ ] **Step 1: Write the failing tests.**
  - `test_facade`: create → load `BoxTextured.glb` → instantiate → fit → render → readback on both backends; the centre probe is not background; validation 0.
  - `OrientationTest`, `NegativeScaleTest`: front faces correct (probes).
  - `AlphaBlendModeTest`: the mask cut-out pixel is background; the blend pixel is a mix.
  - `RecursiveSkeletons` / `SimpleSkin` rest pose: renders, validation 0.
  - Default material on a material-less mesh.
- [ ] **Step 2: FAIL.**
- [ ] **Step 3: Implement.** Move both apps to the facade. Remove the F2 K-D deviation note in the worklist.
- [ ] **Step 4: Run** all tests plus the F2 golden cases, which must be unchanged.
- [ ] **Step 5: Commit** `feat(renderer): scene, draw lists, facade`.

### Task 5: Reference mode — IBL, tonemap, background, fit camera

**Files:**
- Create:
  - `engine/renderer/{Ibl.cpp,IblCpu.cpp,Tonemap.cpp,Skybox.cpp}`
  - `shaders/ibl/{equirect_to_cube.hlsl,prefilter_ggx.hlsl,prefilter_lambert.hlsl,brdf_lut.hlsl}`
  - `shaders/post/tonemap.hlsl`
  - `tests/unit/renderer/test_reference_mode.cpp`
- Modify: `scripts/fetch_assets.py` (environment files from `settings.json` `pinnedFiles`, sha256 checked), `engine/core` cvars

**Interfaces:**
- CVars:
  - `r.ReferenceMode` (bool);
  - `r.IBL.Environment` (path or name, default `Cannon_Exterior`);
  - `r.IBL.Rotation` (deg, default 90);
  - `r.IBL.Intensity`;
  - `r.Exposure`;
  - `r.Tonemap = PbrNeutral|Aces|None`;
  - `r.IBL.Resolution`, `r.IBL.SpecularSamples` (K-F);
  - `r.Background = Environment|Color`;
  - `r.Background.Blur`.

  Reference mode forces the `settings.json` values at `CVarSource::Code` and logs any cvar it overrides.
- The prefilter matches Sample-Renderer v1.1.0 `ibl_filtering.js` + shaders by value: cube size, mip count, sample count, LOD bias, Lambertian sample count and LUT resolution. The GPU path is used on T1/T2 and the CPU path at T0. Both produce the same cubes within 2/255 (test).
- The HDR is loaded with stb_image `.hdr` and kept in float.

- [ ] **Step 1: Write the failing tests.**
  - Reference cases for the 5 existing refs (`DamagedHelmet`, `MetalRoughSpheres`, `NormalTangentTest`, `AlphaBlendModeTest`, `TextureCoordinateTest`) on vulkan and d3d11 at 512×512, mean FLIP ≤ 0.05.
    - If an asset cannot reach that, lower its gate only with a reason naming the formula difference found.
    - The threshold must never be raised past 0.10 without escalating in the worklist as BLOCKED.
  - CPU vs GPU prefilter equality.
  - Review Focus 5 degenerate cameras.
- [ ] **Step 2: FAIL.**
- [ ] **Step 3: Implement.** Iterate by comparing diff maps in `report.html`. Record in the worklist each formula ported and the Sample-Renderer file it came from.
- [ ] **Step 4: Run** `run_golden --phase f03`. The 10 cases (5 assets × 2 backends) PASS.
- [ ] **Step 5: Commit** `feat(renderer): reference mode IBL + PBR Neutral`.

### Task 6: Core reference set (§7.3.2 core subset)

**Files:**
- Create: `tests/golden/f03/core_subset.json`
- Modify: `tests/golden/ref/sample-viewer/*.png` + `settings.json`, `tests/golden/f03/cases.json`, `tools/refgen` (only if a needed option is missing; no browser download)

**Interfaces:**
- `core_subset.json` lists every Sample-Assets model at the pinned commit that uses **no extension other than** `KHR_draco_mesh_compression`, `EXT_meshopt_compression`, `KHR_mesh_quantization`, `KHR_texture_basisu`, `EXT_texture_webp` and `KHR_lights_punctual` (render with punctual off, as the refs do).
  - The list is generated by a script from each model's `extensionsUsed`; the script is committed.
  - The plain `glTF` variant is the case. The decoder variants each get one extra case, compared to the same reference.
  - Models above 64 MiB or with known Sample-Viewer limits can be excluded only with a reason in the file.
- References come from `node tools/refgen/render.mjs --models <list> --out tests/golden/ref/sample-viewer`.
  - Firefox only; the `settings.json` merge is by model.
  - The existing 5 PNGs must stay byte-identical (`pngSha256`).

- [ ] **Step 1: Generate the list and the references.** Add the cases (vulkan + d3d11 per model).
- [ ] **Step 2: Run** `run_golden --phase f03`. Record the failures.
- [ ] **Step 3: Fix renderer bugs until every case passes its threshold.** Each per-asset threshold above 0.05 carries a reason. Any case that still fails is marked in `cases.json` as `"expectedFail": "<reason>"` and in the worklist as BLOCKED with evidence.
  - Expected-fail cases may be at most 10% of the subset; otherwise the gate fails.
  - An asset the loader or renderer cannot render is never silently excluded.
- [ ] **Step 4: Run.** The report shows all cases with diff maps.
- [ ] **Step 5: Commit** `test(f3): core Sample-Assets reference set`.

### Task 7: Punctual lights + shadows (normal mode), lighting quality cvars

**Files:**
- Create: `engine/renderer/{Lights.cpp,Shadows.cpp}`, `shaders/pbr/lights.hlsli` (direct BRDF from `brdf.hlsli`; F1 left it unused), `shaders/shadow/shadow.hlsl`, `tests/golden/f03/lights_cases.json`
- Modify: `shaders/pbr/pbr.hlsl` (light loop), `engine/renderer/Passes.cpp`

**Interfaces:**
- `KHR_lights_punctual` directional/point/spot with range and inner/outer cone, as in Sample-Renderer `punctual.glsl` (cite).
- Forward light loop: `r.Lighting.MaxLights` (T0 default 8 per object, T1/T2 64 in F3; clustered is F8) and `r.Lighting.Specular`.
- Shadows:
  - directional: CSM with `r.Shadow.CascadeCount` 1–4;
  - spot: one map;
  - point: a cube map, gated by `r.Shadow.PointLights`.
- More cvars: `r.Shadow.MapResolution`, `r.Shadow.Filter = Hard|PCF`, `r.Shadow.Distance`.
- `sg.ShadowQuality` and `sg.LightingQuality` 0..4 set these with an initial table in `Config/DefaultScalability.ini`. The full Low→Cinematic table and frame times are F5 (K-F).

- [ ] **Step 1: Write the failing cases.**
  - Sample-Viewer refs with `usePunctual` on (refgen option; Sample-Viewer has no shadows) for `LightsPunctualLamp`, compared with shadows off in reference mode.
  - Normal-mode own goldens with shadows on: a directional-light plane + box, plus spot and point. The shadowed probe is darker than the lit probe by ≥ 40%.
  - `sg.ShadowQuality=0` vs 3 differ; `r.Shadow.PointLights=0` removes the point shadow.
- [ ] **Step 2: FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.**
- [ ] **Step 5: Commit** `feat(renderer): punctual lights, CSM/spot/point shadows, lighting cvars`.

### Task 8: Viewer `.glb`, gate, bench

**Files:** `apps/viewer/*`, `tests/viewer/*`, `docs/worklist.md`

**Interfaces:**
- Viewer:
  - `viewer [--backend ...] <file.glb|.gltf>`, plus drag-drop of a file onto the window;
  - orbit camera (LMB rotate, RMB pan, wheel zoom, `F` = fit);
  - panels: asset info (extensions used and their status: loaded / ignored with reason), lights (list + toggles) and the `sg.*` / `r.ReferenceMode` toggles.
- `--frames N --screenshot` work as in F2.

- [ ] **Step 1: ctest entries.**
  - `viewer.glb.{vulkan,d3d11}`: `DamagedHelmet.glb` for 120 frames; the screenshot is not background at the centre; validation 0.
  - `viewer.dragdrop` (posts `WM_DROPFILES`).
- [ ] **Step 2: Implement and run.** Run the viewer by hand with DamagedHelmet on both backends.
- [ ] **Step 3: Bench (Release, 2560×1440).** `headless --bench 300` per backend:
  - `DamagedHelmet` (reference mode);
  - `Sponza` (normal mode, shadows on, 1 directional + 8 point lights).

  Record the numbers in the worklist. This is the new K-E baseline; F2's pbr-spheres must not regress > 10%.
- [ ] **Step 4: Full gate in a fresh origin clone at a short path:**
  - fetch tools + assets;
  - Debug + Release build with 0 warnings in our targets;
  - ctest 0 skipped;
  - unittest;
  - `run_golden --phase f02` and `--phase f03` exit 0;
  - `tests/golden/out/f03/report.html` produced;
  - conformance.json: 0 unexpected errors.
- [ ] **Step 5: Worklist** f03.t01..t08 + f03 row, with open items: CI PENDING-CI, Metal PENDING-CI, animation playback F4, material extensions F6, expected-fail list. Commit `docs(worklist): F3 gate evidence` and push `f03-gltf`.
