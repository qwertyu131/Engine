# F0 — Skeleton & Verification Infrastructure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a green CMake skeleton (core lib + CVar core), the headless→golden-diff→HTML-report test chain, the pinned glTF test-asset/validator fetch, a registry-verified glTF extension list, the worklist, and Windows CI — so every later phase has an automatic gate and a visual report.

**Architecture:** `engine` static library (only `core/` in F0) consumed by `apps/headless` and `tools/golden`. Golden tool compares PNGs with NVIDIA FLIP + PSNR and writes JSON; a report generator turns JSON into `report.html` (render / reference / diff side by side). No GPU code in F0: `headless` writes a deterministic CPU test pattern so the whole chain runs end-to-end now and gets a real backend in F2.

**Tech Stack:** C++20, CMake 4.3.2, Visual Studio 18 2026 generator (MSVC), Ninja for non-Windows presets, FetchContent (pinned tags), doctest, stb_image / stb_image_write, NVIDIA FLIP (C++ header, BSD-3), Python 3.14 (stdlib only) for scripts, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-11-27-cross-platform-gltf-renderer-design.md` (§3, §5.1, §5.2, §7.2, §7.3, §8, §10, §11 F0, §12, §14 K-B/K-G)

## Global Constraints

- C++20; CMake ≥ 3.28 (`cmake_minimum_required(VERSION 3.28)`); every FetchContent dependency pinned to an exact tag or commit SHA.
- Directory layout exactly as spec §5.2 (`engine/core`, `apps/headless`, `tools/golden`, `tests/unit`, `tests/golden`, `tests/assets`).
- Closed-source SDKs never committed (K5); `third_party/sdk/` is git-ignored.
- Test images/assets downloaded at configure/fetch time are never committed except `tests/golden/ref/` reference PNGs (K-B) and the tiny synthetic fixtures under `tests/golden/fixtures/`.
- Golden tool exit codes: `0` = all PASS, `1` = at least one FAIL, `2` = usage / IO error.
- No "done" claim without the reviewer re-running the gate commands (spec §12).
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Windows commands run from a plain PowerShell (VS generator needs no vcvars).

## Review Focus

1. **Images of different sizes or channel counts** given to `golden` → must exit `2` with a clear message, never crash or silently resize. (Test in Task 3.)
2. **Missing / unreadable reference PNG** (new test with no ref yet) → that case is `FAIL` with reason `missing-reference` in JSON and report, not a crash. (Test in Task 3 + Task 4.)
3. **CVar set from a lower-priority source after a higher one** (ini after console) → value must not change. (Test in Task 2.)
4. **Paths with spaces / non-ASCII** (user is on `C:\Users\Heros\Desktop\...`, Turkish locale) → tools accept UTF-8 paths on Windows. (Test in Task 3: fixture copied into a dir named `ç ğ ü test`.)
5. **Asset fetch run twice / interrupted** → idempotent; re-run verifies pinned commit, does not re-download or corrupt. (Test in Task 6.)

---

### Task 1: CMake skeleton, presets, core lib, unit test harness

**Files:**
- Create: `CMakeLists.txt`, `CMakePresets.json`, `cmake/Dependencies.cmake`, `engine/CMakeLists.txt`, `engine/core/Log.h`, `engine/core/Log.cpp`, `engine/core/Assert.h`, `tests/unit/CMakeLists.txt`, `tests/unit/test_main.cpp`, `tests/unit/core/test_log.cpp`, `README.md`

**Interfaces:**
- Produces: CMake target `engine` (STATIC, alias `engine::engine`), include root `engine/` (so `#include "core/Log.h"`); target `engine_unit_tests` registered with CTest.
- `namespace eng::log { enum class Level { Trace, Debug, Info, Warn, Error, Fatal }; void setLevel(Level); Level level(); void setSink(std::function<void(Level, std::string_view)>); void write(Level, std::string_view msg); }`
- Macros `ENG_LOG_INFO(fmt, ...)`, `ENG_LOG_WARN`, `ENG_LOG_ERROR` (std::format based), `ENG_ASSERT(cond)` / `ENG_ASSERT_MSG(cond, msg)` (active when `ENG_ENABLE_ASSERTS=1`, default ON for Debug).
- Presets (configure + build + test): `win-msvc` (generator `Visual Studio 18 2026`, binaryDir `build/win-msvc`), `linux-clang` (Ninja), `macos-clang` (Ninja, used by CI later). Placeholders for backend presets from spec §5.1 are NOT added yet (YAGNI; they arrive with their backends).

