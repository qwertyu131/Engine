"""Validate the committed MSL with Apple's Metal compiler (plan F1 Task 5, spec K2). macOS only.

Usage:
    python3 scripts/check_metal.py [--dir tests/shaders/msl] [--sdk macosx --sdk iphoneos]
                                   [--std metal3.0] [--xcrun xcrun] [--out metal-results.json]

For every <dir>/*.metal and every SDK runs
    xcrun -sdk <sdk> metal -std=<std> -c <file> -o <tmp>/<file>.<sdk>.air
prints "PASS <sdk> <file>" / "FAIL <sdk> <file>" (+ the compiler output), and writes --out as
    {"std", "sdks", "passed", "failed", "results": [{"file", "sdk", "status", "output"}]}.
shaderc generates MSL 3.0 for the macOS platform (tools/shaderc/Compiler.cpp); the iphoneos compile of
the same text checks the language subset only, not iOS-specific codegen choices.

Exit codes: 0 all passed, 1 any compile failure, 2 usage error / no .metal files / xcrun not found.
Stdlib only.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable

REPO = Path(__file__).resolve().parents[1]
DEFAULT_DIR = REPO / "tests" / "shaders" / "msl"
DEFAULT_SDKS = ["macosx", "iphoneos"]

Runner = Callable[[list[str]], "tuple[int, str]"]


def run_process(cmd: list[str]) -> tuple[int, str]:
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def metal_command(xcrun: str, sdk: str, std: str, src: Path, air: Path) -> list[str]:
    return [xcrun, "-sdk", sdk, "metal", f"-std={std}", "-c", str(src), "-o", str(air)]


def main(argv: list[str] | None = None, runner: Runner = run_process) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dir", type=Path, default=DEFAULT_DIR)
    ap.add_argument("--sdk", action="append", help="repeatable (default: macosx and iphoneos)")
    ap.add_argument("--std", default="metal3.0")
    ap.add_argument("--xcrun", default="xcrun")
    ap.add_argument("--out", type=Path, help="results JSON")
    try:
        args = ap.parse_args(argv)
    except SystemExit as e:
        return 2 if e.code else 0
    sdks = args.sdk or DEFAULT_SDKS
    files = sorted(args.dir.glob("*.metal")) if args.dir.is_dir() else []
    if not files:
        print(f"check_metal: no .metal files in {args.dir}", file=sys.stderr)
        return 2

    results = []
    with tempfile.TemporaryDirectory(prefix="check-metal-") as tmp:
        for sdk in sdks:
            for src in files:
                air = Path(tmp) / f"{src.name}.{sdk}.air"
                try:
                    rc, output = runner(metal_command(args.xcrun, sdk, args.std, src, air))
                except FileNotFoundError:
                    print(f"check_metal: tool not found: {args.xcrun} (needs Xcode command line tools on macOS)",
                          file=sys.stderr)
                    return 2
                status = "passed" if rc == 0 else "failed"
                results.append({"file": src.name, "sdk": sdk, "status": status, "output": output.strip()})
                if rc == 0:
                    print(f"PASS {sdk} {src.name}")
                else:
                    print(f"FAIL {sdk} {src.name} (exit {rc}):\n{output.strip()}", file=sys.stderr)

    passed = sum(1 for r in results if r["status"] == "passed")
    failed = len(results) - passed
    if args.out:
        args.out.write_text(json.dumps({"std": args.std, "sdks": sdks, "passed": passed, "failed": failed,
                                        "results": results}, indent=2) + "\n", encoding="utf-8")
    print(f"check_metal: {passed} passed, {failed} failed ({len(files)} files x {len(sdks)} SDKs, -std={args.std})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
