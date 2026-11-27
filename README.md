# Engine

Cross-platform, native-API glTF 2.0 renderer (C++20, CMake).

Design: `docs/superpowers/specs/2026-11-27-cross-platform-gltf-renderer-design.md`
Plans: `docs/superpowers/plans/`

## Build (Windows, plain PowerShell)

Requirements: Visual Studio 18 2026 (MSVC), CMake >= 3.28, git.

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
| `tests/unit/` | doctest unit tests (one CTest entry per suite: `unit.<suite>`, plus `unit.all`) |
| `cmake/Dependencies.cmake` | pinned FetchContent dependencies |

Options: `ENG_BUILD_TESTS` (ON), `ENG_ENABLE_ASSERTS` (`DEBUG_ONLY` default, `ON`, `OFF`).
