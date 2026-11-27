"""Tests for scripts/msl_artifacts.py and scripts/check_metal.py (plan F1 Task 5; processes faked).

Run: python -m unittest discover tests
The real freshness check is ctest `shaderc.msl_artifacts_fresh` (needs the built shaderc); the real
`xcrun metal` run is the macOS CI job `macos-metal`.
"""
from __future__ import annotations

import importlib.util
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, REPO / "scripts" / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


ma = _load("msl_artifacts")
cm = _load("check_metal")

EXPECTED_NAMES = sorted(
    f"pbr.T2_SKIN{s}_MORPH{m}_UNLIT{u}.{e}.metal"
    for s in (0, 1) for m in (0, 1) for u in (0, 1) for e in ("VSMain", "PSMain"))


class FakeShaderc:
    """Writes '<key> <entry>' as the MSL text and prints shaderc's PENDING-CI line."""

    def __init__(self) -> None:
        self.calls: list[list[str]] = []

    def __call__(self, cmd: list[str]) -> tuple[int, str, str]:
        self.calls.append(cmd)
        arg = {cmd[i]: cmd[i + 1] for i in range(len(cmd) - 1) if cmd[i].startswith("--")}
        out = Path(arg["--out"])
        out.mkdir(parents=True, exist_ok=True)
        f = out / f"pbr.{arg['--entry']}.{arg['--target']}.metal"
        f.write_text(f"// {out.name} {arg['--entry']}\n", encoding="utf-8")
        return 0, f"shaderc: msl {arg['--entry']} -> {f} [PENDING-CI]\n", ""


class MslArtifactsTest(unittest.TestCase):
    def run_main(self, argv: list[str], runner) -> tuple[int, str]:
        err = io.StringIO()
        with redirect_stdout(io.StringIO()), redirect_stderr(err):
            rc = ma.main(argv, runner=runner)
        return rc, err.getvalue()

    def test_writes_the_16_msl_permutations_then_check_passes(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            fake_exe = Path(d) / "shaderc.exe"
            fake_exe.write_bytes(b"")
            dest = Path(d) / "msl"
            fake = FakeShaderc()
            rc, _ = self.run_main(["--shaderc", str(fake_exe), "--dest", str(dest)], fake)
            self.assertEqual(rc, 0)
            self.assertEqual(sorted(p.name for p in dest.glob("*.metal")), EXPECTED_NAMES)
            self.assertTrue(all(c[c.index("--target") + 1] == "msl" for c in fake.calls))
            self.assertEqual(len(fake.calls), 16)
            rc, err = self.run_main(["--shaderc", str(fake_exe), "--dest", str(dest), "--check"], FakeShaderc())
            self.assertEqual(rc, 0, err)

    def test_check_reports_different_missing_and_stale_files(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            fake_exe = Path(d) / "shaderc.exe"
            fake_exe.write_bytes(b"")
            dest = Path(d) / "msl"
            self.run_main(["--shaderc", str(fake_exe), "--dest", str(dest)], FakeShaderc())
            (dest / EXPECTED_NAMES[0]).write_text("// edited\n", encoding="utf-8")
            (dest / EXPECTED_NAMES[1]).unlink()
            (dest / "pbr.T9_OLD.PSMain.metal").write_text("// stale\n", encoding="utf-8")
            rc, err = self.run_main(["--shaderc", str(fake_exe), "--dest", str(dest), "--check"], FakeShaderc())
            self.assertEqual(rc, 1)
            self.assertIn(f"differs from fresh shaderc output: {EXPECTED_NAMES[0]}", err)
            self.assertIn(f"missing in {dest}: {EXPECTED_NAMES[1]}", err)
            self.assertIn("not produced by shaderc (stale): pbr.T9_OLD.PSMain.metal", err)

    def test_shaderc_missing_is_exit_2(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            rc, err = self.run_main(["--shaderc", str(Path(d) / "nope.exe"), "--check"], FakeShaderc())
        self.assertEqual(rc, 2)
        self.assertIn("tool not found: shaderc", err)

    def test_committed_artifacts_are_the_16_permutations(self) -> None:
        self.assertEqual(sorted(p.name for p in ma.DEFAULT_DEST.glob("*.metal")), EXPECTED_NAMES)


class FakeXcrun:
    def __init__(self, fail: set[tuple[str, str]] | None = None, missing: bool = False) -> None:
        self.fail = fail or set()  # (file name, sdk)
        self.missing = missing
        self.calls: list[list[str]] = []

    def __call__(self, cmd: list[str]) -> tuple[int, str]:
        if self.missing:
            raise FileNotFoundError(cmd[0])
        self.calls.append(cmd)
        sdk, src = cmd[cmd.index("-sdk") + 1], Path(cmd[cmd.index("-c") + 1])
        if (src.name, sdk) in self.fail:
            return 1, f"{src}:3:1: error: unknown type name 'bogus'\n"
        Path(cmd[cmd.index("-o") + 1]).write_bytes(b"air")
        return 0, ""


class CheckMetalTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name) / "msl"
        self.dir.mkdir()
        for n in ("a.metal", "b.metal"):
            (self.dir / n).write_text("// msl\n", encoding="utf-8")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_main(self, argv: list[str], runner) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = cm.main(argv, runner=runner)
        return rc, out.getvalue(), err.getvalue()

    def test_every_file_compiles_for_macosx_and_iphoneos(self) -> None:
        fake = FakeXcrun()
        results = Path(self.tmp.name) / "results.json"
        rc, out, _ = self.run_main(["--dir", str(self.dir), "--out", str(results)], fake)
        self.assertEqual(rc, 0)
        self.assertEqual(len(fake.calls), 4)
        sdks = sorted((Path(c[c.index("-c") + 1]).name, c[c.index("-sdk") + 1]) for c in fake.calls)
        self.assertEqual(sdks, [("a.metal", "iphoneos"), ("a.metal", "macosx"),
                                ("b.metal", "iphoneos"), ("b.metal", "macosx")])
        for c in fake.calls:
            self.assertEqual(c[0], "xcrun")
            self.assertEqual(c[c.index("-sdk") + 2], "metal")
            self.assertIn("-std=metal3.0", c)
        summary = json.loads(results.read_text(encoding="utf-8"))
        self.assertEqual((summary["passed"], summary["failed"]), (4, 0))
        self.assertIn("PASS macosx a.metal", out)

    def test_compile_error_is_exit_1_with_the_metal_output(self) -> None:
        rc, _, err = self.run_main(["--dir", str(self.dir)], FakeXcrun(fail={("b.metal", "iphoneos")}))
        self.assertEqual(rc, 1)
        self.assertIn("FAIL iphoneos b.metal", err)
        self.assertIn("unknown type name 'bogus'", err)

    def test_no_metal_files_is_exit_2(self) -> None:
        empty = Path(self.tmp.name) / "empty"
        empty.mkdir()
        rc, _, err = self.run_main(["--dir", str(empty)], FakeXcrun())
        self.assertEqual(rc, 2)
        self.assertIn("no .metal files", err)

    def test_xcrun_missing_is_exit_2(self) -> None:
        rc, _, err = self.run_main(["--dir", str(self.dir)], FakeXcrun(missing=True))
        self.assertEqual(rc, 2)
        self.assertIn("tool not found: xcrun", err)


if __name__ == "__main__":
    unittest.main()
