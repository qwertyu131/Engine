// Pure helpers for the glTF-Sample-Viewer reference generator (no browser, no network).
import zlib from 'node:zlib';
import crypto from 'node:crypto';

export const MODES = ['auto', 'firefox-headless', 'firefox-headed-offscreen'];

const USAGE = `usage: node tools/refgen/render.mjs --models <A,B,...|all> --out <dir>
       [--width 512] [--height 512] [--mode auto|firefox-headless|firefox-headed-offscreen]
       [--firefox <firefox.exe>] [--settings <settings.json>]`;

export function parseArgs(argv) {
  const a = { models: null, out: null, width: 512, height: 512, mode: 'auto', settings: null, firefox: null };
  for (let i = 0; i < argv.length; i++) {
    const k = argv[i];
    const v = () => {
      if (i + 1 >= argv.length) throw new Error(`missing value for ${k}\n${USAGE}`);
      return argv[++i];
    };
    switch (k) {
      case '--models': a.models = v().split(',').map((s) => s.trim()).filter(Boolean); break;
      case '--out': a.out = v(); break;
      case '--width': a.width = Number(v()); break;
      case '--height': a.height = Number(v()); break;
      case '--mode': a.mode = v(); break;
      case '--settings': a.settings = v(); break;
      case '--firefox': a.firefox = v(); break;
      case '-h': case '--help': throw new Error(USAGE);
      default: throw new Error(`unknown argument '${k}'\n${USAGE}`);
    }
  }
  for (const d of ['width', 'height']) {
    if (!Number.isInteger(a[d]) || a[d] < 1 || a[d] > 8192) throw new Error(`--${d} must be an integer in 1..8192`);
  }
  if (!MODES.includes(a.mode)) throw new Error(`--mode must be one of ${MODES.join(', ')}`);
  return a;
}

// WebGL readPixels returns RGBA rows bottom-up; PNG wants RGB rows top-down.
export function rgbaBottomUpToRgbTopDown(rgba, width, height) {
  const out = new Uint8Array(width * height * 3);
  for (let y = 0; y < height; y++) {
    const src = (height - 1 - y) * width * 4;
    const dst = y * width * 3;
    for (let x = 0; x < width; x++) {
      out[dst + x * 3] = rgba[src + x * 4];
      out[dst + x * 3 + 1] = rgba[src + x * 4 + 1];
      out[dst + x * 3 + 2] = rgba[src + x * 4 + 2];
    }
  }
  return out;
}

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, 'latin1'), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}

// Deterministic 8-bit RGB PNG (filter 0 on every row, zlib level 9, no ancillary chunks).
export function encodePngRgb(rgb, width, height) {
  const stride = width * 3;
  const raw = Buffer.alloc((stride + 1) * height);
  for (let y = 0; y < height; y++) {
    raw[y * (stride + 1)] = 0;
    Buffer.from(rgb.buffer, rgb.byteOffset + y * stride, stride).copy(raw, y * (stride + 1) + 1);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  return Buffer.concat([
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

// Mean / std-dev over all channel values of an RGB buffer.
export function pixelStats(rgb) {
  let sum = 0, sq = 0;
  for (let i = 0; i < rgb.length; i++) { sum += rgb[i]; sq += rgb[i] * rgb[i]; }
  const n = rgb.length || 1;
  const mean = sum / n;
  return { mean, stdDev: Math.sqrt(Math.max(0, sq / n - mean * mean)) };
}

export function sha256Hex(buf) {
  return crypto.createHash('sha256').update(buf).digest('hex');
}

// Fraction of RGBA pixels where any of R, G, B differs by more than `threshold` between two
// equally sized buffers (alpha ignored). Used to prove a model is on screen: model frame vs. the
// same camera rendering the environment only.
export function differingPixelFraction(rgbaA, rgbaB, threshold) {
  if (rgbaA.length !== rgbaB.length || rgbaA.length % 4 !== 0) {
    throw new Error(`size mismatch: ${rgbaA.length} vs ${rgbaB.length} bytes`);
  }
  const n = rgbaA.length / 4;
  let diff = 0;
  for (let i = 0; i < rgbaA.length; i += 4) {
    if (Math.abs(rgbaA[i] - rgbaB[i]) > threshold || Math.abs(rgbaA[i + 1] - rgbaB[i + 1]) > threshold
      || Math.abs(rgbaA[i + 2] - rgbaB[i + 2]) > threshold) diff++;
  }
  return n ? diff / n : 0;
}
