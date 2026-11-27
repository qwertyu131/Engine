# Worklist

One row per task (spec §12). Columns:

- **ID**: `fNN.tMM` = phase NN, task MM of that phase's plan (`fNN` alone = phase line not yet planned).
- **Task**: short name, matching the plan heading.
- **Status**: `TODO` / `WIP` / `DONE` / `BLOCKED`. `BLOCKED` always carries concrete evidence (exact command + error).
- **Evidence**: commit SHA(s), test names, golden report path, CI run. Only things that exist and were run.
- **Note**: anything a reviewer must know (limits, deviations, follow-ups).

`DONE` here means implemented with the implementer's gate commands green. The phase is accepted only
after the reviewer re-runs the gate commands itself (spec §12) and records that in the Note column.

## F0: Skeleton and verification infrastructure

Plan: `docs/superpowers/plans/2026-11-27-f00-skeleton-and-verification.md`.

| ID | Task | Status | Evidence | Note |
|---|---|---|---|---|
| f00.t01 | CMake skeleton, presets, core log/assert, doctest | DONE | `ebc0a21`, `1a35e3a`; `ctest --preset win-msvc -C Debug` (unit.* log/assert) | presets `win-msvc` (VS 18 2026), `linux-clang`, `macos-clang` (Ninja Multi-Config) |
| f00.t02 | CVar core | DONE | `caedb81`; `tests/unit/core/test_cvar.cpp` via ctest | source priority per spec §8; float parse locale-independent (`from_chars`) |
| f00.t03 | Golden diff tool (FLIP + PSNR) | DONE | `91007cb`, `131aff1`; `tests/unit/golden/test_compare.cpp`, ctest `golden.cli.*` (exit 0/1/2, `missing-reference`, UTF-8 dir) | Review Focus 1, 2, 4 |
| f00.t04 | Visual HTML report | DONE | `1437b91`; `tests/golden/test_report.py` | self-contained, offline, base64 images |
| f00.t05 | `apps/headless` skeleton + `run_golden` driver | DONE | `67d44d5`; `python scripts/run_golden.py --phase f00 --build-dir build/win-msvc --config Debug` -> exit 0, `tests/golden/out/f00/report.html` (1 PASS); ctest `headless.cli.*` | CPU test pattern only; `--bench 10` local: `frame_ms_avg=2.146 frame_ms_p95=2.19` (Debug) |
| f00.t06 | Test assets, validator, verified extension list | DONE | `f9927cb`, `407ff88`; `tests/unit/scripts/test_fetch_assets.py`; `tests/assets/assets.lock.json`; `docs/gltf-extensions.md` | Sample-Assets @ `e124e8d`, glTF-Validator 2.0.0-dev.3.10, registry @ `da88a3b` |
| f00.t07 | Sample-Viewer reference images (K-B) | DONE | `8e34a62`, `ff683bc` (fit camera applies world transforms first, as the app does), `0b82b97` (clearColor = app value `[0,0,0,1]`; animations as the app, sampled at t = 0; model-vs-environment-only visibility check); `tests/golden/ref/sample-viewer/{DamagedHelmet,MetalRoughSpheres,NormalTangentTest,AlphaBlendModeTest,TextureCoordinateTest}.png` + `settings.json`; `node --test tools/refgen/test/` (7 pass) | `@khronosgroup/gltf-viewer` 1.1.0 + Chrome 157 (Puppeteer 25.17.0), launch mode `headless-new`, real GPU (ANGLE D3D11, RTX 3070 Ti); punctual lights off; PNGs byte-identical across re-runs and unchanged by `0b82b97` (all 5 `pngSha256` equal); modelCoverage 22-70%; animated models checked in a scratch dir only (BoxAnimated, AnimatedMorphCube, AnimatedTriangle: animationIndices [0], deterministic, not committed); app parity is by value (app @ `8be3925` uses newer renderer APIs than 1.1.0, see `tools/refgen/README.md`); DamagedHelmet checked against Sample-Viewer's own snapshot, MetalRoughSpheres against the asset's screenshot |
| f00.t08 | Worklist + Windows/Linux CI | BLOCKED | `8460c7d`, `ff683bc`, `0b82b97` (actions pinned to commit SHAs: checkout v7.0.1, setup-python v7.1.0, upload-artifact v7.0.2); `.github/workflows/ci.yml`, this file; local gate green in a clean clone of `0b82b97` (configure + Debug build exit 0, 0 compiler warnings, ctest 26/26, `python -m unittest discover tests` 20 OK, run_golden exit 0); branch `f00-skeleton` pushed | BLOCKED (external, 2026-11-27): PR #1 opened -> run 46043239052, both jobs `failure` with 0 steps; check-run annotation: "The job was not started because your account is locked due to a billing issue." Owner must fix GitHub billing, then re-run. Earlier state: trigger is push/PR to `main` only; `GET /repos/qwertyu131/Engine/actions/runs` -> `total_count: 0`, `GET /repos/qwertyu131/Engine/pulls?state=all` -> `[]` (2026-11-27). Needs the repo owner to open a PR `f00-skeleton` -> `main` (or merge). Linux job not run on Linux yet. Partial pre-check only: Clang 22.1.3 (VS-bundled, target x86_64-pc-windows-msvc, Ninja Multi-Config) built everything with `-Wall -Wextra -Wpedantic -Werror` on `engine`, ctest 26/26; this does not cover libstdc++/glibc headers |

