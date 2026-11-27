"""ctest driver of apps/viewer (plan F2 Task 6). Runs the viewer once or twice and checks the result.

Usage:
    python tests/viewer/run_viewer.py --viewer <exe> --backend vulkan|d3d11 --check frames|resize|ui --work <dir>

Checks:
    frames  `--frames 120 --size 800x600 --screenshot <work>/frames.png` exits 0 within the timeout and the
            screenshot's cube centre (400, 300) is not the background (51, 51, 51).
    resize  `--script resize --frames 60` exits 0 (validation errors would be exit 4); the log shows every
            script step with the client size the window really got, including 1x1 and minimised (0x0).
    ui      two runs with r.Viewer.ShowUI=0 / 1: the probe (200, 18) on the first panel's title bar (fixed
            position, where the cube never is) is the background with the UI off and differs from it with
            the UI on.
    click   UI input end to end: while the viewer runs, Win32 mouse messages are posted to its window (class
            eng.Window) at the r.Viewer.ShowUI checkbox of the CVar panel; the click goes Win32 message ->
            platform InputEvent -> ImGui hit test -> CVar set (Console) -> next frames without panels, so the
            final screenshot's probe (200, 18) shows the background like a ShowUI=0 run. The messages are
            posted after the viewer logs its first presented frame (the panels exist then). The checkbox position
            is the layout at 100 % display scaling (the panels are at fixed pixel positions).
A viewer that exits 3 (backend not available on this machine) gives exit 77 = ctest Skipped.

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
SKIP = 77
CHECKBOX = (27, 356)  # r.Viewer.ShowUI checkbox centre, client pixels (CVar panel at (10, 320))
BACKGROUND = (51, 51, 51)
UI_PROBE = (200, 18)  # title bar of the Device panel (near black; the background is (51, 51, 51))


def _decode():
    spec = importlib.util.spec_from_file_location("run_golden", REPO / "scripts" / "run_golden.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.decode_png


decode_png = _decode()


def post_click(x: int, y: int) -> bool:
    """Finds the viewer window and posts move / down / up messages at client (x, y). Windows only."""
    import ctypes
    import time
    user32 = ctypes.windll.user32
    user32.FindWindowW.restype = ctypes.c_void_p
    user32.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
    hwnd = None
    for _ in range(100):  # up to ~10 s for the window to appear
        hwnd = user32.FindWindowW("eng.Window", None)
        if hwnd:
            break
        time.sleep(0.1)
    if not hwnd:
        print("click: viewer window not found")
        return False
    time.sleep(0.3)  # a few more frames with the panels up
    lp = (y << 16) | (x & 0xFFFF)
    WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, MK_LBUTTON = 0x0200, 0x0201, 0x0202, 0x0001
    for msg, wp in ((WM_MOUSEMOVE, 0), (WM_LBUTTONDOWN, MK_LBUTTON), (WM_LBUTTONUP, 0)):
        user32.PostMessageW(hwnd, msg, wp, lp)
        time.sleep(0.1)
    print(f"click: posted at ({x}, {y})", flush=True)
    return True


def run(cmd: list[str], during=None) -> tuple[int | None, str]:
    """Runs the viewer; `during` is called once the viewer logged its first presented frame."""
    import threading
    print("run:", subprocess.list2cmdline(cmd), flush=True)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                            errors="replace")
    lines: list[str] = []
    first = threading.Event()

    def reader():
        for line in proc.stdout:
            lines.append(line)
            if "first frame presented" in line:
                first.set()
        first.set()

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    if during is not None:
        if not first.wait(TIMEOUT_S) or proc.poll() is not None or not during():
            print("FAIL: could not drive the running viewer")
    try:
        proc.wait(timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        print(f"HANG: viewer did not exit within {TIMEOUT_S} s (not killed): {subprocess.list2cmdline(cmd)}")
        return None, ""
    t.join(5)
    out = "".join(lines)
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
    ap.add_argument("--check", required=True, choices=["frames", "resize", "ui", "click"])
    ap.add_argument("--work", required=True)
    a = ap.parse_args(argv)
    work = Path(a.work)
    work.mkdir(parents=True, exist_ok=True)
    base = [a.viewer, "--backend", a.backend]

    if a.check == "frames":
        shot = work / "frames.png"
        shot.unlink(missing_ok=True)
        rc, _ = run(base + ["--frames", "120", "--size", "800x600", "--screenshot", str(shot)])
        if rc == 3:
            print("SKIP: backend not available on this machine (viewer exit 3)")
            return SKIP
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
        if rc == 3:
            print("SKIP: backend not available on this machine (viewer exit 3)")
            return SKIP
        if rc != 0:
            print(f"FAIL: exit {rc}")
            return 1
        for needle in ("script: client 800x600", "script: client 1x1", "script: minimised client 0x0",
                       "script: restored", "script: client 1280x720", "validation messages: 0"):
            if needle not in out:
                print(f"FAIL: '{needle}' not in the viewer log")
                return 1
        return 0

    if a.check == "click":
        shot = work / "click.png"
        shot.unlink(missing_ok=True)
        # vsync on: 600 frames take several seconds, the click lands in the middle of the run
        rc, out = run(base + ["--frames", "600", "--size", "800x600", "--screenshot", str(shot)],
                      during=lambda: post_click(*CHECKBOX))
        if rc == 3:
            print("SKIP: backend not available on this machine (viewer exit 3)")
            return SKIP
        if rc != 0:
            print(f"FAIL: exit {rc}")
            return 1
        got = pixel(shot, *UI_PROBE)
        print(f"after the click: probe {UI_PROBE} = {got} (background {BACKGROUND})")
        if not all(abs(got[c] - BACKGROUND[c]) <= 3 for c in range(3)):
            print("FAIL: the panels are still drawn: the click did not turn r.Viewer.ShowUI off")
            return 1
        return 0

    shots = {}
    for ui in (0, 1):
        shot = work / f"ui{ui}.png"
        shot.unlink(missing_ok=True)
        rc, _ = run(base + ["--frames", "30", "--size", "800x600", "--cvar", f"r.Viewer.ShowUI={ui}",
                            "--screenshot", str(shot)])
        if rc == 3:
            print("SKIP: backend not available on this machine (viewer exit 3)")
            return SKIP
        if rc != 0:
            print(f"FAIL: ShowUI={ui}: exit {rc}")
            return 1
        shots[ui] = pixel(shot, *UI_PROBE)
        print(f"ShowUI={ui}: probe {UI_PROBE} = {shots[ui]}")
    if not all(abs(shots[0][c] - BACKGROUND[c]) <= 3 for c in range(3)):
        print("FAIL: with the UI off the probe is not the background")
        return 1
    if all(abs(shots[1][c] - BACKGROUND[c]) <= 3 for c in range(3)):
        print("FAIL: with the UI on the probe is still the background")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
