"""Independent model of builtin:pbr-spheres (plan F2 Task 5) and its golden probe table.

Usage:
    python scripts/f02_pbr_model.py --probes W H     # prints the probe list (JSON) for a W x H render

The scene (engine/renderer/builtin/PbrSpheresScene.cpp, constants documented in engine/renderer/builtin/Scenes.h) is modelled
here from the same constants, NOT the same code: camera, sphere grid, the analytic environment (sky /
ground gradient + sun disk), the Khronos PBR Neutral tone mapper and the gamma 2.2 output encode. The
probe colours are computed from the analytic environment along each pixel's camera ray, never read
from a render, so a Y flip, an X mirror or a cube-face orientation error made identically on both
backends cannot cancel (the environment is asymmetric in X and Y and the sun is off every axis).

Probes (each {x, y, ...}, pixel coordinates, origin top-left):
    background   {x, y, rgba, tol}            the environment lookup along the pixel ray
    unlit        {x, y, rgba, tol}            unlit sphere centre = baseColorFactor through the output
                                              transform (factors with min channel 0 and peak < 0.76, so
                                              PBR Neutral is the identity; then gamma 2.2)
    reflection   {x, y, channelGreater: [a, b], margin}
                                              mirror metal sphere (metallic 1, roughness 0): channel a >=
                                              channel b + margin. Above its centre it reflects the sky
                                              (blue > red), below it the ground (red > blue); drawn
                                              inside-out (wrong winding) the two swap
    sun          {x, y, darkerThan: {x, y}, margin}
                                              the rough dielectric sphere's centre is darker (R+G+B lower
                                              by at least `margin`) than the smooth dielectric sphere's
                                              centre; both centres reflect into the sun disk

Stdlib only.
"""
from __future__ import annotations

import argparse
import json
import math
import sys

# --- scene constants (must equal engine/renderer/builtin/Scenes.h) ----------------------------------------
EYE = (0.0, 0.0, 12.0)            # looking down -Z, +Y up, right-handed
FOV_Y_DEG = 32.0
GRID_COLS, GRID_ROWS = 5, 5       # metallic = col / 4 (X), roughness = row / 4 (Y, row 0 at the top)
SPACING = 1.1
RADIUS = 0.45
UNLIT_ROW = 5                     # one extra row of unlit spheres below the grid
BASE_COLOR = (0.95, 0.64, 0.54, 1.0)
UNLIT_COLORS = [                  # min channel 0, peak < 0.76: Khronos PBR Neutral is the identity
    (0.70, 0.20, 0.00),
    (0.00, 0.60, 0.30),
    (0.25, 0.00, 0.70),
    (0.55, 0.50, 0.00),
    (0.00, 0.35, 0.65),
]
SUN_DIR_UNNORMALIZED = (0.18, -0.045, 1.0)
SUN_RADIUS_DEG = 18.0
SUN_RADIANCE = (6.0, 5.6, 5.0)
SKY_HORIZON = (0.85, 0.80, 0.70)
SKY_ZENITH = (0.20, 0.40, 0.85)
GROUND_NEAR = (0.30, 0.25, 0.20)
GROUND_FAR = (0.12, 0.10, 0.08)
GRADIENT_SCALE = 2.5              # t = min(1, |d.y| * 2.5)
X_TINT = 0.5                      # radiance *= 1 + 0.5 * d.x (left/right asymmetry)
EXPOSURE = 1.0


def _norm(v):
    s = math.sqrt(sum(c * c for c in v))
    return tuple(c / s for c in v)


SUN_DIR = _norm(SUN_DIR_UNNORMALIZED)


def sphere_centre(col: int, row: int) -> tuple[float, float, float]:
    return ((col - 2) * SPACING, (2.5 - row) * SPACING, 0.0)