## F1: Shader pipeline spike

Plan: `docs/superpowers/plans/2026-11-27-f01-shader-pipeline-spike.md`. Branch `f01-shader-spike`.

| ID | Task | Status | Evidence | Note |
|---|---|---|---|---|
| f01.t01 | Shader toolchain fetch/build | DONE | `f0c8d5f`; `tests/unit/scripts/test_fetch_shader_tools.py` (13 OK; RED first: `FileNotFoundError ... scripts\fetch_shader_tools.py`); `python scripts/fetch_shader_tools.py` -> exit 0 (first run 6m19s: DXC download + Dawn checkout + Tint Release build), second run exit 0 in 0.36 s, all "up to date", no network/build; `python scripts/fetch_shader_tools.py --verify` -> exit 0; `third_party/tools/dxc/bin/x64/dxc.exe --version` -> `dxcompiler.dll: 1.9(5512-01b62ad4)(1.9.2609.5) - 1.9.2609.5 (01b62ad47)` | Pins in `third_party/tools/tools.lock.json`: DXC `v1.9.2609` (`dxc_2026_09_29.zip`, sha256 `ad31b1fc...a7e7f1`, hashed locally = GitHub asset digest), Dawn `3981d398` (= `chromium/8188`), SPIRV-Cross `a0fba56c` (= tag `vulkan-sdk-1.4.341.0`, read by `cmake/Dependencies.cmake` from the lock). Deviations: (1) `tint.exe --version` does not exist at this Dawn pin: output `unknown flag: --version` + usage, exit 1; the Tint version is the Dawn commit in `third_party/tools/tint/.dawn-commit`. (2) `TINT_BUILD_WGSL_READER=ON` in addition to the plan's list: needed to validate WGSL with `tint`. (3) Dawn sub-dependencies (abseil, SPIRV-Headers, SPIRV-Tools, jinja2, markupsafe, webgpu-headers) are read from the pinned Dawn commit's `DEPS` (text-parsed, never executed), not added to the lock. (4) The "read-only-file rmtree fix from F0 review" does not exist in `scripts/fetch_assets.py` (only plain `shutil.rmtree`); implemented as `rmtree_force` in `fetch_shader_tools.py` + test. `--verify` with a missing tool -> exit 2 `tool not found: <name> (run scripts/fetch_shader_tools.py)` (Review Focus 3). spirv-val/glslangValidator come from `$VULKAN_SDK/Bin`, fxc from Windows SDK 10.0.26100; `ENG_DXC` never resolves to the Vulkan SDK's `dxc.exe` |
| f01.t02 | `shaderc` core + CLI, triangle through all 7 targets | DONE | `fcb43c2`, CLI non-ASCII --src test in the follow-up commit; `shaderc_unit_tests` (ctest `unit.shaderc`: 5 cases, 314 assertions; RED first: build failed, `ShaderTestTools.h(16,8): error C4430` with empty `Compiler.h`); ctest `shaderc.cli.{all_targets_vs,all_targets_fs,utf8_src_and_out,utf8_src_dxbc_direct,dxbc_direct,tool_missing,usage,bad_target,compile_error}`; `shaderc compile --src shaders/test/triangle.hlsl --entry VSMain --stage vs --target all --out build/shaders --validate` -> exit 0 (6 `[passed]`, msl `[PENDING-CI]`); full `ctest --test-dir build/win-msvc -C Debug` 34/34 | Deviations: `validate()` returns `std::expected<ValidateStatus, std::string>` (`Passed` / `PendingCI`) instead of `expected<void>` so MSL can report `PENDING-CI`; `CompileRequest::dxbcPath` added for `--dxbc-path` (provisional default `cross`, decided in Task 5); extra files `Targets.cpp`, `Process.{h,cpp}` (CreateProcessW, UTF-16 args), `tests/unit/shaderc/{test_main.cpp,ShaderTestTools.h}`; extra CLI flag `--tool <name>=<path>`. Missing tool -> ctest `Skipped` with `SKIP: tool not found: <name> ...` (checked by hiding tint.exe), never a pass. DXBC: FXC SM5 rejects `space` (`error X3721: space is only supported for shader targets 5.1 and higher`), so both paths flatten to D3D11 slots per register class in (space, index) order (recorded as `targetBinding.register`); direct path = DXC `-P` preprocess + register rewrite + FXC. GLSL ES: inter-stage variables renamed `vary_loc<N>` (ES 3.00 links by name), `fixup_clipspace`, highp default precision, combined samplers `<texture>_<sampler>`. MSL 3.0 (macOS) with explicit per-kind indices. WGSL validation = `tint --validate --format wgsl` round trip. Output byte-identical across two runs (spot check; Task 4 owns the test). MSL not validated on this machine (K2, CI billing lock f00.t08) |

