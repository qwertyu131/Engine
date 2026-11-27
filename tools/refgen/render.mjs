// Renders glTF-Sample-Viewer reference images headlessly (spec K-B, plan F0 Task 7).
//
//   node tools/refgen/render.mjs --models DamagedHelmet,MetalRoughSpheres --out tests/golden/ref/sample-viewer
//   node tools/refgen/render.mjs --models all --out <dir> --width 512 --height 512
//
// Drives the official Khronos glTF sample renderer (npm @khronosgroup/gltf-viewer, pinned in
// package-lock.json) inside the installed Firefox (Puppeteer, WebDriver BiDi; no browser download).
// Every fixed rendering parameter is written to <out>/settings.json; F3 reference mode must
// reproduce exactly those. A model whose render is blank, throws, or logs a console error is a
// hard failure: nothing is written for it and the process exits non-zero. Nothing is faked.
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import puppeteer from 'puppeteer';
import { parseArgs, rgbaBottomUpToRgbTopDown, encodePngRgb, pixelStats, sha256Hex, differingPixelFraction } from './lib.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '..', '..');
const VIEWER_DIST = path.join(HERE, 'node_modules', '@khronosgroup', 'gltf-viewer', 'dist');
const DRACO_DIR = path.join(HERE, 'node_modules', 'draco3d');
const MODELS_DIR = path.join(REPO, 'tests', 'assets', 'glTF-Sample-Assets', 'Models');
const ENV_CACHE = path.join(HERE, '.cache', 'env');

// Files the npm package does not ship. Pinned by commit SHA + sha256; downloaded into the
// git-ignored cache (never committed).
const PINNED_FILES = {
  'Cannon_Exterior.hdr': {
    url: 'https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Environments/3a030b54c2e4f0f4d469eec2f98f47398c9a2a30/Cannon_Exterior.hdr',
    sha256: '7d65926ed2fa41c121738af2addf1fd5f1ae42d12ff77255e2679777f06c984b',
    license: 'CC0-1.0 (Greg Zaal), see Cannon_Exterior.hdr.license in the same commit',
  },
  'lut_sheen_E.png': {
    url: 'https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Renderer/d59d1d612ab6a6c242ff561b753fd4623a0ee4bd/assets/images/lut_sheen_E.png',
    sha256: '7f21d7754dd3a2a972d9d1298ee3e67e20c5b2f21969095d322a1bc20f8b2f04',
    license: 'Apache-2.0 (glTF-Sample-Renderer v1.1.0)',
  },
};

// Fixed rendering parameters. Values mirror the glTF-Sample-Viewer app defaults
// (KhronosGroup/glTF-Sample-Viewer@8be392572d5def1cd6d18d3487e0b959c6f3c7c5: src/main.js,
// src/logic/viewer_store.js) except where plan F0 Task 7 overrides them (punctual lights off).
// The app at that commit drives a newer renderer than the 1.1.0 npm package (it calls
// uniformEnvironmentColor and AnimationTimer.setTimeOverride, which 1.1.0 lacks), so "mirrors
// the app" means: same values, applied through the 1.1.0 API.
const RENDER_SETTINGS = {
  environment: 'Cannon_Exterior',
  environmentNote: 'first entry (index 0, selectedEnvironment: 0) of the Sample-Viewer environment list, low_resolution_hdrs branch',
  environmentRotationDeg: 90.0,
  environmentRotationNote: 'Sample-Viewer rotation "+Z" == 90 degrees',
  toneMap: 'Khronos PBR Neutral',
  exposure: 1.0,
  exposureNote: 'Sample-Viewer UI exposure 0 EV -> factor 1/2^0',
  useIBL: true,
  iblIntensity: 1.0,
  usePunctual: false,
  useDirectionalLightsWithDisabledIBL: true,
  renderEnvironmentMap: true,
  blurEnvironmentMap: true,
  clearColor: [0, 0, 0, 1],
  clearColorNote: 'float RGBA passed straight to gl.clearColor; Sample-Viewer app value when no colour is selected (syncClearColor in src/main.js). Not visible while renderEnvironmentMap is true: the environment covers the whole background',
  internalMSAA: 4,
  morphing: true,
  skinning: true,
  animation: 'as the app (applyLoadedGltf): every animation not overlapping an earlier one (gltf.nonDisjointAnimations), sampled at t = 0 (AnimationTimer paused, elapsedSec() == 0); models without animations render in rest pose',
  camera: 'scene.applyTransformHierarchy, then UserCamera.resetView(gltf, sceneIndex) default fit camera on the rest pose (before animation is applied, as the app); yfov 45 deg; aspect = width/height; no orbit',
  sceneIndex: 'gltf.scene ?? 0',
  variant: 'KHR_materials_variants: none (default materials)',
  contextAttributes: { alpha: false, antialias: true, preserveDrawingBuffer: true },
  framesBeforeCapture: 3,
};