- [ ] **Step 1:** Write `tests/unit/core/test_log.cpp`: `TEST_CASE("log sink receives message at or above level")` — set sink capturing into vector, `setLevel(Warn)`, write Info + Error, assert captured size == 1 and level == Error and text == "boom".
- [ ] **Step 2:** Configure+build: `cmake --preset win-msvc; cmake --build --preset win-msvc --config Debug` — Expected: fails (no sources yet).
- [ ] **Step 3:** Implement CMake files, presets, `Log`, `Assert`; doctest via FetchContent (pinned tag `v2.4.11` or newer exact tag).
- [ ] **Step 4:** `ctest --preset win-msvc -C Debug --output-on-failure` — Expected: 100% tests passed.
- [ ] **Step 5:** Commit `build: cmake skeleton, core log/assert, doctest harness`.

### Task 2: CVar core

**Files:**
- Create: `engine/core/CVar.h`, `engine/core/CVar.cpp`, `tests/unit/core/test_cvar.cpp`

**Interfaces:**
- Consumes: `eng::log` (warnings on rejected sets).
- Produces:
  - `enum class eng::CVarSource : uint8_t { Default=0, Scalability=1, PlatformIni=2, ProjectIni=3, UserIni=4, CommandLine=5, Console=6, Code=7 };` (priority order = spec §8)
  - `enum class eng::CVarFlags : uint32_t { None=0, ReadOnly=1, RequiresRestart=2, RequiresPipelineRebuild=4, Cheat=8 };` (bitmask operators)
  - `using CVarValue = std::variant<bool, int32_t, float, std::string>;`
  - `class eng::CVarRegistry { static CVarRegistry& get(); CVar* registerBool/Int/Float/String(std::string_view name, T def, std::string_view help, CVarFlags = None, optional min/max for Int/Float); CVar* find(std::string_view name) const; bool set(std::string_view name, std::string_view value, CVarSource); void forEach(fn) const; uint64_t onChanged(std::function<void(const CVar&)>); void removeListener(uint64_t); }`
  - `class eng::CVar { name(), help(), flags(), source(), value() const -> const CVarValue&, getBool/getInt/getFloat/getString, bool set(CVarValue, CVarSource); }`
  - Names are case-insensitive (`r.Shadow.Quality` == `R.SHADOW.QUALITY`), stored with original case.

- [ ] **Step 1:** Tests in `test_cvar.cpp`:
  - `"register and read default"`: int `r.Test.A` default 3 → getInt()==3, source()==Default.
  - `"lower priority source cannot override higher"`: set "5" Console, then set "7" ProjectIni → returns false, getInt()==5.
  - `"equal or higher priority overrides"`: Console "5" then Code "9" → 9.
  - `"int clamp to min/max"`: min 0 max 4, set "10" → 4 (and log warn).
  - `"parse failure rejected"`: set "abc" on int → false, value unchanged.
  - `"ReadOnly rejects non-Default/Code"`: ReadOnly cvar, Console set → false.
  - `"case-insensitive lookup"`.
  - `"onChanged fires once per effective change, not on no-op"`.
  - `"duplicate register returns existing and warns"`.
- [ ] **Step 2:** Run `ctest --preset win-msvc -C Debug -R cvar` — Expected: FAIL (not built).
- [ ] **Step 3:** Implement. Bool parse accepts `0/1/true/false/on/off` (case-insensitive); float parse uses `std::from_chars` (locale-independent — Turkish locale uses `,` decimal).
- [ ] **Step 4:** Run tests — Expected: PASS.
- [ ] **Step 5:** Commit `feat(core): cvar registry with source priority`.

### Task 3: Golden diff tool (`tools/golden`)

