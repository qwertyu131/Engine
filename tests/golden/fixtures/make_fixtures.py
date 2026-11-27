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
CHANNELS = {GRAY: 1, RGB: 3}


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


FIXTURES = {
    "solid_red_64.png": (64, 64, RGB, lambda x, y: RED),
    "solid_red_64_copy.png": (64, 64, RGB, lambda x, y: RED),
    "red_with_dot_64.png": (64, 64, RGB, red_with_dot),
    "solid_red_32.png": (32, 32, RGB, lambda x, y: RED),
    # RGB image filled with mid gray: same size/channels as solid_red_64, large difference.
    "gray_64.png": (64, 64, RGB, lambda x, y: (128, 128, 128)),
    # 1-channel (grayscale) PNG: same size as solid_red_64, different channel count.
    "gray_64_l8.png": (64, 64, GRAY, lambda x, y: (128,)),
}


def main() -> None:
    for name, (w, h, ct, fn) in FIXTURES.items():
        write_png(HERE / name, w, h, ct, fn)
        print(f"wrote {name} ({w}x{h}, {CHANNELS[ct]} ch)")


if __name__ == "__main__":
    main()