// Browser: the user's installed Firefox (user decision 2026-11-27: no Chrome on this machine).
// Puppeteer drives it over WebDriver BiDi with a throw-away profile; nothing is downloaded.
// Override the path with --firefox <exe> or REFGEN_FIREFOX.
function firefoxPath(cliPath) {
  const candidates = [cliPath, process.env.REFGEN_FIREFOX,
    'C:\\Program Files\\Mozilla Firefox\\firefox.exe',
    'C:\\Program Files (x86)\\Mozilla Firefox\\firefox.exe',
    '/usr/bin/firefox', '/Applications/Firefox.app/Contents/MacOS/firefox'].filter(Boolean);
  const found = candidates.find((p) => fs.existsSync(p));
  if (!found) throw new Error(`Firefox not found (tried: ${candidates.join(', ')}); pass --firefox <path>`);
  return found;
}

const FIREFOX_PREFS = {
  'webgl.force-enabled': true,        // ignore the GPU blocklist (like Chrome's --ignore-gpu-blocklist)
  'webgl.disabled': false,
  'webgl.enable-webgl2': true,
  'layers.acceleration.force-enabled': true,
  'gfx.webrender.all': true,
};

function launchConfigs(mode) {
  const all = {
    'firefox-headless': { headless: true, args: [] },
    'firefox-headed-offscreen': { headless: false, args: ['-width', '800', '-height', '800'] },
  };
  const order = mode === 'auto' ? ['firefox-headless', 'firefox-headed-offscreen'] : [mode];
  for (const name of order) if (!all[name]) throw new Error(`unknown --mode '${name}' (auto|${Object.keys(all).join('|')})`);
  return order.map((name) => ({ name, ...all[name] }));
}

async function ensurePinnedFiles() {
  await fsp.mkdir(ENV_CACHE, { recursive: true });
  for (const [name, pin] of Object.entries(PINNED_FILES)) {
    const dst = path.join(ENV_CACHE, name);
    if (fs.existsSync(dst) && sha256Hex(await fsp.readFile(dst)) === pin.sha256) continue;
    const part = dst + '.part';
    await fsp.rm(part, { force: true });
    console.log(`fetch ${pin.url}`);
    const res = await fetch(pin.url);
    if (!res.ok) throw new Error(`download failed ${res.status} ${pin.url}`);
    const buf = Buffer.from(await res.arrayBuffer());
    const got = sha256Hex(buf);
    if (got !== pin.sha256) throw new Error(`sha256 mismatch for ${name}: expected ${pin.sha256}, got ${got}`);
    await fsp.writeFile(part, buf);
    await fsp.rename(part, dst);
  }
}

function listAllModels() {
  return fs.readdirSync(MODELS_DIR, { withFileTypes: true }).filter((d) => d.isDirectory()).map((d) => d.name).sort();
}

// Prefer the self-contained binary variant, then plain glTF.
function resolveModel(name) {
  for (const variant of ['glTF-Binary', 'glTF']) {
    const dir = path.join(MODELS_DIR, name, variant);
    if (!fs.existsSync(dir)) continue;
    const ext = variant === 'glTF-Binary' ? '.glb' : '.gltf';
    const file = fs.readdirSync(dir).find((f) => f.toLowerCase().endsWith(ext));
    if (file) return { variant, file, urlPath: `/models/${encodeURIComponent(name)}/${variant}/${encodeURIComponent(file)}` };
  }
  return null;
}