def environment(d) -> tuple[float, float, float]:
    """Linear HDR radiance of the environment in world direction d (normalized)."""
    if sum(d[i] * SUN_DIR[i] for i in range(3)) >= math.cos(math.radians(SUN_RADIUS_DEG)):
        return SUN_RADIANCE
    t = min(1.0, abs(d[1]) * GRADIENT_SCALE)
    a, b = (SKY_HORIZON, SKY_ZENITH) if d[1] >= 0.0 else (GROUND_NEAR, GROUND_FAR)
    tint = max(0.0, 1.0 + X_TINT * d[0])
    return tuple((a[i] + (b[i] - a[i]) * t) * tint for i in range(3))


def pbr_neutral(c):
    """Khronos PBR Neutral tone mapper (reference formula, KhronosGroup/ToneMapping)."""
    start = 0.8 - 0.04
    desat = 0.15
    x = min(c)
    offset = x - 6.25 * x * x if x < 0.08 else 0.04
    c = [v - offset for v in c]
    peak = max(c)
    if peak < start:
        return tuple(c)
    d = 1.0 - start
    new_peak = 1.0 - d * d / (peak + d - start)
    c = [v * new_peak / peak for v in c]
    g = 1.0 - 1.0 / (desat * (peak - new_peak) + 1.0)
    return tuple(v * (1.0 - g) + new_peak * g for v in c)


def encode(c) -> tuple[int, int, int, int]:
    """Output transform of the scene: exposure, PBR Neutral, gamma 2.2, UNORM8 (round to nearest)."""
    t = pbr_neutral([v * EXPOSURE for v in c])
    return tuple(int(math.floor(max(0.0, min(1.0, v)) ** (1.0 / 2.2) * 255.0 + 0.5)) for v in t) + (255,)


def ray(px: float, py: float, w: int, h: int):
    """World direction (normalized) of the camera ray through continuous pixel position (px, py)."""
    f = 1.0 / math.tan(math.radians(FOV_Y_DEG) / 2.0)
    ndc_x = px / w * 2.0 - 1.0
    ndc_y = 1.0 - py / h * 2.0
    return _norm((ndc_x * (w / h) / f, ndc_y / f, -1.0))


def project(p, w: int, h: int) -> tuple[float, float]:
    f = 1.0 / math.tan(math.radians(FOV_Y_DEG) / 2.0)
    vx, vy, vz = (p[i] - EYE[i] for i in range(3))
    ndc_x = f / (w / h) * vx / -vz
    ndc_y = f * vy / -vz
    return (ndc_x + 1.0) * 0.5 * w, (1.0 - ndc_y) * 0.5 * h


def hits_sphere(px: float, py: float, w: int, h: int) -> bool:
    d = ray(px, py, w, h)
    for row in range(GRID_ROWS + 1):
        for col in range(GRID_COLS):
            c = sphere_centre(col, row)
            oc = tuple(EYE[i] - c[i] for i in range(3))
            b = sum(oc[i] * d[i] for i in range(3))
            cc = sum(v * v for v in oc) - RADIUS * RADIUS
            if b * b - cc >= 0.0:
                return True
    return False


def centre_pixel(col: int, row: int, w: int, h: int) -> tuple[int, int]:
    sx, sy = project(sphere_centre(col, row), w, h)
    return int(math.floor(sx)), int(math.floor(sy))


def centre_reflection(col: int, row: int):
    """Reflection direction at the visible centre of a sphere = direction from the centre to the eye."""
    c = sphere_centre(col, row)
    return _norm(tuple(EYE[i] - c[i] for i in range(3)))


def angle_to_sun_deg(d) -> float:
    return math.degrees(math.acos(max(-1.0, min(1.0, sum(d[i] * SUN_DIR[i] for i in range(3))))))


def sphere_hit(px: float, py: float, w: int, h: int, col: int, row: int):
    """(point, normal) of the near intersection of the pixel ray with sphere (col, row), or None."""
    d = ray(px, py, w, h)
    c = sphere_centre(col, row)
    oc = tuple(EYE[i] - c[i] for i in range(3))
    b = sum(oc[i] * d[i] for i in range(3))
    disc = b * b - (sum(v * v for v in oc) - RADIUS * RADIUS)
    if disc < 0.0:
        return None
    t = -b - math.sqrt(disc)
    p = tuple(EYE[i] + t * d[i] for i in range(3))
    return p, tuple((p[i] - c[i]) / RADIUS for i in range(3))


