# F1 — Shader Pipeline Spike Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove spec §5.4: one HLSL source compiles to all 7 outputs (DXIL, DXBC, SPIR-V, MSL, GLSL ES 3.00, GLSL ES 3.20, WGSL), each output passes its validator, and resource bindings are consistent across targets — for a full glTF core PBR shader with its feature-group permutations (spec §14 K-A).

**Architecture:** `tools/shaderc` = library `shaderc_core` + CLI `shaderc`. HLSL → DXC → DXIL (D3D12) and SPIR-V (Vulkan). SPIR-V → SPIRV-Cross → MSL / GLSL ES / HLSL SM5 (→ FXC → DXBC for D3D11). SPIR-V → Tint → WGSL. Every compile also writes a reflection JSON (one schema for all targets) that F2's RHI uses to build bind groups. Runtime image equality across D3D11/D3D12/Vulkan moves to the F2 gate (no RHI exists yet; spec §5.4's runtime clause is verified there with these exact shaders).

**Tech Stack:** C++20, DXC (official DirectXShaderCompiler release, pinned, with `dxil.dll` for signing), FXC (`fxc.exe` from Windows SDK 10.0.26100), SPIRV-Cross (FetchContent, pinned SHA, as a library), Tint (built from Dawn at a pinned SHA, CLI only: SPIR-V reader + WGSL writer), Vulkan SDK 1.4.341.1 tools (`spirv-val`, `glslangValidator`), nlohmann/json (already pinned in F0).

**Spec:** `docs/superpowers/specs/2026-11-27-cross-platform-gltf-renderer-design.md` (§5.4, §6, §14 K-A, §3 K2)

## Global Constraints

- All F0 Global Constraints still apply (C++20 for `engine`; pins; commit trailer; no closed-source SDK in git).
- Downloaded/built tools live under `third_party/tools/` (git-ignored); versions + sha256 in `third_party/tools/tools.lock.json` (committed).
- No runtime cross-compiler in the engine (K-A). `shaderc` is an offline build tool.
- Metal: `xcrun metal` validation runs only in CI on macOS (K2). CI is currently blocked by a GitHub billing lock (worklist f00.t08) → the Metal step is written and committed but its result is `PENDING-CI`, never "passed".
- Binding convention (all shaders): `register(<type><index>, space<S>)`; **index unique per space across b/t/s/u**. Space = bind group: `space0` frame, `space1` view/pass, `space2` material, `space3` draw/object. Mapping: Vulkan/WGSL `set|group = S`, `binding = index`; MSL buffer/texture/sampler indices = from SPIRV-Cross resource bindings assigned by the reflection (recorded in JSON); GLSL ES = flattened `S*16 + index` for 3.20 `layout(binding)`, name-based for 3.00 (JSON records names); D3D = native registers.
- Feature-group defines (K-A), each `0|1`: `FG_SKIN`, `FG_MORPH`, `FG_CLEARCOAT_SHEEN_SPEC` (reserved, empty in F1), `FG_TRANSMISSION_VOLUME` (reserved, empty in F1), `FG_UNLIT`. F1 implements bodies for core PBR + `FG_SKIN` + `FG_MORPH` + `FG_UNLIT`; reserved groups must compile as no-ops.
- Tier limits used in shaders (spec §14 K-E): T0 joints ≤ 64 (UBO array), active morph targets ≤ 8 (vertex attributes / UBO weights); T1/T2 skin and morph via `StructuredBuffer`. Selected by define `TIER=0|1|2`.

## Review Focus

1. **Binding collision** — a shader using `b0` and `t0` in the same space must be rejected by `shaderc` with an error naming both registers (convention violation), not silently produce colliding Vulkan bindings. (Task 3 test.)
2. **Feature unsupported on a target** (e.g. `StructuredBuffer` with `--target glsl-es300`, TIER≥1) → `shaderc` exits non-zero with a message naming target + feature; it never emits a shader that fails later at driver link. (Task 3 test.)
3. **Validator missing** (tool not fetched) → clear error `tool not found: spirv-val (run scripts/fetch_shader_tools.py)`, exit 2. (Task 1 test.)
4. **Permutation determinism** — compiling the same source+defines twice yields byte-identical output for every target (build caching depends on it). (Task 4 test.)
5. **Non-ASCII / spaced paths** for `--src` and `--out` (repo lives under `C:\Users\Heros\Desktop`) work. (Task 2 test.)