const MIME = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm',
  '.json': 'application/json', '.gltf': 'model/gltf+json', '.glb': 'model/gltf-binary', '.bin': 'application/octet-stream',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.ktx2': 'image/ktx2', '.webp': 'image/webp', '.hdr': 'application/octet-stream',
};

const PAGE = `<!doctype html><html><head><meta charset="utf-8"><title>refgen</title>
<link rel="icon" href="data:,">
<style>html,body{margin:0;background:#000}canvas{display:block}</style></head>
<body><canvas id="c"></canvas>
<script src="/viewer/libs/libktx.js"></script>
<script src="/draco/draco_decoder_nodejs.js"></script>
<script type="module">
import { GltfView, GltfState } from '/viewer/gltf-viewer.module.js';
const S = await (await fetch('/settings')).json();
const canvas = document.getElementById('c');
canvas.width = S.width; canvas.height = S.height;
canvas.style.width = S.width + 'px'; canvas.style.height = S.height + 'px';
const gl = canvas.getContext('webgl2', S.contextAttributes);
if (!gl) throw new Error('webgl2 context unavailable');
const dbg = gl.getExtension('WEBGL_debug_renderer_info');
const view = new GltfView(gl);
// Same wiring as the Sample-Viewer app: Draco from the draco3d package, wasm served next to it.
const dracoLib = (opts) => DracoDecoderModule({ ...opts, locateFile: () => '/draco/draco_decoder.wasm' });
const loader = view.createResourceLoader(dracoLib, undefined, '/viewer/libs/');
const state = view.createState();
const p = state.renderingParameters;
p.toneMap = GltfState.ToneMaps.KHR_PBR_NEUTRAL;
p.debugOutput = GltfState.DebugOutput.NONE;
p.exposure = S.exposure; p.useIBL = S.useIBL; p.iblIntensity = S.iblIntensity; p.usePunctual = S.usePunctual;
p.useDirectionalLightsWithDisabledIBL = S.useDirectionalLightsWithDisabledIBL;
p.renderEnvironmentMap = S.renderEnvironmentMap; p.blurEnvironmentMap = S.blurEnvironmentMap;
p.environmentRotation = S.environmentRotationDeg; p.clearColor = S.clearColor; p.internalMSAA = S.internalMSAA;
p.morphing = S.morphing; p.skinning = S.skinning;
state.environment = await loader.loadEnvironment('/env/' + S.environment + '.hdr', { lut_sheen_E_file: '/env/lut_sheen_E.png' });
if (!state.environment) throw new Error('environment failed to load');
function readback() {
  const px = new Uint8Array(S.width * S.height * 4);
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  gl.readPixels(0, 0, S.width, S.height, gl.RGBA, gl.UNSIGNED_BYTE, px);
  return px;
}
function b64(px) {
  let bin = '';
  for (let i = 0; i < px.length; i += 0x8000) bin += String.fromCharCode.apply(null, px.subarray(i, i + 0x8000));
  return btoa(bin);
}
window.__refgen = {
  info: {
    renderer: dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER),
    vendor: dbg ? gl.getParameter(dbg.UNMASKED_VENDOR_WEBGL) : gl.getParameter(gl.VENDOR),
    version: gl.getParameter(gl.VERSION),
    toneMapConst: p.toneMap,
  },
  async render(url) {
    state.gltf = await loader.loadGltf(url);
    state.sceneIndex = state.gltf.scene ?? 0;
    state.cameraIndex = undefined;
    // As the app: enable every animation that does not overlap an earlier one; sample at t = 0.
    // The timer is left paused with pausedTime 0 (elapsedSec() == 0). setFixedTime(0) is NOT
    // used: elapsedSec() returns fixedTime || wallclock, so 0 would fall through to wall-clock time.
    state.animationIndices = [];
    for (let i = 0; i < (state.gltf.animations?.length ?? 0); i++) {
      if (!state.gltf.nonDisjointAnimations(state.animationIndices).includes(i)) state.animationIndices.push(i);
    }
    state.animationTimer.pause();
    state.animationTimer.reset();
    if (!state.animationTimer.paused || state.animationTimer.elapsedSec() !== 0) throw new Error('animation timer not at t = 0');
    state.variant = undefined;
    // As the Sample-Viewer app does (src/main.js applyLoadedGltf): world transforms first,
    // otherwise the fit camera measures untransformed extents.
    state.gltf.scenes[state.sceneIndex].applyTransformHierarchy(state.gltf);
    state.userCamera.perspective.aspectRatio = S.width / S.height;
    state.userCamera.resetView(state.gltf, state.sceneIndex);
    const stats = view.gatherStatistics(state);
    for (let i = 0; i < S.framesBeforeCapture; i++) view.renderFrame(state, S.width, S.height);
    gl.finish();
    const px = readback();
    // Environment-only frame with the same camera, taken AFTER the model capture so it cannot
    // affect the reference pixels. The renderer caches drawables per scene object
    // (preparedScene !== scene), so a separate scene object without root nodes is swapped in,
    // rendered, and the original put back.
    const scene = state.gltf.scenes[state.sceneIndex];
    const empty = Object.assign(Object.create(Object.getPrototypeOf(scene)), scene, { nodes: [] });
    let bg, bgStats;
    try {
      state.gltf.scenes[state.sceneIndex] = empty;
      // gatherStatistics cannot be used here (it reduces an empty array without a seed and throws).
      bgStats = { meshCount: empty.gatherNodes(state.gltf).filter((n) => n.mesh !== undefined).length };
      for (let i = 0; i < S.framesBeforeCapture; i++) view.renderFrame(state, S.width, S.height);
      gl.finish();
      bg = readback();
    } finally {
      state.gltf.scenes[state.sceneIndex] = scene;
    }
    return { pixels: b64(px), background: b64(bg), backgroundStats: bgStats, stats, animationIndices: [...state.animationIndices] };
  },
};
window.__refgenReady = true;
</script></body></html>`;

