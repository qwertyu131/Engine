"""Regenerate the synthetic golden-tool fixtures (stdlib only, deterministic).

Usage: python tests/golden/fixtures/make_fixtures.py

Output is byte-identical on every run/platform: fixed zlib level, no timestamps,
no ancillary chunks.
"""
from __future__ import annotations

import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent

# PNG colour types
GRAY = 0
RGB = 2
RGBA = 6
CHANNELS = {GRAY: 1, RGB: 3, RGBA: 4}


def _chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def write_png(path: Path, width: int, height: int, color_type: int, pixel) -> None:
    """pixel(x, y) -> tuple of CHANNELS[color_type] ints in 0..255."""
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # filter: None
        for x in range(width):
            rows.extend(pixel(x, y))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0)
    data = (b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr)
            + _chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + _chunk(b"IEND", b""))
    path.write_bytes(data)


RED = (255, 0, 0)


def red_with_dot(x: int, y: int):
    # One black pixel in the centre: a small, local difference (FLIP mean well below 0.05).
    return (0, 0, 0) if (x, y) == (32, 32) else RED


def _noise(x: int, y: int, seed: int) -> int:
    """Deterministic hash noise in 0..255 (integer only: identical on every platform)."""
    h = (x * 374761393 + y * 668265263 + seed * 2147483647) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return (h ^ (h >> 16)) & 0xFF


def flip_a(x: int, y: int):
    # Gradients, a hard edge and a fine checker: exercises FLIP's colour and edge (feature) terms.
    r = (x * 255) // 95
    g = (y * 255) // 63
    b = 255 if (x // 4 + y // 4) % 2 else 40
    if 30 <= x < 60 and 20 <= y < 44:
        r, g, b = 250, 240, 20
    return (r, g, b)


def flip_b(x: int, y: int):
    # flip_a with noise, a shifted edge and a colour shift in one corner.
    r, g, b = flip_a(min(x + 1, 95), y) if 30 <= x < 64 else flip_a(x, y)
    n = _noise(x, y, 7) // 16 - 8
    r, g, b = (max(0, min(255, c + n)) for c in (r, g, b))
    if x >= 72 and y >= 44:
        r, g, b = g, b, r
    return (r, g, b)


FIXTURES = {
    "solid_red_64.png": (64, 64, RGB, lambda x, y: RED),
    "solid_red_64_copy.png": (64, 64, RGB, lambda x, y: RED),
    "red_with_dot_64.png": (64, 64, RGB, red_with_dot),
    "solid_red_32.png": (32, 32, RGB, lambda x, y: RED),
    # RGB image filled with mid gray: same size/channels as solid_red_64, large difference.
    "gray_64.png": (64, 64, RGB, lambda x, y: (128, 128, 128)),
    # 1-channel (grayscale) PNG: same size as solid_red_64, different channel count.
    "gray_64_l8.png": (64, 64, GRAY, lambda x, y: (128,)),
    # RGBA pair that differs only in alpha (plan F2 Task 1: alpha is compared by default).
    "alpha_opaque_64.png": (64, 64, RGBA, lambda x, y: (200, 100, 50, 255)),
    "alpha_half_64.png": (64, 64, RGBA, lambda x, y: (200, 100, 50, 128)),
    # FLIP cross-check against NVIDIA's reference implementation (flip_upstream/expected.json).
    "flip_upstream/a.png": (96, 64, RGB, flip_a),
    "flip_upstream/b.png": (96, 64, RGB, flip_b),
}


def main() -> None:
    for name, (w, h, ct, fn) in FIXTURES.items():
        (HERE / name).parent.mkdir(parents=True, exist_ok=True)
        write_png(HERE / name, w, h, ct, fn)
        print(f"wrote {name} ({w}x{h}, {CHANNELS[ct]} ch)")


if __name__ == "__main__":
    main()