**Files:**
- Create: `tools/golden/CMakeLists.txt`, `tools/golden/Image.h/.cpp`, `tools/golden/Compare.h/.cpp`, `tools/golden/main.cpp`, `tests/unit/golden/test_compare.cpp`, `tests/golden/fixtures/{solid_red_64.png, solid_red_64_copy.png, red_with_dot_64.png, solid_red_32.png, gray_64.png}` (+ `tests/golden/fixtures/make_fixtures.py` that regenerates them, stdlib only via zlib/struct)

**Interfaces:**
- Produces library target `golden_core` and exe `golden`.
- `struct eng::golden::Image { int width, height, channels; std::vector<uint8_t> rgba8; }` — always expanded to RGBA8 on load; original channel count kept in `channels`.
- `std::expected<Image, std::string> loadPng(const std::filesystem::path&)`, `bool savePng(const std::filesystem::path&, const Image&)`
- `struct CompareResult { double flipMean; double psnr; Image diffHeatmap; };`
- `std::expected<CompareResult, std::string> compare(const Image& test, const Image& ref)` — error if size differs (`"size mismatch: 64x64 vs 32x32"`).
- CLI: `golden compare --test <png> --ref <png> --out-diff <png> --threshold-flip <float=0.05> [--json <file>]` and `golden batch --manifest <json> --out <dir>` where manifest = `{"phase":"f00","cases":[{"name":..., "test":..., "ref":..., "thresholdFlip":0.05}]}`; batch writes `<out>/results.json`: `{"phase","cases":[{"name","status":"PASS|FAIL","reason":"ok|over-threshold|missing-reference|missing-test|size-mismatch","flipMean","psnr","test","ref","diff","frameTimeMs":null}]}`.
- UTF-8 paths on Windows: `main` uses `wmain` or `std::filesystem::u8path`-equivalent; manifest strings parsed as UTF-8. JSON via `nlohmann/json` (FetchContent pinned).

- [ ] **Step 1:** Tests: identical images → flipMean < 1e-6, psnr == +inf (or ≥ 99); red vs red_with_dot → flipMean > 0 and < 0.05; red vs gray → flipMean > 0.05; size mismatch → error string contains "size mismatch"; load of fixture copied into `ç ğ ü test/` temp dir succeeds.
- [ ] **Step 2:** Run — Expected FAIL.
- [ ] **Step 3:** Implement. FLIP from NVlabs/flip (pinned commit; use its C++ CPU path, LDR FLIP, default PPD 67.0). Heatmap = FLIP magma map. PSNR on RGB.
- [ ] **Step 4:** Run tests — PASS. Then CLI checks:
  - `golden compare --test fixtures/solid_red_64.png --ref fixtures/solid_red_64_copy.png --out-diff out.png` → exit 0
  - same with `gray_64.png` → exit 1
  - with `solid_red_32.png` → exit 2
  - batch manifest containing a case whose ref does not exist → exit 1, JSON reason `missing-reference`.
- [ ] **Step 5:** Commit `feat(tools): golden image diff (FLIP + PSNR)`.

### Task 4: Visual HTML report (spec K-G)

**Files:**
- Create: `tools/golden/report.py` (stdlib only), `tests/golden/test_report.py` (unittest)

**Interfaces:**
- Consumes: `results.json` from Task 3.
- Produces: `python tools/golden/report.py --results <dir>/results.json --out <dir>/report.html` — single self-contained HTML (images embedded as base64 data URIs so the file can be opened/moved anywhere). Per case: name, PASS/FAIL badge, reason, flipMean, psnr, frameTimeMs (or "—"), three images side by side (Render / Reference / FLIP diff) with click-to-zoom; header with phase, date, totals, filter buttons (All / Fail). Missing images show a labelled placeholder box. Works offline (no CDN). Light+dark via `prefers-color-scheme`.

- [ ] **Step 1:** `test_report.py`: build a results.json with one PASS and one `missing-reference` FAIL using fixtures; run report; assert HTML contains `"PASS"`, `"missing-reference"`, exactly 2 case blocks, ≥ 1 `data:image/png;base64,`, and no `http://`/`https://` references.
- [ ] **Step 2:** `python -m unittest tests/golden/test_report.py` — FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run — PASS.
- [ ] **Step 5:** Commit `feat(tools): self-contained visual golden report`.