function startServer(settings) {
  const roots = { '/viewer/': VIEWER_DIST, '/draco/': DRACO_DIR, '/models/': MODELS_DIR, '/env/': ENV_CACHE };
  const server = http.createServer(async (req, res) => {
    try {
      const url = decodeURIComponent(new URL(req.url, 'http://x').pathname);
      if (url === '/' || url === '/index.html') return send(res, 200, MIME['.html'], PAGE);
      if (url === '/settings') return send(res, 200, MIME['.json'], JSON.stringify(settings));
      for (const [prefix, root] of Object.entries(roots)) {
        if (!url.startsWith(prefix)) continue;
        const file = path.resolve(root, url.slice(prefix.length));
        if (!file.startsWith(path.resolve(root) + path.sep)) return send(res, 403, 'text/plain', 'forbidden');
        const data = await fsp.readFile(file);
        return send(res, 200, MIME[path.extname(file).toLowerCase()] ?? 'application/octet-stream', data);
      }
      send(res, 404, 'text/plain', 'not found');
    } catch {
      send(res, 404, 'text/plain', 'not found');
    }
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve(server)));
}

function send(res, code, type, body) {
  res.writeHead(code, { 'Content-Type': type, 'Cache-Control': 'no-store' });
  res.end(body);
}

// Minimum pixel std-dev for a frame to count as "rendered something". A flat clear colour is 0;
// a blurred environment background alone is low but non-zero, so it cannot prove the model is
// visible. That is done by comparing the model frame with an environment-only frame from the
// same camera: at least MIN_MODEL_COVERAGE of the pixels must differ by more than
// MODEL_DIFF_THRESHOLD in some RGB channel (catches an off-frame or invisible model).
const MIN_STDDEV = 2.0;
const MODEL_DIFF_THRESHOLD = 8;
const MIN_MODEL_COVERAGE = 0.01;

async function openPage(browser, baseUrl, width, height, errors) {
  const page = await browser.newPage();
  await page.setViewport({ width, height, deviceScaleFactor: 1 });
  page.on('pageerror', (e) => errors.push(`pageerror: ${e.message}`));
  page.on('console', (m) => { if (m.type() === 'error') errors.push(`console.error: ${m.text()}`); });
  page.on('requestfailed', (r) => errors.push(`requestfailed: ${r.url()} ${r.failure()?.errorText}`));
  page.on('response', (r) => { if (r.status() >= 400) errors.push(`http ${r.status()}: ${r.url()}`); });
  await page.goto(baseUrl, { waitUntil: 'load' });
  // Fail fast: a page error during setup will never turn into readiness.
  const deadline = Date.now() + 120000;
  while (!(await page.evaluate(() => window.__refgenReady === true))) {
    const fatal = errors.find((e) => e.startsWith('pageerror'));
    if (fatal) throw new Error(`page setup failed: ${fatal}`);
    if (Date.now() > deadline) throw new Error(`page not ready after 120 s: ${errors.join(' | ')}`);
    await new Promise((r) => setTimeout(r, 200));
  }
  return page;
}

