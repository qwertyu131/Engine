# F1 spike: shader pipeline (HLSL -> 7 targets)

- Date: 2026-11-27. Branch `f01-shader-spike`. Plan: `docs/superpowers/plans/2026-11-27-f01-shader-pipeline-spike.md` (Task 5).
- Spec: §5.4 (named risk), §6.1 (tiers), §14 K-A (permutation model), §3 K2 (no Mac).
- Machine: Windows 11, RTX 3070 Ti, VS 2026 (MSVC), CMake 4.3.2.

## Result

One HLSL source, `shaders/pbr/pbr.hlsl`, compiles to all 7 outputs. It covers glTF core metal-roughness,
normal, emissive, occlusion, IBL, skin, morph and unlit. The matrix is 56 define sets x 2 stages = 112
compiles. Every non-MSL output passes its validator.

MSL is `PENDING-CI`. `xcrun metal` runs only on macOS (K2), and the CI job cannot run while the GitHub
account is billing-locked (worklist f00.t08).

The runtime clause of §5.4 (same image on D3D11 / D3D12 / Vulkan, FLIP < threshold) is not part of F1.
There is no RHI yet. That clause moves to the F2 gate, with these exact shaders (plan F1 Architecture).

**D3D11 path decision: `--dxbc-path cross` stays the default.** The cross path is DXC -> SPIR-V ->
SPIRV-Cross HLSL SM 5.0 -> FXC. It now carries a fix that this spike found: `-fspv-preserve-interface`
for the pixel stage. The evidence and the rejected alternative are in "DXBC path comparison" below.

## Tools (versions as printed by the tools)

| Tool | Version | Source |
|---|---|---|
| DXC | `dxcompiler.dll: 1.9(5512-01b62ad4)(1.9.2609.5) - 1.9.2609.5 (01b62ad47)` | `third_party/tools/dxc` (release `v1.9.2609`, sha256 in `tools.lock.json`) |
| FXC | `Microsoft (R) Direct3D Shader Compiler 10.1`, file version `10.0.26100.9457` (`fxc.exe` and `d3dcompiler_47.dll`) | Windows SDK 10.0.26100 |
| SPIRV-Cross | library at `a0fba56c34a6700f1724bf9b751da5b488a3775c` (tag `vulkan-sdk-1.4.341.0`) | FetchContent, pin read from `tools.lock.json` |
| Tint | Dawn `3981d398f827da44210ad8996dee9a0ab034b520` (`tint.exe` has no `--version` at this pin) | built by `scripts/fetch_shader_tools.py` |
| spirv-val | `SPIRV-Tools v2026.1 v2026.1-0-gfbe4f3ad` | Vulkan SDK 1.4.341.1 |
| glslangValidator | `Glslang Version: 11:16.2.0` | Vulkan SDK 1.4.341.1 |
| xcrun metal | not run (K2, PENDING-CI) | macOS CI job `macos-metal` |

## Per target

The numbers come from this run:

    python scripts/compile_shaders.py --out build/spike_default --jobs 8 --shaderc build/win-msvc/tools/shaderc/Debug/shaderc.exe

It used a Debug shaderc built from the Task 5 source, with the default DXBC path. The run printed
`total 112, ok 96, failed 0, pendingCi 16`, and `msTotal` was 9907.1.

- **Bytes** are deterministic (Review Focus 4). A fresh run must give the same values.
- **ms** is the wall-clock time of one shaderc process, with 8 parallel jobs. It covers DXC preprocess,
  SPIR-V, reflection, the backend and the validator. It changes from run to run.
- For scale only: a Release shaderc built from `9e35a78` (before the Task 5 fix, run with
  `--dxbc-path cross`) gave an `msTotal` of about 9.2 s for the same matrix.

