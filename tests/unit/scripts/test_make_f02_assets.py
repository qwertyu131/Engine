"""Tests for scripts/make_f02_assets.py (plan F2 Task 3): the committed cube texture is fresh, its tiles
carry the documented colours, and the probe table (an independent Python projection of the
builtin:textured-cube camera) is self-consistent.

Run: python -m unittest tests/unit/scripts/test_make_f02_assets.py
"""
from __future__ import annotations

import importlib.util
import io
import struct
import sys
import unittest
import zlib
from contextlib import redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


def _load_module():
    spec = importlib.util.spec_from_file_location("make_f02_assets", REPO / "scripts" / "make_f02_assets.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["make_f02_assets"] = mod
    spec.loader.exec_module(mod)
    return mod


mk = _load_module()


def decode_png(data: bytes) -> tuple[int, int, bytes]:
    """Minimal decoder for the RGBA8, filter-0 PNGs this script writes."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        (n,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            assert (depth, ctype) == (8, 6)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    rows = []
    for y in range(h):
        line = raw[y * (w * 4 + 1):(y + 1) * (w * 4 + 1)]
        assert line[0] == 0
        rows.append(line[1:])
    return w, h, b"".join(rows)


class CubeTextureTest(unittest.TestCase):
    def test_committed_png_is_fresh(self):
        committed = (REPO / "tests" / "assets" / "f02" / "cube_faces.png").read_bytes()
        self.assertEqual(committed, mk.cube_faces_png(), "run python scripts/make_f02_assets.py")

    def test_tile_centres_have_the_face_colours(self):
        w, h, px = decode_png(mk.cube_faces_png())
        self.assertEqual((w, h), (256, 256))
        for face, colour in enumerate(mk.FACE_COLORS):
            x0, y0 = mk.tile_origin(face)
            cx, cy = x0 + mk.TILE_W // 2, y0 + mk.TILE_H // 2
            i = (cy * w + cx) * 4
            self.assertEqual(tuple(px[i:i + 4]), (*colour, 255), f"face {mk.FACE_NAMES[face]}")

    def test_letter_is_off_centre(self):
        # The glyph sits in the tile's upper-left corner: every pixel with local x >= SOLID_FROM[0] or
        # y >= SOLID_FROM[1] is solid face colour (this includes the tile centre).
        w, _, px = decode_png(mk.cube_faces_png())
        sx, sy = mk.SOLID_FROM
        for face, colour in enumerate(mk.FACE_COLORS):
            x0, y0 = mk.tile_origin(face)
            for y in range(y0, y0 + mk.TILE_H):
                for x in range(x0, x0 + mk.TILE_W):
                    if x - x0 >= sx or y - y0 >= sy:
                        i = (y * w + x) * 4
                        self.assertEqual(tuple(px[i:i + 3]), colour)
            # ... and the letter exists (some white pixels in the upper-left quadrant).
            white = 0
            for y in range(y0, y0 + mk.TILE_H // 2):
                for x in range(x0, x0 + mk.TILE_W // 2):
                    i = (y * w + x) * 4
                    white += tuple(px[i:i + 3]) == mk.LETTER_COLOR
            self.assertGreater(white, 100, f"face {mk.FACE_NAMES[face]}")


class ProbeTest(unittest.TestCase):
    def test_visible_faces_and_probes(self):
        probes = mk.cube_probes(128, 128)
        visible = sorted(p["face"] for p in probes if p["kind"] == "face-centre")
        self.assertEqual(visible, sorted(["+Z", "-X", "+Y"]))
        for p in probes:
            self.assertTrue(0 <= p["x"] < 128 and 0 <= p["y"] < 128, p)
            # Every probe pixel centre hits the solid interior of a visible face's tile.
            self.assertIn(p["hitFace"], ["+Z", "-X", "+Y"], p)
            # Solid area with a filtering margin: away from the tile edges and from the glyph box
            # (local 0.35 + 0.1 margin).
            self.assertTrue(0.15 <= p["u"] <= 0.85 and 0.15 <= p["v"] <= 0.85, p)
            self.assertTrue(p["u"] >= 0.45 or p["v"] >= 0.45, p)
        depth = [p for p in probes if p["kind"] == "depth"]
        self.assertEqual(len(depth), 1)
        self.assertNotEqual(depth[0]["face"], depth[0]["hitFace"])  # hidden face covered by a near one

    def test_cli_prints_the_table(self):
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(mk.main(["--probes", "128", "128"]), 0)
        self.assertIn("face-centre", out.getvalue())
        self.assertIn("depth", out.getvalue())


if __name__ == "__main__":
    unittest.main()
