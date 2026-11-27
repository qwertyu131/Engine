"""Tests for scripts/compile_shaders.py (stdlib unittest; the shaderc process is faked).

Run: python -m unittest discover tests   (or: python -m unittest tests/unit/scripts/test_compile_shaders.py)
The real compile of every permutation is the gate command `python scripts/compile_shaders.py --out <dir>`.
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


def _load_module():
    spec = importlib.util.spec_from_file_location("compile_shaders", REPO / "scripts" / "compile_shaders.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["compile_shaders"] = mod
    spec.loader.exec_module(mod)
    return mod


cs = _load_module()
PERMS = REPO / "shaders" / "pbr" / "permutations.json"


class FakeShaderc:
    """Stands in for the shaderc process: writes the output file and prints what shaderc prints."""

    def __init__(self, fail: set[tuple[str, str, str]] | None = None) -> None:
        self.fail = fail or set()  # (target, entry, key)
        self.calls: list[list[str]] = []

    def __call__(self, cmd: list[str]) -> tuple[int, str, str]:
        self.calls.append(cmd)
        arg = {cmd[i]: cmd[i + 1] for i in range(len(cmd) - 1) if cmd[i].startswith("--")}
        target, entry, out = arg["--target"], arg["--entry"], Path(arg["--out"])
        key = out.name
        if (target, entry, key) in self.fail:
            return 1, "", f"shaderc: {target} {entry}: dxc failed (exit 1)"
        out.mkdir(parents=True, exist_ok=True)
        f = out / f"pbr.{entry}.{target}.bin"
        f.write_bytes(b"x" * 10)
        status = "PENDING-CI" if target == "msl" else "passed"
        return 0, f"shaderc: {target} {entry} -> {f} [{status}]\n", ""


class MatrixTest(unittest.TestCase):
    def test_matrix_is_8_per_target_per_stage(self) -> None:
        perms = cs.load_permutations(PERMS)
        jobs = cs.expand_jobs(perms)
        self.assertEqual(len(jobs), 7 * 8 * 2)
        per = {}
        for j in jobs:
            per[(j.target, j.stage)] = per.get((j.target, j.stage), 0) + 1
        self.assertEqual(set(per.values()), {8})
        self.assertEqual(len(per), 14)
        self.assertEqual({t for j in jobs if j.target == "glsl-es300" for t in [j.defines["TIER"]]}, {"0"})
        self.assertEqual({t for j in jobs if j.target == "dxil" for t in [j.defines["TIER"]]}, {"2"})
        self.assertEqual({t for j in jobs if j.target == "wgsl" for t in [j.defines["TIER"]]}, {"1"})

    def test_defines_are_in_canonical_order(self) -> None:
        jobs = cs.expand_jobs(cs.load_permutations(PERMS))
        self.assertEqual(list(jobs[0].defines),
                         ["TIER", "FG_SKIN", "FG_MORPH", "FG_UNLIT", "FG_CLEARCOAT_SHEEN_SPEC", "FG_TRANSMISSION_VOLUME"])
        self.assertEqual(jobs[0].key, "T2_SKIN0_MORPH0_UNLIT0")

    def test_excluded_must_be_complement_of_tiers(self) -> None:
        perms = json.loads(PERMS.read_text(encoding="utf-8"))
        perms["excluded"] = [e for e in perms["excluded"] if e["target"] != "wgsl"]
        with self.assertRaisesRegex(ValueError, "wgsl"):
            cs.check_permutations(perms)


class RunTest(unittest.TestCase):
    def run_main(self, fake: FakeShaderc, out: Path, extra: list[str] | None = None) -> tuple[int, str]:
        shaderc = out.parent / "shaderc.exe"
        shaderc.write_bytes(b"")
        buf = io.StringIO()
        with redirect_stdout(buf), redirect_stderr(buf):
            rc = cs.main(["--out", str(out), "--shaderc", str(shaderc), "--jobs", "4"] + (extra or []), runner=fake)
        return rc, buf.getvalue()

    def test_all_ok_summary(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "ç ğ out"
            fake = FakeShaderc()
            rc, _ = self.run_main(fake, out)
            self.assertEqual(rc, 0)
            s = json.loads((out / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(s["total"], 112)
            self.assertEqual(s["ok"], 96)
            self.assertEqual(s["failed"], [])
            self.assertEqual(len(s["pendingCi"]), 16)
            self.assertTrue(all("msl" in p for p in s["pendingCi"]))
            self.assertIsInstance(s["msTotal"], (int, float))
            self.assertEqual(len(s["items"]), 112)
            self.assertEqual(s["items"][0]["bytes"], 10)
            # One output directory per (target, permutation): nothing overwritten.
            dirs = {c[c.index("--out") + 1] for c in fake.calls}
            self.assertEqual(len(dirs), 56)
            self.assertTrue(all("--validate" in c for c in fake.calls))

    def test_failure_exits_1_and_is_listed(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "out"
            fake = FakeShaderc(fail={("wgsl", "PSMain", "T1_SKIN0_MORPH0_UNLIT1")})
            rc, _ = self.run_main(fake, out)
            self.assertEqual(rc, 1)
            s = json.loads((out / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(len(s["failed"]), 1)
            self.assertEqual(s["failed"][0]["id"], "wgsl/T1_SKIN0_MORPH0_UNLIT1/PSMain")
            self.assertIn("dxc failed", s["failed"][0]["error"])
            self.assertEqual(s["ok"] + len(s["failed"]) + len(s["pendingCi"]), s["total"])

    def test_dxbc_path_is_passed_through(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            fake = FakeShaderc()
            rc, _ = self.run_main(fake, Path(d) / "out", ["--dxbc-path", "direct"])
            self.assertEqual(rc, 0)
            dxbc = [c for c in fake.calls if c[c.index("--target") + 1] == "dxbc"]
            self.assertTrue(dxbc)
            self.assertTrue(all(c[c.index("--dxbc-path") + 1] == "direct" for c in dxbc))

    def test_missing_shaderc_exits_2(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            buf = io.StringIO()
            with redirect_stdout(buf), redirect_stderr(buf):
                rc = cs.main(["--out", str(Path(d) / "out"), "--shaderc", str(Path(d) / "no-shaderc.exe")],
                             runner=FakeShaderc())
            self.assertEqual(rc, 2)
            self.assertIn("tool not found: shaderc", buf.getvalue())


if __name__ == "__main__":
    unittest.main()