### Task 5: `apps/headless` skeleton + `run_golden` driver

**Files:**
- Create: `apps/headless/CMakeLists.txt`, `apps/headless/main.cpp`, `apps/headless/TestPattern.h/.cpp`, `scripts/run_golden.py`, `tests/golden/f00/manifest.json`, `tests/golden/ref/f00/test_pattern_256.png`, `tests/unit/headless/test_pattern.cpp`

**Interfaces:**
- Consumes: `golden_core` (`savePng`), `eng::CVarRegistry`, `eng::log`.
- Produces CLI: `headless --backend <name> --scene <path|builtin:test-pattern> --width N --height N --out <png> [--cvar name=value]... [--bench <frames>]`.
  - F0 supports only `--backend cpu` + `builtin:test-pattern`; any other backend → exit 3 with message `backend '<x>' not available in this build` (real backends register in F2).
  - `--cvar` applies with `CVarSource::CommandLine`.
  - `--bench N` renders N times, prints `frame_ms_avg=<float> frame_ms_p95=<float>` on stdout.
- `eng::Image8 makeTestPattern(int w, int h)` — deterministic: 8×8 color checker (sRGB primaries + grays) with a 1-px white border; same output on every platform.
- `scripts/run_golden.py --phase f00 --build-dir build/win-msvc --config Debug` → runs headless for each manifest case, then `golden batch`, then `report.py`; writes `tests/golden/out/f00/{results.json,report.html}`; exit code = golden's.

- [ ] **Step 1:** Unit test: `makeTestPattern(256,256)` pixel (0,0) == white, pixel (16,16) == first checker color, hash of buffer == recorded constant.
- [ ] **Step 2:** Run — FAIL.
- [ ] **Step 3:** Implement; generate `tests/golden/ref/f00/test_pattern_256.png` once from the implementation and commit it.
- [ ] **Step 4:** `python scripts/run_golden.py --phase f00 --build-dir build/win-msvc --config Debug` → exit 0, `tests/golden/out/f00/report.html` exists and shows 1 PASS. `headless --backend d3d12 ...` → exit 3.
- [ ] **Step 5:** Commit `feat(apps): headless skeleton and golden driver`.

### Task 6: Test assets, validator, verified extension list

**Files:**
- Create: `scripts/fetch_assets.py`, `tests/assets/assets.lock.json`, `docs/gltf-extensions.md`, `tests/unit/scripts/test_fetch_assets.py`
- Modify: `.gitignore` (add `tests/assets/glTF-Sample-Assets/`, `tests/assets/tools/`)

**Interfaces:**
- `assets.lock.json`: `{"glTF-Sample-Assets":{"repo":"https://github.com/KhronosGroup/glTF-Sample-Assets","commit":"<sha>"},"gltf-validator":{"version":"<ver>","url":"<release zip url>","sha256":"<hash>"}}`
- `python scripts/fetch_assets.py [--only sample-assets|validator] [--verify]` — sparse/shallow git fetch of the pinned commit into `tests/assets/glTF-Sample-Assets/`; validator zip downloaded to `tests/assets/tools/`, sha256-checked. Idempotent: existing checkout at the right commit → "up to date", no network re-download. Wrong commit → re-checkout. Partial download (.part file) → discarded and restarted.
- `docs/gltf-extensions.md`: table `Extension | Prefix | Status (Ratified / Release Candidate / Draft / Vendor) | Spec §7.2 target? | Sample assets that exercise it`, built from KhronosGroup/glTF `extensions/README.md` at a recorded commit SHA + link. Any difference vs spec §7.2's preliminary list is listed in a "Differences from spec" section (to be folded back into the spec by the reviewer phase).

- [ ] **Step 1:** Unit test (stdlib unittest, network mocked): second run with matching commit performs no download call; leftover `.part` is removed; sha mismatch raises with message `sha256 mismatch`.
- [ ] **Step 2:** Run — FAIL.
- [ ] **Step 3:** Implement; then run real fetch; validate `tests/assets/glTF-Sample-Assets/Models/DamagedHelmet/glTF-Binary/DamagedHelmet.glb` with the validator → 0 errors.
- [ ] **Step 4:** Tests PASS; run fetch twice → second prints `up to date`.
- [ ] **Step 5:** Commit `chore(assets): pinned sample assets, validator, verified extension list`.