def mirror_reflection(px: float, py: float, w: int, h: int, col: int, row: int):
    hit = sphere_hit(px, py, w, h, col, row)
    if hit is None:
        return None
    d = ray(px, py, w, h)
    n = hit[1]
    k = 2.0 * sum(d[i] * n[i] for i in range(3))
    return tuple(d[i] - k * n[i] for i in range(3))


REFLECTION_ANGLE_DEG = 25.0  # probe points: +-25 deg above / below the mirror sphere's centre
REFLECTION_MARGINS = (20, 10)  # B - R above (sky), R - B below (ground)


BACKGROUND_TOL = 3   # bilinear lookup of a 32 x 32 cube vs the analytic function (+ filtering differences)
UNLIT_TOL = 1        # plan: the unlit row equals baseColorFactor exactly (+-1/255)
SUN_MARGIN = 10      # R+G+B difference; measured difference is several times larger


def probes(w: int, h: int) -> list[dict]:
    out = []
    m = max(4, w // 40)
    for name, (x, y) in (("background top-left", (m, m)), ("background top-right", (w - 1 - m, m)),
                         ("background bottom-left", (m, h - 1 - m)),
                         ("background bottom-right", (w - 1 - m, h - 1 - m))):
        if hits_sphere(x + 0.5, y + 0.5, w, h):
            raise ValueError(f"{name} probe ({x}, {y}) hits a sphere")
        out.append({"kind": "background", "name": name, "x": x, "y": y,
                    "rgba": list(encode(environment(ray(x + 0.5, y + 0.5, w, h)))), "tol": BACKGROUND_TOL})
    for col, colour in enumerate(UNLIT_COLORS):
        x, y = centre_pixel(col, UNLIT_ROW, w, h)
        out.append({"kind": "unlit", "name": f"unlit sphere {col} baseColorFactor {list(colour)}", "x": x, "y": y,
                    "rgba": list(encode(colour)), "tol": UNLIT_TOL})
    mcol = GRID_COLS - 1  # metallic 1, roughness 0 (row 0): a tinted mirror
    for sign, (a, b), margin, what in ((1.0, (2, 0), REFLECTION_MARGINS[0], "sky: blue > red"),
                                       (-1.0, (0, 2), REFLECTION_MARGINS[1], "ground: red > blue")):
        ang = math.radians(REFLECTION_ANGLE_DEG) * sign
        c = sphere_centre(mcol, 0)
        p = (c[0], c[1] + RADIUS * math.sin(ang), c[2] + RADIUS * math.cos(ang))
        fx, fy = project(p, w, h)
        x, y = int(math.floor(fx)), int(math.floor(fy))
        r = mirror_reflection(x + 0.5, y + 0.5, w, h, mcol, 0)
        if r is None or r[1] * sign < 0.3:
            raise ValueError(f"reflection probe ({x}, {y}) does not reflect the {what.split(':')[0]}")
        out.append({"kind": "reflection",
                    "name": f"mirror metal sphere {'above' if sign > 0 else 'below'} centre reflects the {what}",
                    "x": x, "y": y, "channelGreater": [a, b], "margin": margin})
    sx, sy = centre_pixel(0, 0, w, h)  # metallic 0, roughness 0
    rx, ry = centre_pixel(0, GRID_ROWS - 1, w, h)  # metallic 0, roughness 1
    out.append({"kind": "sun", "name": "rough dielectric centre darker than smooth dielectric centre (sun reflection)",
                "x": rx, "y": ry, "darkerThan": {"x": sx, "y": sy}, "margin": SUN_MARGIN})
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--probes", nargs=2, type=int, metavar=("W", "H"), required=True)
    args = ap.parse_args(argv)
    print(json.dumps(probes(*args.probes), indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
