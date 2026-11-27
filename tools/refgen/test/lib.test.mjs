// Unit tests for the pure helpers of the reference generator.
// Run: node --test tools/refgen/test/
import { test } from 'node:test';
import assert from 'node:assert/strict';
import zlib from 'node:zlib';
import { parseArgs, encodePngRgb, rgbaBottomUpToRgbTopDown, pixelStats, sha256Hex } from '../lib.mjs';

test('parseArgs reads models list, out, size and defaults', () => {
  const a = parseArgs(['--models', 'DamagedHelmet,MetalRoughSpheres', '--out', 'x/y', '--width', '256']);
  assert.deepEqual(a.models, ['DamagedHelmet', 'MetalRoughSpheres']);
  assert.equal(a.out, 'x/y');
  assert.equal(a.width, 256);
  assert.equal(a.height, 512);
  assert.equal(a.mode, 'auto');
});

test('parseArgs rejects unknown flags and bad sizes', () => {
  assert.throws(() => parseArgs(['--bogus']), /unknown argument/);
  assert.throws(() => parseArgs(['--width', '0']), /--width/);
  assert.throws(() => parseArgs(['--mode', 'wat']), /--mode/);
});

test('rgbaBottomUpToRgbTopDown flips rows and drops alpha', () => {
  // 1x2 image: bottom row (first in GL order) red, top row green.
  const gl = Uint8Array.from([255, 0, 0, 255, 0, 255, 0, 255]);
  const rgb = rgbaBottomUpToRgbTopDown(gl, 1, 2);
  assert.deepEqual([...rgb], [0, 255, 0, 255, 0, 0]);
});

test('encodePngRgb writes a valid 8-bit RGB PNG that round-trips', () => {
  const w = 2, h = 2;
  const rgb = Uint8Array.from([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]);
  const png = encodePngRgb(rgb, w, h);
  assert.deepEqual([...png.subarray(0, 8)], [137, 80, 78, 71, 13, 10, 26, 10]);
  assert.equal(png.readUInt32BE(16), w);
  assert.equal(png.readUInt32BE(20), h);
  assert.equal(png[24], 8); // bit depth
  assert.equal(png[25], 2); // colour type RGB
  // Locate IDAT and inflate it back.
  let off = 8, idat = [];
  while (off < png.length) {
    const len = png.readUInt32BE(off);
    const type = png.toString('latin1', off + 4, off + 8);
    if (type === 'IDAT') idat.push(png.subarray(off + 8, off + 8 + len));
    off += 12 + len;
  }
  const raw = zlib.inflateSync(Buffer.concat(idat));
  assert.deepEqual([...raw], [0, 1, 2, 3, 4, 5, 6, 0, 7, 8, 9, 10, 11, 12]);
});

test('pixelStats: flat image has zero std-dev, varied image does not', () => {
  const flat = new Uint8Array(3 * 16).fill(58);
  assert.equal(pixelStats(flat).stdDev, 0);
  const varied = Uint8Array.from({ length: 48 }, (_, i) => (i * 37) % 256);
  assert.ok(pixelStats(varied).stdDev > 10);
});

test('sha256Hex matches known digest', () => {
  assert.equal(sha256Hex(Buffer.from('abc')),
    'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
});
