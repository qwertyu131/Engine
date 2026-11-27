"""ctest driver of apps/viewer (plan F2 Task 6). Runs the viewer once or twice and checks the result.

Usage:
    python tests/viewer/run_viewer.py --viewer <exe> --backend vulkan|d3d11 --check frames|resize|ui --work <dir>

Checks:
    frames  `--frames 120 --size 800x600 --screenshot <work>/frames.png` exits 0 within the timeout and the
            screenshot's cube centre (400, 300) is not the background (51, 51, 51).
    resize  `--script resize --frames 60` exits 0 (validation errors would be exit 4); the log shows every
            script step with the client size the window really got, including 1x1 and minimised (0x0).
    ui      two runs with r.Viewer.ShowUI=0 / 1: the probe (24, 48) inside the first panel (fixed position,
            where the cube never is) differs between the screenshots.

The viewer is never killed: a run that does not exit within the timeout is reported as a HANG with its
command line, and the test fails (plan F2 Global Constraints: agents never kill a process).
Exit code 0 = pass, 1 = fail. Stdlib only.
"""
from __future__ import annotations

import argparse
import importlib.util
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TIMEOUT_S = 60
BACKGROUND = (51, 51, 51)
UI_PROBE = (24, 48)


def _decode():
    spec = importlib.util.spec_from_file_location("run_golden", REPO / "scripts" / "run_golden.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.decode_png


decode_png = _decode()


def run(cmd: list[str]) -> tuple[int | None, str]:
    print("run:", subprocess.list2cmdline(cmd), flush=True)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                            errors="replace")
    try:
        out, _ = proc.communicate(timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        print(f"HANG: viewer did not exit within {TIMEOUT_S} s (not killed): {subprocess.list2cmdline(cmd)}")
        return None, ""
    sys.stdout.write(out)
    return proc.returncode, out


def pixel(png: Path, x: int, y: int) -> tuple[int, int, int, int]:
    w, h, px = decode_png(png.read_bytes())
    if not (0 <= x < w and 0 <= y < h):
        raise ValueError(f"probe ({x}, {y}) outside {w}x{h}")
    i = (y * w + x) * 4
    return tuple(px[i:i + 4])


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--viewer", required=True)
    ap.add_argument("--backend", required=True)
    ap.add_argument("--check", required=True, choices=["frames", "resize", "ui"])
    ap.add_argument("--work", required=True)
    a = ap.parse_args(argv)
    work = Path(a.work)
    work.mkdir(parents=True, exist_ok=True)
    base = [a.viewer, "--backend", a.backend]

    if a.check == "frames":
        shot = work / "frames.png"
        shot.unlink(missing_ok=True)
        rc, _ = run(base + ["--frames", "120", "--size", "800x600", "--screenshot", str(shot)])
        if rc != 0:
            print(f"FAIL: exit {rc}")
            return 1
        got = pixel(shot, 400, 300)
        print(f"cube centre (400, 300) = {got}")
        if all(abs(got[c] - BACKGROUND[c]) <= 3 for c in range(3)):
            print("FAIL: the cube centre shows the background")
            return 1
        return 0

    if a.check == "resize":
        rc, out = run(base + ["--script", "resize", "--frames", "60"])
        if rc != 0:
            print(f"FAIL: exit {rc}")
            return 1
        for needle in ("script: client 800x600", "script: client 1x1", "script: minimised client 0x0",
                       "script: restored", "script: client 1280x720", "validation messages: 0"):
            if needle not in out:
                print(f"FAIL: '{needle}' not in the viewer log")
                return 1
        return 0

    shots = {}
    for ui in (0, 1):
        shot = work / f"ui{ui}.png"
        shot.unlink(missing_ok=True)
        rc, _ = run(base + ["--frames", "30", "--size", "800x600", "--cvar", f"r.Viewer.ShowUI={ui}",
                            "--screenshot", str(shot)])
        if rc != 0:
            print(f"FAIL: ShowUI={ui}: exit {rc}")
            return 1
        shots[ui] = pixel(shot, *UI_PROBE)
        print(f"ShowUI={ui}: probe {UI_PROBE} = {shots[ui]}")
    if shots[0] == shots[1]:
        print("FAIL: the UI probe is identical with the UI on and off")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
