"""Golden-image driver: headless render -> golden batch -> probes -> HTML report (plan F0 Task 5, F2 Task 5).

Usage:
    python scripts/run_golden.py --phase f02 --build-dir build/win-msvc [--config Debug] [--out-dir DIR]

Reads tests/golden/<phase>/cases.json (or, if absent, tests/golden/<phase>/manifest.json):
    {"phase": str, "cases": [ <render case> | <compare case> ... ]}

Render case (rendered by headless, compared against a reference PNG):
    "name": str,                       # plain file name, unique
    "backend": str, "scene": str,      # passed to headless --backend / --scene
    "width": int, "height": int,
    "cvars": ["name=value", ...],      # optional, headless --cvar (CommandLine source)
    "bench": int,                      # optional, headless --bench N -> frameTimeMs = frame_ms_avg
    "ref": str,                        # reference PNG, relative to the manifest directory
    "thresholdFlip": number,           # optional (golden default 0.05)
    "thresholdReason": str,            # required when thresholdFlip is given (plan F2 Task 5)
    "out": str,                        # optional: headless --out, repo-relative (Review Focus 5:
                                       #   non-ASCII / spaced paths); default <out-dir>/render/<name>.png
    "probes": [ ... ]                  # optional absolute pixel checks on the render, each
                                       #   {"x", "y", "rgba": [r,g,b,a], "tol": int}  |max channel diff| <= tol
                                       #   {"x", "y", "darkerThan": {"x", "y"}, "margin": int}
                                       #        R+G+B at (x, y) + margin <= R+G+B at darkerThan
                                       #   {"x", "y", "channelGreater": [a, b], "margin": int}
                                       #        channel a >= channel b + margin (0 R, 1 G, 2 B)
                                       #   plus free-form "name" / "kind"

Compare case (no render: one case's output against another's, e.g. Vulkan vs D3D11):
    "name": str, "test": <render case name>, "compareWith": <render case name>,
    "thresholdFlip": number, "thresholdReason": str

Writes into the output directory (default tests/golden/out/<phase>/):
    render/<name>.png     headless output (unless the case has "out")
    batch_manifest.json   manifest handed to `golden batch`
    <name>.diff.png       FLIP heatmaps (golden)
    results.json          golden results + per case "backend" / "backendPair", "scene", "probes"
    report.html           self-contained visual report (tools/golden/report.py)

Outputs of a previous run are deleted first, so a stale render can never mask a headless failure:
a case whose render fails shows up as FAIL / missing-test (and its probes fail).

Exit code: 0 all PASS; 1 at least one FAIL (golden FAIL or a failed probe); 2 usage / IO error (also when
golden, the report or the manifest cannot be produced). Stdlib only.
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
EXIT_ERROR = 2
CASE_NAME = re.compile(r"^[A-Za-z0-9_.-]+$")
FRAME_MS_AVG = re.compile(r"frame_ms_avg=([0-9.eE+-]+)")


class DriverError(Exception):
    pass


def find_exe(build_dir: Path, subdir: str, name: str, config: str) -> Path:
    """Multi-config generators (VS, Ninja Multi-Config) put binaries in <subdir>/<config>/."""
    candidates = []
    for base in (build_dir / subdir / config, build_dir / subdir):
        for exe in (name + ".exe", name):
            candidates.append(base / exe)
    for c in candidates:
        if c.is_file():
            return c
    raise DriverError(f"cannot find '{name}' under {build_dir / subdir} (config {config}); build first")


def manifest_path(golden_dir: Path, phase: str) -> Path:
    """tests/golden/<phase>/cases.json, else manifest.json (phase f00)."""
    d = golden_dir / phase
    return d / "cases.json" if (d / "cases.json").is_file() else d / "manifest.json"


def _int(v) -> bool:
    return isinstance(v, int) and not isinstance(v, bool)


def _check_probe(name: str, i: int, p, w: int, h: int) -> None:
    where = f"case '{name}': probe {i}"
    if not isinstance(p, dict):
        raise DriverError(f"{where} must be an object")
    for key in ("x", "y"):
        if not _int(p.get(key)):
            raise DriverError(f"{where}: '{key}' must be an integer")
    if not (0 <= p["x"] < w and 0 <= p["y"] < h):
        raise DriverError(f"{where}: ({p['x']}, {p['y']}) is outside the {w}x{h} image")
    if "darkerThan" in p:
        o = p["darkerThan"]
        if not isinstance(o, dict) or not _int(o.get("x")) or not _int(o.get("y")) or \
                not (0 <= o["x"] < w and 0 <= o["y"] < h):
            raise DriverError(f"{where}: 'darkerThan' must be {{x, y}} inside the image")
        if not _int(p.get("margin")) or p["margin"] < 0:
            raise DriverError(f"{where}: 'margin' must be an integer >= 0")
        return
    if "channelGreater" in p:
        cg = p["channelGreater"]
        if not isinstance(cg, list) or len(cg) != 2 or not all(_int(v) and 0 <= v <= 3 for v in cg) or cg[0] == cg[1]:
            raise DriverError(f"{where}: 'channelGreater' must be two different channel indices in [0, 3]")
        if not _int(p.get("margin")) or p["margin"] < 0:
            raise DriverError(f"{where}: 'margin' must be an integer >= 0")
        return
    rgba = p.get("rgba")
    if not isinstance(rgba, list) or len(rgba) != 4 or not all(_int(v) and 0 <= v <= 255 for v in rgba):
        raise DriverError(f"{where}: 'rgba' must be 4 integers in [0, 255]")
    if not _int(p.get("tol")) or p["tol"] < 0:
        raise DriverError(f"{where}: 'tol' must be an integer >= 0")


def load_manifest(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise DriverError(f"cannot read manifest '{path}': {e}") from e
    if not isinstance(data, dict) or not isinstance(data.get("phase"), str) or not isinstance(data.get("cases"), list):
        raise DriverError("manifest must be an object with string 'phase' and array 'cases'")
    seen = set()
    renders = set()
    compares = []
    for c in data["cases"]:
        if not isinstance(c, dict):
            raise DriverError("each case must be an object")
        name = c.get("name")
        if not isinstance(name, str) or not CASE_NAME.match(name) or name in (".", ".."):
            raise DriverError(f"invalid case name {name!r}")
        if name.lower() in seen:
            raise DriverError(f"duplicate case name '{name}'")
        seen.add(name.lower())
        thr = c.get("thresholdFlip")
        if thr is not None:
            if not isinstance(thr, (int, float)) or isinstance(thr, bool) or thr < 0:
                raise DriverError(f"case '{name}': 'thresholdFlip' must be a number >= 0")
            if "thresholdReason" in c and (not isinstance(c["thresholdReason"], str) or not c["thresholdReason"].strip()):
                raise DriverError(f"case '{name}': 'thresholdReason' must be a non-empty string")
        if "compareWith" in c:
            for key in ("test", "compareWith"):
                if not isinstance(c.get(key), str):
                    raise DriverError(f"case '{name}': '{key}' must be a case name")
            if thr is None or "thresholdReason" not in c:
                raise DriverError(f"case '{name}': a compare case needs 'thresholdFlip' and 'thresholdReason'")
            compares.append(c)
            continue
        for key in ("backend", "scene", "ref"):
            if not isinstance(c.get(key), str):
                raise DriverError(f"case '{name}': '{key}' must be a string")
        for key in ("width", "height"):
            if not _int(c.get(key)) or c[key] < 1:
                raise DriverError(f"case '{name}': '{key}' must be a positive integer")
        cvars = c.get("cvars", [])
        if not isinstance(cvars, list) or not all(isinstance(v, str) for v in cvars):
            raise DriverError(f"case '{name}': 'cvars' must be a list of 'name=value' strings")
        bench = c.get("bench")
        if bench is not None and (not _int(bench) or bench < 1):
            raise DriverError(f"case '{name}': 'bench' must be a positive integer")
        if "out" in c and (not isinstance(c["out"], str) or not c["out"].lower().endswith(".png")):
            raise DriverError(f"case '{name}': 'out' must be a repo-relative .png path")
        if "probes" in c:
            if not isinstance(c["probes"], list):
                raise DriverError(f"case '{name}': 'probes' must be a list")
            for i, p in enumerate(c["probes"]):
                _check_probe(name, i, p, c["width"], c["height"])
        if thr is not None and "thresholdReason" not in c:
            raise DriverError(f"case '{name}': 'thresholdFlip' needs a 'thresholdReason' (plan F2 Task 5)")
        renders.add(name)
    for c in compares:
        for key in ("test", "compareWith"):
            if c[key] not in renders:
                raise DriverError(f"case '{c['name']}': {key} '{c[key]}' is not a render case")
    return data


def case_output(case: dict, out_dir: Path) -> Path:
    """Where headless writes a render case's PNG."""
    if "out" in case:
        return (REPO / case["out"]).resolve()
    return out_dir / "render" / f"{case['name']}.png"