| Target | Backend (tier) | Define sets x stages | Validator | Result | Bytes VS min-max (sum) | Bytes PS min-max (sum) | ms mean VS / PS |
|---|---|---|---|---|---|---|---|
| dxil | D3D12 (T2) | 8 x 2 | `dxc -dumpbin` + `dxil.dll` signature | 16 passed | 6992-11660 (75304) | 7032-11180 (72848) | 915.5 / 780.8 |
| dxbc | D3D11 FL11 (T1) | 8 x 2 | FXC success | 16 passed | 2856-8888 (46704) | 3240-7840 (44320) | 714.9 / 580.0 |
| spirv | Vulkan (T2) | 8 x 2 | `spirv-val` | 16 passed | 3184-7880 (44304) | 4576-11740 (65264) | 696.0 / 538.6 |
| msl | Metal 3 (T2) | 8 x 2 | `xcrun metal` (CI only) | 16 PENDING-CI | 1843-6205 (32116) | 2890-7838 (42912) | 573.4 / 431.2 |
| glsl-es300 | GLES 3.0 / WebGL2 (T0) | 8 x 2 | `glslangValidator -S <stage>` | 16 passed | 1608-4520 (24496) | 2947-8132 (44316) | 716.4 / 590.4 |
| glsl-es320 | GLES 3.2 (T1) | 8 x 2 | `glslangValidator -S <stage>` | 16 passed | 1782-4391 (24664) | 2993-7637 (42520) | 780.3 / 667.2 |
| wgsl | WebGPU (T1) | 8 x 2 | `tint --validate` round trip | 16 passed | 2104-5652 (31012) | 2943-8123 (44264) | 839.6 / 735.9 |

The per-item numbers are in the appendix, for spot checks against a fresh `summary.json`.

## DXBC path comparison (direct vs cross)

Both paths compile all 16 DXBC permutations (T1 x skin x morph x unlit x VS/PS). So "it compiles and FXC
accepts it" does not tell the paths apart. The comparison also checks **D3D11 VS -> PS signature
linkage**.

D3D11 links stages by semantic name, semantic index and register. Shader creation accepts a mismatched
pair; the D3D11 debug layer reports it at `Draw` (id 343 below). So no compile-time validator sees a
mismatch.

| | direct (HLSL -> DXC `-P` -> register rewrite -> FXC) | cross at `9e35a78` (before Task 5) | cross with Task 5 fix (default) |
|---|---|---|---|
| PBR compiles (16 DXBC) | 16/16 | 16/16 | 16/16 |
| VS -> PS linkage, static signature check (8 pairs) | 8/8 | **4/8**: all `FG_UNLIT=1` pairs fail | 8/8 |
| VS -> PS linkage, D3D11 WARP debug layer `Draw` (8 pairs, scratch program) | 8/8, 0 errors | **4/8**: id 343 on all `FG_UNLIT=1` pairs | 8/8, 0 errors |
| DXBC bytes, 16 files | 91448 | 90640 | 91024 |
| Instruction slots VS skin+morph / PS lit / PS unlit | 227 / 154 / 43 | 239 / 154 / 43 | 239 / 154 / 43 |
| Temp registers VS skin+morph | 16 | 23 | 23 |
| Inter-stage semantics | source names (`WORLD_POSITION`, `TANGENT`, `BINORMAL`, `NORMAL`, `TEXCOORD0/1`, `COLOR0`) | `TEXCOORD<location>` (vertex inputs keep source semantics via `add_vertex_attribute_remap`) | same as cross |
| FXC warnings, lit PS | X3571 (pow of a possibly negative value) | X3571 + X3557 (SPIRV-Cross `do {} while(false)`) | X3571 + X3557 |
| Front end | DXC preprocessor, then **FXC parses the source** | DXC parses the source (same front end as the other 6 targets) | same as cross |
| `shaders/test/bindings_ok.hlsl` (DXC-valid fixture) | **fails**: `bindings_ok.hlsl(14,9-12): error X3000: syntax error: unexpected token 'Pass'` (FXC does not accept the effect keyword `Pass` as an identifier) | compiles | compiles |

### Cross path defect, found and fixed

