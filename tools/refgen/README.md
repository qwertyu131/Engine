# refgen — glTF-Sample-Viewer reference images (spec K-B)

Renders the "truth" images that F3 reference mode (`r.ReferenceMode=1`) is compared against.
It drives the official Khronos sample renderer (`@khronosgroup/gltf-viewer`, WebGL2) in a
the machine's installed **Firefox** through Puppeteer (WebDriver BiDi, throw-away profile;
no browser is downloaded — user decision 2026-11-27: no Chrome on this machine). Output: `tests/golden/ref/sample-viewer/<Model>.png`
(8-bit RGB, 512x512) plus `settings.json`, which records every fixed parameter.

## Setup (once)

```powershell
cd tools/refgen
npm ci                                   # exact versions from package-lock.json
python ../../scripts/fetch_assets.py --only sample-assets   # models under tests/assets/
```

`.puppeteerrc.cjs` sets `skipDownload: true`, so `npm ci` downloads no browser. Firefox is found at
`C:\Program Files\Mozilla Firefoxirefox.exe` (override: `--firefox <exe>` or `REFGEN_FIREFOX`). `node_modules/` and `.cache/` are git-ignored.

## Run

```powershell
node tools/refgen/render.mjs --models DamagedHelmet,MetalRoughSpheres,NormalTangentTest,AlphaBlendModeTest,TextureCoordinateTest --out tests/golden/ref/sample-viewer
node tools/refgen/render.mjs --models all --out <dir>          # every model in tests/assets
node --test tools/refgen/test/                                 # unit tests for the pure helpers
```

Options: `--width/--height` (default 512), `--mode auto|firefox-headless|firefox-headed-offscreen`
(default `auto` tries them in that order and uses the first whose probe render passes all checks),
`--settings <file>` (default `<out>/settings.json`, merged by model name across runs).

Exit codes: `0` all models written, `1` at least one model failed (nothing written for it),
`2` setup error (bad arguments, missing model, download/sha256 failure, no launch mode worked;
the last case also writes `<out>/refgen-failure.json` with every attempt's error output).

## Fixed settings (recorded in settings.json)

Values follow the glTF-Sample-Viewer app defaults (`KhronosGroup/glTF-Sample-Viewer@8be3925`,
`src/main.js`, `src/logic/viewer_store.js`), except punctual lights, which plan F0 Task 7 turns off.
Parity with the app is by value, not by code path: the app at that commit drives a newer renderer
than the 1.1.0 npm package (it calls `uniformEnvironmentColor` and
`AnimationTimer.setTimeOverride`/`clearTimeOverride`, which 1.1.0 does not have), so the same
values are applied through the 1.1.0 API.

| Setting | Value |
|---|---|
| Renderer | `@khronosgroup/gltf-viewer` 1.1.0 (= glTF-Sample-Renderer tag v1.1.0, `d59d1d6`), draco3d 1.5.7 |
| Browser | Firefox 160.0.1 (installed) via Puppeteer 25.17.0; mode `firefox-headed-offscreen` (headless Firefox fails to start on this machine: exit code 0, `*** You are running in headless mode.`). Output is bit-identical to the earlier Chrome 157 render (same sha256 for all 5 PNGs, both via ANGLE D3D11 on the RTX 3070 Ti). Firefox reports a sanitised WebGL renderer string. |
| Environment | `Cannon_Exterior.hdr` (Sample-Viewer default, index 0) from glTF-Sample-Environments `low_resolution_hdrs` @ `3a030b5`, sha256-checked |
| Environment rotation | 90 deg ("+Z") |
| Background | environment map rendered and blurred (Sample-Viewer default) |
| Clear colour | `[0, 0, 0, 1]` float RGBA (app value when no colour is selected). Not visible while the environment map is rendered, which covers the whole background |
| Tonemap | Khronos PBR Neutral |
| Exposure | 1.0 |
| IBL | on, intensity 1.0 |
| Punctual lights | **off** |
| Camera | `applyTransformHierarchy`, then `UserCamera.resetView` default fit camera on the rest pose, yfov 45 deg, aspect width/height, no orbit |
| Scene | `gltf.scene ?? 0` |
| Animation | as the app: every animation that does not overlap an earlier one (`gltf.nonDisjointAnimations`), sampled at t = 0 (timer paused, `elapsedSec() == 0`; `setFixedTime(0)` is not used because `fixedTime \|\| wallclock` treats 0 as unset). Models without animations render in rest pose |
| Material variant | none |
| Context | WebGL2, `alpha: false`, `antialias: true` (as the app); 3 frames rendered before readback |
| Model file | `glTF-Binary/*.glb` if present, else `glTF/*.gltf` |

## Checks per model

A model counts as rendered only if: no page error, console error or failed/4xx request occurred;
`gatherStatistics` reports meshCount > 0 and faceCount > 0; pixel std-dev >= 2.0; the readback is
the expected size; and the model is visible. Visibility: after the reference capture, the same
camera renders the environment only (a copy of the scene object with no root nodes is swapped in,
because the renderer caches drawables per scene object). At least 1% of the pixels must differ from
that frame by more than 8 in some RGB channel (`modelCoverage` in settings.json). The environment
frame is taken after the capture, so it cannot change the reference pixels. Each model is rendered twice; `deterministicRerender` in settings.json says
whether both renders were byte-identical.

## Known observations

- The fit camera needs world transforms: like the app (`applyLoadedGltf` in `src/main.js`), the
  generator calls `scene.applyTransformHierarchy` before `UserCamera.resetView`. Without it, models
  with node transforms (e.g. `MetalRoughSpheres`) are framed from untransformed extents.
- `MetalRoughSpheres.png` was checked against the asset's own `screenshot/screenshot.png` at the
  pinned Sample-Assets commit (same layout: silver spheres in front, gold spheres behind).
- `DamagedHelmet.png` was checked visually against the Sample-Viewer's own Playwright snapshot
  `tests/__screenshots__/default-model-chromium-darwin.png` (same framing, orientation, background).
- The references above were produced on an RTX 3070 Ti via ANGLE/D3D11. Another GPU can differ in
  the last bits; F3 comparisons use FLIP thresholds, not byte equality.