---

### Task 1: Shader toolchain fetch/build

**Files:**
- Create: `scripts/fetch_shader_tools.py`, `third_party/tools/tools.lock.json`, `tests/unit/scripts/test_fetch_shader_tools.py`, `cmake/ShaderTools.cmake`
- Modify: `.gitignore` (`third_party/tools/*` except `tools.lock.json`), `cmake/Dependencies.cmake` (SPIRV-Cross pinned)

**Interfaces:**
- `python scripts/fetch_shader_tools.py [--only dxc|tint|all] [--verify]` — DXC release zip (pinned tag, sha256) → `third_party/tools/dxc/bin/x64/{dxc.exe,dxcompiler.dll,dxil.dll}`; Tint: shallow clone Dawn at pinned SHA, CMake configure with only `TINT_BUILD_CMD_TOOLS=ON TINT_BUILD_SPV_READER=ON TINT_BUILD_WGSL_WRITER=ON` (all other Dawn/Tint options OFF), build Release → `third_party/tools/tint/tint.exe`. Idempotent like F0 `fetch_assets.py` (reuse its helpers, incl. the read-only-file rmtree fix from F0 review).
- `cmake/ShaderTools.cmake` exports cache vars `ENG_DXC`, `ENG_FXC` (found via Windows SDK 10.0.26100 `bin/x64/fxc.exe`), `ENG_TINT`, `ENG_SPIRV_VAL`, `ENG_GLSLANG` (from `$ENV{VULKAN_SDK}/Bin`); missing tool → configure warning, `shaderc` tests for that target are skipped with reason, never silently passed.
- Lock schema: `{"dxc":{"tag","url","sha256"},"dawn":{"repo","commit"},"spirv-cross":{"commit"}}`.

- [ ] Step 1: unittest (network mocked): second run no download; sha mismatch → `sha256 mismatch`; missing tool path → message `tool not found: <name> (run scripts/fetch_shader_tools.py)`.
- [ ] Step 2: run → FAIL. Step 3: implement, run real fetch/build. Step 4: `third_party/tools/dxc/bin/x64/dxc.exe --version` prints the DXC version; the Tint version is the Dawn commit in `third_party/tools/tint/.dawn-commit` (`tint.exe` at the pinned Dawn commit has no `--version` flag; corrected in plan F2 Task 1); unittest PASS.
- [ ] Step 5: commit `build(tools): pinned DXC, Tint, SPIRV-Cross toolchain`.

### Task 2: `shaderc` core + CLI, one trivial shader through all targets

**Files:**
- Create: `tools/shaderc/CMakeLists.txt`, `tools/shaderc/{Compiler.h,Compiler.cpp,Targets.h,Reflection.h,Reflection.cpp,Validate.h,Validate.cpp,main.cpp}`, `shaders/test/triangle.hlsl`, `tests/unit/shaderc/test_targets.cpp`