### Task 7: Reference images from glTF-Sample-Viewer (spec K-B) — feasibility-gated

**Files:**
- Create: `tools/refgen/package.json`, `tools/refgen/render.mjs`, `tools/refgen/README.md`, `tests/golden/ref/sample-viewer/<Model>.png` (initial set), `tests/golden/ref/sample-viewer/settings.json`

**Interfaces:**
- `node tools/refgen/render.mjs --models <list|all> --out tests/golden/ref/sample-viewer --width 512 --height 512` — drives glTF-Sample-Viewer (pinned version, npm) in headless Chrome via Puppeteer (pinned), fixed: environment = the Sample-Viewer default "Cannon_Exterior" (or whatever is its default — record exact name), tonemap = Khronos PBR Neutral, exposure 1.0, camera = Sample-Viewer default fit camera, IBL on, punctual lights off, animation at t=0. All of these recorded in `settings.json`; F3 reference mode must reproduce exactly these.
- Initial set (at least): `DamagedHelmet`, `MetalRoughSpheres`, `NormalTangentTest`, `AlphaBlendModeTest`, `TextureCoordinateTest`.

- [ ] **Step 1:** Spike: install pinned packages, render `DamagedHelmet` once; check image is non-blank (std-dev of pixels > threshold).
- [ ] **Step 2:** If WebGPU/WebGL headless works → render the initial set; commit PNGs + settings.json. If it cannot work on this machine after exhausting: (a) headless=new, (b) `--use-angle=d3d11`, (c) headed Chrome with offscreen window — mark worklist row `BLOCKED` with the exact error output, and keep the script. Do NOT fabricate references.
- [ ] **Step 3:** Commit `feat(tools): sample-viewer reference generator`.

### Task 8: Worklist + Windows CI

**Files:**
- Create: `docs/worklist.md`, `.github/workflows/ci.yml`

**Interfaces:**
- `docs/worklist.md`: header explaining columns; table `ID | Task | Status (TODO/WIP/DONE/BLOCKED) | Evidence (commit / test / report path) | Note`; rows `f00.t01`…`f00.t08` filled with real evidence; rows `f01`…`f15` as TODO phase lines from spec §11; standing notes K2 (Metal runtime unverified), K3 (Android emulator only).
- `ci.yml`: on push/PR to `main`; job `windows` (`windows-latest`): checkout, `cmake --preset win-msvc`, build Debug, `ctest`, `python -m unittest discover tests`, `python scripts/run_golden.py --phase f00 ...`, upload `tests/golden/out/f00/report.html` as artifact. Job `linux` (`ubuntu-latest`): `linux-clang` preset configure/build/ctest. (macOS job arrives in F1 with `xcrun metal`.)

- [ ] **Step 1:** Fill worklist with real evidence from Tasks 1–7.
- [ ] **Step 2:** Push (`git push`); wait for CI; both jobs green (check via GitHub REST API `GET /repos/qwertyu131/Engine/actions/runs?branch=main&per_page=1`).
- [ ] **Step 3:** Commit `ci: windows+linux build, tests, golden report artifact; worklist`.

## F0 Gate (reviewer runs all of these itself)

1. Clean clone → `cmake --preset win-msvc && cmake --build --preset win-msvc --config Debug` green, no warnings at `/W4` in `engine/` (warnings-as-errors on for `engine` target).
2. `ctest --preset win-msvc -C Debug` 100% pass; `python -m unittest discover tests` pass.
3. `python scripts/run_golden.py --phase f00 ...` exit 0; open `report.html`, confirm render/ref/diff visible.
4. `golden` CLI exit codes 0/1/2 as specified (Review Focus 1–2).
5. `docs/gltf-extensions.md` has registry commit SHA and the differences section.
6. CI latest run on `main` green (both jobs).
7. Task 7 either has committed reference PNGs or a BLOCKED row with exact error evidence.
