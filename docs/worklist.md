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
| f00.t07 | Sample-Viewer reference images (K-B) | DONE | `8e34a62`, `ff683bc` (fit camera now applies world transforms first, as the app does); `tests/golden/ref/sample-viewer/{DamagedHelmet,MetalRoughSpheres,NormalTangentTest,AlphaBlendModeTest,TextureCoordinateTest}.png` + `settings.json`; `node --test tools/refgen/test/` (6 pass) | `@khronosgroup/gltf-viewer` 1.1.0 + Chrome 157 (Puppeteer 25.17.0), launch mode `headless-new`, real GPU (ANGLE D3D11, RTX 3070 Ti); punctual lights off; PNGs byte-identical across re-runs; DamagedHelmet checked against Sample-Viewer's own snapshot, MetalRoughSpheres against the asset's screenshot |
| f00.t08 | Worklist + Windows/Linux CI | WIP | `8460c7d`, `ff683bc`; `.github/workflows/ci.yml`, this file; local gate green in a clean clone of `ff683bc` (configure + Debug build exit 0, ctest 26/26, `python -m unittest discover tests` 20 OK, run_golden exit 0); branch pushed (`git push origin f00-skeleton`: `3e18665..8460c7d`) | CI not yet run: trigger is push/PR to `main` only, and `GET /repos/qwertyu131/Engine/actions/runs` returned `total_count: 0` after the push. Needs a PR to `main` (or merge) by someone with authority. Linux job never built (no WSL/Docker here) |

## Later phases (spec §11)

| ID | Task | Status | Evidence | Note |
|---|---|---|---|---|
| f01 | Shader pipeline spike: HLSL -> 6 targets, one PBR shader | TODO | | MSL check via `xcrun metal` in CI (K2) |
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