**Interfaces:**
- `enum class eng::sc::Target { DXIL, DXBC, SPIRV, MSL, GLSL_ES300, GLSL_ES320, WGSL };` `enum class Stage { Vertex, Fragment, Compute };`
- `enum class DxbcPath { Direct, Cross };`
- `struct CompileRequest { std::filesystem::path src; std::string entry; Stage stage; Target target; std::vector<std::pair<std::string,std::string>> defines; std::vector<std::filesystem::path> includeDirs; DxbcPath dxbcPath = DxbcPath::Cross; /* DXBC only */ };`
- `struct CompileResult { std::vector<uint8_t> bytecode; std::string text; /* MSL/GLSL/WGSL/HLSL */ nlohmann::json reflection; std::vector<std::string> diagnostics; };`
- `std::expected<CompileResult, std::string> compile(const CompileRequest&, const ToolPaths&)` and `enum class ValidateStatus { Passed, PendingCI };` `std::expected<ValidateStatus, std::string> validate(Target, const CompileResult&, const ToolPaths&)` (interface text corrected to the implementation in plan F2 Task 1) (spirv-val; `dxc -dumpbin` + dxil.dll signing check; `fxc` success = valid DXBC; `glslangValidator -S <stage>` with `#version 300 es` / `320 es`; `tint --validate` on WGSL; MSL → `PENDING-CI` status, not error).
- CLI: `shaderc compile --src <f> --entry <e> --stage vs|fs|cs --target <t>|all [--define K=V]... [-I dir]... --out <dir> [--validate]`; output names `<stem>.<entry>.<target>.<ext>` + `<stem>.<entry>.<target>.refl.json`; exit 0 ok, 1 compile/validate failure, 2 usage/tool missing.
- DXBC path: implement BOTH (a) source → FXC `ps_5_0/vs_5_0` directly and (b) DXC→SPIR-V→SPIRV-Cross HLSL SM5→FXC; flag `--dxbc-path direct|cross` (default decided in Task 5 from evidence).

- [ ] Step 1: `test_targets.cpp`: for each of 7 targets, compile `triangle.hlsl` VS+FS → non-empty output; `validate` OK (MSL → status `PENDING-CI`); a copy of the file under `ç ğ ü test/` compiles.
- [ ] Step 2: FAIL. Step 3: implement. Step 4: PASS; `shaderc compile --src shaders/test/triangle.hlsl --entry VSMain --stage vs --target all --out build/shaders --validate` exit 0.
- [ ] Step 5: commit `feat(tools): shaderc HLSL to 7 targets with validation`.

### Task 3: Unified reflection + binding convention enforcement

**Files:**
- Modify: `tools/shaderc/Reflection.cpp`, `tools/shaderc/Compiler.cpp`
- Create: `tests/unit/shaderc/test_reflection.cpp`, `shaders/test/{bindings_ok.hlsl,bindings_collide.hlsl,needs_storage.hlsl}`

**Interfaces:**
- Reflection JSON schema (all targets): `{"stage","entry","target","resources":[{"name","kind":"cbuffer|texture|sampler|storage_ro|storage_rw","space","index","arraySize","sizeBytes"?, "targetBinding":{"set"?,"binding"?,"mslIndex"?,"glslBinding"?,"glslName"?,"register"?}}],"vertexInputs":[{"location","semantic","format"}],"pushConstants":null}`. `space`/`index`/`kind`/`name` must be identical across all 7 targets for the same entry (test).
- Convention errors: `binding collision in space<S>: b<i> and t<i>`; `target <t> does not support <feature> (TIER=<n>)`.

- [ ] Step 1: tests: `bindings_ok` → identical (name,kind,space,index) set across targets; `bindings_collide` → error text above; `needs_storage` with `glsl-es300` → unsupported error; with `spirv` → OK.
- [ ] Steps 2–4: FAIL → implement → PASS. Step 5: commit `feat(tools): unified reflection and binding convention checks`.

### Task 4: glTF core PBR shader + permutations

**Files:**
- Create: `shaders/common/{brdf.hlsli,bindings.hlsli,vertex.hlsli,skin.hlsli,morph.hlsli,tonemap.hlsli}`, `shaders/pbr/pbr.hlsl` (`VSMain`, `PSMain`), `shaders/pbr/permutations.json`, `tests/unit/shaderc/test_pbr.cpp`, `scripts/compile_shaders.py`

