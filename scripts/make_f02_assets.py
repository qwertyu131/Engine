"""F2 test assets (plan F2 Task 3) and the probe table of builtin:textured-cube.

Usage:
    python scripts/make_f02_assets.py                 # writes tests/assets/f02/cube_faces.png
    python scripts/make_f02_assets.py --check         # exit 1 if the committed PNG's pixels are stale
    python scripts/make_f02_assets.py --probes W H    # prints the cube probe pixels for a W x H render

cube_faces.png (256 x 256, RGBA8, sRGB-encoded colours): a 3 x 2 grid of 80 x 120 px tiles inside an
8 px grey border, one tile per cube face, in face order +X, -X, +Y, -Y, +Z, -Z (tile i at column i % 3,
row i // 3). Each tile is a solid colour (FACE_COLORS) with a white letter (R L U D F B) in its
upper-left corner, so the tile centre is solid and the letter shows the face orientation.

The probe table is an independent Python model of the scene in engine/renderer/builtin/CubeScene.cpp (same constants,
not the same code): camera, model rotation, per-face UV mapping, ray cast against the cube. The C++
tests hard-code its output; a Y flip, UV-origin or winding error in the renderer therefore cannot be
cancelled by the same error in the probe positions.

Stdlib only; the pixels are identical across runs. The compressed bytes depend on the zlib build (Windows
CPython 3.14 ships zlib-ng), so freshness is checked on the decoded pixels, never on the file bytes.
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / "tests" / "assets" / "f02" / "cube_faces.png"

SIZE = 256
BORDER = 8
TILE_W, TILE_H = 80, 120
FACE_NAMES = ["+X", "-X", "+Y", "-Y", "+Z", "-Z"]
LETTERS = ["R", "L", "U", "D", "F", "B"]
FACE_COLORS = [  # sRGB bytes
    (200, 60, 50),   # +X red
    (60, 170, 70),   # -X green
    (50, 90, 200),   # +Y blue
    (220, 200, 60),  # -Y yellow
    (180, 70, 190),  # +Z magenta
    (60, 190, 200),  # -Z cyan
]
BORDER_COLOR = (128, 128, 128)
LETTER_COLOR = (245, 245, 245)
GLYPH_SCALE = 5
GLYPH_OFFSET = 2  # px from the tile's top-left corner
# The glyph box is [2, 27) x [2, 37) px of the tile; everything with local x >= 28 or y >= 42
# (35 % of the tile) is solid face colour.
SOLID_FROM = (28, 42)

# 5 x 7 glyphs, rows top-down, '#' = set.
GLYPHS = {
    "R": ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "U": ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "D": ["####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."],
    "F": ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
}

# --- builtin:textured-cube scene constants (must equal engine/renderer/builtin/CubeScene.cpp) -------------
UV_INSET = 2.0           # px: each face samples its tile inset by 2 px (no bleeding from neighbours)
CAMERA_EYE = (0.0, 0.0, 3.0)  # looking at the origin, +Y up, right-handed view
FOV_Y_DEG = 45.0
ROT_Y_DEG = 30.0         # model = Rx(20) * Ry(30): first about +Y, then about +X
ROT_X_DEG = 20.0
# Per face (outward normal, right, up) as seen from outside; local (u, v) = (0, 0) is the face's
# top-left: position = 0.5 * n + (u - 0.5) * right + (0.5 - v) * up.
FACE_FRAMES = [
    ((1, 0, 0), (0, 0, -1), (0, 1, 0)),   # +X
    ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),   # -X
    ((0, 1, 0), (1, 0, 0), (0, 0, -1)),   # +Y
    ((0, -1, 0), (1, 0, 0), (0, 0, 1)),   # -Y
    ((0, 0, 1), (1, 0, 0), (0, 1, 0)),    # +Z
    ((0, 0, -1), (-1, 0, 0), (0, 1, 0)),  # -Z
]


def tile_origin(face: int) -> tuple[int, int]:
    return BORDER + TILE_W * (face % 3), BORDER + TILE_H * (face // 3)


def cube_faces_rgba() -> bytes:
    px = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            c = BORDER_COLOR
            for face in range(6):
                x0, y0 = tile_origin(face)
                if x0 <= x < x0 + TILE_W and y0 <= y < y0 + TILE_H:
                    c = FACE_COLORS[face]
                    gx = (x - x0 - GLYPH_OFFSET) // GLYPH_SCALE
                    gy = (y - y0 - GLYPH_OFFSET) // GLYPH_SCALE
                    if x - x0 >= GLYPH_OFFSET and y - y0 >= GLYPH_OFFSET and gx < 5 and gy < 7:
                        if GLYPHS[LETTERS[face]][gy][gx] == "#":
                            c = LETTER_COLOR
            px += bytes((*c, 255))
    return bytes(px)


def png_bytes(w: int, h: int, rgba: bytes) -> bytes:
    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def cube_faces_png() -> bytes:
    return png_bytes(SIZE, SIZE, cube_faces_rgba())


def decode_png(data: bytes) -> tuple[int, int, bytes]:
    """(width, height, RGBA8 pixels) of an 8-bit RGBA, non-interlaced PNG whose rows all use filter 0 (the
    PNGs png_bytes writes). Raises ValueError for anything else."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        (n,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body[:13])
            if (depth, ctype, interlace) != (8, 6, 0):
                raise ValueError("not 8-bit RGBA non-interlaced")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * 4 + 1
    rows = []
    for y in range(h):
        line = raw[y * stride:(y + 1) * stride]
        if line[0] != 0:
            raise ValueError("unsupported PNG row filter")
        rows.append(line[1:])
    return w, h, b"".join(rows)