1. The `FG_UNLIT=1` pixel shader reads only the UV and color inputs.
2. DXC drops the unused inputs from the SPIR-V entry point. SPIRV-Cross then emits only the used ones.
3. FXC packs those from register 0: `TEXCOORD4` (UV, `xy`) and `TEXCOORD5` (`zw`) at register 0,
   `TEXCOORD6` (color) at register 1.
4. The vertex shader writes them at registers 4, 4 and 5.

The D3D11 debug layer (WARP, `D3D11_CREATE_DEVICE_DEBUG`) reported this at `Draw`:

    ERROR id=343 ID3D11DeviceContext::Draw: Vertex Shader - Pixel Shader linkage error: Signatures between
    stages are incompatible. Semantic 'TEXCOORD' is defined for mismatched hardware registers between the
    output stage and input stage.

**Fix** (`tools/shaderc/Compiler.cpp`): the SPIR-V is compiled with `-fspv-preserve-interface`, which keeps
every declared input. This applies only to target dxbc + cross path + pixel stage.

- The vertex stage does not change. The input layout and the reflection `vertexInputs` stay identical
  to the other targets.
- No other target changes.
- Compared with the Task 4 outputs, only the 4 `FG_UNLIT=1` DXBC pixel shaders changed. The other 220 of
  the 224 output files are sha256-identical.

The WARP check is a scratch program and is not in the repo. It loads each VS/PS pair, builds the input
layout from the VS signature, draws once and lists the debug-layer messages.

The check in the repo is the doctest
`pbr: D3D11 VS -> PS signatures link on both DXBC paths; the default path is cross`
(`tests/unit/shaderc/test_pbr.cpp`). It runs on both paths and does this:

- It parses the DXBC `OSGN` and `ISGN` chunks.
- Every PS input must match a VS output in semantic, index and register, with the mask a subset.
  Values generated by the rasterizer are skipped.
- Before the fix, the test failed with 12 register mismatches, all on cross `FG_UNLIT=1` pairs.

### Decision: default `cross`

The reasons, in order:

1. **One front end for all 7 targets.**
   - DXC parses the HLSL once. Every target, D3D11 included, comes from the same SPIR-V and the same
     reflection.
   - With direct, FXC parses the source as a second front end, with its own language rules. A shader
     that DXC accepts (HLSL 2021) can fail on FXC. The repo already has a case: `bindings_ok.hlsl` and its
     `Pass` cbuffer (error above). Another one, checked with a 1-line pixel shader that returns
     `select(p > 0, p, -p)`: the cross path compiles it, direct fails with
     `error X3004: undeclared identifier 'select'`. HLSL 2021 templates were not tested.
   - Direct would limit every future shader to the common subset of DXC HLSL 2021 and FXC legacy HLSL.
     Only compiling the dxbc target checks that subset; no tool checks it as a rule.
2. **No regex rewrite of source text.** Direct rewrites `register(..., spaceN)` in the preprocessed
   source. Cross sets the D3D11 slots on SPIR-V decorations.
3. **Equal results once fixed.** Both paths get 16/16 compiles and 8/8 linkage. Their bytes are within
   0.5 % of each other.

Costs accepted with cross:

- Inter-stage semantics show as `TEXCOORDn` in captures (RenderDoc / PIX).
- The skin+morph VS uses up to 12 more instruction slots and 7 more temps, because FXC sees SPIRV-Cross
  output, not the source.
- FXC prints the X3557 warning.

`--dxbc-path direct` stays available and tested as a fallback: unit tests, ctest
`shaderc.cli.dxbc_direct` and `shaderc.cli.utf8_src_dxbc_direct`, and the linkage test.

## Limitations found per target

- **DXBC / D3D11.**
  - FXC SM 5.0 has no register spaces (`error X3721: space is only supported for shader targets 5.1 and
    higher`). shaderc flattens (space, index) to one D3D11 slot per register class, in (space, index)
    order. It records the slot as `targetBinding.register`.
  - Pixel-shader UAV slots start at the render-target count (`X4509`). F2 must pass `UAVStartSlot` to
    match.
  - shaderc emits SM 5.0, which is FL11_x (T1). The T0 D3D11 FL10_x backend needs SM 4.0 profiles and is
    not covered. F2 measures FL10.
  - **The VS and PS of a draw must come from the same DXBC path.** The two paths use different
    inter-stage semantics (`WORLD_POSITION` vs `TEXCOORD0`).