**Interfaces:**
- `pbr.hlsl` implements glTF 2.0 core metal-roughness exactly as the glTF spec Appendix B / Khronos Sample-Viewer (needed for reference mode K-B): Lambert diffuse + GGX/Smith-correlated/Schlick specular, `baseColorFactor*texture*COLOR_0`, metallicRoughness (B=metal, G=rough), normal map with `scale` (MikkTSpace TBN, tangent.w sign), occlusion with `strength`, emissive, alphaMode as a material-cbuffer uniform branch (`alphaMode` 0/1/2 + `alphaCutoff`; not a define, keeps the count within K-A's ≤ 64), double-sided normal flip, IBL (prefiltered specular cube + irradiance + BRDF LUT bindings), Khronos PBR Neutral tonemap in `tonemap.hlsli`. Texture-coordinate set selection per texture (0/1) and KHR_texture_transform uniform slots exist (identity in F1).
- Bindings per `bindings.hlsli` following the Global convention (space0 frame cbuffer + IBL textures/sampler; space2 material cbuffer + 5 textures + samplers; space3 object cbuffer + skin/morph resources).
- `permutations.json`: list of define sets = cartesian of `TIER∈{0,1,2}` × `FG_SKIN` × `FG_MORPH` × `FG_UNLIT` (= 24 per stage before per-target exclusions; ≤ 8 per target per stage), with per-target exclusions (`TIER≥1` excluded for glsl-es300; `TIER=2` only for dxil/spirv/msl/wgsl... record the exact matrix in the file).
- `python scripts/compile_shaders.py --out build/shaders [--jobs N]` compiles every permutation × target × stage, runs validation, writes `build/shaders/summary.json` (`{"total","ok","failed":[...],"pendingCi":[...],"msTotal"}`), exit 1 on any failure.

- [ ] Step 1: `test_pbr.cpp`: one permutation per tier compiles+validates for every allowed target; reflection space/index identical across targets; compiling twice → byte-identical outputs (Review Focus 4).
- [ ] Steps 2–4: FAIL → implement → PASS; `compile_shaders.py` → `failed: []`.
- [ ] Step 5: commit `feat(shaders): glTF core PBR shader with feature-group permutations`.

### Task 5: Spike report, D3D11 path decision, macOS CI step

**Files:**
- Create: `docs/spikes/f01-shader-pipeline.md`
- Modify: `.github/workflows/ci.yml` (add job `macos-metal` on `macos-latest`: build `shaderc` deps not needed → run a small script that takes the committed MSL outputs from `compile_shaders.py` artifact and runs `xcrun -sdk macosx metal -c` and `xcrun -sdk iphoneos metal -c` on each), `docs/worklist.md` (rows f01.t01–t05), spec §5.4 (D3D11 path decided, "PENDING-CI" note)

**Interfaces:**
- Report contents: per target — tool versions, output size, compile ms (from summary.json), validator result; DXBC path comparison (direct vs cross: success rate across all DXBC permutations, output size, any semantic loss) → decision + evidence; per-target limitations found (e.g. GLSL ES 3.00 constraints, WGSL uniformity issues, MSL argument mapping); risks for F2+.
- Committed MSL artifacts for CI: `tests/shaders/msl/*.metal` (generated, small subset: one per tier × FG combo actually used) so the macOS job needs no Windows tools.

- [ ] Step 1: write report from real `summary.json`; decide DXBC default; set `--dxbc-path` default accordingly; re-run `compile_shaders.py` → green.
- [ ] Step 2: add macOS job; commit; push branch `f01-shader-spike`. Record CI result; with the billing lock it stays `PENDING-CI` with the annotation text as evidence.
- [ ] Step 3: commit `docs(spike): F1 shader pipeline report and D3D11 path decision`.

## F1 Gate (reviewer runs all of these itself)

1. Clean clone of `f01-shader-spike` → `python scripts/fetch_shader_tools.py` → configure + build Debug green (0 warnings in `engine`), `ctest` 100%, `python -m unittest discover tests` OK.
2. `python scripts/compile_shaders.py --out <tmp>` → `failed: []`; every non-MSL output validated by its validator; MSL listed under `pendingCi`.
3. Reflection consistency test passes; Review Focus 1–5 tests exist and pass.
4. `docs/spikes/f01-shader-pipeline.md` numbers match a fresh `summary.json` (spot-check 3 rows).
5. Metal: CI job exists; result reported honestly (`PENDING-CI` + billing-lock evidence, or real result if CI is unlocked).
