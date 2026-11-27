"""Golden-image driver: headless render -> golden batch -> HTML report (plan F0 Task 5).

Usage:
    python scripts/run_golden.py --phase f00 --build-dir build/win-msvc [--config Debug] [--out-dir DIR]

Reads tests/golden/<phase>/manifest.json:
    {"phase": "f00", "cases": [{
        "name": str,                       # plain file name, unique
        "backend": str, "scene": str,      # passed to headless --backend / --scene
        "width": int, "height": int,
        "cvars": ["name=value", ...],      # optional, headless --cvar (CommandLine source)
        "bench": int,                      # optional, headless --bench N -> frameTimeMs = frame_ms_avg
        "ref": str,                        # reference PNG, relative to the manifest directory
        "thresholdFlip": number}]}         # optional (golden default 0.05)

Writes into the output directory (default tests/golden/out/<phase>/):
    render/<name>.png     headless output
    batch_manifest.json   manifest handed to `golden batch`
    <name>.diff.png       FLIP heatmaps (golden)
    results.json          golden results
    report.html           self-contained visual report (tools/golden/report.py)

Outputs of a previous run are deleted first, so a stale render can never mask a headless failure:
a case whose render fails shows up as FAIL / missing-test.

Exit code = golden's (0 all PASS, 1 at least one FAIL, 2 usage / IO error); 2 if the report or
the manifest cannot be produced. Stdlib only.
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
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


def load_manifest(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise DriverError(f"cannot read manifest '{path}': {e}") from e
    if not isinstance(data, dict) or not isinstance(data.get("phase"), str) or not isinstance(data.get("cases"), list):
        raise DriverError("manifest must be an object with string 'phase' and array 'cases'")
    seen = set()
    for c in data["cases"]:
        if not isinstance(c, dict):
            raise DriverError("each case must be an object")
        name = c.get("name")
        if not isinstance(name, str) or not CASE_NAME.match(name) or name in (".", ".."):
            raise DriverError(f"invalid case name {name!r}")
        if name.lower() in seen:
            raise DriverError(f"duplicate case name '{name}'")
        seen.add(name.lower())
        for key in ("backend", "scene", "ref"):
            if not isinstance(c.get(key), str):
                raise DriverError(f"case '{name}': '{key}' must be a string")
        for key in ("width", "height"):
            if not isinstance(c.get(key), int) or isinstance(c.get(key), bool) or c[key] < 1:
                raise DriverError(f"case '{name}': '{key}' must be a positive integer")
        cvars = c.get("cvars", [])
        if not isinstance(cvars, list) or not all(isinstance(v, str) for v in cvars):
            raise DriverError(f"case '{name}': 'cvars' must be a list of 'name=value' strings")
        bench = c.get("bench")
        if bench is not None and (not isinstance(bench, int) or isinstance(bench, bool) or bench < 1):
            raise DriverError(f"case '{name}': 'bench' must be a positive integer")
        thr = c.get("thresholdFlip")
        if thr is not None and (not isinstance(thr, (int, float)) or isinstance(thr, bool) or thr < 0):
            raise DriverError(f"case '{name}': 'thresholdFlip' must be a number >= 0")
    return data


def clean_outputs(out_dir: Path, cases: list[dict]) -> None:
    """Removes only files this driver / golden / report produce (never the whole directory)."""
    shutil.rmtree(out_dir / "render", ignore_errors=True)
    for name in ("results.json", "report.html", "batch_manifest.json"):
        (out_dir / name).unlink(missing_ok=True)
    for c in cases:
        (out_dir / f"{c['name']}.diff.png").unlink(missing_ok=True)


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


def run(args: argparse.Namespace) -> int:
    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = Path.cwd() / build_dir
    manifest_path = REPO / "tests" / "golden" / args.phase / "manifest.json"
    out_dir = Path(args.out_dir) if args.out_dir else REPO / "tests" / "golden" / "out" / args.phase
    out_dir = out_dir.resolve()

    manifest = load_manifest(manifest_path)
    headless = find_exe(build_dir, "apps/headless", "headless", args.config)
    golden = find_exe(build_dir, "tools/golden", "golden", args.config)

    out_dir.mkdir(parents=True, exist_ok=True)
    clean_outputs(out_dir, manifest["cases"])
    (out_dir / "render").mkdir()

    batch_cases = []
    for case in manifest["cases"]:
        png = out_dir / "render" / f"{case['name']}.png"
        frame_ms = render_case(headless, case, png)
        bc = {"name": case["name"], "test": f"render/{case['name']}.png",
              "ref": str((manifest_path.parent / case["ref"]).resolve()), "frameTimeMs": frame_ms}
        if case.get("thresholdFlip") is not None:
            bc["thresholdFlip"] = case["thresholdFlip"]
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

    report = out_dir / "report.html"
    rep = subprocess.run([sys.executable, str(REPO / "tools" / "golden" / "report.py"),
                          "--results", str(results), "--out", str(report)],
                         capture_output=True, text=True, encoding="utf-8", errors="replace")
    sys.stdout.write(rep.stdout)
    sys.stderr.write(rep.stderr)
    if rep.returncode != 0 or not report.is_file():
        print(f"run_golden: report generation failed (exit {rep.returncode})", file=sys.stderr)
        return EXIT_ERROR

    print(f"run_golden: phase {manifest['phase']}: {'PASS' if golden_rc == 0 else 'FAIL'}; report: {report}")
    return golden_rc


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--phase", required=True, help="phase id, e.g. f00 (reads tests/golden/<phase>/manifest.json)")
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