- **GLSL ES 3.00 (T0).**
  - It has no storage buffers, storage images or compute. shaderc rejects them by name:
    `target glsl-es300 does not support storage buffers (TIER=<n>)`.
  - T0 skin uses a UBO array of 64 joints. T0 morph has position deltas only, as 8 vertex attributes.
    Skin + morph uses 16 attributes, the ES 3.0 / WebGL2 minimum.
  - ES 3.00 links varyings by name, so shaderc renames them `vary_loc<N>`.
  - Samplers are combined as `<texture>_<sampler>`.
  - There is no `textureQueryLevels`. The mip count is a frame-cbuffer uniform.
- **GLSL ES 3.20 (T1).** Vertex-stage SSBOs need `GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS > 0`, but the
  ES 3.2 minimum is 0. F2 / F11 DeviceCaps must check it and fall back to T0.
- **WGSL (T1).**
  - shaderc rejects read-write storage in the vertex stage, and resource arrays (`binding_array` is not
    core WGSL).
  - Validation is a Tint WGSL -> WGSL round trip, not a browser pipeline creation. So the uniformity
    analysis passed in Tint only. Browser checks come in F10.
- **MSL (T2).**
  - The output is MSL 3.0 for the macOS platform. Buffer, texture and sampler indices are explicit per
    kind, taken from the reflection.
  - This machine has no Metal compiler.
  - The iphoneos compile in CI checks only the language subset of the macOS-generated text. shaderc does
    not use iOS-specific SPIRV-Cross options, and they are unverified.
- **All T1/T2.** Morph indexes by `SV_VertexID`. D3D excludes `BaseVertexLocation`, but Vulkan, GL and
  WebGPU vertex indices include the base vertex. F2 must draw morph meshes with base vertex 0, or pass an
  offset.

## Metal (K2): PENDING-CI

- **Committed MSL.** The files are `tests/shaders/msl/pbr.<key>.<entry>.metal`: the 16 msl permutations
  (T2 x 8 feature-group combinations x VS/PS).
  - `python scripts/msl_artifacts.py` generates them.
  - ctest `shaderc.msl_artifacts_fresh` fails if they differ from fresh shaderc output.
  - `.gitattributes` keeps them byte-exact (`-text`).
- **CI job `macos-metal`** (`.github/workflows/ci.yml`, `macos-latest`) needs no Windows tool. It does
  this:
  1. Gets the Metal toolchain, with `xcodebuild -downloadComponent MetalToolchain` if Xcode lacks it.
  2. Runs `python scripts/check_metal.py --dir tests/shaders/msl`. That script runs
     `xcrun -sdk macosx metal -std=metal3.0 -c` and `xcrun -sdk iphoneos metal -std=metal3.0 -c` on each
     file.
  3. Uploads `metal-results.json`.