def clean_outputs(out_dir: Path, cases: list[dict]) -> None:
    """Removes only files this driver / golden / report produce (never the whole directory)."""
    shutil.rmtree(out_dir / "render", ignore_errors=True)
    for name in ("results.json", "report.html", "batch_manifest.json"):
        (out_dir / name).unlink(missing_ok=True)
    for c in cases:
        (out_dir / f"{c['name']}.diff.png").unlink(missing_ok=True)
        if "out" in c:
            case_output(c, out_dir).unlink(missing_ok=True)


# --- PNG decoding (probes) -----------------------------------------------------------------------

def decode_png(data: bytes) -> tuple[int, int, bytes]:
    """(width, height, RGBA8 pixels) of an 8-bit RGBA or RGB, non-interlaced PNG (any row filter)."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, w, h, ch = 8, b"", 0, 0, 0
    while pos + 8 <= len(data):
        (n,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body[:13])
            if depth != 8 or interlace != 0 or ctype not in (2, 6):
                raise ValueError("only 8-bit RGB / RGBA non-interlaced PNGs are supported")
            ch = 4 if ctype == 6 else 3
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * ch
    out = bytearray()
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - ch] if i >= ch else 0
            b = prev[i]
            c = prev[i - ch] if i >= ch else 0
            if f == 0:
                pred = 0
            elif f == 1:
                pred = a
            elif f == 2:
                pred = b
            elif f == 3:
                pred = (a + b) // 2
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
            else:
                raise ValueError(f"invalid PNG row filter {f}")
            line[i] = (line[i] + pred) & 0xFF
        if ch == 4:
            out += line
        else:
            for x in range(w):
                out += line[x * 3:x * 3 + 3] + b"\xff"
        prev = line
    return w, h, bytes(out)


def check_probes(w: int, h: int, px: bytes, probes: list[dict]) -> list[dict]:
    """Evaluates probes on an RGBA8 image; returns each probe with 'got' (+ 'gotOther') and 'pass'."""
    def at(x: int, y: int):
        if not (0 <= x < w and 0 <= y < h) or len(px) < w * h * 4:
            return None
        i = (y * w + x) * 4
        return list(px[i:i + 4])

    out = []
    for p in probes:
        r = dict(p)
        got = at(p["x"], p["y"])
        r["got"] = got
        if "darkerThan" in p:
            other = at(p["darkerThan"]["x"], p["darkerThan"]["y"])
            r["gotOther"] = other
            r["pass"] = got is not None and other is not None and sum(got[:3]) + p["margin"] <= sum(other[:3])
        elif "channelGreater" in p:
            a, b = p["channelGreater"]
            r["pass"] = got is not None and got[a] >= got[b] + p["margin"]
        else:
            r["pass"] = got is not None and all(abs(got[c] - p["rgba"][c]) <= p["tol"] for c in range(4))
        out.append(r)
    return out


def merge_case_info(results: dict, extra: dict[str, dict]) -> int:
    """Adds `extra[name]` (backend / backendPair / scene / probes) to results.json cases; a case with a
    failed probe becomes FAIL / probe-failed. Returns the number of FAIL cases afterwards."""
    for case in results.get("cases", []):
        info = extra.get(case.get("name"), {})
        case.update(info)
        failed = [p for p in info.get("probes", []) if not p.get("pass")]
        if failed:
            msg = "; ".join(f"probe ({p['x']}, {p['y']}) {p.get('name', '')}: got {p.get('got')}".rstrip()
                            for p in failed)
            if case.get("status") == "PASS":
                case["status"] = "FAIL"
                case["reason"] = "probe-failed"
            case["detail"] = (case.get("detail") + "; " if case.get("detail") else "") + f"{len(failed)} probe(s) failed: {msg}"
    return sum(1 for c in results.get("cases", []) if c.get("status") != "PASS")


def render_case(headless: Path, case: dict, png: Path) -> float | None:
    """Runs headless for one case. Returns frame_ms_avg (when benchmarked), None otherwise."""
    cmd = [str(headless), "--backend", case["backend"], "--scene", case["scene"],
           "--width", str(case["width"]), "--height", str(case["height"]), "--out", str(png)]
    for cv in case.get("cvars", []):
        cmd += ["--cvar", cv]
    if case.get("bench") is not None:
        cmd += ["--bench", str(case["bench"])]
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    if proc.returncode != 0:
        print(f"run_golden: case '{case['name']}': headless exited {proc.returncode} "
              f"(reported as missing-test)", file=sys.stderr)
        return None
    m = FRAME_MS_AVG.search(proc.stdout)
    return float(m.group(1)) if m else None


def read_rgba(png: Path) -> tuple[int, int, bytes]:
    try:
        return decode_png(png.read_bytes())
    except (OSError, ValueError, zlib.error, struct.error):
        return 0, 0, b""


def run(args: argparse.Namespace) -> int:
    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = Path.cwd() / build_dir
    mpath = manifest_path(REPO / "tests" / "golden", args.phase)
    out_dir = Path(args.out_dir) if args.out_dir else REPO / "tests" / "golden" / "out" / args.phase
    out_dir = out_dir.resolve()

    manifest = load_manifest(mpath)
    cases = manifest["cases"]
    renders = {c["name"]: c for c in cases if "compareWith" not in c}
    headless = find_exe(build_dir, "apps/headless", "headless", args.config)
    golden = find_exe(build_dir, "tools/golden", "golden", args.config)

    out_dir.mkdir(parents=True, exist_ok=True)
    clean_outputs(out_dir, cases)
    (out_dir / "render").mkdir()

    frame_ms: dict[str, float | None] = {}
    extra: dict[str, dict] = {}
    for case in renders.values():
        png = case_output(case, out_dir)
        png.parent.mkdir(parents=True, exist_ok=True)
        frame_ms[case["name"]] = render_case(headless, case, png)
        info: dict = {"backend": case["backend"], "scene": case["scene"]}
        if case.get("probes"):
            info["probes"] = check_probes(*read_rgba(png), case["probes"])
        extra[case["name"]] = info

    batch_cases = []
    for case in cases:
        if "compareWith" in case:
            a, b = renders[case["test"]], renders[case["compareWith"]]
            bc = {"name": case["name"], "test": str(case_output(a, out_dir)), "ref": str(case_output(b, out_dir)),
                  "frameTimeMs": None}
            extra[case["name"]] = {"backendPair": f"{a['backend']} vs {b['backend']}", "scene": a["scene"],
                                   "compare": f"{a['name']} vs {b['name']}"}
        else:
            png = case_output(case, out_dir)
            test = f"render/{case['name']}.png" if "out" not in case else str(png)
            bc = {"name": case["name"], "test": test, "ref": str((mpath.parent / case["ref"]).resolve()),
                  "frameTimeMs": frame_ms[case["name"]]}
        if case.get("thresholdFlip") is not None:
            bc["thresholdFlip"] = case["thresholdFlip"]
        if case.get("thresholdReason"):
            extra.setdefault(case["name"], {})["thresholdReason"] = case["thresholdReason"]
        batch_cases.append(bc)

    batch_manifest = out_dir / "batch_manifest.json"
    batch_manifest.write_text(json.dumps({"phase": manifest["phase"], "cases": batch_cases}, indent=2,
                                         ensure_ascii=False) + "\n", encoding="utf-8")

    proc = subprocess.run([str(golden), "batch", "--manifest", str(batch_manifest), "--out", str(out_dir)],
                          capture_output=True, text=True, encoding="utf-8", errors="replace")
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    golden_rc = proc.returncode
    results = out_dir / "results.json"
    if golden_rc not in (0, 1) or not results.is_file():
        print(f"run_golden: golden batch failed (exit {golden_rc})", file=sys.stderr)
        return EXIT_ERROR

    try:
        res = json.loads(results.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        print(f"run_golden: cannot read {results}: {e}", file=sys.stderr)
        return EXIT_ERROR
    failed = merge_case_info(res, extra)
    results.write_text(json.dumps(res, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    for c in res.get("cases", []):
        for p in c.get("probes", []):
            if not p.get("pass"):
                print(f"run_golden: case '{c['name']}': probe ({p['x']}, {p['y']}) {p.get('name', '')} FAILED: "
                      f"got {p.get('got')}" + (f" vs {p.get('gotOther')}" if "gotOther" in p else
                                               f", channelGreater {p['channelGreater']} margin {p['margin']}"
                                               if "channelGreater" in p else
                                               f", expected {p.get('rgba')} +-{p.get('tol')}"), file=sys.stderr)
    rc = 1 if failed else 0
    if golden_rc == 1:
        rc = 1

    report = out_dir / "report.html"
    rep = subprocess.run([sys.executable, str(REPO / "tools" / "golden" / "report.py"),
                          "--results", str(results), "--out", str(report)],
                         capture_output=True, text=True, encoding="utf-8", errors="replace")
    sys.stdout.write(rep.stdout)
    sys.stderr.write(rep.stderr)
    if rep.returncode != 0 or not report.is_file():
        print(f"run_golden: report generation failed (exit {rep.returncode})", file=sys.stderr)
        return EXIT_ERROR

    print(f"run_golden: phase {manifest['phase']}: {'PASS' if rc == 0 else 'FAIL'}; report: {report}")
    return rc


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--phase", required=True, help="phase id, e.g. f02 (reads tests/golden/<phase>/cases.json)")
    ap.add_argument("--build-dir", required=True, help="CMake binary dir, e.g. build/win-msvc")
    ap.add_argument("--config", default="Debug", help="build configuration (default Debug)")
    ap.add_argument("--out-dir", help="output directory (default tests/golden/out/<phase>)")
    try:
        args = ap.parse_args(argv)
    except SystemExit as e:
        return EXIT_ERROR if e.code else 0
    if not CASE_NAME.match(args.phase):
        print(f"run_golden: invalid phase '{args.phase}'", file=sys.stderr)
        return EXIT_ERROR
    try:
        return run(args)
    except DriverError as e:
        print(f"run_golden: {e}", file=sys.stderr)
        return EXIT_ERROR
    except OSError as e:
        print(f"run_golden: IO error: {e}", file=sys.stderr)
        return EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main())