def is_fresh(path: Path = OUT) -> bool:
    """True if `path` decodes to exactly the pixels this script generates."""
    try:
        return path.is_file() and decode_png(path.read_bytes()) == (SIZE, SIZE, cube_faces_rgba())
    except (ValueError, zlib.error, struct.error):
        return False


# --- independent scene model (probes) -----------------------------------------------------------

def _rot(p):
    """model rotation: Ry(30) then Rx(20) (column vectors: v' = Rx * Ry * v)."""
    a, b = math.radians(ROT_Y_DEG), math.radians(ROT_X_DEG)
    x, y, z = p
    x, z = x * math.cos(a) + z * math.sin(a), -x * math.sin(a) + z * math.cos(a)
    y, z = y * math.cos(b) - z * math.sin(b), y * math.sin(b) + z * math.cos(b)
    return (x, y, z)


def _add(a, b, s=1.0):
    return tuple(a[i] + s * b[i] for i in range(3))


def _dot(a, b):
    return sum(a[i] * b[i] for i in range(3))


def _project(p, w, h):
    """World point -> pixel (origin top-left), perspective about the camera on +Z."""
    f = 1.0 / math.tan(math.radians(FOV_Y_DEG) / 2.0)
    vx, vy, vz = p[0] - CAMERA_EYE[0], p[1] - CAMERA_EYE[1], p[2] - CAMERA_EYE[2]
    ndc_x = f * (w / h) ** -1 * vx / -vz
    ndc_y = f * vy / -vz
    return (ndc_x + 1.0) * 0.5 * w, (1.0 - ndc_y) * 0.5 * h


def _ray(px, py, w, h):
    """Camera ray through pixel (px, py) (continuous coordinates)."""
    f = 1.0 / math.tan(math.radians(FOV_Y_DEG) / 2.0)
    ndc_x = px / w * 2.0 - 1.0
    ndc_y = 1.0 - py / h * 2.0
    return (ndc_x * (w / h) / f, ndc_y / f, -1.0)


def _hit(px, py, w, h):
    """Nearest cube face hit through pixel centre (px, py): (face, u, v) or None."""
    d = _ray(px, py, w, h)
    best = None
    for face, (n, r, u) in enumerate(FACE_FRAMES):
        nw, rw, uw = _rot(n), _rot(r), _rot(u)
        centre = tuple(0.5 * c for c in nw)
        denom = _dot(nw, d)
        if abs(denom) < 1e-12:
            continue
        t = _dot(nw, _add(centre, CAMERA_EYE, -1.0)) / denom
        if t <= 0:
            continue
        p = _add(CAMERA_EYE, d, t)
        rel = _add(p, centre, -1.0)
        lu, lv = _dot(rel, rw) + 0.5, 0.5 - _dot(rel, uw)
        if 0.0 <= lu <= 1.0 and 0.0 <= lv <= 1.0 and (best is None or t < best[0]):
            best = (t, face, lu, lv)
    return None if best is None else best[1:]


# Depth probe: the midpoint of the far edge between the hidden faces +X and -Y (model space).
DEPTH_PROBE_POINT = (0.5, -0.5, 0.0)


def _probe(kind: str, face: str, point, w: int, h: int) -> dict:
    sx, sy = _project(point, w, h)
    x, y = int(math.floor(sx)), int(math.floor(sy))
    hit = _hit(x + 0.5, y + 0.5, w, h)
    hf, lu, lv = hit if hit else (None, -1.0, -1.0)
    return {"kind": kind, "face": face, "x": x, "y": y, "screen": (round(sx, 3), round(sy, 3)),
            "hitFace": FACE_NAMES[hf] if hf is not None else None, "u": round(lu, 4), "v": round(lv, 4),
            "srgb": FACE_COLORS[hf] if hf is not None else None}


def cube_probes(w: int, h: int) -> list[dict]:
    """Face-centre probes of the visible faces + one depth probe: the hidden far edge +X/-Y, which
    projects inside the visible +Z face (with depth testing off and the hidden faces drawn last, the
    far edge's faces would cover it)."""
    probes = []
    for face, (n, _, _) in enumerate(FACE_FRAMES):
        nw = _rot(n)
        centre = tuple(0.5 * c for c in nw)
        if _dot(nw, _add(CAMERA_EYE, centre, -1.0)) > 0:
            probes.append(_probe("face-centre", FACE_NAMES[face], centre, w, h))
    probes.append(_probe("depth", "+X/-Y edge", _rot(DEPTH_PROBE_POINT), w, h))
    return probes


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true", help="exit 1 if the committed PNG differs")
    ap.add_argument("--probes", nargs=2, type=int, metavar=("W", "H"), help="print the probe table")
    args = ap.parse_args(argv)
    if args.probes:
        for p in cube_probes(*args.probes):
            print(f"{p['kind']:<12} face {p['face']}: pixel ({p['x']}, {p['y']}) screen {p['screen']} -> "
                  f"hit {p['hitFace']} local uv ({p['u']}, {p['v']}) srgb {p['srgb']}")
        return 0
    if args.check:
        if not is_fresh():
            print(f"make_f02_assets: {OUT} is stale (run python scripts/make_f02_assets.py)", file=sys.stderr)
            return 1
        print(f"make_f02_assets: {OUT} up to date")
        return 0
    data = cube_faces_png()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(data)
    print(f"make_f02_assets: wrote {OUT} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