- **Result: PENDING-CI, not passed.**
  - The workflow triggers on push or PR to `main` only.
  - The account is billing-locked (f00.t08: "The job was not started because your account is locked due
    to a billing issue.").
  - `docs/worklist.md` row f01.t05 records the run state for this branch.
- The Metal runtime is not verified, by design (K2). Hosted runners only compile.

## Risks for F2+

1. **Only signature parsing and a scratch WARP run check D3D11 linkage.** F2's D3D11 backend should
   enable the debug layer in tests and fail on any linkage message.
2. **The cross path depends on `-fspv-preserve-interface` for pixel shaders.**
   - If a future pixel shader declares inputs that its VS does not write, D3D11 rejects the pair.
   - The linkage test covers only the PBR shader.
   - F5's `.shaderpack` builder should run the signature check on every VS/PS pair it packs.
3. **The runtime image equality of §5.4 is still open** (D3D11 / D3D12 / Vulkan, FLIP < threshold). It is
   the F2 gate. A mismatch there may reopen this decision.
   **Update (plan F2 Task 5, 2026-11-28): parity split.** The D3D11 vs Vulkan leg is done in F2:
   `builtin:pbr-spheres` renders this `pbr.hlsl` with spirv `T2_SKIN0_MORPH0_UNLIT0` (Vulkan) and the default
   `cross`-path dxbc `T1_SKIN0_MORPH0_UNLIT0` (D3D11), plus the UNLIT1 row; `run_golden --phase f02` compares
   each backend against the committed D3D11 reference and Vulkan directly against D3D11 (mean FLIP <= 0.03),
   with absolute probes from `scripts/f02_pbr_model.py`. Measured on the RTX 3070 Ti: D3D11 vs Vulkan
   flipMean 0 (byte-identical); WARP vs Vulkan flipMean 0.0066. The `cross` decision stands. The D3D12 leg
   moves to the F7 gate.
4. **MSL stays unvalidated until CI is unlocked.** A Metal compile failure would need changes to the
   SPIRV-Cross MSL options or to the shaders. It would surface late (F13).
5. **Tint has no version string.** The Dawn commit is the version. After an update, re-run the whole
   matrix (`compile_shaders.py`) and the WGSL checks.
6. **Compile cost is low.** One permutation for one target takes about 0.5-0.9 s (Debug shaderc,
   8 jobs; no per-step breakdown was measured). The 112 compiles take about 10 s, so K-A's
   budget of ≤ 64 variants per backend will not slow the build much.

## Appendix: per-item results (`build/spike_default/summary.json`)

| id | status | bytes | ms |
|---|---|---:|---:|
| `dxil/T2_SKIN0_MORPH0_UNLIT0/PSMain` | ok | 11180 | 867.1 |
| `dxil/T2_SKIN0_MORPH0_UNLIT0/VSMain` | ok | 6992 | 988.6 |
| `dxil/T2_SKIN0_MORPH0_UNLIT1/PSMain` | ok | 7032 | 798.4 |
| `dxil/T2_SKIN0_MORPH0_UNLIT1/VSMain` | ok | 6992 | 919.4 |
| `dxil/T2_SKIN0_MORPH1_UNLIT0/PSMain` | ok | 11180 | 845.9 |
| `dxil/T2_SKIN0_MORPH1_UNLIT0/VSMain` | ok | 8688 | 894.9 |
| `dxil/T2_SKIN0_MORPH1_UNLIT1/PSMain` | ok | 7032 | 760.3 |
| `dxil/T2_SKIN0_MORPH1_UNLIT1/VSMain` | ok | 8688 | 883.8 |
| `dxil/T2_SKIN1_MORPH0_UNLIT0/PSMain` | ok | 11180 | 750.2 |
| `dxil/T2_SKIN1_MORPH0_UNLIT0/VSMain` | ok | 10312 | 902.9 |
| `dxil/T2_SKIN1_MORPH0_UNLIT1/PSMain` | ok | 7032 | 692.1 |
| `dxil/T2_SKIN1_MORPH0_UNLIT1/VSMain` | ok | 10312 | 869.2 |
| `dxil/T2_SKIN1_MORPH1_UNLIT0/PSMain` | ok | 11180 | 804.0 |
| `dxil/T2_SKIN1_MORPH1_UNLIT0/VSMain` | ok | 11660 | 919.5 |
| `dxil/T2_SKIN1_MORPH1_UNLIT1/PSMain` | ok | 7032 | 728.2 |
| `dxil/T2_SKIN1_MORPH1_UNLIT1/VSMain` | ok | 11660 | 945.4 |
| `dxbc/T1_SKIN0_MORPH0_UNLIT0/PSMain` | ok | 7840 | 691.4 |
| `dxbc/T1_SKIN0_MORPH0_UNLIT0/VSMain` | ok | 2856 | 657.2 |
| `dxbc/T1_SKIN0_MORPH0_UNLIT1/PSMain` | ok | 3240 | 534.0 |
| `dxbc/T1_SKIN0_MORPH0_UNLIT1/VSMain` | ok | 2856 | 683.9 |
| `dxbc/T1_SKIN0_MORPH1_UNLIT0/PSMain` | ok | 7840 | 610.9 |
| `dxbc/T1_SKIN0_MORPH1_UNLIT0/VSMain` | ok | 3784 | 690.3 |
| `dxbc/T1_SKIN0_MORPH1_UNLIT1/PSMain` | ok | 3240 | 531.8 |
| `dxbc/T1_SKIN0_MORPH1_UNLIT1/VSMain` | ok | 3784 | 683.5 |
| `dxbc/T1_SKIN1_MORPH0_UNLIT0/PSMain` | ok | 7840 | 629.9 |
| `dxbc/T1_SKIN1_MORPH0_UNLIT0/VSMain` | ok | 7824 | 721.5 |
| `dxbc/T1_SKIN1_MORPH0_UNLIT1/PSMain` | ok | 3240 | 499.7 |
| `dxbc/T1_SKIN1_MORPH0_UNLIT1/VSMain` | ok | 7824 | 727.5 |
| `dxbc/T1_SKIN1_MORPH1_UNLIT0/PSMain` | ok | 7840 | 636.4 |
| `dxbc/T1_SKIN1_MORPH1_UNLIT0/VSMain` | ok | 8888 | 750.5 |
| `dxbc/T1_SKIN1_MORPH1_UNLIT1/PSMain` | ok | 3240 | 505.6 |
| `dxbc/T1_SKIN1_MORPH1_UNLIT1/VSMain` | ok | 8888 | 805.0 |
| `spirv/T2_SKIN0_MORPH0_UNLIT0/PSMain` | ok | 11740 | 524.4 |
| `spirv/T2_SKIN0_MORPH0_UNLIT0/VSMain` | ok | 3184 | 710.6 |
| `spirv/T2_SKIN0_MORPH0_UNLIT1/PSMain` | ok | 4576 | 530.7 |
| `spirv/T2_SKIN0_MORPH0_UNLIT1/VSMain` | ok | 3184 | 734.3 |
| `spirv/T2_SKIN0_MORPH1_UNLIT0/PSMain` | ok | 11740 | 634.7 |
| `spirv/T2_SKIN0_MORPH1_UNLIT0/VSMain` | ok | 4664 | 783.0 |
| `spirv/T2_SKIN0_MORPH1_UNLIT1/PSMain` | ok | 4576 | 560.6 |
| `spirv/T2_SKIN0_MORPH1_UNLIT1/VSMain` | ok | 4664 | 653.6 |
| `spirv/T2_SKIN1_MORPH0_UNLIT0/PSMain` | ok | 11740 | 550.0 |
| `spirv/T2_SKIN1_MORPH0_UNLIT0/VSMain` | ok | 6424 | 637.8 |
| `spirv/T2_SKIN1_MORPH0_UNLIT1/PSMain` | ok | 4576 | 486.9 |
| `spirv/T2_SKIN1_MORPH0_UNLIT1/VSMain` | ok | 6424 | 655.6 |
| `spirv/T2_SKIN1_MORPH1_UNLIT0/PSMain` | ok | 11740 | 527.2 |
| `spirv/T2_SKIN1_MORPH1_UNLIT0/VSMain` | ok | 7880 | 689.4 |
| `spirv/T2_SKIN1_MORPH1_UNLIT1/PSMain` | ok | 4576 | 494.2 |
| `spirv/T2_SKIN1_MORPH1_UNLIT1/VSMain` | ok | 7880 | 703.5 |
| `msl/T2_SKIN0_MORPH0_UNLIT0/PSMain` | pendingCi | 7838 | 481.8 |
| `msl/T2_SKIN0_MORPH0_UNLIT0/VSMain` | pendingCi | 1843 | 539.5 |
| `msl/T2_SKIN0_MORPH0_UNLIT1/PSMain` | pendingCi | 2890 | 390.6 |
| `msl/T2_SKIN0_MORPH0_UNLIT1/VSMain` | pendingCi | 1843 | 548.1 |
| `msl/T2_SKIN0_MORPH1_UNLIT0/PSMain` | pendingCi | 7838 | 486.8 |
| `msl/T2_SKIN0_MORPH1_UNLIT0/VSMain` | pendingCi | 2688 | 558.3 |
| `msl/T2_SKIN0_MORPH1_UNLIT1/PSMain` | pendingCi | 2890 | 401.3 |
| `msl/T2_SKIN0_MORPH1_UNLIT1/VSMain` | pendingCi | 2688 | 571.8 |
| `msl/T2_SKIN1_MORPH0_UNLIT0/PSMain` | pendingCi | 7838 | 472.9 |
| `msl/T2_SKIN1_MORPH0_UNLIT0/VSMain` | pendingCi | 5322 | 585.7 |
| `msl/T2_SKIN1_MORPH0_UNLIT1/PSMain` | pendingCi | 2890 | 405.6 |
| `msl/T2_SKIN1_MORPH0_UNLIT1/VSMain` | pendingCi | 5322 | 563.0 |
| `msl/T2_SKIN1_MORPH1_UNLIT0/PSMain` | pendingCi | 7838 | 453.4 |
| `msl/T2_SKIN1_MORPH1_UNLIT0/VSMain` | pendingCi | 6205 | 637.9 |
| `msl/T2_SKIN1_MORPH1_UNLIT1/PSMain` | pendingCi | 2890 | 357.6 |
| `msl/T2_SKIN1_MORPH1_UNLIT1/VSMain` | pendingCi | 6205 | 583.1 |
| `glsl-es300/T0_SKIN0_MORPH0_UNLIT0/PSMain` | ok | 8132 | 642.5 |
| `glsl-es300/T0_SKIN0_MORPH0_UNLIT0/VSMain` | ok | 1608 | 679.8 |
| `glsl-es300/T0_SKIN0_MORPH0_UNLIT1/PSMain` | ok | 2947 | 544.0 |
| `glsl-es300/T0_SKIN0_MORPH0_UNLIT1/VSMain` | ok | 1608 | 688.0 |
| `glsl-es300/T0_SKIN0_MORPH1_UNLIT0/PSMain` | ok | 8132 | 609.5 |
| `glsl-es300/T0_SKIN0_MORPH1_UNLIT0/VSMain` | ok | 2492 | 649.8 |
| `glsl-es300/T0_SKIN0_MORPH1_UNLIT1/PSMain` | ok | 2947 | 512.5 |
| `glsl-es300/T0_SKIN0_MORPH1_UNLIT1/VSMain` | ok | 2492 | 712.0 |
| `glsl-es300/T0_SKIN1_MORPH0_UNLIT0/PSMain` | ok | 8132 | 676.1 |
| `glsl-es300/T0_SKIN1_MORPH0_UNLIT0/VSMain` | ok | 3628 | 734.8 |
| `glsl-es300/T0_SKIN1_MORPH0_UNLIT1/PSMain` | ok | 2947 | 570.5 |
| `glsl-es300/T0_SKIN1_MORPH0_UNLIT1/VSMain` | ok | 3628 | 736.7 |
| `glsl-es300/T0_SKIN1_MORPH1_UNLIT0/PSMain` | ok | 8132 | 626.0 |
| `glsl-es300/T0_SKIN1_MORPH1_UNLIT0/VSMain` | ok | 4520 | 768.9 |
| `glsl-es300/T0_SKIN1_MORPH1_UNLIT1/PSMain` | ok | 2947 | 542.3 |
| `glsl-es300/T0_SKIN1_MORPH1_UNLIT1/VSMain` | ok | 4520 | 761.3 |
| `glsl-es320/T1_SKIN0_MORPH0_UNLIT0/PSMain` | ok | 7637 | 715.8 |
| `glsl-es320/T1_SKIN0_MORPH0_UNLIT0/VSMain` | ok | 1782 | 772.7 |
| `glsl-es320/T1_SKIN0_MORPH0_UNLIT1/PSMain` | ok | 2993 | 651.3 |
| `glsl-es320/T1_SKIN0_MORPH0_UNLIT1/VSMain` | ok | 1782 | 750.1 |
| `glsl-es320/T1_SKIN0_MORPH1_UNLIT0/PSMain` | ok | 7637 | 677.8 |
| `glsl-es320/T1_SKIN0_MORPH1_UNLIT0/VSMain` | ok | 2522 | 775.0 |
| `glsl-es320/T1_SKIN0_MORPH1_UNLIT1/PSMain` | ok | 2993 | 599.9 |
| `glsl-es320/T1_SKIN0_MORPH1_UNLIT1/VSMain` | ok | 2522 | 789.5 |
| `glsl-es320/T1_SKIN1_MORPH0_UNLIT0/PSMain` | ok | 7637 | 662.7 |
| `glsl-es320/T1_SKIN1_MORPH0_UNLIT0/VSMain` | ok | 3637 | 774.2 |
| `glsl-es320/T1_SKIN1_MORPH0_UNLIT1/PSMain` | ok | 2993 | 655.1 |
| `glsl-es320/T1_SKIN1_MORPH0_UNLIT1/VSMain` | ok | 3637 | 821.9 |
| `glsl-es320/T1_SKIN1_MORPH1_UNLIT0/PSMain` | ok | 7637 | 733.0 |
| `glsl-es320/T1_SKIN1_MORPH1_UNLIT0/VSMain` | ok | 4391 | 816.8 |
| `glsl-es320/T1_SKIN1_MORPH1_UNLIT1/PSMain` | ok | 2993 | 641.7 |
| `glsl-es320/T1_SKIN1_MORPH1_UNLIT1/VSMain` | ok | 4391 | 742.4 |
| `wgsl/T1_SKIN0_MORPH0_UNLIT0/PSMain` | ok | 8123 | 722.2 |
| `wgsl/T1_SKIN0_MORPH0_UNLIT0/VSMain` | ok | 2104 | 823.6 |
| `wgsl/T1_SKIN0_MORPH0_UNLIT1/PSMain` | ok | 2943 | 638.4 |
| `wgsl/T1_SKIN0_MORPH0_UNLIT1/VSMain` | ok | 2104 | 932.5 |
| `wgsl/T1_SKIN0_MORPH1_UNLIT0/PSMain` | ok | 8123 | 835.3 |
| `wgsl/T1_SKIN0_MORPH1_UNLIT0/VSMain` | ok | 3534 | 927.1 |
| `wgsl/T1_SKIN0_MORPH1_UNLIT1/PSMain` | ok | 2943 | 784.3 |
| `wgsl/T1_SKIN0_MORPH1_UNLIT1/VSMain` | ok | 3534 | 981.0 |
| `wgsl/T1_SKIN1_MORPH0_UNLIT0/PSMain` | ok | 8123 | 844.3 |
| `wgsl/T1_SKIN1_MORPH0_UNLIT0/VSMain` | ok | 4216 | 937.5 |
| `wgsl/T1_SKIN1_MORPH0_UNLIT1/PSMain` | ok | 2943 | 789.3 |
| `wgsl/T1_SKIN1_MORPH0_UNLIT1/VSMain` | ok | 4216 | 762.1 |
| `wgsl/T1_SKIN1_MORPH1_UNLIT0/PSMain` | ok | 8123 | 662.8 |
| `wgsl/T1_SKIN1_MORPH1_UNLIT0/VSMain` | ok | 5652 | 781.1 |
| `wgsl/T1_SKIN1_MORPH1_UNLIT1/PSMain` | ok | 2943 | 611.0 |
| `wgsl/T1_SKIN1_MORPH1_UNLIT1/VSMain` | ok | 5652 | 571.6 |
