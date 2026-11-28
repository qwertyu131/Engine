# Facade changelog (spec K-D)

The library facade is `include/engine/*.h`. Hosts (`apps/headless`, `apps/viewer`, later other applications) use only
these headers and link only the CMake target `engine::facade`; `tests/unit/scripts/test_facade_includes.py` checks both
mechanically. The facade is frozen from F7 (semver; every change after that is a versioned entry here). Changes before
the freeze are logged below.

## F3 (plan F3 Task 4): first version

Headers:

| Header | Content |
|---|---|
| `Renderer.h` | `eng::Renderer`, `RendererDesc`, `NativeWindow`, `Image` |
| `AssetHandle.h` | `AssetHandle`, `SceneHandle` |
| `Scene.h` | `AssetInfo`, `SceneInfo` (read-only information; the scene graph stays inside the engine) |
| `Camera.h` | `Camera` (look-at + perspective, world space, +Y up) |
| `Settings.h` | `Settings` (the cvar registry: `set` / `find` / `forEach` / `registry`) |
| `CVar.h` | the cvar system (moved from `engine/core`; spec §8 settings are part of the public API) |
| `Result.h` | `eng::Error`, `eng::Result` (moved from `engine/core`) |
| `rhi/Types.h`, `rhi/DeviceCaps.h` | RHI value types (`Backend`, `Format`, ...) and `DeviceCaps` (moved from `engine/rhi`; `Renderer::caps()` returns them, as the plan's interface says) |
| `Window.h` | `eng::platform::Window` (moved from `engine/platform`; the viewer's window) |

The moved headers keep forwarding headers at their old paths, so engine-internal code is unchanged.

The plan's interface (`create`, `loadAsset`, `instantiate`, `fitCamera`, `setCamera`, `renderFrame`, `readback`,
`settings`, `resize`, `caps`) is implemented as written. Additions, each needed by a host that moved to the facade:

| Addition | Why |
|---|---|
| `RendererDesc::vsync`, `debugUi`, `shaderDir`, `assetDir` | the viewer's present mode and ImGui overlay; the shader library of the `shaders` build step; the built-in scenes' assets |
| `loadAsset("builtin:<name>")`, `static builtinScenes()` | the F2 built-in scenes stay available behind the facade (plan F3 Task 4) |
| `static compiledBackends()` | `--help` and backend checks of both apps |
| `setActiveScene(SceneHandle)` | `instantiate` makes its scene active; a host with several scenes can switch |
| `setTime(float)` | the viewer's spinning cube (built-in scenes); glTF scenes stay at rest until F4 |
| `assetInfo(AssetHandle)`, `sceneInfo(SceneHandle)` | extensions used / diagnostics (viewer asset panel, Task 8) and scene extents (tests) |
| `validationErrorCount()`, `finish()` | exit code 4 of both apps: `finish()` releases every GPU object and waits, so teardown validation messages are counted before the exit code (as the F2 apps did) |
| `requestCapture()` | viewer screenshots: the back buffer is read before present, `readback()` returns it |
| `setVsync(bool)` | the viewer's `r.VSync` cvar at run time |
| `beginUi(dt, dpiScale, events)` | the viewer's Dear ImGui panels; Dear ImGui (`<imgui.h>`) is a public dependency of `engine::facade` |

Renderer cvars registered by `Renderer::create`: `r.Background` (`Environment` | `Color`) and `r.Background.Color`
(`r,g,b` display values). Their names are the plan's Task 5 names; Task 4 registers them early because its probes need
a known background. `r.Asset.MaxTextureSize`, `r.Asset.MaxVertices`, `r.Asset.MaxFileBytes` (plan F3 Task 2) are also
registered there, so hosts can set them before `loadAsset`.