## Later phases (spec §11)

| ID | Task | Status | Evidence | Note |
|---|---|---|---|---|
| f01 | Shader pipeline spike: HLSL -> 6 targets, one PBR shader | WIP | see F1 table above | MSL check via `xcrun metal` in CI (K2) |
| f02 | RHI core: Vulkan + D3D11 + windowed viewer | TODO | | |
| f03 | glTF loader + core PBR | TODO | | reference mode must reproduce `tests/golden/ref/sample-viewer/settings.json` |
| f04 | Animation | TODO | | |
| f05 | Capability resolver + permutations + settings | TODO | | |
| f06 | Material extensions | TODO | | |
| f07 | D3D12 backend | TODO | | |
| f08 | Frame graph + TAA + post | TODO | | |
| f09 | Upscalers (Windows) | TODO | | |
| f10 | Web: WebGPU + WebGL2 | TODO | | |
| f11 | Android: Vulkan + GLES3 | TODO | | K3: emulator only |
| f12 | Linux | TODO | | |
| f13 | Metal (macOS + iOS) | TODO | | K2: build + MSL validation only, runtime NOT verified |
| f14 | Frame generation + RT extras (optional) | TODO | | |
| f15 | Real Mac verification (deferred) | TODO | | K2 |

## Standing notes

- **K2: Metal runtime unverified.** No Mac or iOS device. Metal and iOS are checked only by build and
  MSL compile (`xcrun metal`) on hosted macOS runners. Every phase report repeats this.
- **K3: Android emulator only.** No physical Android device. Android runtime tests run on the
  emulator; real-device behaviour is unverified.
