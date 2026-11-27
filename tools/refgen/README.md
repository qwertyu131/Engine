# refgen — glTF-Sample-Viewer reference images (spec K-B)

Renders the "truth" images that F3 reference mode (`r.ReferenceMode=1`) is compared against.
It drives the official Khronos sample renderer (`@khronosgroup/gltf-viewer`, WebGL2) in a
pinned headless Chrome through Puppeteer. Output: `tests/golden/ref/sample-viewer/<Model>.png`
(8-bit RGB, 512x512) plus `settings.json`, which records every fixed parameter.

## Setup (once)

```powershell
cd tools/refgen
npm ci                                   # exact versions from package-lock.json
npx puppeteer browsers install chrome    # pinned Chrome -> tools/refgen/.cache/puppeteer (.puppeteerrc.cjs)
python ../../scripts/fetch_assets.py --only sample-assets   # models under tests/assets/
```

`npm ci` may warn that the puppeteer postinstall script did not run; the explicit
`browsers install` line above replaces it. `node_modules/` and `.cache/` are git-ignored.

## Run

```powershell
node tools/refgen/render.mjs --models DamagedHelmet,MetalRoughSpheres,NormalTangentTest,AlphaBlendModeTest,TextureCoordinateTest --out tests/golden/ref/sample-viewer
node tools/refgen/render.mjs --models all --out <dir>          # every model in tests/assets
node --test tools/refgen/test/                                 # unit tests for the pure helpers
```

Options: `--width/--height` (default 512), `--mode auto|headless-new|angle-d3d11|headed-offscreen`
(default `auto` tries them in that order and uses the first whose probe render passes all checks),
`--settings <file>` (default `<out>/settings.json`, merged by model name across runs).

Exit codes: `0` all models written, `1` at least one model failed (nothing written for it),
`2` setup error (bad arguments, missing model, download/sha256 failure, no launch mode worked;
the last case also writes `<out>/refgen-failure.json` with every attempt's error output).

## Fixed settings (recorded in settings.json)

Values follow the glTF-Sample-Viewer app defaults (`KhronosGroup/glTF-Sample-Viewer@8be3925`,
`src/main.js`, `src/logic/viewer_store.js`), except punctual lights, which plan F0 Task 7 turns off.

| Setting | Value |
|---|---|
| Renderer | `@khronosgroup/gltf-viewer` 1.1.0 (= glTF-Sample-Renderer tag v1.1.0, `d59d1d6`), draco3d 1.5.7 |
| Browser | Chrome 157.0.8103.50 via Puppeteer 25.17.0 |
| Environment | `Cannon_Exterior.hdr` (Sample-Viewer default, index 0) from glTF-Sample-Environments `low_resolution_hdrs` @ `3a030b5`, sha256-checked |
| Environment rotation | 90 deg ("+Z") |
| Background | environment map rendered and blurred (Sample-Viewer default) |
| Tonemap | Khronos PBR Neutral |
| Exposure | 1.0 |
| IBL | on, intensity 1.0 |
| Punctual lights | **off** |
| Camera | `UserCamera.resetView` default fit camera, yfov 45 deg, aspect width/height, no orbit |
| Scene | `gltf.scene ?? 0` |
| Animation | none, t = 0 |
| Material variant | none |
| Context | WebGL2, `alpha: false`, `antialias: true` (as the app); 3 frames rendered before readback |
| Model file | `glTF-Binary/*.glb` if present, else `glTF/*.gltf` |

## Checks per model

A model counts as rendered only if: no page error, console error or failed/4xx request occurred;
`gatherStatistics` reports meshCount > 0 and faceCount > 0; pixel std-dev >= 2.0; and the readback is
the expected size. Each model is rendered twice; `deterministicRerender` in settings.json says
whether both renders were byte-identical.

## Known observations

- `MetalRoughSpheres` appears small in the frame. That is the Sample-Viewer 1.1.0 fit-camera result
  for that model, kept as-is on purpose (the reference must match the viewer, not look nice).
- `DamagedHelmet.png` was checked visually against the Sample-Viewer's own Playwright snapshot
  `tests/__screenshots__/default-model-chromium-darwin.png` (same framing, orientation, background).
- The references above were produced on an RTX 3070 Ti via ANGLE/D3D11. Another GPU can differ in
  the last bits; F3 comparisons use FLIP thresholds, not byte equality.
