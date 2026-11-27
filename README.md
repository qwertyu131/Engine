# Engine

Cross-platform, native-API glTF 2.0 renderer (C++20, CMake).

Design: `docs/superpowers/specs/2026-11-27-cross-platform-gltf-renderer-design.md`
Plans: `docs/superpowers/plans/`

## Build (Windows, plain PowerShell)

Requirements: Visual Studio 18 2026 (MSVC), git, CMake >= 3.28 (the `Visual Studio 18 2026`
generator itself needs a CMake release that knows it; verified with CMake 4.3.2).

```powershell
cmake --preset win-msvc
cmake --build --preset win-msvc --config Debug
ctest --preset win-msvc -C Debug --output-on-failure
```

Linux / macOS (Ninja Multi-Config, clang):

```sh
cmake --preset linux-clang        # or macos-clang
cmake --build --preset linux-clang --config Debug
ctest --preset linux-clang -C Debug --output-on-failure
```

## Layout

| Path | Content |
|---|---|
| `engine/core/` | log, assert, cvar registry |
| `tools/golden/` | `golden` image diff CLI (NVIDIA FLIP + PSNR) and `golden_core` library |
| `apps/headless/` | `headless` offscreen render -> PNG (F0: `cpu` backend, `builtin:test-pattern` only) |
| `scripts/` | `run_golden.py` (headless -> golden -> report driver) |
| `tests/unit/` | doctest unit tests (one CTest entry per suite: `unit.<suite>`, plus `unit.all`) |
| `tests/golden/` | golden / headless CLI exit-code tests (`golden.cli.*`, `headless.cli.*`), fixtures (`fixtures/make_fixtures.py`), per-phase manifests (`<phase>/manifest.json`), committed references (`ref/<phase>/`) |
| `cmake/Dependencies.cmake` | pinned FetchContent dependencies |

Options: `ENG_BUILD_TESTS` (ON), `ENG_ENABLE_ASSERTS` (`DEBUG_ONLY` default, `ON`, `OFF`).

## Golden image diff

```powershell
build\win-msvc\tools\golden\Debug\golden.exe compare --test a.png --ref b.png --out-diff diff.png [--threshold-flip 0.05] [--json r.json]
build\win-msvc\tools\golden\Debug\golden.exe batch --manifest manifest.json --out outdir
```

Exit codes: `0` all PASS, `1` at least one FAIL, `2` usage / IO error (in `compare`, also a size or
channel-count mismatch). `batch` writes `outdir/results.json` (schema in `tools/golden/Batch.h`;
paths relative to `results.json`) and one `<case>.diff.png` FLIP heatmap per compared case.
Visual report (self-contained HTML, images embedded, works offline):
`python tools/golden/report.py --results outdir/results.json --out outdir/report.html`.
Python tests: `python -m unittest discover tests`.
The golden targets are compiled as C++23 (`std::expected`); `engine` stays C++20.

## Headless render + golden run

```powershell
build\win-msvc\apps\headless\Debug\headless.exe --backend cpu --scene builtin:test-pattern --width 256 --height 256 --out tp.png [--cvar name=value]... [--bench N]
python scripts/run_golden.py --phase f00 --build-dir build/win-msvc --config Debug
```

`headless` exit codes: `0` ok, `2` usage / IO error (also unknown `--cvar` or rejected value),
`3` backend not available in this build (F0 has only `cpu`). `--bench N` prints
`frame_ms_avg=<ms> frame_ms_p95=<ms>`. `run_golden.py` renders every case of
`tests/golden/<phase>/manifest.json`, runs `golden batch` and `report.py`, and writes
`tests/golden/out/<phase>/{results.json,report.html}`; its exit code is golden's.