async function renderOne(page, model, width, height, errors) {
  const before = errors.length;
  const r1 = await page.evaluate((u) => window.__refgen.render(u), model.urlPath);
  const r2 = await page.evaluate((u) => window.__refgen.render(u), model.urlPath);
  const newErrors = errors.slice(before);
  const rgba1 = Buffer.from(r1.pixels, 'base64');
  const rgba2 = Buffer.from(r2.pixels, 'base64');
  const rgb = rgbaBottomUpToRgbTopDown(rgba1, width, height);
  const st = pixelStats(rgb);
  const problems = [...newErrors];
  if (rgba1.length !== width * height * 4) problems.push(`readback size ${rgba1.length}`);
  if (st.stdDev < MIN_STDDEV) problems.push(`blank frame (pixel std-dev ${st.stdDev.toFixed(3)} < ${MIN_STDDEV})`);
  if (!r1.stats || !(r1.stats.meshCount > 0) || !(r1.stats.faceCount > 0)) problems.push(`no geometry rendered: ${JSON.stringify(r1.stats)}`);
  const bg = Buffer.from(r1.background, 'base64');
  if (!r1.backgroundStats || r1.backgroundStats.meshCount !== 0) problems.push(`environment-only frame still has geometry: ${JSON.stringify(r1.backgroundStats)}`);
  let coverage = 0;
  if (bg.length === rgba1.length) {
    coverage = differingPixelFraction(rgba1, bg, MODEL_DIFF_THRESHOLD);
    if (coverage < MIN_MODEL_COVERAGE) problems.push(`model not visible: ${(coverage * 100).toFixed(3)}% of pixels differ from the environment-only frame (< ${MIN_MODEL_COVERAGE * 100}%)`);
  } else {
    problems.push(`environment-only readback size ${bg.length}`);
  }
  const h1 = sha256Hex(rgba1), h2 = sha256Hex(rgba2);
  return { rgb, stats: st, sceneStats: r1.stats, coverage, animationIndices: r1.animationIndices, deterministic: h1 === h2, problems };
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  if (!args.models || !args.out) throw new Error('--models and --out are required');
  const names = args.models.length === 1 && args.models[0] === 'all' ? listAllModels() : args.models;
  const models = names.map((n) => ({ name: n, ...(resolveModel(n) ?? {}) }));
  const missing = models.filter((m) => !m.urlPath).map((m) => m.name);
  if (missing.length) throw new Error(`model(s) not found under ${MODELS_DIR}: ${missing.join(', ')} (run scripts/fetch_assets.py)`);

  await ensurePinnedFiles();
  const settings = { ...RENDER_SETTINGS, width: args.width, height: args.height };
  const server = await startServer(settings);
  const baseUrl = `http://127.0.0.1:${server.address().port}/`;
  const outDir = path.resolve(args.out);
  await fsp.mkdir(outDir, { recursive: true });

  const attempts = [];
  let browser, page, mode, errors, info;
  try {
    // Spike / launch-mode selection: the first mode whose DamagedHelmet-or-first-model render
    // passes every check is used for the whole batch.
    for (const cfg of launchConfigs(args.mode)) {
      errors = [];
      try {
        browser = await puppeteer.launch({
          browser: 'firefox', executablePath: firefoxPath(args.firefox), headless: cfg.headless,
          args: cfg.args, extraPrefsFirefox: FIREFOX_PREFS, protocolTimeout: 300000,
        });
        page = await openPage(browser, baseUrl, args.width, args.height, errors);
        const probe = models.find((m) => m.name === 'DamagedHelmet') ?? models[0];
        const r = await renderOne(page, probe, args.width, args.height, errors);
        if (r.problems.length) throw new Error(r.problems.join(' | '));
        info = await page.evaluate(() => window.__refgen.info);
        mode = cfg.name;
        attempts.push({ mode: cfg.name, ok: true });
        break;
      } catch (e) {
        attempts.push({ mode: cfg.name, ok: false, error: String(e.message ?? e), pageErrors: errors.slice(0, 20) });
        console.error(`launch mode '${cfg.name}' failed: ${e.message ?? e}`);
        if (browser) await browser.close().catch(() => {});
        browser = undefined;
      }
    }
    if (!mode) {
      await fsp.writeFile(path.join(outDir, 'refgen-failure.json'), JSON.stringify({ attempts }, null, 2) + '\n');
      throw new Error(`no launch mode produced a valid render; see ${path.join(outDir, 'refgen-failure.json')}`);
    }

    const results = [];
    let failed = 0;
    for (const m of models) {
      const r = await renderOne(page, m, args.width, args.height, errors);
      if (r.problems.length) {
        failed++;
        console.error(`FAIL ${m.name}: ${r.problems.join(' | ')}`);
        results.push({ name: m.name, status: 'FAIL', problems: r.problems });
        continue;
      }
      const png = encodePngRgb(r.rgb, args.width, args.height);
      await fsp.writeFile(path.join(outDir, `${m.name}.png`), png);
      console.log(`ok   ${m.name}  std-dev=${r.stats.stdDev.toFixed(2)} model-coverage=${(r.coverage * 100).toFixed(2)}% animations=[${r.animationIndices}] deterministic=${r.deterministic}`);
      results.push({
        name: m.name, status: 'OK', variant: m.variant, file: m.file, png: `${m.name}.png`, pngSha256: sha256Hex(png),
        pixelStdDev: Number(r.stats.stdDev.toFixed(3)), modelCoverage: Number(r.coverage.toFixed(4)), animationIndices: r.animationIndices,
        deterministicRerender: r.deterministic, sceneStats: r.sceneStats,
      });
    }

    const pkgLock = JSON.parse(await fsp.readFile(path.join(HERE, 'package-lock.json'), 'utf8'));
    const assetsLock = JSON.parse(await fsp.readFile(path.join(REPO, 'tests', 'assets', 'assets.lock.json'), 'utf8'));
    const doc = {
      generator: 'tools/refgen/render.mjs',
      renderer: { package: '@khronosgroup/gltf-viewer', version: pkgLock.packages['node_modules/@khronosgroup/gltf-viewer'].version, sourceTag: 'KhronosGroup/glTF-Sample-Renderer v1.1.0 (d59d1d612ab6a6c242ff561b753fd4623a0ee4bd)', draco3d: pkgLock.packages['node_modules/draco3d'].version },
      browser: { puppeteer: pkgLock.packages['node_modules/puppeteer'].version, version: await browser.version(), launchMode: mode, webgl: info },
      launchAttempts: attempts,
      sampleAssets: assetsLock['glTF-Sample-Assets'],
      pinnedFiles: PINNED_FILES,
      settings: { ...settings, modelDiffThreshold: MODEL_DIFF_THRESHOLD, minModelCoverage: MIN_MODEL_COVERAGE },
      models: results,
    };
    // Keep settings.json stable across re-runs that render the same set: merge by model name.
    const settingsPath = args.settings ? path.resolve(args.settings) : path.join(outDir, 'settings.json');
    if (fs.existsSync(settingsPath)) {
      const prev = JSON.parse(await fsp.readFile(settingsPath, 'utf8'));
      const byName = new Map((prev.models ?? []).map((x) => [x.name, x]));
      for (const x of results) byName.set(x.name, x);
      doc.models = [...byName.values()].sort((a, b) => a.name.localeCompare(b.name));
    }
    await fsp.writeFile(settingsPath, JSON.stringify(doc, null, 2) + '\n');
    console.log(`wrote ${settingsPath} (mode ${mode}, ${info.renderer})`);
    if (failed) process.exitCode = 1;
  } finally {
    if (browser) await browser.close().catch(() => {});
    server.close();
  }
}

main().catch((e) => { console.error(e.message ?? e); process.exitCode = 2; });
